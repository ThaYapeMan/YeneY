"""The startup setting defaults, warns, and is read/logged only once."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='sonos-stream-content-') as temp:
    source, exe = Path(temp, 'setting.cpp'), Path(temp, 'setting')
    source.write_text('''#include "upnp/stream_content.h"
#include <cassert>
int main(int, char** argv) {
    const auto want = argv[1][0] == 's' ? upnp::StreamContentMode::Structured : argv[1][0] == 'p' ? upnp::StreamContentMode::Plain : upnp::StreamContentMode::Off;
    assert(upnp::streamContentMode() == want);
    setenv("SONOS_LMS_STREAM_CONTENT", "changed", 1);
    assert(upnp::streamContentMode() == want);
}''')
    subprocess.run(['g++', '-Wall', '-Wextra', '-I', str(root), str(source), '-o', str(exe)], check=True)
    for value in (None, 'structured', 'plain', 'off', '', 'invalid'):
        env = dict(os.environ)
        env.pop('SONOS_LMS_STREAM_CONTENT', None)
        if value is not None: env['SONOS_LMS_STREAM_CONTENT'] = value
        mode = value if value in ('plain', 'off') else 'structured'
        result = subprocess.run([str(exe), mode], env=env, check=True, capture_output=True, text=True)
        expected = f'SONOS_LMS_STREAM_CONTENT={mode}\n'
        if value in ('', 'invalid'): expected = f"Warning: invalid SONOS_LMS_STREAM_CONTENT='{value}'; using structured\n" + expected
        assert result.stdout == expected, result.stdout
        print(f'PASS: stream-content setting {value!r}: {mode}, startup log/read once')
