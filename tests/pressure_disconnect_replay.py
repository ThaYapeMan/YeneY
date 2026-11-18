# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Gate the old shared input and the controller's new start independently."""


def run(c, immutable, fetch):
    cli, mac, wait, sonos, items = (c[key] for key in ('cli', 'mac', 'wait', 'sonos', 'items'))
    tap = c['tap']
    wait(tap.http_gate.held.is_set, 10, 'shared input prefix held')
    start = len(tap.records)
    target = 7
    commands_before = len(c['proxy'].records)
    soap_before = len(sonos.requests)
    tap.hold_control.set()
    cli('playlist index ' + str(target), mac)
    wait(tap.held_start.is_set, 10, 'controller q+s held before delivery')
    tap.http_gate.release.set()
    wait(lambda: any(packet['op'] == 'DSCO' for packet in tap.records[start:]),
         10, 'older shared input closed after controller selection')
    dsco = [packet['body'] for packet in tap.records[start:] if packet['op'] == 'DSCO']
    observed = int(cli('playlist index ?', mac)[-1])
    print(f'PRESSURE REPLAY before held starts delivered expected={target} observed={observed} DSCO={dsco}',
          flush=True)
    tap.release_control.set()
    assert observed == target, ('old shared input changed the LMS selection', target, observed, dsco)
    wait(lambda: sonos.position['TrackURI'] in immutable and sonos.position['TrackURI'] != immutable[0],
         20, 'pressure replay renderer selected a new row')
    rendered = immutable.index(sonos.position['TrackURI'])
    print(f'PRESSURE REPLAY expected={target} renderer={rendered}', flush=True)
    assert rendered == target, ('pressure replay rendered the wrong row', target, rendered, dsco)
    assert [row['uri'] for row in items()] == immutable
    fetch(target)
    wait(lambda: any(read[1] == immutable[target] for read in sonos.audible_reads),
         15, 'pressure replay target audio reached the renderer')
    assert dsco == ['00'], dsco
    assert not [entry for entry in c['proxy'].records[commands_before:]
                if ' playlist index ' in entry['request'] and not entry['request'].endswith(' ?')]
    writes = [entry for entry in sonos.requests[soap_before:] if entry[0] == 'Seek']
    assert len(writes) == 1 and writes[0][1]['Unit'] == 'TRACK_NR', writes
    assert not [entry for entry in sonos.requests[soap_before:] if entry[0] in
                ('AddURIToQueue', 'AddMultipleURIsToQueue', 'RemoveTrackRangeFromQueue')]
