"""Regenerate the synthetic gapless fixtures with the libmp3lame C API.

Optional maintenance tool, never needed by make test. Pass a libmp3lame shared
library path as the sole argument; see README.md for the recorded version.
"""
import ctypes as c
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'third_party/yeney-core/tests'))
from decoders_test import signal_data

library = c.CDLL(sys.argv[1])
library.lame_init.restype = c.c_void_p
for name in ('in_samplerate', 'num_channels', 'brate', 'quality', 'bWriteVbrTag'):
    getattr(library, 'lame_set_' + name).argtypes = [c.c_void_p, c.c_int]
for name in ('lame_init_params', 'lame_close'):
    getattr(library, name).argtypes = [c.c_void_p]
library.lame_encode_buffer_interleaved.argtypes = [c.c_void_p, c.POINTER(c.c_short), c.c_int, c.c_void_p, c.c_int]
library.lame_encode_flush.argtypes = [c.c_void_p, c.c_void_p, c.c_int]
library.lame_get_lametag_frame.argtypes = [c.c_void_p, c.c_void_p, c.c_size_t]
library.lame_get_lametag_frame.restype = c.c_size_t
for start, count in ((0, 70130), (70130, 80060)):
    raw, _ = signal_data(count, start=start)
    encoder = library.lame_init()
    assert encoder
    try:
        for key, value in (('in_samplerate', 44100), ('num_channels', 2),
                           ('brate', 192), ('quality', 2), ('bWriteVbrTag', 1)):
            assert getattr(library, 'lame_set_' + key)(encoder, value) == 0
        assert library.lame_init_params(encoder) == 0
        buffer = c.create_string_buffer(262144)
        samples = (c.c_short * (count * 2)).from_buffer_copy(raw)
        n = library.lame_encode_buffer_interleaved(encoder, samples, count, buffer, len(buffer))
        assert n >= 0
        data = buffer.raw[:n]
        n = library.lame_encode_flush(encoder, buffer, len(buffer))
        assert n >= 0
        data += buffer.raw[:n]
        n = library.lame_get_lametag_frame(encoder, buffer, len(buffer))
        assert 0 < n <= len(buffer)
        data = buffer.raw[:n] + data[n:]
        (Path(__file__).parent / f'lame-{start}.mp3').write_bytes(data)
    finally:
        library.lame_close(encoder)
