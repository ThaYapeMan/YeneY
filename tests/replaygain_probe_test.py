"""S9 packet, PCM and measurement fixtures; no physical devices or network I/O."""
import importlib.util
import math
import os
from pathlib import Path
import random
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('probe', ROOT / 'scripts/replaygain_probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
PLAYER = '00:11:22:33:44:55'
LMS, BRIDGE, SONOS = '192.0.2.1', '192.0.2.2', '192.0.2.3'


def invalid(function, *args):
    try:
        function(*args)
    except probe.Invalid:
        return
    raise AssertionError(f'{function.__name__} accepted invalid evidence')


def strm(gain, player=PLAYER):
    packet = bytearray(28)
    packet[:5] = b'strms'
    struct.pack_into('!I', packet, 18, gain)
    packet += f'GET /stream.mp3?player={player} HTTP/1.0\r\n\r\n'.encode()
    return struct.pack('!H', len(packet)) + packet


def wire(link, src, dst, sport, dport, seq, payload, ipv6=False):
    tcp = struct.pack('!HHIIBBHHH', sport, dport, seq & 0xffffffff, 0, 0x50, 0x18, 65535, 0, 0) + payload
    if ipv6:
        family, proto = socket.AF_INET6, 0x86dd
        ip = struct.pack('!IHBB', 6 << 28, len(tcp), 6, 64) + socket.inet_pton(family, src) + socket.inet_pton(family, dst)
    else:
        proto = 0x0800
        ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20+len(tcp), 0, 0, 64, 6, 0,
                         socket.inet_aton(src), socket.inet_aton(dst))
    headers = {1: b'\0'*12 + struct.pack('!H', proto),
               113: b'\0'*14 + struct.pack('!H', proto),
               276: struct.pack('!H', proto) + b'\0'*18}
    return headers[link] + ip + tcp


