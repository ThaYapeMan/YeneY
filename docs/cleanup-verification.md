# Cleanup verification, 24 October 2026

Baseline: bridge `c3baa3d`, core `39ba6e0`. The unchanged baseline source was
built and tested in an isolated ignored directory before running the new full
suite. Its full `make test` passed in 1390.468 seconds (23 minutes 10.47 seconds),
including real LMS 9.1.1 and the required LampaStream consumer.

The report supplies the after-run duration and transcripts. Counts use tracked
`.py`, `.cpp` and `.h` source/helper files below `tests/`, excluding
`tests/archive/`; media fixtures are not test-source files. Baseline bridge/core
counts are 109/22. The active bridge Makefile invokes 47 distinct Python test
entry points; three completed experiment suites are replaced by the script-path,
debug-settings and remote-selection regression suites. One-off comparison tools
are source/helper files but are not silently added to every `make test` run.

The unchanged real-LMS acceptance script covers joins, controls, hi-res, M4A
lengths, remote sources and external stop/pause. Installer/state-directory tests
are retained. The targeted queue/device suites, debug cadence, reference checks
and installer checks pass after cleanup.

`tests/queue_refactor_compare.py --baseline c3baa3d` verifies all 20 relay method
bodies, constructor initializers and state declaration order token-for-token.
Thus the single worker, lock order and existing production log literals are
unchanged. Only the explicitly requested debug deprecation diagnostics are new.
The part-7 owner suite adds candidate diagnostics and clearer remote SKIP notes.

To compare the complete finite audio entities, run:

```
python3 tests/queue_wav_compare.py --before /path/to/baseline/yeney --after ./yeney
```

All four targeted 44.1 kHz items match: 2,116,844 bytes each, SHA-256
`f72aebd8ee6d9ca38ac52aef1bd3a3e6d5e4b1c7b49d5dd0ce97e365c8a5ec40`.
The existing wire fixtures are retained without edits. HTTP constants, URL
formats, core rendering, transport order and polling intervals are unchanged.
The complete setting inventory is in [settings-audit.md](settings-audit.md).
