"""The startup setting defaults, warns, and is read/logged only once."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='sonos-title-format-') as temp:
    source, exe = Path(temp, 'setting.cpp'), Path(temp, 'setting')
    source.write_text('''#include "upnp/title_format.h"
#include <cassert>
int main(int, char** argv) {
    const auto want = argv[1][0] == 't' ? upnp::TitleFormat::Title : upnp::TitleFormat::ArtistTitle;
    assert(upnp::titleFormat() == want);
    setenv("YENEY_TITLE_FORMAT", "changed", 1);
    assert(upnp::titleFormat() == want);
}''')
    subprocess.run(['g++', '-Wall', '-Wextra', '-I', str(root), str(source), '-o', str(exe)], check=True)
    for value in (None, 'artist-title', 'title', '', 'invalid'):
        env = dict(os.environ)
        env.pop('YENEY_TITLE_FORMAT', None)
        if value is not None: env['YENEY_TITLE_FORMAT'] = value
        mode = 'title' if value == 'title' else 'artist-title'
        result = subprocess.run([str(exe), mode], env=env, check=True, capture_output=True, text=True)
        expected = f'YENEY_TITLE_FORMAT={mode}\n'
        if value in ('', 'invalid'): expected = f"Warning: invalid YENEY_TITLE_FORMAT='{value}'; using artist-title\n" + expected
        assert result.stdout == expected, result.stdout
        print(f'PASS: title-format setting {value!r}: {mode}, startup log/read once')
