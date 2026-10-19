"""Room startup policy through the real binary; all sockets stay on loopback."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import signal
import socket
import subprocess
import threading
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
HOST = '127.0.0.21'
SOAP = 'http://schemas.xmlsoap.org/soap/envelope/'


class Speaker(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_SUBSCRIBE(self):
        self.send_response(412)
        self.send_header('Content-Length', '0')
        self.send_header('Connection', 'close')
        self.end_headers()

    do_UNSUBSCRIBE = do_SUBSCRIBE

    def do_POST(self):
        self.rfile.read(int(self.headers['Content-Length']))
        action = self.headers['SOAPAction'].strip('"').split('#')[1]
        self.server.actions.append(action)
        service = 'AVTransport'
        values = ''
        if action == 'GetZoneGroupState':
            service = 'ZoneGroupTopology'
            coordinator = 'RINCON_00112233445501400'
            if self.server.group == 'member':
                coordinator = 'RINCON_66778899AABB01400'
            other = '' if self.server.group == 'solo' else (
                f'<ZoneGroupMember UUID="RINCON_66778899AABB01400" ZoneName="Other" '
                f'Location="http://{HOST}:1400/xml/device_description.xml"/>')
            topology = (f'<ZoneGroupState><ZoneGroups><ZoneGroup Coordinator="{coordinator}" ID="test">'
                        f'<ZoneGroupMember UUID="RINCON_00112233445501400" ZoneName="Study" '
                        f'Location="http://{HOST}:1400/xml/device_description.xml"/>{other}'
                        '</ZoneGroup></ZoneGroups></ZoneGroupState>')
            values = '<ZoneGroupState>'+escape(topology)+'</ZoneGroupState>'
        elif action == 'GetTransportInfo':
            values = '<CurrentTransportState>STOPPED</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus>'
        elif action == 'GetMediaInfo':
            values = '<CurrentURI/>'
        elif action == 'GetPositionInfo':
            values = '<RelTime>0:00:00</RelTime>'
        elif action == 'GetVolume':
            service = 'RenderingControl'
            values = '<CurrentVolume>20</CurrentVolume>'
        else:
            raise AssertionError('Unexpected device action: '+action)
        body = (f'<s:Envelope xmlns:s="{SOAP}"><s:Body><u:{action}Response '
                f'xmlns:u="urn:schemas-upnp-org:service:{service}:1">{values}'
                f'</u:{action}Response></s:Body></s:Envelope>').encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)


def receive(sock, size):
    data = b''
    while len(data) < size:
        chunk = sock.recv(size-len(data))
        assert chunk, 'short HELO'
        data += chunk
    return data


server = ThreadingHTTPServer((HOST, 1400), Speaker)
server.daemon_threads = True
worker = threading.Thread(target=server.serve_forever)
worker.start()
try:
    cases = [(None, 'solo', 'default-stream'), ('stream', 'solo', 'configured-stream'),
             ('auto', 'solo', 'auto-phase4'), ('invalid', 'solo', 'invalid-setting'),
             ('', 'solo', 'invalid-setting'), ('queue', 'coordinator', 'group-rate-undecided'),
             ('queue', 'member', 'group-rate-undecided'), ('queue', 'solo', 'queue-unavailable')]
    reference = None
    for mode, group, reason in cases:
        server.group, server.actions = group, []
        env = dict(os.environ, YENEY_TIMING_PROBE='0', YENEY_TIMING_PUBLISH='0')
        env.pop('YENEY_PLAYBACK_MODE', None)
        if mode is not None:
            env['YENEY_PLAYBACK_MODE'] = mode
        with socket.socket() as lms:
            lms.bind(('127.0.0.1', 0))
            lms.listen(1)
            lms.settimeout(6)
            process = subprocess.Popen([str(ROOT/'yeney'), '--room=Study', '--ip='+HOST,
                                        '--server=127.0.0.1:'+str(lms.getsockname()[1])],
                                       env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                connection, _ = lms.accept()
                with connection:
                    connection.settimeout(3)
                    header = receive(connection, 8)
                    assert header[:4] == b'HELO', header
                    hello = header+receive(connection, int.from_bytes(header[4:], 'big'))
                    if reference is None:
                        reference = hello
                    assert hello == reference, (mode, group, hello, reference)
                    process.send_signal(signal.SIGTERM)
                    try:
                        output = process.communicate(timeout=20)[0]
                    except subprocess.TimeoutExpired as error:
                        raise AssertionError((mode, group, error.output)) from error
                    assert process.returncode == 0, output
                selected = 'stream'
                assert f'selected={selected} reason={reason}' in output, output
                assert output.count('playback selection room=') == 1, output
                assert 'queue playback unavailable:' not in output, output
                assert ('invalid YENEY_PLAYBACK_MODE=' in output) == (reason == 'invalid-setting'), output
                assert set(server.actions) <= {'GetZoneGroupState', 'GetTransportInfo', 'GetVolume',
                                               'GetPositionInfo', 'GetMediaInfo'}, server.actions
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
    print('PASS playback mode startup: actual yeney, default/stream/auto/invalid/grouped/guarded queue byte-identical HELO, one startup log, no queue mutation')
finally:
    server.shutdown()
    server.server_close()
    worker.join()
