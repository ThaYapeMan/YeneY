#!/usr/bin/env python3
"""Offline S9 measurement. Only Python stdlib and the flac executable are used.

Classic pcap (tcpdump -w): Ethernet, Linux SLL/SLL2; IPv4/IPv6 TCP.
Capture gaps/conflicting retransmissions and ambiguous starts are invalid evidence.
"""
import argparse
import math
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
from urllib.parse import parse_qs, urlsplit


class Invalid(ValueError):
    pass


def packets(path):
    with open(path, 'rb') as f:
        header = f.read(24)
        endian = {b'\xd4\xc3\xb2\xa1': '<', b'\xa1\xb2\xc3\xd4': '>',
                  b'\x4d\x3c\xb2\xa1': '<', b'\xa1\xb2\x3c\x4d': '>'}.get(header[:4])
        if not endian or len(header) != 24:
            raise Invalid('expected classic pcap from tcpdump -w')
        link = struct.unpack(endian + 'I', header[20:24])[0]
        if link not in (1, 113, 276):
            raise Invalid(f'unsupported pcap link type {link}')
        while raw := f.read(16):
            if len(raw) != 16:
                break  # capture interrupted while writing its final packet
            _, _, size, _ = struct.unpack(endian + 'IIII', raw)
            if size > 16 * 1024 * 1024:
                raise Invalid('unreasonable captured packet length')
            raw = f.read(size)
            if len(raw) != size:
                break
            yield link, raw


def tcp_packet(link, raw):
    offset, protocol_at = {1: (14, 12), 113: (16, 14), 276: (20, 0)}[link]
    if len(raw) < offset:
        return None
    proto = int.from_bytes(raw[protocol_at:protocol_at+2], 'big')
    while proto in (0x8100, 0x88a8):
        if len(raw) < offset + 4:
            return None
        proto = int.from_bytes(raw[offset+2:offset+4], 'big')
        offset += 4
    ip = raw[offset:]
    if proto == 0x0800 and len(ip) >= 20:
        ihl = (ip[0] & 15)*4
        length = int.from_bytes(ip[2:4], 'big')
        if ip[9] != 6 or ihl < 20 or len(ip) < ihl:
            return None
        if int.from_bytes(ip[6:8], 'big') & 0x3fff:
            raise Invalid('fragmented IPv4 is not supported')
        src, dst = socket.inet_ntop(socket.AF_INET, ip[12:16]), socket.inet_ntop(socket.AF_INET, ip[16:20])
        tcp = ip[ihl:length]
    elif proto == 0x86dd and len(ip) >= 40:
        src, dst = socket.inet_ntop(socket.AF_INET6, ip[8:24]), socket.inet_ntop(socket.AF_INET6, ip[24:40])
        end = 40 + int.from_bytes(ip[4:6], 'big')
        pos, nxt = 40, ip[6]
        while nxt in (0, 43, 60):
            if pos+2 > len(ip):
                return None
            nxt, step = ip[pos], (ip[pos+1]+1)*8
            pos += step
        if nxt == 44:
            raise Invalid('fragmented IPv6 is not supported')
        if nxt != 6:
            return None
        tcp = ip[pos:end]
    else:
        return None
    if len(tcp) < 20:
        return None
    sport, dport, seq = struct.unpack('!HHI', tcp[:8])
    offset = (tcp[12] >> 4)*4
    if offset < 20 or len(tcp) < offset:
        return None
    syn = bool(tcp[13] & 2)
    return (src, sport, dst, dport), (seq + syn) & 0xffffffff, tcp[offset:], syn


