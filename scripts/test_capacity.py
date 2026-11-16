#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Read effective processor capacity inside this container's cgroup mount."""
import json
import os
from pathlib import Path
import subprocess


def capacity():
    root = Path('/sys/fs/cgroup')
    directories = {root}
    for line in Path('/proc/self/cgroup').read_text().splitlines():
        if line.startswith('0::'):
            current = root/line[3:].lstrip('/')
            while current != root and root in current.parents:
                if current.is_dir():
                    directories.add(current)
                current = current.parent
    affinity = len(os.sched_getaffinity(0))
    limits = [float(affinity)]
    quotas = {}
    cpusets = {}
    for directory in sorted(directories):
        quota = directory/'cpu.max'
        if quota.is_file():
            raw = quota.read_text().strip()
            amount, period = raw.split()
            quotas[str(quota)] = raw
            if amount != 'max':
                limits.append(int(amount)/int(period))
        cpuset = directory/'cpuset.cpus.effective'
        if cpuset.is_file():
            raw = cpuset.read_text().strip()
            if raw:
                count = 0
                for part in raw.split(','):
                    ends = part.split('-')
                    count += int(ends[-1])-int(ends[0])+1
                limits.append(float(count))
                cpusets[str(cpuset)] = raw
    return dict(cores=min(limits), affinity_cpus=affinity,
                nproc=int(subprocess.check_output(['nproc'], text=True)),
                cgroup_quotas=quotas, cgroup_cpusets=cpusets)


if __name__ == '__main__':
    print(json.dumps(capacity(), sort_keys=True))
