# Study, 11 October 2026

Trimmed from the owner's untracked `tests/fixtures/study-journal.txt`,
`study-cold-start.txt` and `study-warm-start.txt` (the requested dated source
directory was absent). Journal keeps timing request/edge/lock/prior, core
STMs/STMt/boundary, stream creation and final tap-boundary messages for cold
PID 43745 and warm PID 43848. Position files retain only original sample rows,
including identity; the original residual column is not used for fitting.

The checked stream is stream 2, not the earlier LMS restart-resume stream 1.
Track 32671 is 48 kHz, 589 seconds; 32678 is 44.1 kHz. The owner's stop at
14:44:12 is intentional. No generated device replies are saved here.

The C++ acquisition test first replays recorded interval constraints, then
simulates changed scheduler requests using the final field-supported interval
phase/rate and captured request RTT sequence. These counterfactual replies
cannot be literal journal responses. The position and live-drift tests are
explicit deterministic state-machine tests. The real bridge wire test covers
the new-rate audible gate; none of these tests proves acoustic onset.
