#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Execute the shared test manifest with isolated groups and a quiet phase."""
import argparse
import concurrent.futures
import json
import math
import os
from pathlib import Path
import hashlib
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
from test_capacity import capacity

ROOT = Path(__file__).resolve().parents[1]
ACTIVE = set()
LOCK = threading.RLock()
CANCEL = threading.Event()


def command(step):
    return step['command'].replace('$(MAKE)', 'make').replace('$(CURDIR)', str(ROOT)).replace('$$', '$')


def stop_children():
    CANCEL.set()
    with LOCK:
        children = list(ACTIVE)
    # A test may create another session. Capture its descendants as well as
    # process groups before terminating anything that could orphan them.
    descendants = {os.getpid(), *(p.pid for p in children)}
    processes = {}
    for entry in Path('/proc').iterdir():
        if entry.name.isdigit():
            try:
                fields = (entry/'stat').read_text().rsplit(')', 1)[1].split()
                processes[int(entry.name)] = (int(fields[1]), fields[19])
            except (OSError, ValueError, IndexError):
                pass
    previous = -1
    while previous != len(descendants):
        previous = len(descendants)
        descendants.update(pid for pid, (parent, stamp) in processes.items() if parent in descendants)
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for pid in reversed(sorted(descendants)):
            if pid == os.getpid():
                continue
            try:
                fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
                if pid in processes and fields[19] == processes[pid][1]:
                    os.kill(pid, sig)
            except (OSError, ValueError, IndexError):
                pass
        if sig == signal.SIGTERM:
            time.sleep(1)


def interrupted(number, frame):
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, signal.SIG_IGN)
    stop_children()
    raise KeyboardInterrupt


def cpu_usage():
    return int(dict(line.split() for line in Path('/sys/fs/cgroup/cpu.stat').read_text().splitlines())['usage_usec'])


