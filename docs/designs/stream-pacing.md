# Stream pacing qualification

On 19 October 2026, the owner qualified Study on bridge `5a686ad` with
`/usr/local/bin/yeney-stream-pacing-suite --room Study --hours 2`: **PACING OK**.
The run collected 720 samples at 10-second intervals over one connection, with
719/720 samples locked, zero overwrites and zero drops. Qualified lead was
1406–1499 ms, with a 1452 ms median in both hours; measured drift was about
11.5 ppm. This result supports making Sonos-clock pacing the default.

The default is now `YENEY_STREAM_PACING=sonos`; invalid settings warn and use
`sonos`. Explicit `wall` remains a per-room opt-out via a systemd override:

```ini
[Service]
Environment=YENEY_STREAM_PACING=wall
```

Restart only the selected room after changing its override. The legacy wire
fixtures run explicitly with `YENEY_STREAM_PACING=wall`; settings and status
tests also verify the unset Sonos default. HTTP constants, URLs and transport
ordering are unchanged.

Qualify other rooms with
`/usr/local/bin/yeney-stream-pacing-suite --room <room> --hours 2`. See
[device verification](../../DEVICE-VERIFICATION.md#stream-pacing-selection-and-measurements)
for the clock model, lead bounds, diagnostics and owner-run procedure.
