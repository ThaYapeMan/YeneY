"""Compare real stream output for unset, explicit stream and deferred auto modes."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
from pathlib import Path
import os
import re
import tempfile
import time
ROOT = Path(__file__).resolve().parents[1]
# Reuse the real-binary/fake-LMS/fake-speaker driver without its scenario runner.
namespace = {'__file__': str(ROOT/'tests/player_engine_test.py')}
exec((ROOT/'tests/player_engine_test.py').read_text().split('\nwith tempfile.TemporaryDirectory')[0], namespace)
Run, pcm = namespace['Run'], namespace['pcm']
class Standalone(namespace['Device']):
    def handle_post(self):
        action = self.headers['SOAPAction'].strip('"').split('#')[1]
        if action == 'GetZoneGroupState':
            self.rfile.read(int(self.headers['Content-Length']))
            topology = ('<ZoneGroupState><ZoneGroups><ZoneGroup Coordinator="RINCON_00112233445501400" ID="solo">'
                        '<ZoneGroupMember UUID="RINCON_00112233445501400" ZoneName="Study" '
                        'Location="http://127.0.0.1:1400/xml/device_description.xml"/>'
                        '</ZoneGroup></ZoneGroups></ZoneGroupState>')
            values = '<ZoneGroupState>'+namespace['escape'](topology)+'</ZoneGroupState>'
            soap = namespace['SOAP']
            self.send(f'<s:Envelope xmlns:s="{soap}"><s:Body><u:GetZoneGroupStateResponse '
                      f'xmlns:u="urn:schemas-upnp-org:service:ZoneGroupTopology:1">{values}'
                      '</u:GetZoneGroupStateResponse></s:Body></s:Envelope>')
            return
        super().handle_post()
namespace['Device'] = Standalone
previous = os.environ.get('YENEY_PLAYBACK_MODE')
try:
    captured = []
    with tempfile.TemporaryDirectory(prefix='yeney-queue-guard-') as folder:
        for mode in (None, 'stream', 'auto'):
            if mode is None:os.environ.pop('YENEY_PLAYBACK_MODE',None)
            else:os.environ['YENEY_PLAYBACK_MODE'] = mode
            name=mode or 'default'
            run = Run(name, Path(folder))
            try:
                body, _ = pcm(seconds=1)
                run.start(body, fmt='p')
                run.decoded_track()
                run.device.position = 2
                run.completed()
                run.until(lambda: run.captures and len(run.captures[0][1]) > 1000)
                time.sleep(.1)
                mutations = [(action, re.sub(rb'session=[A-Za-z0-9_-]+', b'session=SESSION', wire))
                             for action, wire in run.device.requests
                             if action in ('SetAVTransportURI', 'Play', 'Pause', 'Stop')]
                # HTTP body bytes and production SOAP bodies (apart from the
                # deliberately fresh process session token) must match exactly.
                captured.append((bytes(run.captures[0][1]), mutations, run.sources[0].request,
                                 next(p['body'] for p in run.lms.packets if p['op']=='HELO')))
            finally:
                run.close()
            log = (Path(folder)/name/'bridge.log').read_text()
            assert log.count('playback selection room=') == 1, log
            assert 'reason='+('default-stream' if mode is None else 'auto-phase4' if mode=='auto' else 'configured-stream') in log,log
        assert captured[0] == captured[1] == captured[2], [(len(c[0]), c[1], c[2]) for c in captured]
    print('PASS default/stream/auto stream equivalence: real yeney/fake LMS, identical HELO, encoded FLAC body, SOAP transport order/bodies and source HTTP request; only process session token normalized')
finally:
    if previous is None: os.environ.pop('YENEY_PLAYBACK_MODE', None)
    else: os.environ['YENEY_PLAYBACK_MODE'] = previous
