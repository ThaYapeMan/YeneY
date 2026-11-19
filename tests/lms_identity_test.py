"""Check the first Slimproto HELO from both playback modes on loopback."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import os
import socket
import struct
import subprocess
import tempfile
import unittest
from xml.etree import ElementTree as ET
from xml.sax.saxutils import escape

from queue_binary_test import Coordinator, Device, ROOT


class IdentitySpeaker(Device):
    def do_GET(self):
        self.send(f'<root><device><displayName>{escape(self.server.product)}</displayName>'
                  '</device></root>')

    def action(self, service, action, args):
        values, fault = super().action(service, action, args)
        if action == 'GetZoneGroupState':
            topology = ET.fromstring(values['ZoneGroupState'])
            member = topology.find(".//ZoneGroupMember[@UUID='RINCON_TEST']")
            member.set('SoftwareVersion', self.server.firmware)
            values['ZoneGroupState'] = ET.tostring(topology, encoding='unicode')
        return values, fault


def captured_rooms():
    notification = ET.parse(ROOT / 'tests/fixtures/zone-group-notify.xml')
    topology = ET.fromstring(next(node.text for node in notification.iter()
                                  if node.tag.endswith('ZoneGroupState')))
    return {member.get('ZoneName'): member.get('SoftwareVersion')
            for member in topology.iter('ZoneGroupMember')}


MODELS = [
    ('Sonos Play:1', 'Sonos Play:1 (YeneY)'),
    ('Sonos Port', 'Sonos Port (YeneY)'),
    ('Play:1', 'Sonos Play:1 (YeneY)'),
    ('', 'Sonos (YeneY)'),
    ('  Sonos   Play:,1  ', 'Sonos Play:1 (YeneY)'),
    ('Sonos Play:\t1', 'Sonos Play:1 (YeneY)'),
    ('X' * 200, 'Sonos ' + 'X' * 50 + ' (YeneY)'),
]
VERSIONS = [
    ('86.10-80260', '86.10'),
    ('97.1-80312', '97.1'),
    ('86.10', '86.10'),
    ('', ''),
    ('86.,10', '86.10'),
]


class LmsIdentityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = os.environ.get('YENEY_ACCEPTANCE_BINARY', str(ROOT / 'yeney'))
        cls.version = subprocess.check_output([cls.binary, '--version'], text=True).strip()
        cls.speaker = Coordinator(('127.0.0.1', 1400), IdentitySpeaker)

    @classmethod
    def tearDownClass(cls):
        cls.speaker.close()

    def hello(self, mode, product, firmware):
        self.speaker.product, self.speaker.firmware = product, firmware
        with (socket.socket() as listener,
              tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as state):
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listener.settimeout(8)
            port = listener.getsockname()[1]
            env = dict(os.environ, YENEY_PLAYBACK_MODE=mode, YENEY_LMS_ICON='',
                       YENEY_LMS_VOLUME='off', STATE_DIRECTORY=state)
            with open(os.path.join(state, 'bridge.log'), 'w+') as log:
                proc = subprocess.Popen(
                    [self.binary, '--room=Study', '--ip=127.0.0.1', f'--server=127.0.0.1:{port}'],
                    env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    conn, _ = listener.accept()
                    with conn:
                        conn.settimeout(5)
                        with conn.makefile('rb') as wire:
                            opcode, length = struct.unpack('!4sI', wire.read(8))
                            self.assertEqual(opcode, b'HELO')
                            return wire.read(length)[36:].decode()
                except (OSError, struct.error) as error:
                    log.seek(0)
                    self.fail(f'{error}\n{log.read()}')
                finally:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()

    def test_lampastream_player_type_stays_yeney_for_every_model(self):
        for mode in ('queue', 'stream'):
            for product, _ in MODELS:
                with self.subTest(mode=mode, product=product):
                    entries = self.hello(mode, product, '').split(',')
                    self.assertEqual([x for x in entries if x.startswith('Model=')], ['Model=yeney'])

    def test_model_text_table(self):
        for product, expected in MODELS:
            with self.subTest(product=product):
                entries = self.hello('queue', product, '').split(',')
                self.assertIn('ModelName=' + expected, entries)

    def test_version_text_table(self):
        for firmware, expected in VERSIONS:
            with self.subTest(firmware=firmware):
                entries = self.hello('stream', 'Sonos Play:1', firmware).split(',')
                self.assertIn('Firmware=' + self.firmware_text(expected), entries)

    def firmware_text(self, short_version):
        return (self.version[:-1] + '; Sonos ' + short_version + ')'
                if short_version else self.version)

    def test_captured_rooms_and_complete_capabilities(self):
        versions = captured_rooms()
        rooms = [
            ('Study', 'Play:1', 'Sonos Play:1 (YeneY)', '86.10'),
            ('MBR', 'Sonos Play:1', 'Sonos Play:1 (YeneY)', '86.10'),
            ('Sonos Port', 'Sonos Port', 'Sonos Port (YeneY)', '97.1'),
        ]
        for mode, rate in (('queue', 192000), ('stream', 48000)):
            for room, product, model, short_version in rooms:
                with self.subTest(mode=mode, room=room):
                    caps = self.hello(mode, product, versions[room])
                    print(f'{mode} {room}: {caps}', flush=True)
                    self.assertEqual(caps.split(','), [
                        'Model=yeney', 'ModelName=' + model, 'AccuratePlayPoints=1',
                        f'MaxSampleRate={rate}', 'alc', 'flc', 'mp3', 'aif', 'pcm',
                        'Firmware=' + self.firmware_text(short_version),
                    ])


if __name__ == '__main__':
    unittest.main(verbosity=2)
