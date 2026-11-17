#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Exercise verdicts, completeness, namespaces, cancellation and serial order."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT/'scripts/test-runner.py'


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR') or '/var/tmp')
        self.base = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def invocation(self, commands, jobs=2, plan=None):
        steps = [dict(name=f'dummy-{n}', command=cmd, phase='parallel', environment={}, prerequisites=[])
                 for n, cmd in enumerate(commands)]
        names = [s['name'] for s in steps]
        manifest = self.base/'steps.json'
        manifest.write_text(json.dumps(dict(steps=steps, orders=dict(full=names, fast=names))))
        args = [sys.executable, str(RUNNER), '--no-build', '--manifest', str(manifest)]
        if plan is not None:
            path = self.base/'plan.json'
            path.write_text(json.dumps(plan))
            args += ['--plan', str(path)]
        env = dict(os.environ, YENEY_ACTIVE_TEST_RUN=str(self.base/'run'), YENEY_TEST_JOBS=str(jobs), YENEY_TEST_VERBOSE='1')
        env.pop('YENEY_TEST_FAIL_FAST', None)
        return args, env

    def run_steps(self, commands, jobs=2, plan=None):
        args, env = self.invocation(commands, jobs, plan)
        return subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)

    def test_01_failure_named(self):
        result = self.run_steps(['exit 7', 'echo other-group-finished'])
        self.assertNotEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('FAILED step=dummy-0 group=group-1 log=', result.stdout)
        self.assertIn('other-group-finished', result.stdout)
        print('P6.1 PASS failing dummy fails run and is named; other group finishes')

    def test_02_missing_plan_step(self):
        result = self.run_steps(['true', 'true'], plan=[['dummy-0'], []])
        self.assertNotEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn("missing=['dummy-1']", result.stdout)
        print('P6.2 PASS missing group-plan step fails completeness')

    def test_03_fixed_port(self):
        code = "import os,socket,time; assert os.geteuid()=="+str(os.geteuid())+"; s=socket.socket(); s.bind(('127.0.0.1',3483)); s.listen(); print('uid='+str(os.geteuid()),flush=True); time.sleep(1)"
        import shlex
        command = sys.executable+' -c '+shlex.quote(code)
        result = self.run_steps([command, command])
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(result.stdout.count('uid='+str(os.geteuid())), 2)
        print('P6.3 PASS same fixed loopback port in different groups; uid='+str(os.geteuid()))

    def test_04_shared_memory(self):
        code = "import os,time; p='/dev/shm/yeney-runner-collision'; f=os.open(p,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600); time.sleep(1); os.close(f); os.unlink(p)"
        import shlex
        command = sys.executable+' -c '+shlex.quote(code)
        result = self.run_steps([command, command])
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        print('P6.4 PASS same shared-memory name in different groups')

    def test_05_interrupt_descendants(self):
        marker = 'yeney-runner-child-'+str(time.time_ns())
        code = "import subprocess,time,pathlib,os; subprocess.Popen(['"+sys.executable+"','-c','import time; time.sleep(60)', '"+marker+"'],start_new_session=True); pathlib.Path(os.environ['TMPDIR'],'ready').touch(); time.sleep(60)"
        import shlex
        args, env = self.invocation([sys.executable+' -c '+shlex.quote(code)])
        proc = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            end = time.monotonic()+10
            while not list(self.base.rglob('ready')) and time.monotonic()<end:
                time.sleep(.02)
            self.assertTrue(list(self.base.rglob('ready')))
            proc.send_signal(signal.SIGINT)
            output = proc.communicate(timeout=15)[0]
            self.assertEqual(proc.returncode, 130, output)
            remaining = []
            for path in Path('/proc').glob('[0-9]*/cmdline'):
                try:
                    if marker.encode() in path.read_bytes():
                        remaining.append(path.parent.name)
                except OSError:
                    pass
            self.assertEqual(remaining, [])
        finally:
            if proc.poll() is None:
                proc.kill(); proc.wait()
        print('P6.5 PASS interrupt leaves no child, including a separate child session')

    def test_06_serial_order(self):
        result = self.run_steps(['echo first', 'echo second', 'echo third'], jobs=1)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        rows = json.loads((self.base/'run/summary.json').read_text())['results']
        self.assertEqual([r['name'] for r in rows], ['dummy-0', 'dummy-1', 'dummy-2'])
        starts = [line.split()[1] for line in result.stdout.splitlines() if line.startswith('START ')]
        self.assertEqual(starts, ['dummy-0', 'dummy-1', 'dummy-2'])
        print('P6.6 PASS YENEY_TEST_JOBS=1 executes serial order')

    def test_07_processor_time(self):
        import shlex
        command = sys.executable+' -c '+shlex.quote("import time; start=time.process_time(); x=1; exec('while time.process_time()-start<.15: x=(x*31+1)%999983')")
        result = self.run_steps([command, command])
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        rows = json.loads((self.base/'run/summary.json').read_text())['results']
        self.assertTrue(all(row['cpu']>=.15 for row in rows), rows)
        print('P6.7 PASS processor time includes the command inside each namespace')

    def test_08_duplicate_plan_step(self):
        result = self.run_steps(['true', 'true'], plan=[['dummy-0'], ['dummy-0']])
        self.assertNotEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn("duplicated=['dummy-0']", result.stdout)
        print('P6.8 PASS duplicated group-plan step fails completeness')

    def test_09_compact_console_order(self):
        args, env = self.invocation(['sleep .05; echo first-output', 'echo second-output'])
        env.pop('YENEY_TEST_VERBOSE')
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        names = [line.split()[1] for line in result.stdout.splitlines() if line.startswith('STEP ')]
        self.assertEqual(names, ['dummy-0', 'dummy-1'])
        combined = (self.base/'run/combined.log').read_text()
        self.assertLess(combined.index('first-output'), combined.index('second-output'))
        print('P6.9 PASS compact console and full combined transcript retain manifest order')

    def test_10_serial_fixture_phase(self):
        import shlex
        ready = self.base/'fixture-ready'
        args, env = self.invocation(['touch '+shlex.quote(str(ready)), 'test -f '+shlex.quote(str(ready))])
        manifest = self.base/'steps.json'
        data = json.loads(manifest.read_text())
        data['steps'][0]['phase'] = 'serial'
        manifest.write_text(json.dumps(data))
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        rows = json.loads((self.base/'run/summary.json').read_text())['results']
        self.assertEqual(rows[0]['group'], 'serial-fixtures')
        self.assertEqual(rows[1]['exit'], 0)
        print('P6.10 PASS private fixture preparation finishes before isolated groups start')

    def test_11_default_uses_available_capacity(self):
        import math
        args, env = self.invocation(['true', 'true'])
        env.pop('YENEY_TEST_JOBS')
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        summary = json.loads((self.base/'run/summary.json').read_text())
        groups = json.loads((self.base/'run/plan.json').read_text())
        self.assertEqual(len(groups), max(1, math.ceil(summary['capacity']['cores'])))
        print('P6.11 PASS default group count uses the capacity measured at this run start')


    def test_12_failure_replay_environment(self):
        import shlex
        args, env = self.invocation(['test "$YENEY_REPLAY_PROBE" = kept; echo "$YENEY_REPLAY_PROBE"; exit 7'])
        env.update(YENEY_REPLAY_PROBE='kept', YENEY_TEST_CACHE=str(self.base/'cache'))
        failed = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(failed.returncode, 0)
        line = next(line for line in failed.stdout.splitlines() if line.startswith('Run alone on this binary: '))
        replay = shlex.split(line.split(': ', 1)[1])
        env.update(YENEY_REPLAY_PROBE='wrong', YENEY_ACTIVE_TEST_RUN=str(self.base/'replay'))
        result = subprocess.run(replay, env=env, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        progress = next(line.split(': ', 1)[1] for line in result.stdout.splitlines() if line.startswith('Test progress: '))
        log = (Path(progress)/'serial/dummy-0.log').read_text()
        self.assertEqual(log.strip(), 'kept')
        self.assertIn('failed=[\'dummy-0\']', result.stdout)
        print('P6.12 PASS failure command restores manifest, environment, namespace and binary guard')

    def test_13_lane_verdicts_and_affinity(self):
        sys.path.insert(0, str(RUNNER.parent))
        self.addCleanup(sys.path.pop, 0)
        spec = importlib.util.spec_from_file_location('runner', RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        steps = [
            dict(name='sensitive', command='echo lane-position; exit 7'),
            dict(name='failure', command='echo genuine-failure; exit 8'),
        ]
        admit = []
        original = runner.run_group

        def run_group(group, directory, name, isolated, **kwargs):
            if isolated:
                assert not set(kwargs['cores']) & os.sched_getaffinity(0)
            elif group[0]['name'] == 'sensitive':
                group = [dict(group[0], command='echo alone-position')]
            return original(group, directory, name, isolated, **kwargs)

        run = self.base/'lane-run'
        run.mkdir()
        with patch.object(runner, 'run_group', run_group):
            results = runner.run_lanes(steps, run, 2, lambda: admit.append(True))
        layout = json.loads((run/'lanes.json').read_text())
        reserved = set(layout['runner_cores'])
        for lane in layout['lanes']:
            self.assertTrue(reserved.isdisjoint(lane['cores']))
            reserved.update(lane['cores'])
        sensitive, failure = (next(row for row in results if row['name'] == step['name']) for step in steps)
        self.assertEqual(admit, [True, True])
        self.assertTrue(runner.completeness(steps[:1], [sensitive]))
        self.assertFalse(runner.completeness(steps, results))
        self.assertIn('load-sensitive', runner.lane_verdict(sensitive))
        self.assertIn('lane-position', Path(sensitive['lane_attempt']['log']).read_text())
        self.assertIn('alone-position', Path(sensitive['log']).read_text())
        self.assertEqual(failure['exit'], 8)
        self.assertEqual(failure['lane_attempt']['exit'], 8)

if __name__ == '__main__':
    unittest.main()
