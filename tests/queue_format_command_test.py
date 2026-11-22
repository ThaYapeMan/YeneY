# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class FormatCommand(unittest.TestCase):
    def test_read_switch_and_unknown_room(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            root = Path(directory)
            config = root / "config"
            initial = "# Local settings\nroom.Study=yes\nroom.Sonos Port=yes\n"
            config.write_text(initial)
            calls = root / "calls"
            for name, code in {
                "systemctl": """
record.open("a").write(" ".join(sys.argv[1:]) + "\\n")
if sys.argv[1] == "show":
    print("test-invocation")
""",
                "journalctl": """
unit = next(arg for arg in sys.argv if arg.startswith("yeney@"))
room = "Sonos Port" if "Port" in unit else "Study"
settings = dict(line.split("=", 1) for line in config.read_text().splitlines() if "=" in line)
form = settings[f"room.{room}.YENEY_QUEUE_FORMAT"]
print("yeney: queue format=" + (form + " block=256" if form == "flac" else form))
""",
            }.items():
                tool = root / name
                tool.write_text("#!/usr/bin/env python3\nimport os, sys\nfrom pathlib import Path\n"
                                "config = Path(os.environ['YENEY_CONFIG'])\n"
                                "record = config.with_name('calls')\n" + code)
                tool.chmod(0o755)
            env = dict(os.environ, YENEY_CONFIG=str(config), PATH=f"{root}{os.pathsep}{os.environ['PATH']}")

            def command(*args):
                return subprocess.run(["python3", ROOT / "scripts/yeney-format", *args], env=env,
                                      text=True, capture_output=True)

            result = command()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Sonos Port", result.stdout)
            self.assertIn("Study", result.stdout)
            self.assertEqual(result.stdout.count("flac"), 2)
            self.assertEqual(config.read_text(), initial)
            self.assertFalse(calls.exists())

            result = command("wav", "Sonos Port")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("queue format=wav", result.stdout)
            self.assertIn("room.Sonos Port.YENEY_QUEUE_FORMAT=wav", config.read_text())
            self.assertNotIn("room.Study.YENEY_QUEUE_FORMAT", config.read_text())
            self.assertIn(r"restart yeney@Sonos\x20Port.service", calls.read_text())

            result = command("flac", "all")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.count("queue format=flac block=256"), 2)
            self.assertIn("room.Study.YENEY_QUEUE_FORMAT=flac", config.read_text())
            self.assertIn("room.Sonos Port.YENEY_QUEUE_FORMAT=flac", config.read_text())
            self.assertIn("restart yeney@Study.service", calls.read_text())
            before = config.read_text(), calls.read_text()
            result = command("wav", "Nobody")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Sonos Port", result.stderr)
            self.assertIn("Study", result.stderr)
            self.assertEqual((config.read_text(), calls.read_text()), before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
