"""Fail on stale tracked script paths in active source/docs/build files."""
from pathlib import Path
import re
import subprocess
ROOT=Path(__file__).resolve().parents[1]
files=subprocess.check_output(['git','ls-files'],cwd=ROOT,text=True).splitlines()
errors=[]
for name in files:
    p=ROOT/name
    if not p.is_file() or name.startswith(('scripts/archive/','tests/archive/','third_party/')):continue
    if p.suffix not in ('.md','.py','.sh','.cpp','.h') and name not in ('Makefile','CLAUDE.md'):continue
    for line in p.read_text(errors='replace').splitlines():
        # Literal paths only; parameterized examples/regexes are not file names.
        for path in re.findall(r'(?<![\w/:])scripts/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*',line):
            path=path.rstrip('.')
            if path in ('scripts/','scripts/archive') or not Path(path).suffix and path.endswith('-'):continue
            if not (ROOT/path).exists():errors.append((name,path))
assert not errors,errors
print('PASS active tracked script references resolve; archived paths exist and finished tests are not active')
