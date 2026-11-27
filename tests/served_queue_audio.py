# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Decode YeneY queue responses to little-endian, interleaved 24-bit PCM."""

import ctypes
import ctypes.util
import dataclasses
import struct


def _table(width, polynomial):
    mask = (1 << width) - 1
    high = 1 << (width - 1)
    result = []
    for byte in range(256):
        value = byte << (width - 8)
        for _ in range(8):
            value = ((value << 1) ^ (polynomial if value & high else 0)) & mask
        result.append(value)
    return result


_CRC8 = _table(8, 0x07)
_CRC16 = _table(16, 0x8005)


def _crc8(data):
    value = 0
    for byte in data:
        value = _CRC8[value ^ byte]
    return value


def _crc16_python(data):
    value = 0
    for byte in data:
        value = ((value << 8) & 65535) ^ _CRC16[(value >> 8) ^ byte]
    return value


def _crc16():
    # FFmpeg is already used for generated media. Use its CRC routine when
    # available so validating every frame of the long soaks remains cheap.
    try:
        lib = ctypes.CDLL(ctypes.util.find_library("avutil") or "libavutil.so")
        lib.av_crc_get_table.argtypes = (ctypes.c_int,)
        lib.av_crc_get_table.restype = ctypes.c_void_p
        lib.av_crc.argtypes = (ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t)
        lib.av_crc.restype = ctypes.c_uint32
        table = lib.av_crc_get_table(1)  # AV_CRC_16_ANSI
        if table:
            def accelerated(data):
                value = lib.av_crc(table, 0, data, len(data))
                return (value >> 8) | ((value & 255) << 8)

            if accelerated(b"123456789") == _crc16_python(b"123456789"):
                return accelerated
    except (OSError, AttributeError):
        pass
    return _crc16_python


_crc16_value = _crc16()


