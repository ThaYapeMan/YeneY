#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Measure one child tree's processor time without cross-group accounting."""
from pathlib import Path
import resource
import subprocess
import sys

child = subprocess.run(sys.argv[2:])
usage = resource.getrusage(resource.RUSAGE_CHILDREN)
Path(sys.argv[1]).write_text(f'{usage.ru_utime} {usage.ru_stime}\n')
raise SystemExit(child.returncode)
