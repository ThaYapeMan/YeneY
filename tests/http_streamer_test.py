"""Real SBStreamer ownership and headers over the own TCP listener."""
import queue
import socket
import subprocess
import threading
import time

p = subprocess.Popen(['./streamer-test', 'http-network'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
hello = p.stdout.readline().split()
assert hello[0] == 'HTTP', hello
port, session = int(hello[1]), hello[2]
lines, messages = [], queue.Queue()
def collect():
    for line in p.stdout:
        lines.append(line)
        messages.put(line)
reader = threading.Thread(target=collect)
reader.start()
clients = []
try:
    # Establish all sockets first, then deliver complete GETs in a tight burst.
    for _ in range(4):
        clients.append(socket.create_connection(('127.0.0.1', port), timeout=3))
    wire = f'GET /music/squeezebox.flac?session={session}&stream=1 HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode()
    start = time.monotonic_ns()
    for c in clients: c.sendall(wire)
    elapsed = (time.monotonic_ns() - start) / 1e6
    assert elapsed < 5, elapsed
    deadline = time.monotonic() + 3
    while sum('STANDBY' in l for l in lines) != 3:
        assert time.monotonic() < deadline, lines
        messages.get(timeout=3)
    assert sum(' ACTIVE' in l for l in lines) == 1, lines
    # Thread scheduling may choose any socket as ACTIVE. Exactly one gets
    # headers; the other three must remain silent.
    received = []
    for c in clients:
        c.settimeout(.1)
        try: received.append(c.recv(65536))
        except socket.timeout: received.append(b'')
    headers = [r for r in received if r]
    assert len(headers) == 1 and b'HTTP/1.1 200 OK' in headers[0], received
    assert b'Server: libnoson/2.13.2' in headers[0] and b'Connection: close' in headers[0]
    print(f'PASS: real own HTTP + SBStreamer: ACTIVE + 3 STANDBY GETs sent in {elapsed:.3f} ms; exactly one 200, three silent standbys')
finally:
    for c in clients: c.close()
    p.stdin.write('\n'); p.stdin.flush()
    p.wait(timeout=10); reader.join(timeout=2)
    print(''.join(lines), end='')
    assert p.returncode == 0