def reassemble(segments):
    """Unwrap sequence numbers, sort, and verify overlaps without filling gaps."""
    anchor = segments[0][0]
    ordered = sorted((((seq-anchor+2**31) % 2**32)-2**31, data) for seq, data in segments)
    start = ordered[0][0]
    result = bytearray()
    for seq, data in ordered:
        pos = seq-start
        if pos > len(result):
            raise Invalid(f'TCP capture gap of {pos-len(result)} bytes')
        overlap = min(len(data), len(result)-pos)
        if result[pos:pos+overlap] != data[:overlap]:
            raise Invalid('conflicting TCP retransmission')
        result.extend(data[overlap:])
    return bytes(result)


def flows(path):
    streams, current, syns = {}, {}, {}
    for link, raw in packets(path):
        packet = tcp_packet(link, raw)
        if packet is None:
            continue
        key, seq, data, syn = packet
        if syn and syns.get(key) != seq:
            current[key] = current.get(key, 0)+1
            syns[key] = seq
        if data:
            streams.setdefault((*key, current.get(key, 0)), []).append((seq, data))
    return streams


def starts(data, player=None):
    """Resync a capture started mid-connection, then follow server frame lengths.

    strm_packet is packed: gain at opcode offset 18, HTTP request at 28.
    Local LMS streams identify the player in /stream.mp3?player=<MAC>.
    """
    pos = 0
    found = []
    while pos+6 <= len(data):
        length = int.from_bytes(data[pos:pos+2], 'big')
        opcode = data[pos+2:pos+6]
        if length < 4 or not re.fullmatch(rb'[a-z][a-z0-9]{3}', opcode):
            pos += 1
            continue
        end = pos+2+length
        if end > len(data):
            pos += 1
            continue
        frame = data[pos+2:end]
        if opcode == b'strm' and len(frame) >= 28 and frame[4:5] == b's':
            if player:
                request = frame[28:].split(b'\r\n', 1)[0].decode('ascii', errors='replace').split()
                if len(request) < 2 or parse_qs(urlsplit(request[1]).query).get('player') != [player]:
                    pos = end
                    continue
            found.append(int.from_bytes(frame[18:22], 'big'))
        pos = end
    return found


def unchunk(data):
    result = bytearray()
    pos = 0
    while pos < len(data):
        end = data.find(b'\r\n', pos)
        if end < 0:
            break
        size_text = data[pos:end].split(b';', 1)[0]
        if not re.fullmatch(rb'[0-9a-fA-F]+', size_text):
            raise Invalid('invalid HTTP chunk size')
        size = int(size_text, 16)
        if not size:
            break
        pos = end+2
        result.extend(data[pos:pos+size])
        if pos+size+2 > len(data):
            break  # retain available payload of truncated last chunk
        if data[pos+size:pos+size+2] != b'\r\n':
            raise Invalid('invalid HTTP chunk terminator')
        pos += size+2
    return bytes(result)


def http_audio(data):
    header, sep, body = data.partition(b'\r\n\r\n')
    if not sep or not re.match(rb'HTTP/1\.[01] 200(?: |\r)', header):
        return b''
    fields = {}
    for line in header.split(b'\r\n')[1:]:
        key, colon, value = line.partition(b':')
        if colon:
            fields[key.lower()] = value.strip().lower()
    if b'chunked' in fields.get(b'transfer-encoding', b''):
        body = unchunk(body)
    elif b'content-length' in fields:
        body = body[:int(fields[b'content-length'])]
    return body if body.startswith(b'fLaC') else b''


def extract(path, speaker, player=None):
    streams = flows(path)
    candidates = []
    for key, segments in streams.items():
        src, sport, dst, _, _ = key
        if 1400 <= sport <= 1409 and dst == speaker:
            body = http_audio(reassemble(segments))
            if body:
                candidates.append((len(body), key, body))
    if not candidates:
        raise Invalid('no complete HTTP header and FLAC response to coordinator')
    _, chosen, audio = max(candidates, key=lambda row: row[0])
    gains = []
    for key, segments in streams.items():
        if key[1] == 3483 and key[2] == chosen[0]:
            gains.extend(starts(reassemble(segments), player))
    if len(gains) != 1:
        raise Invalid(f'expected one matching strm s; found {len(gains)} (wrong player, missing capture or multiple starts)')
    return gains[0], audio, chosen


