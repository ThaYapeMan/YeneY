# FULLQ-01: stopped at the partial-seek representation boundary

The round is stopped under its explicit exception: core stays pinned at
`9d5aa42`, and work must stop if a core change seems needed. The static mirror
is **not delivered**. The two preliminary commits are reverted by a subsequent
commit; published history is preserved. The production window remains in place.

## Evidence

A disposable LMS 9.1.1 and loopback renderer experiment published twelve rows
and completed seven LMS index changes with one TRACK_NR Seek and zero queue
edits each. These checks did not establish byte identity or calibrated audible
latency and are not acceptance of Q1/Q2 in full.

The unchanged B1 behavioral check then failed. Only the old initial four-row
assertion was changed to the complete playlist length. B1 held the original
producer before the seek point, issued `time 12`, and observed:

```
queue window seek lms_index=0 position_ms=12000 decision=fallback reason=prefix-unavailable lms_commands=0
```

After playing the partial to completion and moving to index 1, the check timed
out with:

```
AssertionError: real LMS deadline: Next window committed with deferred frame-zero history
```

That check requires the history row to have a different URI from the immutable
seek suffix, before an app can select it. Its subsequent assertions require a
frame-zero WAV byte-identical to the original production, a single switch, and
no selection of the suffix. Those are behavioral requirements, not obsolete
six-row/window-size assertions, so they were not weakened.

## Representation problem

At a prefix-unavailable seek, the current API produces a suffix WAV. The existing
relay publishes it under a new URI. On leaving it, the window editor replaces
that suffix with a deferred frame-zero row. A static mirror must also make Back
safe, but the following attempted shortcut is invalid:

1. Leave the suffix URI in the row, avoiding Add/Remove during navigation.
2. Rebind its deferred route to a fresh frame-zero production on Back.

The full WAV has a different length and frame-zero coordinate. Reusing that URI
changes its immutable resource promise, and the app first selects a URI that
still denotes the suffix. Giving the full WAV a different URI instead requires
a Sonos row edit; moving that edit into a delayed worker does not make it
independent of navigation.

A stable full-length resource with an absent prefix would need production that
can obtain the missing source region while preserving the selected LMS seek
clock. The pinned core's start/refetch hooks consume LMS-supplied starts; they
do not expose an independent full-source/region producer to this bridge. See
`core/sink.h` queue-start/refetch hooks and `core/queue_mode.cpp` received-start
and refetch handling in the pinned submodule. Such support appears to need a
core change or the sparse full-length work explicitly deferred to rounds 2–3.
This is the reason for applying the stop exception, not a claim that every
possible alternative architecture has been disproved.

No core implementation or pin was changed. No owner LMS or speaker was
contacted. No deployment occurred. The report records incomplete acceptance
rather than presenting the experiment as a device fix.
