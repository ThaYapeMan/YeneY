#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Check header dependencies and incremental product builds."""
import argparse
from collections import defaultdict
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CORE = Path('third_party/yeney-core')
ALAC = CORE / 'third_party/alac'
START = '1e7c403'
PRE_REACH = 'e689d69'


def run(directory, *args, capture=False):
    return subprocess.run(args, cwd=directory, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def git(directory, *args):
    return run(directory, 'git', *args, capture=True).strip()


def product_objects(tree):
    # A product-only build emits bridge and core library objects, plus the
    # independently compiled shared-memory sink in the bridge root.
    return set(tree.glob('*.o')) | set((tree / 'upnp').glob('*.o')) | {
        path for path in (tree / CORE).rglob('*.o') if '.git' not in path.parts
    }


def touch_headers():
    objects = sorted(path for path in product_objects(ROOT)
                     if not path.is_relative_to(ROOT / CORE))
    expected = defaultdict(set)
    for obj in objects:
        dependency = obj.with_suffix('.o.d')
        rule = dependency.read_text().replace('\\\n', ' ').splitlines()[0]
        target, prerequisites = rule.split(':', 1)
        assert target == str(obj.relative_to(ROOT)), rule
        for name in shlex.split(prerequisites):
            if name.endswith('.h'):
                expected[(ROOT / name).resolve()].add(target)
    assert objects and expected[ROOT / 'upnp/stream_server.h'] & {'wav_writer.o'}
    headers = {ROOT / name for name in git(ROOT, 'ls-files', '*.h').splitlines()}
    headers.update({path for path in expected if not path.is_relative_to(ROOT / CORE)})

    def scheduled():
        output = run(ROOT, 'make', '-n', 'all', capture=True)
        names = {str(path.relative_to(ROOT)) for path in objects}
        rebuilt = set()
        for line in output.splitlines():
            args = shlex.split(line.rstrip('\\'))
            if '-c' in args and '-o' in args:
                target = args[args.index('-o') + 1]
                if target in names:
                    rebuilt.add(target)
        return rebuilt

    assert not scheduled(), 'touch check needs a settled product build'
    for header in sorted(headers):
        stat = header.stat()
        try:
            os.utime(header, None)
            rebuilt = scheduled()
        finally:
            os.utime(header, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        assert rebuilt == expected[header], (
            f'{header.relative_to(ROOT)}: missing {sorted(expected[header] - rebuilt)}, '
            f'unexpected {sorted(rebuilt - expected[header])}')
    print(f'PASS header touches: {len(headers)} headers and {len(objects)} bridge objects')


def checkout(tree, revision):
    run(tree, 'git', 'checkout', '--quiet', '--detach', revision)
    run(tree / CORE, 'git', 'checkout', '--quiet', '--detach',
        git(tree, 'rev-parse', f'HEAD:{CORE}'))
    run(tree / ALAC, 'git', 'checkout', '--quiet', '--detach',
        git(tree / CORE, 'rev-parse', 'HEAD:third_party/alac'))


def clone_tree(tree, revision):
    for part in (Path('.'), CORE, ALAC):
        run(ROOT, 'git', 'clone', '--quiet', '--local', '--no-hardlinks', '--no-checkout',
            str(ROOT / part), str(tree / part))
    checkout(tree, revision)


def build(tree, jobs, *args):
    started = time.monotonic()
    run(tree, 'make', f'-j{jobs}', *args)
    print(f'Product build wall {time.monotonic() - started:.3f} s', flush=True)


def checksums(tree):
    return {str(path.relative_to(tree)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(product_objects(tree) | {tree / 'yeney'})}


def compare(before, after, label):
    differing = sorted(name for name in before.keys() | after.keys()
                       if before.get(name) != after.get(name))
    assert not differing, f'{label}: differing artifacts {differing}'


def upgrade(jobs):
    final = git(ROOT, 'rev-parse', 'HEAD')
    with tempfile.TemporaryDirectory(prefix='yeney-build-upgrade-') as directory:
        temp = Path(directory)
        tree = temp / 'tree'
        incremental = {}
        for older in (PRE_REACH, START):
            print(f'Building {older}, then updating to {final} in place', flush=True)
            clone_tree(tree, older)
            build(tree, jobs)
            checkout(tree, final)
            build(tree, jobs)
            incremental[older] = checksums(tree)
            shutil.rmtree(tree)

        print(f'Building {final} from scratch at the same path', flush=True)
        clone_tree(tree, final)
        build(tree, jobs)
        fresh = checksums(tree)
        for older, artifacts in incremental.items():
            compare(artifacts, fresh, older)
            print(f'PASS upgrade from {older}: {len(fresh)} identical artifact checksums')

        version = run(tree, './yeney', '--version', capture=True).strip()
        expected = (tree / 'VERSION').read_text().strip() + '-' + git(tree, 'rev-parse', '--short=7', 'HEAD')
        expected += ' (core ' + git(tree / CORE, 'rev-parse', '--short=7', 'HEAD') + ')'
        assert version == expected, (version, expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('check', choices=('touch', 'upgrade'))
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.check == 'touch':
        touch_headers()
    else:
        upgrade(args.jobs)


if __name__ == '__main__':
    main()