def capture(path, audio, gain, link=1, ipv6=False, broken=False):
    src, bridge, speaker = (('2001:db8::1', '2001:db8::2', '2001:db8::3') if ipv6 else (LMS, BRIDGE, SONOS))
    header = struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 262144, link)
    # Include a complete chunk and a deliberately truncated last chunk.
    body = f'{len(audio)//2:x};fixture=yes\r\n'.encode() + audio[:len(audio)//2] + b'\r\n'
    body += f'{len(audio):x}\r\n'.encode() + audio[len(audio)//2:]
    http = b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n' + body
    packets = []
    # Unrelated room and smaller valid response must not win selection.
    for a, b, port, peer, data in [(src, bridge, 3483, 40000, b'\x00\x04vers' + strm(gain)),
                                  (src, bridge, 3483, 40001, strm(123, 'other')),
                                  (bridge, speaker, 1401, 42000, http),
                                  (bridge, speaker, 1402, 42001, b'HTTP/1.0 200 OK\r\n\r\nfLaCsmall')]:
        seq = 0xfffffff0  # wraparound, with out-of-order delivery and duplicates
        chunks = [(seq+i, data[i:i+137]) for i in range(0, len(data), 137)]
        if broken and port == 1401 and len(chunks) > 3:
            chunks.pop(2)
        order = chunks[1::2] + chunks[::2] + chunks[:1]
        for position, chunk in order:
            packet = wire(link, a, b, port, peer, position, chunk, ipv6)
            packets.append(struct.pack('<IIII', 1, 0, len(packet), len(packet))+packet)
    path.write_bytes(header + b''.join(packets))


def wav(bits, values, rate=1000, extensible=True):
    width = bits//8
    pcm = b''.join(int(v).to_bytes(width, 'little', signed=True) for v in values)
    fmt = struct.pack('<HHIIHH', 0xfffe if extensible else 1, 1, rate, rate*width, width, bits)
    if extensible:
        fmt += struct.pack('<HHI', 22, bits, 4) + bytes.fromhex('0100000000001000800000aa00389b71')
    body = b'WAVE' + b'JUNK\x01\0\0\0x\0' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt
    body += b'data' + struct.pack('<I', len(pcm)) + pcm
    return b'RIFF' + struct.pack('<I', len(body)) + body


with tempfile.TemporaryDirectory(prefix='yeney-rg-probe-') as directory:
    tmp = Path(directory)
    raw_gain = round(65536*10**(-8.01/20))
    for link in (1, 113, 276):
        for ipv6 in (False, True):
            path = tmp / 'capture.pcap'
            capture(path, b'fLaC'+bytes(range(256))*4, raw_gain, link, ipv6)
            gain, payload, _ = probe.extract(path, '2001:db8::3' if ipv6 else SONOS, PLAYER)
            assert gain == raw_gain and payload == b'fLaC'+bytes(range(256))*4
    assert abs(probe.gain_db(raw_gain)+8.01) < .001
    assert probe.gain_db(0) == 0
    assert probe.starts(b'partial-prefix' + strm(raw_gain), PLAYER) == [raw_gain]
    assert probe.starts(strm(raw_gain)[:-5], PLAYER) == []
    invalid(probe.reassemble, [(1, b'abc'), (2, b'XX')])
    invalid(probe.reassemble, [(1, b'abc'), (5, b'e')])
    capture(path, b'fLaC'+bytes(range(256))*4, raw_gain, broken=True)
    invalid(probe.extract, path, SONOS, PLAYER)
    assert probe.unchunk(b'3\r\nabc\r\n5\r\nde') == b'abcde'
    invalid(probe.unchunk, b'wat\r\nabc')
    assert probe.http_audio(b'HTTP/1.1 302 Found\r\n\r\nfLaC') == b''
    print('PASS: ReplayGain pcap Ethernet/SLL/SLL2 IPv4/IPv6, sequence wrap, reordered/retransmitted TCP, player isolation, strm offset 18, largest HTTP and truncated chunks')
    print('PASS: ReplayGain missing segments, conflicting retransmissions and malformed chunks are INVALID')

    for bits in (16, 24):
        for ext in (False, True):
            path = tmp/'pcm.wav'
            value = 1 << (bits-2)
            path.write_bytes(wav(bits, [value, -value]*500, extensible=ext))
            env, peak, fmt = probe.wav_envelope(path)
            assert fmt == (1000, 1, bits) and len(env) == 100
            assert all(abs(x-.25) < 1e-10 for x in env)
            assert abs(peak+6.020599913) < 1e-6
    print('PASS: ReplayGain WAV PCM/extensible 16/24-bit signed samples, odd-sized chunks, 10 ms mean square and peak dBFS')

    rng = random.Random(2026)
    original = [rng.uniform(.01, .8) for _ in range(2400)]
    ratio = 10**(-8.01/10)
    shifted = [0.0]*237 + [value*ratio for value in original]
    corr, lag, overlap, measured = probe.align_levels(shifted, original)
    assert corr > .999999 and lag == -2.37 and overlap > 20
    assert abs(measured+8.01) < .05
    # Excluded startup energy must not influence either alignment or levels.
    shifted[:100] = [1e4]*100
    original[:100] = [1e4]*100
    corr, lag, _, measured = probe.align_levels(shifted, original)
    assert corr > .999999 and abs(measured+8.01) < .05
    invalid(probe.align_levels, [1]*1000, [1]*1000)
    other = [rng.random() for _ in original]
    track = (raw_gain, shifted, -10, (1000, 1, 24), ('fixture',), 0)
    off = (0, original, -2, (1000, 1, 24), ('fixture',), 0)
    assert probe.evaluate(track, off, -8.01)[0] == 'PASS'
    assert probe.evaluate(track, off, -6.0, -8.01)[0] == 'PASS'
    assert probe.evaluate(track, off, -6.0)[0] == 'FAIL'
    assert probe.evaluate(track, (1, *off[1:]), -8.01)[0] == 'FAIL'
    assert probe.evaluate((raw_gain, original, *track[2:]), off, -8.01)[0] == 'FAIL'
    assert probe.evaluate((raw_gain, other, *track[2:]), off, -8.01)[0] == 'INVALID'
    print('PASS: ReplayGain alignment +/-6 s, excluded first second, -8.01 dB within 0.05 dB, confirmed LMS adjustment, missing gain, ignored gain and low-correlation INVALID')

    if not shutil.which('flac'):
        print('SKIP: ReplayGain real-FLAC end-to-end fixture: flac missing; apt-get install -y flac')
    else:
        rate = 8000
        # Nonperiodic amplitude envelope with stereo-independent packet boundaries.
        levels = [rng.uniform(.02, .7) for _ in range(1000)]
        signal = [int((1 << 22)*levels[i//80]*math.sin(2*math.pi*317*i/rate)) for i in range(rate*10)]
        scaled = [0]*int(.37*rate) + [round(x*raw_gain/65536) for x in signal]
        for name, values, sent in [('track', scaled, raw_gain), ('off', signal, 0)]:
            source = tmp/f'{name}-source.wav'
            source.write_bytes(wav(24, values, rate=rate))
            flac = tmp/f'{name}-source.flac'
            subprocess.run(['flac', '-f', '-s', '-o', str(flac), str(source)], check=True, capture_output=True)
            capture(tmp/f'{name}.pcap', flac.read_bytes(), sent, 276)
        report = subprocess.run([sys.executable, str(ROOT/'scripts/replaygain_probe.py'),
            '--track', str(tmp/'track.pcap'), '--off', str(tmp/'off.pcap'), '--speaker', SONOS,
            '--player', PLAYER, '--tag-db=-8.01'], capture_output=True, text=True)
        assert report.returncode == 0, report.stdout + report.stderr
        assert (tmp/'track.wav').exists() and (tmp/'off.wav').exists()
        assert abs(float(re.search(r'measured=([-0-9.]+)', report.stdout)[1])+8.01) < .05
        print(report.stdout, end='')
        print('PASS: ReplayGain real FLAC -> synthetic SLL2 capture -> reassembled/chunk-decoded FLAC -> decoded WAV -> aligned -8.01 dB measurement')
