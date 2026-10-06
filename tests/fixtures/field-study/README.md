# Study, 6 October 2026

Trimmed from the owner's untracked `field-2026-10-06` captures. Position files
retain only MONOTONIC and LMS position columns (all 2400 samples each); the
original cross-track residual column is deliberately discarded. Cold journal
retains only estimator/raw/edge, initial stream and STMs evidence from process
39975 through the first gapless boundary. Warm journal retains stream/lock/STMs
messages from process 40110. Original wall-clock prefixes and unrelated device,
HTTP and metadata messages are omitted. No regenerated device responses are
present in these fixtures.

`field_cold_test.cpp` verifies the literal rejection sequence and original
interval-clock lock gates. Its counterfactual scheduler uses observed first-five
midpoint ticks, the field-supported late phase and persisted 10.850 ppm rate,
and captured RTTs. It generates the requests the old process never made; this
is a simulation, not a literal replay or new physical measurement.

`lms_position_check_test.py` fits each track separately in both captures and
checks stream-based cold/warm lock times of 325.213/6.703 seconds. The cold
second segment contains the 750 ms zero-elapsed interpolation freeze.