def competing_tests():
    ancestors = {os.getpid()}
    current = os.getpid()
    while current:
        try:
            current = int(Path(f'/proc/{current}/stat').read_text().rsplit(')', 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            break
        ancestors.add(current)
    busy = []
    for entry in Path('/proc').glob('[0-9]*/cmdline'):
        if int(entry.parent.name) in ancestors:
            continue
        try:
            args = [part.decode(errors='replace') for part in entry.read_bytes().split(b'\0') if part]
        except OSError:
            continue
        if not args:
            continue
        executable = Path(args[0]).name
        if (executable in ('make', 'gmake') and any(arg in ('test', 'test-run', 'test-serial', 'test-fast', 'test-fast-run') for arg in args[1:])) or executable in ('pytest', 'pytest-3', 'py.test') or (executable.startswith('python') and args[1:3] == ['-m', 'pytest']):
            busy.append(str(entry.parent.name)+' '+shlex.join(args))
    return busy


def quiet(run, fitness=False):
    while not CANCEL.is_set():
        busy = competing_tests()
        if busy:
            print('ADMISSION waiting for competing tests: '+repr(busy), flush=True)
            CANCEL.wait(5)
            continue
        measured_capacity = capacity()
        cores = measured_capacity['cores']
        start = time.monotonic()
        before = cpu_usage()
        if CANCEL.wait(30):
            break
        percent = (cpu_usage()-before)/1e6/(time.monotonic()-start)/cores*100
        entry = dict(capacity=measured_capacity, cgroup_percent=percent, interval_s=time.monotonic()-start)
        with (run/'quiet-admission.jsonl').open('a') as log:
            log.write(json.dumps(entry)+'\n')
        print(f'QUIET cores={cores} cgroup_percent={percent:.6f} required_below=15', flush=True)
        if percent >= 15 or competing_tests():
            continue
        if fitness:
            log_path = run/('fitness-'+str(time.time_ns())+'.log')
            with log_path.open('w') as log:
                result = subprocess.run([sys.executable, 'third_party/yeney-core/tests/decimator_quality.py', '--speed-only'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            print('FITNESS '+str(log_path)+' exit='+str(result.returncode), flush=True)
            if result.returncode:
                raise RuntimeError('conversion fitness failed; see '+str(log_path))
        return
    raise KeyboardInterrupt


def namespace(group, env):
    # Bubblewrap configures loopback before dropping namespace capabilities.
    # Keeping the numeric uid matters to the real server's permission checks.
    args = ['bwrap', '--unshare-user', '--uid', str(os.geteuid()), '--gid', str(os.getegid()),
            '--unshare-net', '--unshare-ipc', '--unshare-pid', '--die-with-parent',
            '--ro-bind', '/', '/', '--dev', '/dev', '--proc', '/proc', '--tmpfs', '/dev/shm',
            '--bind', str(group/'tmp'), '/tmp',
            '--bind', str(group/'var-tmp'), '/var/tmp', '--bind', str(group), str(group), '--chdir', str(ROOT)]
    return args


def execute(step, group, isolated, cores=None, retry=False):
    name = step['name']
    log = group/(name+'.log')
    env = dict(os.environ, **step.get('environment', {}))
    env.update(YENEY_TEST_CACHE=str(group/'cache'), YENEY_ACTIVE_TEST_RUN=str(group), PYTHONDONTWRITEBYTECODE='1')
    if isolated:
        env.update(HOME=str(group/'home'), TMPDIR=str(group/'tmp'), YENEY_TEST_CACHE=str(group/'cache'),
                   YENEY_ACTIVE_TEST_RUN=str(group), PYTHONDONTWRITEBYTECODE='1',
               XDG_CACHE_HOME=str(group/'cache/xdg'), XDG_CONFIG_HOME=str(group/'home/.config'),
               XDG_DATA_HOME=str(group/'home/.local/share'), XDG_STATE_HOME=str(group/'home/.local/state'),
               XDG_RUNTIME_DIR=str(group/'runtime'))
    shared = Path(os.environ.get('YENEY_TEST_CACHE', Path.home()/'.cache/yeney-test'))
    env.setdefault('YENEY_REAL_LMS_HOME', str(shared/'real-lms-9.1.1'))
    env.setdefault('YENEY_REAL_LMS_PERL5', str(shared/'real-lms-perl5'))
    metrics = group/(name+'.usage')
    inner = [sys.executable, str(ROOT/'scripts/test-step-time.py'), str(metrics), '/bin/sh', '-c', command(step)]
    args = (namespace(group, env) if isolated else []) + inner
    if cores:
        args = ['taskset', '--cpu-list', ','.join(map(str, cores)), *args]
    verbose = os.environ.get('YENEY_TEST_VERBOSE') == '1'
    if verbose:
        print(f'START {name} group={group.name} log={log}', flush=True)
    started = time.monotonic()
    with log.open('wb') as output:
        child = subprocess.Popen(args, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        with LOCK:
            ACTIVE.add(child)
        if CANCEL.is_set():
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            child.wait()
        finally:
            with LOCK:
                ACTIVE.discard(child)
    try:
        user, system = map(float, metrics.read_text().splitlines()[-1].split())
        processor = user + system
    except (OSError, ValueError, IndexError):
        processor = 0.0
    result = dict(name=name, group=group.name, command=command(step), exit=child.returncode,
                  wall=time.monotonic()-started, cpu=processor, log=str(log))
    (group/(name+'.json')).write_text(json.dumps(result, indent=2)+'\n')
    if verbose:
        print(f'END {name} group={group.name} exit={child.returncode} wall={result["wall"]:.3f} cpu={result["cpu"]:.3f}', flush=True)
    if child.returncode and not retry and os.environ.get('YENEY_TEST_FAIL_FAST') == '1':
        stop_children()
    return result


def group_directory(run, name):
    group = run/name
    group.mkdir(parents=True, exist_ok=True)
    for folder in ('home', 'tmp', 'var-tmp', 'cache', 'runtime'):
        (group/folder).mkdir(mode=0o700, exist_ok=True)
    return group


def run_group(steps, run, name, isolated, stop_on_failure=False, cores=None, retry=False):
    started = time.monotonic()
    group = group_directory(run, name)
    results = []
    for step in steps:
        if CANCEL.is_set():
            break
        results.append(execute(step, group, isolated, cores, retry))
        if stop_on_failure and results[-1]['exit']:
            break
    if isolated:
        for folder in ('home', 'tmp', 'var-tmp', 'cache', 'runtime'):
            source = group/folder
            for path in source.rglob('*'):
                if path.is_file() and not path.is_symlink() and path.suffix in ('.log', '.txt', '.json', '.jsonl', '.md', '.tsv', '.csv', '.xml'):
                    destination = group/'evidence'/folder/path.relative_to(source)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, destination)
            shutil.rmtree(source)
    (group/'group-runtime.json').write_text(json.dumps(dict(wall=time.monotonic()-started, cpu=sum(r['cpu'] for r in results)), indent=2))
    return results


def plan(steps, jobs, balanced):
    groups = [[] for _ in range(jobs)]
    totals = [0.0]*jobs
    if not balanced:
        for n, step in enumerate(steps):
            groups[n % jobs].append(step)
    else:
        for step in sorted(steps, key=lambda s: s.get('seconds', 1), reverse=True):
            index = min(range(jobs), key=lambda i: totals[i])
            groups[index].append(step)
            totals[index] += step.get('seconds', 1)
    return groups


def run_lanes(steps, run, count, admit):
    affinity = os.sched_getaffinity(0)
    physical = {}
    for cpu in sorted(affinity):
        siblings = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list').read_text().strip()
        physical.setdefault(siblings, []).append(cpu)
    lanes = [step for step in steps if not step.get('run_alone')]
    alone = [step for step in steps if step.get('run_alone')]
    count = min(count, len(physical) - 1, len(lanes))
    if count < 2:
        return run_group(steps, run, 'quiet', False)
    available = sorted(physical.values(), key=len, reverse=True)
    runner_cores = available.pop()
    lane_cores = [[] for _ in range(count)]
    for siblings in available:
        min(lane_cores, key=len).extend(siblings)
    assignments = list(zip(plan(lanes, count, True), lane_cores))
    (run/'lanes.json').write_text(json.dumps(dict(runner_cores=runner_cores, lanes=[
        dict(cores=cores, steps=[step['name'] for step in group]) for group, cores in assignments
    ]), indent=2)+'\n')
    os.sched_setaffinity(0, runner_cores)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=count) as pool:
            futures = [pool.submit(run_group, group, run, f'lane-{i}', True, cores=cores, retry=True)
                       for i, (group, cores) in enumerate(assignments, 1)]
            first_results = [row for future in futures for row in future.result()]
    finally:
        os.sched_setaffinity(0, affinity)

    results = {row['name']: row for row in first_results}
    for step in steps:
        first = results.get(step['name'])
        if first is not None and not first['exit']:
            continue
        if CANCEL.is_set():
            raise KeyboardInterrupt
        admit()
        group = 'second-opinion' if first is not None else 'run-alone'
        result = run_group([step], run, group, False)[0]
        if first is not None:
            result['lane_attempt'] = first
            if not result['exit']:
                result['load_sensitive'] = True
        results[step['name']] = result
    return list(results.values())


def lane_verdict(result):
    if 'lane_attempt' not in result:
        return ''
    label = 'load-sensitive' if result.get('load_sensitive') else 'second opinion failed'
    return f"{label}: {result['name']} lane_log={result['lane_attempt']['log']} alone_log={result['log']}"



def completeness(steps, results):
    expected = [s['name'] for s in steps]
    actual = [s['name'] for s in results]
    missing = sorted(set(expected)-set(actual))
    extra = sorted(set(actual)-set(expected))
    duplicates = sorted(n for n in set(actual) if actual.count(n) != 1)
    failed = [r['name'] for r in results if r['exit'] != 0]
    passed = not (missing or extra or duplicates or failed) and len(expected) == len(actual)
    print(f'COMPLETENESS {"PASS" if passed else "FAIL"} expected={len(expected)} executed={len(actual)} missing={missing} extra={extra} duplicated={duplicates} failed={failed}', flush=True)
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('full', 'fast'), default='full')
    parser.add_argument('--serial', action='store_true')
    parser.add_argument('--step')
    parser.add_argument('--binary-sha256', help='Refuse a one-step rerun if yeney changed')
    parser.add_argument('--environment-file', type=Path)
    parser.add_argument('--isolate-step', action='store_true')
    parser.add_argument('--selection')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--manifest', type=Path, default=ROOT/'scripts/test-steps.json')
    parser.add_argument('--plain-split', action='store_true', default=os.environ.get('YENEY_TEST_SCHEDULE') == 'plain')
    parser.add_argument('--lanes', type=int, default=os.environ.get('YENEY_TEST_LANES', '5'),
                        help='Number of isolated timing lanes (1 runs the quiet phase serially)')
    parser.add_argument('--stop-file', type=Path, default=os.environ.get('YENEY_TEST_STOP_FILE'),
                        help='Interrupt the run when this file appears')
    parser.add_argument('--plan', type=Path, help='Explicit parallel plan; completeness still uses the full manifest')
    args = parser.parse_args()
    if args.lanes < 1:
        parser.error('lane count must be positive')
    if args.environment_file:
        os.environ.update(json.loads(args.environment_file.read_text()))
    full_gate = args.mode == 'full' and not args.step and not args.selection and args.manifest.resolve() == (ROOT/'scripts/test-steps.json').resolve()
    if full_gate and os.environ.get('LAMPASTREAM_CONSUMER') == 'off':
        parser.error('the full gate requires the production require_v1 consumer; a skip is a failure')
    manifest = json.loads(args.manifest.read_text())
    for step in manifest['steps']:
        if step.get('time_judging') and step['phase'] != 'quiet':
            parser.error('time-judging step must use quiet phase: '+step['name'])
        if step['phase'] not in ('parallel', 'serial', 'prepare', 'quiet'):
            parser.error('unknown phase for '+step['name'])
    by_name = {s['name']: s for s in manifest['steps']}
    if len(by_name) != len(manifest['steps']):
        parser.error('duplicate manifest step name')
    steps = [by_name[n] for n in manifest['orders'][args.mode]]
    declared = {s['name'] for s in manifest['steps'] if args.mode in s.get('modes', [])}
    if declared and declared != {s['name'] for s in steps}:
        parser.error('mode order does not equal the declared step membership')
    if args.binary_sha256 and hashlib.sha256((ROOT/'yeney').read_bytes()).hexdigest() != args.binary_sha256:
        parser.error('yeney binary differs from the failing run')
    if len({s['name'] for s in steps}) != len(steps):
        parser.error('duplicate step in serial order')
    if args.step:
        by_name.update((part['name'], dict(step, **part))
                       for step in manifest['steps'] for part in step.get('lane_parts', []))
        if args.step not in by_name:
            parser.error('unknown step '+args.step)
        steps = [by_name[args.step]]
        args.serial = True
    if args.selection:
        steps = [s for s in steps if s.get('target') == args.selection]
        args.serial = True
    if not steps:
        parser.error('empty step selection')
    if args.list:
        for s in steps:
            print(s['name']+'\t'+command(s))
        return 0
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupted)
    if args.stop_file:
        stop_file = args.stop_file.resolve()
        if stop_file.exists():
            parser.error('stop file already exists: ' + str(stop_file))

        def check_stop_file(number, frame):
            if stop_file.exists():
                signal.setitimer(signal.ITIMER_REAL, 0)
                interrupted(number, frame)

        signal.signal(signal.SIGALRM, check_stop_file)
        signal.setitimer(signal.ITIMER_REAL, .2, .2)
    CANCEL.clear()
    run = Path(os.environ.get('YENEY_ACTIVE_TEST_RUN', Path(os.environ.get('YENEY_TEST_CACHE', Path.home()/'.cache/yeney-test'))/'runner'/str(time.time_ns()))).resolve()
    run.mkdir(parents=True, exist_ok=True)
    print('Test progress: '+str(run), flush=True)
    context = {key: value for key, value in os.environ.items() if (key.startswith('YENEY_') or key.startswith('LAMPASTREAM_') or key in ('HOME', 'LANG', 'LC_ALL', 'TZ')) and key not in ('YENEY_ACTIVE_TEST_RUN', 'YENEY_TEST_JOBS', 'YENEY_TEST_LANES', 'YENEY_TEST_STOP_FILE', 'YENEY_TEST_FAIL_FAST', 'YENEY_TEST_VERBOSE', 'YENEY_TEST_SCHEDULE')}
    context_path = run/'run-environment.json'
    context_path.write_text(json.dumps(context, indent=2)+'\n')
    context_path.chmod(0o600)
    started = time.monotonic()
    measured_capacity = capacity()
    (run/'runner-capacity.json').write_text(json.dumps(measured_capacity, indent=2)+'\n')
    print('CAPACITY '+json.dumps(measured_capacity, sort_keys=True), flush=True)
    jobs = int(os.environ.get('YENEY_TEST_JOBS', str(max(1, math.ceil(measured_capacity['cores'])))))
    if jobs < 1:
        parser.error('YENEY_TEST_JOBS must be positive')
    build_jobs = max(1, math.ceil(measured_capacity['cores']))
    build_result = None
    if not (args.no_build or args.step):
        targets = []
        for step in steps:
            reference = step.get('prerequisites', {}).get(args.mode)
            for target in manifest.get('build_sets', {}).get(reference, []):
                if target not in targets:
                    targets.append(target)
        if targets:
            rule = '.PHONY: runner-build-prerequisites\nrunner-build-prerequisites: '+ ' '.join(targets)
            arguments = ['make', '-j', str(build_jobs), '--eval', rule, 'runner-build-prerequisites']
            commands = [shlex.join(arguments)]
            external = set(key for step in steps for key in step.get('additional_prerequisites', []))
            for key in sorted(external):
                declaration = manifest['external_build_sets'][key]
                commands.append(shlex.join(['make', '-j', str(build_jobs), '-C', declaration['directory'], *declaration['targets']]))
            build_step = dict(name='build', command=' && '.join(commands), environment={})
            build_result = execute(build_step, group_directory(run,'build'), False)
            if build_result['exit']:
                print(Path(build_result['log']).read_text(errors='replace'), end='')
                print('FAILED step=build group=build log='+build_result['log']+'\nRun build alone: '+shlex.join(arguments))
                completeness(steps, [])
                return build_result['exit']
    if args.build_only:
        if build_result:
            print(Path(build_result['log']).read_text(errors='replace'), end='')
        return 0
    binary_hash = hashlib.sha256((ROOT/'yeney').read_bytes()).hexdigest() if (ROOT/'yeney').is_file() else None
    (run/'identity.json').write_text(json.dumps(dict(binary_sha256=binary_hash, uid=os.geteuid()), indent=2))
    results = []
    try:
        if full_gate:
            quiet(run, fitness=True)
        if args.serial or jobs == 1:
            if args.step and steps[0].get('time_judging') and args.manifest.resolve() == (ROOT/'scripts/test-steps.json').resolve():
                quiet(run)
            results = run_group(steps, run, 'serial', args.isolate_step, stop_on_failure=True)
        else:
            results += run_group([s for s in steps if s['phase'] == 'prepare'], run, 'prepare', False)
            results += run_group([s for s in steps if s['phase'] == 'serial'], run, 'serial-fixtures', False)
            groups = plan([s for s in steps if s['phase'] == 'parallel'], jobs, not args.plain_split)
            if args.plan:
                groups = [[by_name[name] for name in group] for group in json.loads(args.plan.read_text())]
                if any(step['phase'] != 'parallel' for group in groups for step in group):
                    parser.error('a parallel plan cannot contain quiet or preparation steps')
            (run/'plan.json').write_text(json.dumps([[s['name'] for s in g] for g in groups], indent=2))
            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
                futures = [pool.submit(run_group, group, run, f'group-{n+1}', True) for n, group in enumerate(groups)]
                for future in futures:
                    results += future.result()
            quiet_steps = [s for s in steps if s['phase'] == 'quiet']
            if quiet_steps:
                quiet(run, fitness=full_gate)
            lane_steps = [dict(step, **part) for step in quiet_steps
                          for part in step.get('lane_parts', [{}])] if args.lanes > 1 else quiet_steps
            results += run_lanes(lane_steps, run, args.lanes, lambda: quiet(run, fitness=full_gate))
            if args.lanes > 1:
                steps = [dict(step, **part) for step in steps for part in step.get('lane_parts', [{}])]
    except KeyboardInterrupt:
        stop_children()
        print('INTERRUPTED: all groups terminated', flush=True)
        return 130
    ordered = {s['name']: i for i, s in enumerate(steps)}
    results.sort(key=lambda r: ordered[r['name']])
    with (run/'combined.log').open('w') as combined:
        if build_result:
            combined.write('=== build ===\n'+Path(build_result['log']).read_text(errors='replace'))
        for r in results:
            if verdict := lane_verdict(r):
                combined.write('\n'+verdict+'\n')
                combined.write(Path(r['lane_attempt']['log']).read_text(errors='replace'))
            combined.write(f'\n=== {r["name"]} ({r["group"]}) ===\nCOMMAND {r["command"]}\n')
            combined.write(Path(r['log']).read_text(errors='replace'))
    # Console replay is deterministic even when completion order is different.
    if os.environ.get('YENEY_TEST_VERBOSE') == '1':
        print((run/'combined.log').read_text(errors='replace'), end='', flush=True)
    else:
        for result in results:
            print(f'STEP {result["name"]} group={result["group"]} exit={result["exit"]} wall={result["wall"]:.3f} cpu={result["cpu"]:.3f}', flush=True)
            if verdict := lane_verdict(result):
                print(verdict, flush=True)
            if result['exit']:
                print(Path(result['log']).read_text(errors='replace'), end='')
        print('Combined console: '+str(run/'combined.log'), flush=True)
    consumer_line = None
    if full_gate:
        for result in results:
            if result['command'] == 'make -C third_party/yeney-core test':
                consumer_line = next((line for line in Path(result['log']).read_text(errors='replace').splitlines() if line.startswith('PASS cross-repo LampaStream SHM consumer (require_v1=True)')), None)
                if not consumer_line:
                    result.update(command_exit=result['exit'], exit=1, validation_error='required consumer proof missing or skipped')
                    (run/result['group']/(result['name']+'.json')).write_text(json.dumps(result,indent=2)+'\n')
        print('CONSUMER '+(consumer_line or 'FAIL required require_v1 proof missing'), flush=True)
    complete = completeness(steps, results) and (not full_gate or consumer_line is not None)
    for r in results:
        if r['exit']:
            rerun = shlex.join([sys.executable, str(ROOT/'scripts/run-tests.py'), sys.executable, str(ROOT/'scripts/test-runner.py'), '--mode', args.mode, '--manifest', str(args.manifest.resolve()), '--environment-file', str(context_path), '--step', r['name'], '--no-build'])
            if r['group'].startswith(('group-', 'lane-')):
                rerun += ' --isolate-step'
            if binary_hash:
                rerun += ' --binary-sha256 '+binary_hash
            print(f'FAILED step={r["name"]} group={r["group"]} log={r["log"]}\nRun alone on this binary: {rerun}', flush=True)
    for group in sorted(path.name for path in run.iterdir() if path.is_dir() and (path/'group-runtime.json').is_file()):
        rows = [r for r in results if r['group'] == group]
        group_metrics = json.loads((run/group/'group-runtime.json').read_text())
        print(f'GROUP {group} wall={group_metrics["wall"]:.3f} cpu={group_metrics["cpu"]:.3f} steps='+','.join(r['name'] for r in rows), flush=True)
    for r in sorted(results, key=lambda r: r['wall'], reverse=True)[:5]:
        print(f'LONGEST {r["name"]} wall={r["wall"]:.3f} cpu={r["cpu"]:.3f}', flush=True)
    wall = time.monotonic()-started
    summary = dict(exit=0 if complete else 1, wall=wall, capacity=measured_capacity, build=build_result, results=results)
    (run/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(f'TOTAL wall={wall:.3f} exit={summary["exit"]}', flush=True)
    return summary['exit']


if __name__ == '__main__':
    raise SystemExit(main())
