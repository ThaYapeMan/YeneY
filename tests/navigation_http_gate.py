# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Keep the second natural LMS audio response unfinished until selection."""
import http.client
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class NavigationHTTPGate:
    def __init__(self, hold_request=2):
        self.held = threading.Event()
        self.release = threading.Event()
        self.corrupt_next = threading.Event()
        self.corruptions = 0
        self.retained_verified = threading.Event()
        self.on_corruption = lambda: None
        self.lock = threading.Lock()
        self.requests = 0
        self.prefix_bytes = 0
        self.prefix_limit = 512 * 1024
        gate = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *_):
                pass

            def do_GET(self):
                with gate.lock:
                    gate.requests += 1
                    number = gate.requests
                if number > hold_request:
                    gate.release.set()
                upstream = http.client.HTTPConnection('127.0.0.1', 9000, timeout=10)
                try:
                    upstream.request('GET', self.path, headers=dict(self.headers))
                    response = upstream.getresponse()
                    self.send_response(response.status)
                    for name in ('Content-Type', 'Content-Length'):
                        if response.getheader(name):
                            self.send_header(name, response.getheader(name))
                    self.send_header('Connection', 'close')
                    self.end_headers()
                    first = response.read(gate.prefix_limit)
                    if gate.corrupt_next.is_set():
                        gate.corrupt_next.clear()
                        first = bytearray(first)
                        payload = first.index(b'mdat') + 4
                        # Generated escape ALAC: bit 55 is the first sample's
                        # sign bit, after the uncompressed CPE header.
                        first[payload + 6] ^= 1
                        gate.corruptions += 1
                        gate.on_corruption()
                    self.wfile.write(first)
                    self.wfile.flush()
                    if number == hold_request:
                        gate.prefix_bytes = len(first)
                        gate.held.set()
                        if not gate.release.wait(20):
                            raise AssertionError('unfinished decode-ahead was never selected')
                    while data := response.read(65536):
                        self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # LMS q closes the superseded natural producer.
                finally:
                    upstream.close()
                    self.close_connection = True

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.port = self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def close(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
