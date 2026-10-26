# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Local fake-server runner; injection never appears in the installed CLI."""
import importlib.machinery
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader('queue_suite', str(ROOT/'scripts/yeney-queue-suite'))
spec = importlib.util.spec_from_loader(loader.name, loader)
suite = importlib.util.module_from_spec(spec)
loader.exec_module(suite)
if __name__ == '__main__':
    output, dryrun, bonus, *args = sys.argv[1:]
    if args[args.index('--server')+1] != '127.0.0.1':
        raise RuntimeError('tests must use localhost')
    raise SystemExit(suite.main(args, root=Path(output), lock_root=Path(output), dryrun=dryrun,
                               ready_timeout=4, finish_timeout=5, timeout_bonus=float(bonus)))
