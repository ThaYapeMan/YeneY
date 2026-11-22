# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import time
import unittest

from queue_pacing_test import Fixture, LENGTH, RATE


class QueuePacingTail(unittest.TestCase):
    def test_crossing_exempts_tail(self):
        for form in ("wav", "flac"):
            with self.subTest(form=form), Fixture(YENEY_QUEUE_FORMAT=form,
                                                  YENEY_QUEUE_PACE_TAIL_MS="20000") as fixture:
                length = LENGTH if form == "wav" else 42 + 32 * RATE + sum(
                    11 + (i >= 128) + (i >= 2048) for i in range((32 * 44100 + 255) // 256))
                with fixture.request(f"bytes={length - 23 * RATE}-") as (response, _):
                    start = time.monotonic()
                    self.assertEqual(len(response.read()), 23 * RATE)
                    self.assertLess(time.monotonic() - start, 2)
                self.assertEqual(fixture.diagnostic()["paced"], "1")


if __name__ == "__main__":
    unittest.main()
