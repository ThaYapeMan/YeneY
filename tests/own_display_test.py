"""Production own control with a SOAP boundary stub; no sockets or devices."""
from pathlib import Path
import subprocess
import os
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='sonos-own-display-') as directory:
    executable = Path(directory) / 'test'
    subprocess.run(['g++', '-O2', '-Wall', '-Wextra', '-I', str(ROOT),
                    str(ROOT / 'tests/own_display_fixture.cpp'),
                    *[str(ROOT / ('upnp/' + name + '.cpp')) for name in
                      ('http_server', 'gena', 'own_speaker_control', 'soap', 'xml', 'discovery')],
                    '-lpthread', '-o', str(executable)], check=True)
    for format in ('artist-title', 'title'):
        result = subprocess.run([str(executable)], capture_output=True, text=True, check=True,
                                env={**os.environ, 'YENEY_TITLE_FORMAT': format})
        print(result.stdout, end='')
        message = 'yeney: SOAP action=GetPositionInfo reason=no-reply-held-request state=paused'
        assert result.stdout.count(message) == 2, result.stdout  # two distinct pauses
        assert 'failed' not in result.stdout, result.stdout
