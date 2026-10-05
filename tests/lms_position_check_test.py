# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Deterministic CLI sampling without a live LMS or wall-clock sleeps."""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("position_check", str(root / "scripts/yeney-lms-position-check"))
spec = importlib.util.spec_from_loader(loader.name, loader)
tool = importlib.util.module_from_spec(spec)
loader.exec_module(tool)
mac = "94:9f:3e:fa:ba:66"


class Clock:
    now = 100.0

    def read(self):
        return self.now

    def sleep(self, amount):
        self.now += amount


class Connection:
    def __init__(self, clock, excursions=False):
        self.clock, self.excursions = clock, excursions
        self.commands = []

    def makefile(self, mode):
        assert mode == "rb"
        return self

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def sendall(self, command):
        assert command == f"{mac} time ?\n".encode()
        self.commands.append(command)

    def readline(self, limit):
        assert limit == 4097
        mono = self.clock.now + .005
        position = (mono - 100) * 1.000012 + 10
        if self.excursions and len(self.commands) % 16 in (0, 1):
            position += .5
        self.clock.now += .010
        return f"{mac.replace(':', '%3A')} time {position:.9f}\n".encode()


for excursions in (False, True):
    clock = Clock()
    connection = Connection(clock, excursions)
    samples = tool.collect(connection, mac, 60, clock.read, clock.sleep)
    slope, residuals = tool.fit(samples)
    assert len(samples) == 240
    assert all(abs(samples[i + 1][0] - samples[i][0] - .25) < 1e-8 for i in range(239))
    if excursions:
        assert max(abs(r) for r in residuals) * 1000 > 430
    else:
        assert abs((slope - 1) * 1e6 - 12) < .01
        assert max(abs(r) for r in residuals) * 1000 < .001
print("PASS: owner CLI tool queries only time ?, timestamps RTT midpoint, samples every 0.25 s, recovers 12 ppm and detects 0.5 s excursions")

for value, expected, warning in ((None, 1, False), ("1", 1, False), ("0", 0, False), ("invalid", 1, True), ("", 1, True)):
    env = dict(os.environ)
    env.pop("YENEY_LMS_POSITION_FROM_MODEL", None)
    if value is not None:
        env["YENEY_LMS_POSITION_FROM_MODEL"] = value
    result = subprocess.run([str(root / "lms-position-test"), "setting"], env=env, capture_output=True, text=True, check=True)
    assert f"model={expected}" in result.stdout
    assert result.stdout.count("invalid=") == int(warning)
print("PASS: model-position default/0/1 and invalid fallback parsing")

samples = [(100+i*.25, 10+i*.25 + (-.5 if i < 80 else 0)) for i in range(240)]
assert tool.lock_time(samples, 'yeney: timing-lock state=locked mono=120.000 udn=RINCON') == 20
assert tool.lock_time(samples, '') is None
assert tool.lock_time(samples, 'yeney: timing mono=99 state=locked') == 0
assert tool.lock_time(samples, 'yeney: timing mono=99 state=locked\nyeney: timing-lock state=acquiring mono=100\nyeney: timing-lock state=locked mono=120') == 20
import contextlib
import io
output = io.StringIO()
with contextlib.redirect_stdout(output):
    tool.summary(samples,20)
assert 'largest_step_after_10s_ms=750.000' in output.getvalue()
assert 'backwards_count=0' in output.getvalue()
assert 'before_lock_error samples=80 max_ms=500.000' in output.getvalue()
assert 'after_lock_error samples=160 max_ms=0.000' in output.getvalue()
samples[100] = (samples[100][0],samples[99][1]-.1)
with contextlib.redirect_stdout(output):
    tool.summary(samples,None)
assert 'backwards_count=1' in output.getvalue()
assert 'before_lock_error=unknown' in output.getvalue()
print('PASS: producer lock timestamp, largest step, backwards and separate startup/stable errors')
