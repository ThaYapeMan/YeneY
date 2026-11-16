# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Capture survives silence, incomplete TLS application frames, drops and SIGINT."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import tarfile
import tempfile
import threading
import time
from queue_sonos_fake import Coordinator
from sonos_websocket_fake import LocalApi

spec = importlib.util.spec_from_file_location('capture', Path(os.environ.get('YENEY_CAPTURE_SCRIPT', 'scripts/sonos-local-api-capture.py')))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)
owner = Coordinator()
api = LocalApi(owner, port=0)
api.websocket_url = f'wss://127.0.0.1:{api.server.server_port}/websocket/api'
api.info_extra = dict(deviceId='DEVICE-PRIVATE', locationId='LOCATION-PRIVATE', queueId='QUEUE-PRIVATE',
                      objectId='yeney:private-session-1', url='http://192.0.2.5:1234/audio', encodedUrl='http://192%2E0%2E2%2E5/media/yeney%3Aprivate-session-1', ipv6='2001:db8::1')
try:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        for mode in ('silent', 'partial', 'drop', 'interrupt'):
            api.silent = mode == 'silent'
            api.partial_frame = mode == 'partial'
            destination = root/(mode+'.tar.gz')
            start = time.monotonic()
            if mode == 'drop':
                worker = threading.Timer(.5, api.disconnect); worker.start()
            if mode == 'interrupt':
                code = "import runpy; c=runpy.run_path('scripts/sonos-local-api-capture.py'); print(c['capture']('127.0.0.1',%d,20,%r))" % (api.server.server_port, str(destination))
                process = subprocess.Popen(['python3', '-c', code], stdout=subprocess.PIPE, text=True)
                time.sleep(.6); process.send_signal(signal.SIGINT)
                process.communicate(timeout=5)
                assert process.returncode == 0
            else:
                capture.capture('127.0.0.1', api.server.server_port, 3.5, destination)
            assert time.monotonic()-start < 5
            with tarfile.open(destination) as archive:
                contents = {name: archive.extractfile(name).read() for name in archive.getnames()}
            assert contents['ending.txt'].strip()
            combined = b''.join(contents.values())
            for private in ('DEVICE-PRIVATE','LOCATION-PRIVATE','QUEUE-PRIVATE','yeney:private-session-1','192.0.2.5','127.0.0.1','2001:db8::1','private-session','192%2E0%2E2%2E5'):
                assert private.encode() not in combined, (mode, private)
            if mode == 'partial': assert contents['partial-websocket.bin']
            if mode == 'drop': assert b'EOFError' in contents['ending.txt']
            if mode == 'interrupt': assert b'KeyboardInterrupt' in contents['ending.txt']
            print('PASS G6 '+mode+': bounded capture, tarball, ending and stable placeholders', flush=True)
finally:
    api.close(); owner.close()
