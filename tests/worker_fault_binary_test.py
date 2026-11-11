# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Inject a programming exception into a test-only copy of the real queue worker."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
checkout = Path(os.environ.get('YENEY_WORKER_BASELINE', ROOT))
with tempfile.TemporaryDirectory(prefix='yeney-worker-boundary-', dir=os.environ.get('TMPDIR') or '/var/tmp') as directory:
    work = Path(directory)
    source = (checkout/'core_queue_sink.cpp').read_text()
    assert source.count('            job.run();') == 1
    source = '#include <unistd.h>\n#include <cstdlib>\n' + source.replace('            job.run();', '''            if (const char *trigger = std::getenv("YENEY_TEST_WORKER_TRIGGER"); trigger && access(trigger, F_OK) == 0)
                throw std::logic_error("injected programming error");
            job.run();''')
    (work/'sink.cpp').write_text(source)
    subprocess.run(['g++', '-std=c++17', '-g', '-O2', '-Wall', '-Wextra', '-I'+str(checkout),
                    '-c', str(work/'sink.cpp'), '-o', str(work/'sink.o')], check=True)
    objects = subprocess.check_output(['make', '-s', '--eval', 'worker-test-objects: ; @echo $(OBJS)',
                                       'worker-test-objects'], cwd=checkout, text=True).split()
    objects = [str(work/'sink.o') if name == 'core_queue_sink.o' else str(checkout/name) for name in objects]
    subprocess.run(['g++', '-g', '-o', str(work/'yeney'), *objects,
                    str(checkout/'libqueuecontroller.a'), str(checkout/'libqueueitems.a'),
                    str(checkout/'third_party/yeney-core/libyeneycore.a'),
                    '-lFLAC++', '-lFLAC', '-lcrypto', '-lpthread'], check=True)
    env = dict(os.environ, YENEY_ACCEPTANCE_BINARY=str(work/'yeney'), YENEY_REAL_LMS_NAV_INVARIANTS='1',
               YENEY_NAV_CASE='D6', YENEY_NAV_MEDIA='', YENEY_TEST_WORKER_TRIGGER=str(work/'trigger'))
    raise SystemExit(subprocess.run([sys.executable, str(ROOT/'tests/real_lms_test.py')], env=env).returncode)