@dataclasses.dataclass(frozen=True)
class RowHeader:
    wire: bytes
    rate: int
    samples: int
    bits: int
    flac: bool
    packed_length: int = 0
    seek_points: tuple = ()

    @classmethod
    def read(cls, source, *, wire_length=None):
        magic = source.read(4)
        if magic == b"RIFF":
            wire = magic + source.read(40)
            assert len(wire) == 44 and wire[8:12] == b"WAVE" and wire[36:40] == b"data", wire
            rate = int.from_bytes(wire[24:28], "little")
            samples = int.from_bytes(wire[40:44], "little") // 6
            assert wire[22:24] == b"\x02\0" and wire[34:36] == b"\x18\0"
            return cls(wire, rate, samples, 24, False)
        assert magic == b"fLaC", magic
        wire = magic + source.read(38)
        assert len(wire) == 42 and wire[4] & 0x7f == 0 and wire[5:8] == b"\0\0\x22", wire
        packed = int.from_bytes(wire[18:26], "big")
        rate, channels = packed >> 44, 1 + ((packed >> 41) & 7)
        samples, bits = packed & ((1 << 36) - 1), 1 + ((packed >> 36) & 31)
        assert samples > 0 and rate > 0 and bits in (16, 24) and channels == 2
        if wire[4] & 0x80:
            assert wire[8:12] == b"\x01\0\x01\0"
            return cls(wire, rate, samples, bits, True)
        points = []
        last = False
        while not last:
            header = source.read(4)
            assert len(header) == 4
            last = bool(header[0] & 0x80)
            size = int.from_bytes(header[1:], "big")
            body = source.read(size)
            assert len(body) == size
            wire += header + body
            if header[0] & 0x7f == 3:
                points = [(sample, offset) for sample, offset, _ in struct.iter_unpack(">QQH", body)
                          if sample < samples]
        assert points
        if wire_length is None:
            content_range = source.getheader("Content-Range")
            wire_length = int(content_range.rsplit("/", 1)[1] if content_range else source.getheader("Content-Length"))
        return cls(wire, rate, samples, bits, True, wire_length,
                   tuple((sample, offset + len(wire)) for sample, offset in points))

    @property
    def wire_length(self):
        return self.packed_length or self.declared_length

    @property
    def declared_length(self):
        if not self.flac:
            return 44 + 6 * self.samples
        return self.frame_offset((self.samples + 255) // 256)

    @property
    def pcm_header(self):
        if not self.flac:
            return self.wire
        length = 6 * self.samples
        return struct.pack("<4sI8sIHHIIHH4sI", b"RIFF", length + 36, b"WAVEfmt ", 16,
                           1, 2, self.rate, 6 * self.rate, 6, 24, b"data", length)

    def frame_offset(self, index):
        """Wire byte offset of a fixed FLAC block or the end of the row."""
        extra = sum(max(0, index - limit) for limit in (128, 2048, 65536, 2097152, 67108864))
        return 42 + min(self.samples, 256 * index) * (self.bits // 4) + 12 * index + extra

    def frame_range(self, sample, *, speaker_seek=False):
        """Return the Range start and the first sample to deliver."""
        if not self.flac:
            return 44 + sample * 6, sample
        if self.packed_length:
            if sample >= self.samples:
                return self.wire_length, self.samples
            first, offset = max((point for point in self.seek_points if point[0] <= sample),
                                default=(0, len(self.wire)))
            return offset, first
        if speaker_seek:
            sample = min(self.samples, (sample // 256 + 2) * 256)
        index = (self.samples + 255) // 256 if sample == self.samples else sample // 256
        return self.frame_offset(index), sample


def _read_flac_frame(source, info, index):
    fixed = source.read(5)
    if len(fixed) != 5:
        raise EOFError(f"incomplete FLAC frame header {index}")
    assert fixed[:3] == bytes((0xff, 0xf8, {44100: 0x79, 48000: 0x7a}[info.rate])), ("FLAC frame header", index, fixed)
    expected_depth = 0x1c if info.bits == 24 else 0x18
    assert fixed[3] == expected_depth, (index, fixed[3], info.bits)
    lead = fixed[4]
    length = 1 if lead < 128 else 8 - (255 ^ lead).bit_length()
    expected_width = 1 + sum(index >= limit for limit in (128, 2048, 65536, 2097152, 67108864))
    assert 1 <= length <= 6 and length == expected_width and (lead < 128 or length > 1)
    continuation = source.read(length - 1)
    if len(continuation) != length - 1:
        raise EOFError(f"incomplete FLAC frame number {index}")
    number = lead if length == 1 else lead & ((1 << (7 - length)) - 1)
    for byte in continuation:
        assert byte & 0xc0 == 0x80
        number = (number << 6) | (byte & 0x3f)
    assert number == index, ("out-of-order FLAC frame", index, number)
    size_crc = source.read(3)
    if len(size_crc) != 3:
        raise EOFError(f"incomplete FLAC frame header CRC {index}")
    count = int.from_bytes(size_crc[:2], "big") + 1
    assert count == min(256, info.samples - index * 256), ("FLAC sample count", count, index)
    header = fixed + continuation + size_crc
    assert _crc8(header) == 0, ("FLAC header CRC", index)
    width = info.bits // 8
    channel_size = count * width
    data = source.read(2 * (channel_size + 1) + 2)
    if len(data) != 2 * (channel_size + 1) + 2:
        raise EOFError(f"incomplete FLAC frame {index}")
    assert _crc16_value(header + data) == 0, ("FLAC frame CRC", index)
    assert data[0] == data[channel_size + 1] == 2, ("FLAC subframe is not verbatim", index)
    pcm = bytearray(count * 6)
    for channel, start in enumerate((1, channel_size + 2)):
        for byte in range(width):
            pcm[channel * 3 + 2 - byte::6] = data[start + byte:start + channel_size:width]
    return pcm


_DecoderRead = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
                               ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p)
_DecoderWrite = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.POINTER(ctypes.POINTER(ctypes.c_int32)), ctypes.c_void_p)
_DecoderError = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p)


class PackedDecoder:
    """Reference decoder for packed streams and frame-boundary ranges."""

    def __init__(self, source, header, resync=False):
        self.lib = ctypes.CDLL(ctypes.util.find_library("FLAC"))
        self.lib.FLAC__stream_decoder_new.restype = ctypes.c_void_p
        for name in ("delete", "get_state", "process_single", "process_until_end_of_metadata"):
            getattr(self.lib, "FLAC__stream_decoder_" + name).argtypes = (ctypes.c_void_p,)
        init = self.lib.FLAC__stream_decoder_init_stream
        init.argtypes = (ctypes.c_void_p, _DecoderRead, *([ctypes.c_void_p] * 4),
                         _DecoderWrite, ctypes.c_void_p, _DecoderError, ctypes.c_void_p)
        self.decoder = self.lib.FLAC__stream_decoder_new()
        assert self.decoder
        self.state = {"failure": None, "current": None, "sample": None}
        state = self.state
        # The metadata was already read by RowHeader. Prefix a copy with the
        # MD5 cleared because this may be the tail of the original stream.
        prefix = bytearray(header.wire)
        prefix[26:42] = bytes(16)

        @_DecoderRead
        def read(_decoder, destination, count, _data):
            try:
                if prefix:
                    body = bytes(prefix[:count[0]])
                    del prefix[:len(body)]
                else:
                    body = source.read(count[0])
                ctypes.memmove(destination, body, len(body))
                count[0] = len(body)
                return 0 if body else 1
            except Exception as error:
                state["failure"] = error
                return 2

        @_DecoderWrite
        def write(_decoder, frame, channels, _data):
            try:
                fields = ctypes.cast(frame, ctypes.POINTER(ctypes.c_uint))
                count, number_type = fields[0], fields[5]
                number = ctypes.cast(frame + 24, ctypes.POINTER(ctypes.c_uint64))[0] if number_type else fields[6] * 256
                state["sample"] = number
                result = bytearray(count * 6)
                shift = 24 - header.bits
                for ch in range(2):
                    for i in range(count):
                        sample = channels[ch][i] << shift
                        for byte in range(3):
                            result[6 * i + ch * 3 + byte] = sample >> (8 * byte) & 255
                state["current"] = result
                return 0
            except Exception as error:
                state["failure"] = error
                return 1

        @_DecoderError
        def error(_decoder, status, _data):
            if resync and state["sample"] is None and status in (0, 1, 2):
                return
            state["failure"] = ValueError(f"libFLAC rejected queue response: {status}")

        self.callbacks = read, write, error
        assert init(self.decoder, read, None, None, None, None, write, None, error, None) == 0
        assert self.lib.FLAC__stream_decoder_process_until_end_of_metadata(self.decoder)

    def frame(self):
        self.state["current"] = None
        while self.state["current"] is None:
            assert self.lib.FLAC__stream_decoder_process_single(self.decoder)
            if self.state["failure"]:
                raise self.state["failure"]
            if self.lib.FLAC__stream_decoder_get_state(self.decoder) == 4:
                raise EOFError("FLAC ended before the declared sample count")
        return self.state["current"]

    def __del__(self):
        if getattr(self, "decoder", None):
            self.lib.FLAC__stream_decoder_delete(self.decoder)
            self.decoder = None


class RowReader:
    """Keep partial FLAC blocks across sequential reads and decode either row form."""

    def __init__(self, source, header=None, first_sample=0, resync=False):
        self.source = source
        self.header = header or RowHeader.read(source)
        self.remaining = (self.header.samples - first_sample) * 6
        self.index = first_sample // 256
        self.buffer = bytearray()
        self.skip = first_sample % 256 if header and header.flac else 0
        self.start = first_sample
        self.packed = PackedDecoder(source, self.header, resync) if self.header.packed_length or resync else None
        if resync:
            self.buffer = self.packed.frame()
            self.start = self.packed.state["sample"]
            self.remaining = (self.header.samples - self.start) * 6
            self.skip = 0

    def read_pcm(self, size=None):
        if not self.header.flac:
            return self.source.read(size)
        size = self.remaining if size is None else min(size, self.remaining)
        result = bytearray()
        while len(result) < size:
            if not self.buffer:
                self.buffer = self.packed.frame() if self.packed else _read_flac_frame(self.source, self.header, self.index)
                self.index += 1
                if self.skip:
                    del self.buffer[:self.skip * 6]
                    self.skip = 0
            count = min(size - len(result), len(self.buffer))
            result.extend(self.buffer[:count])
            del self.buffer[:count]
        self.remaining -= size
        if not self.remaining:
            assert not self.source.read(1), "extra bytes after announced FLAC samples"
        return bytes(result)

    def read_wav(self, size=None):
        if size is None:
            return self.header.pcm_header + self.read_pcm()
        return self.header.pcm_header[:size] + self.read_pcm(max(0, size - 44))
