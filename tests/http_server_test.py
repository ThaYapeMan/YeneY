"""Exercise the own listener on loopback, including real TCP backpressure."""
from pathlib import Path
import re
import socket
import subprocess
import time

process = subprocess.Popen(['./http-server-test'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
port = int(process.stdout.readline().split()[1])
def connect():
    client = socket.socket()
    client.settimeout(8)
    client.connect(('127.0.0.1', port))
    return client

def request(wire):
    with connect() as client:
        client.sendall(wire)
        result = b''
        while True:
            try:
                data = client.recv(65536)
            except ConnectionResetError:
                # Rejected oversized input can remain unread when the server
                # closes. Its complete HTTP error must still arrive first.
                assert result.endswith(b'\r\n\r\n'), result
                break
            if not data: break
            result += data
        assert b'Connection: close\r\n' in result, result
        return result

try:
    reply = request(b'GET /stream?value=A%26B+%C3%A9 HTTP/1.1\r\nx-mixed: yes\r\n\r\n')
    assert reply.endswith('A&B é|yes'.encode()), reply
    reply = request(b'HEAD /stream HTTP/1.1\r\n\r\n')
    assert b'Server: SONOS/2.13.2' in reply and b'Content-Type: audio/flac' in reply and reply.endswith(b'\r\n\r\n')
    assert b'404 Not Found' in request(b'GET /missing HTTP/1.1\r\n\r\n')
    assert b'431 Request Header Fields Too Large' in request(b'GET /stream HTTP/1.1\r\nX: ' + b'x' * 16384)
    start = time.monotonic()
    assert b'408 Request Timeout' in request(b'GET /stream HTTP/1.1\r\n')
    assert 4.9 <= time.monotonic() - start < 6
    for path, fixture in (('/avt', 'sonos-lastchange.xml'), ('/rc', 'rendering-lastchange.xml'), ('/zgt', 'zone-group-notify.xml')):
        body = Path('tests/fixtures', fixture).read_bytes()
        headers = (f'NOTIFY {path} HTTP/1.1\r\nNT: upnp:event\r\nNTS: upnp:propchange\r\n'
                   f'SID: uuid:test\r\nSEQ: 7\r\nContent-Length: {len(body)}\r\n').encode()
        assert b'200 OK' in request(headers + b'\r\n' + body)
        for invalid in (headers.replace(b'SEQ: 7', b'SEQ: -1'), headers.replace(b'SEQ: 7', b'SEQ: 4294967296'),
                        headers.replace(b'uuid:test', b'uuid:wrong'), headers + b'Transfer-Encoding: chunked\r\n',
                        headers + b'SID: uuid:duplicate\r\n', headers.replace(b'NT: upnp:event', b'NT: wrong')):
            assert b'412 Precondition Failed' in request(invalid + b'\r\n' + body)
    assert b'404 Not Found' in request(b'NOTIFY /unknown HTTP/1.1\r\n\r\n')
    icon = request(b'GET /images/pulseaudio.png?id=2.13.2 HTTP/1.1\r\n\r\n')
    header, body = icon.split(b'\r\n\r\n', 1)
    expected = Path('upnp/icon.png').read_bytes()
    assert body == expected
    assert b'Content-Type: image/png' in header and b'Cache-Control: public, max-age=86400' in header
    assert f'Content-Length: {len(expected)}'.encode() in header
    head = request(b'HEAD /images/pulseaudio.png HTTP/1.1\r\n\r\n')
    assert head.split(b'\r\n\r\n',1)[1] == b''
    print('PASS: shared-port NOTIFY dispatches AVTransport/RenderingControl/Topology with validation; icon GET/HEAD matches repository asset')
    print('PASS: own HTTP decoded query, case-insensitive headers, HEAD, 404, 16 KiB limit and 5 s header deadline')
    clients = [connect() for _ in range(16)]
    for client in clients:
        client.sendall(b'GET /stream?mode=hold HTTP/1.1\r\n\r\n')
        assert b'200 OK' in client.recv(4096)
    assert b'503 Service Unavailable' in request(b'GET /stream HTTP/1.1\r\n\r\n')
    for client in clients: client.close()
    time.sleep(.15)
    assert b'200 OK' in request(b'HEAD /stream HTTP/1.1\r\n\r\n')
    print('PASS: 16 concurrent connections accepted; connection 17 receives 503 and closes; capacity recovers')
    with connect() as client:
        client.sendall(b'GET /stream?mode=closed HTTP/1.1\r\n\r\n')
        assert b'200 OK' in client.recv(4096)
    assert process.stdout.readline().strip() == 'PEER CLOSED'
    with connect() as client:
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        client.sendall(b'GET /stream?mode=slow HTTP/1.1\r\n\r\n')
        line = process.stdout.readline().split()
        assert line[:2] == ['SEND', 'TIMEOUT'] and 50 <= int(line[2]) < 2000, line
    print('PASS: MSG_PEEK detects client close; SO_SNDTIMEO bounds a stalled TCP receiver')
finally:
    process.communicate('\n', timeout=10)
    assert process.returncode == 0
