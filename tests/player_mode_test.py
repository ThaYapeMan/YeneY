"""Player switch defaults, warnings, and read-once semantics."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sonos-audio-mode-") as tmp:
    source = Path(tmp) / "settings.cpp"
    exe = Path(tmp) / "settings"
    source.write_text('''
#include "player_mode.h"
#include <cassert>
int main(int argc, char** argv) {
    const bool stop = argv[1][0] == '1';
    assert((playerMode() == PlayerMode::Core) == stop);
    setenv("YENEY_PLAYER", stop ? "squeezelite" : "core", 1);
    assert((playerMode() == PlayerMode::Core) == stop);
}
''')
    subprocess.run(["g++", "-Wall", "-I", str(root), str(source), "-o", str(exe)], check=True)
    for value in (None, "squeezelite", "core", "invalid", ""):
        env = dict(os.environ)
        env.pop("YENEY_PLAYER", None)
        if value is not None:
            env["YENEY_PLAYER"] = value
        result = subprocess.run([str(exe), str(int(value == "core"))], env=env,
                                check=True, capture_output=True, text=True)
        assert result.stdout.count("Warning:") == (1 if value in ("invalid", "") else 0)
        assert f"YENEY_PLAYER={'core' if value == 'core' else 'squeezelite'}" in result.stdout
        print(f"PASS: player mode {value!r}: parsing, startup log, read once")
