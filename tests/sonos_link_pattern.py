# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Controlled uneven link with a 1 ms floor and rate-dependent long delays."""
import statistics
import sys


PATTERN = 'uneven-observed'


def delays_ms(index, interval_s):
    """Return question and answer delay, keeping the rare spikes deterministic."""
    if interval_s > .12:
        rtt = 270 if index % 20 == 0 else 130 if index % 5 == 0 else 12 if index % 3 == 0 else 1
    else:
        rtt = 93 if index % 40 == 0 else 38 if index % 10 == 0 else 7 if index % 3 == 0 else 1
    return rtt - .5, .5


def check_delay():
    outbound, inbound = delays_ms(20, .2)
    rtt = outbound + inbound
    # Swapping the directional delays leaves the observed ping RTT intact.
    # Position observations at the answer time differ by the one-way asymmetry.
    assert rtt == 270 and outbound - inbound == 269
    assert abs(outbound - rtt / 2) > 100
    print(f'Identical {rtt:g} ms RTTs permit position sample times {outbound-inbound:g} ms apart')


def check_rate():
    slow = [sum(delays_ms(i, .2)) for i in range(1, 401)]
    fast = [sum(delays_ms(i, .05)) for i in range(1, 401)]
    assert min(slow) == min(fast) == 1
    assert max(slow) == 270 and max(fast) == 93
    assert statistics.mean(slow) > statistics.mean(fast)
    print(f'Controlled model mean RTT at 5 and 20 per second: {statistics.mean(slow):g} and {statistics.mean(fast):g} ms')


if __name__ == '__main__':
    {'delay': check_delay, 'rate': check_rate}[sys.argv[1]]()