def wav_envelope(path):
    raw = Path(path).read_bytes()
    if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
        raise Invalid('decoder did not produce RIFF/WAVE')
    pos, fmt, pcm = 12, None, None
    while pos+8 <= len(raw):
        kind, size = struct.unpack_from('<4sI', raw, pos)
        chunk = raw[pos+8:pos+8+size]
        if kind == b'fmt ': fmt = chunk
        if kind == b'data': pcm = chunk
        pos += 8+size+(size & 1)
    if fmt is None or len(fmt) < 16 or pcm is None:
        raise Invalid('missing WAV fmt/data')
    tag, channels, rate, _, align, bits = struct.unpack_from('<HHIIHH', fmt)
    if tag == 0xfffe:
        pcm_guid = bytes.fromhex('0100000000001000800000aa00389b71')
        if len(fmt) < 40 or int.from_bytes(fmt[16:18], 'little') < 22 or fmt[24:40] != pcm_guid:
            raise Invalid('unsupported extensible WAV subtype')
        valid = int.from_bytes(fmt[18:20], 'little')
        if valid not in (0, bits):
            raise Invalid('unsupported WAV valid sample bits')
    elif tag != 1:
        raise Invalid('WAV must contain integer PCM')
    if bits not in (16, 24) or channels < 1 or align != channels*(bits//8) or rate % 100:
        raise Invalid('expected 16/24-bit PCM at a rate divisible by 100')
    width, block = bits//8, rate//100
    scale = float(1 << (bits-1))
    envelope, total, peak, count = [], 0.0, 0, 0
    for offset in range(0, len(pcm)-align+1, align):
        for ch in range(channels):
            start = offset + ch*width
            value = int.from_bytes(pcm[start:start+width], 'little', signed=True)
            total += (value/scale)**2
            peak = max(peak, abs(value))
        count += 1
        if count == block:
            envelope.append(total/(block*channels))
            total, count = 0.0, 0
    if not envelope or not peak:
        raise Invalid('empty or silent WAV')
    return envelope, 20*math.log10(peak/scale), (rate, channels, bits)


def align_levels(track, off):
    # a[i] corresponds to b[i+lag]. Both first seconds are always excluded.
    best = None
    for lag in range(-600, 601):
        start, end = max(100, 100-lag), min(len(track), len(off)-lag)
        n = end-start
        if n < 300:
            continue
        a, b = track[start:end], off[start+lag:end+lag]
        sa, sb = sum(a), sum(b)
        va = sum(x*x for x in a)-sa*sa/n
        vb = sum(x*x for x in b)-sb*sb/n
        if va <= 1e-20 or vb <= 1e-20:
            continue
        corr = (sum(x*y for x, y in zip(a, b))-sa*sb/n)/math.sqrt(va*vb)
        if best is None or corr > best[0]:
            best = (corr, lag, n, sa, sb)
    if best is None:
        raise Invalid('need at least 3 seconds of nonconstant overlapping audio after first-second exclusion')
    corr, lag, n, sa, sb = best
    if sa <= 0 or sb <= 0:
        raise Invalid('silent overlap')
    return corr, lag/100, n/100, 10*math.log10(sa/sb)


def gain_db(raw):
    return 20*math.log10(raw/65536) if raw else 0.0


def decode(pcap, speaker, player):
    gain, audio, flow = extract(pcap, speaker, player)
    flac_path, wav_path = pcap.with_suffix('.flac'), pcap.with_suffix('.wav')
    flac_path.write_bytes(audio)
    result = subprocess.run(['flac', '-d', '--decode-through-errors', '-f', '-o', str(wav_path), str(flac_path)],
                            capture_output=True, text=True, timeout=120)
    pcap.with_suffix('.decode.log').write_text(result.stdout+result.stderr)
    # Truncated capture tails can produce a nonzero decoder status with usable PCM.
    if not wav_path.exists():
        raise Invalid(f'flac produced no WAV (exit {result.returncode})')
    envelope, peak, fmt = wav_envelope(wav_path)
    return gain, envelope, peak, fmt, flow, result.returncode


def evaluate(track, off, tagged, effective=None):
    sent, envelope, peak, fmt, flow, code = track
    sent_off, envelope_off, peak_off, fmt_off, flow_off, code_off = off
    if fmt != fmt_off:
        raise Invalid(f'audio formats differ: {fmt} vs {fmt_off}')
    corr, lag, overlap, measured = align_levels(envelope, envelope_off)
    db, off_db = gain_db(sent), gain_db(sent_off)
    expected = db-off_db
    tag_ok = sent != 0 and abs(db-tagged) <= .1
    # LMS status exposes playingSong()->replayGain(), after server adjustments.
    adjusted = sent != 0 and effective is not None and abs(db-effective) <= .1
    first_ok = tag_ok or adjusted
    checks = [
        f"{'PASS' if first_ok else 'FAIL'} run 1 sent gain: raw={sent} ({db:.3f} dB); track tag={tagged:.3f} dB" +
        (f'; LMS calculated={effective:.3f} dB' if effective is not None else '') +
        ('; LMS adjustment confirmed, using SENT gain' if adjusted and not tag_ok else ''),
        f"{'PASS' if sent_off == 0 else 'FAIL'} run 2 sent gain: raw={sent_off} ({off_db:.3f} dB; 0 means none)",
        f"{'PASS' if abs(measured-expected) <= .5 else 'FAIL'} output difference: measured={measured:.3f} dB; sent difference={expected:.3f} dB; tolerance=0.5 dB",
        f"{'PASS' if corr >= .95 else 'INVALID'} alignment: correlation={corr:.6f}; threshold=0.95; offset={lag:+.2f} s; overlap={overlap:.2f} s",
        f'peaks: track={peak:.3f} dBFS; off={peak_off:.3f} dBFS; PCM={fmt}',
        f'flows: track={flow}; off={flow_off}; flac exits={code}/{code_off}',
    ]
    status = 'INVALID' if corr < .95 else ('PASS' if first_ok and sent_off == 0 and abs(measured-expected) <= .5 else 'FAIL')
    reason = f'tag={tagged:.3f} dB; sent={db:.3f}/{off_db:.3f} dB (raw {sent}/{sent_off}); measured={measured:.3f} dB; correlation={corr:.6f}; offset={lag:+.2f} s; peaks={peak:.3f}/{peak_off:.3f} dBFS'
    return status, checks, reason


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--track', type=Path, required=True)
    parser.add_argument('--off', type=Path, required=True)
    parser.add_argument('--speaker', required=True)
    parser.add_argument('--player', required=True)
    parser.add_argument('--tag-db', type=float, required=True)
    parser.add_argument('--effective-db', type=float)
    args = parser.parse_args()
    try:
        if not shutil.which('flac'):
            raise Invalid('flac missing: apt-get install -y flac')
        if not math.isfinite(args.tag_db) or (args.effective_db is not None and not math.isfinite(args.effective_db)):
            raise Invalid('nonfinite ReplayGain metadata')
        status, checks, reason = evaluate(decode(args.track, args.speaker, args.player),
                                          decode(args.off, args.speaker, args.player), args.tag_db, args.effective_db)
        print('\n'.join(checks))
    except (Invalid, OSError, subprocess.SubprocessError, ValueError) as exc:
        status, reason = 'INVALID', str(exc)
    print(f'{status} | {reason}')
    return {'PASS': 0, 'FAIL': 1, 'INVALID': 2}[status]


if __name__ == '__main__':
    raise SystemExit(main())
