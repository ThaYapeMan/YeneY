#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import contextlib
import hashlib
import http.client
import os
import socket
import subprocess
import time
import unittest

RATE = 44100 * 6
LENGTH = 44 + 32 * RATE


class Fixture:
    def __init__(self, **settings):
        self.settings = settings

    def __enter__(self):
        env = dict(os.environ, YENEY_QUEUE_PACING="on", YENEY_QUEUE_PACE_RATE="2",
                   YENEY_QUEUE_PACE_FREE_MS="2000", YENEY_QUEUE_PACE_TAIL_MS="20000")
        env.update(self.settings)
        self.process = subprocess.Popen(
            ["./yeney-smoke-server", "--pacing-fixture"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env,
        )
        for line in self.process.stdout:
            if line.startswith("fixture "):
                _, port, self.url, self.short_url = line.split()
                self.port = int(port)
                return self
        self.__exit__()
        raise RuntimeError("queue pacing fixture exited before listening")

    def __exit__(self, *_):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        finally:
            self.process.stdout.close()

    @contextlib.contextmanager
    def request(self, range_value=None, short=False):
        with contextlib.closing(http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)) as client:
            client.connect()
            sock = client.sock
            headers = {"Range": range_value} if range_value else {}
            client.request("GET", self.short_url if short else self.url, headers=headers)
            response = client.getresponse()
            try:
                assert response.status == (206 if range_value else 200), response.status
                yield response, sock
            finally:
                response.close()

    def diagnostic(self):
        for line in self.process.stdout:
            if "queue HTTP method=GET" in line:
                return dict(field.split("=", 1) for field in line.split() if "=" in field)
        raise AssertionError("missing request diagnostic")

    def command(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()


class QueuePacing(unittest.TestCase):
    def test_speed(self):
        with Fixture() as fixture, fixture.request() as (response, _):
            start = time.monotonic()
            received = 0
            for seconds in (1, 2, 5, 10):
                deadline = start + seconds
                while received < LENGTH and time.monotonic() < deadline:
                    received += len(response.read1(8192))
                expected = RATE * (2 + 2 * seconds)
                self.assertAlmostEqual(received, expected, delta=expected * .05,
                                       msg=f"body bytes at {seconds} s")
        with Fixture(YENEY_QUEUE_PACING="off") as fixture, fixture.request() as (response, _):
            start = time.monotonic()
            self.assertEqual(len(response.read()), LENGTH)
            self.assertLess(time.monotonic() - start, 1)
            self.assertEqual(fixture.diagnostic()["paced"], "0")

    def test_same_bytes(self):
        ranges = (None, "bytes=0-8192", "bytes=45-199999", "bytes=-123456")
        with Fixture(YENEY_QUEUE_PACING="off") as off, Fixture() as on:
            for range_value in ranges:
                with self.subTest(range=range_value):
                    with off.request(range_value) as (response, _):
                        reference = hashlib.sha256(response.read()).digest()
                    with on.request(range_value) as (response, _):
                        self.assertEqual(hashlib.sha256(response.read()).digest(), reference)
                    self.assertEqual(on.diagnostic()["paced"],
                                     "0" if range_value == "bytes=-123456" else "1")

    def test_tail(self):
        with Fixture() as fixture:
            for range_value, short, length in (
                (f"bytes={LENGTH - 12 * RATE}-", False, 12 * RATE),
                (None, True, 44 + 10 * RATE),
            ):
                with fixture.request(range_value, short) as (response, _):
                    start = time.monotonic()
                    self.assertEqual(len(response.read()), length)
                    self.assertLess(time.monotonic() - start, 1)
                diagnostic = fixture.diagnostic()
                self.assertEqual((diagnostic["paced"], diagnostic["pace_wait_ms"]), ("0", "0"))

    def test_prompt_end(self):
        for action in ("close", "cancel", "flush"):
            with self.subTest(action=action), Fixture(
                YENEY_QUEUE_PACE_RATE="1", YENEY_QUEUE_PACE_FREE_MS="0",
            ) as fixture:
                with fixture.request() as (response, sock):
                    self.assertEqual(len(response.read(44)), 44)
                    start = time.monotonic()
                    if action == "close":
                        sock.shutdown(socket.SHUT_RDWR)
                    else:
                        fixture.command(action)
                    diagnostic = fixture.diagnostic()
                    self.assertLess(time.monotonic() - start, .1)
                    self.assertEqual(diagnostic["paced"], "1")
                    self.assertLess(int(diagnostic["served_bytes"]), 8192)

    def test_slow_client(self):
        with Fixture() as fixture, fixture.request("bytes=0-2645999") as (response, _):
            start = time.monotonic()
            for step in range(20):
                self.assertEqual(len(response.read(RATE // 2)), RATE // 2)
                time.sleep(max(0, start + (step + 1) * .5 - time.monotonic()))
            diagnostic = fixture.diagnostic()
            self.assertEqual(diagnostic["paced"], "1")
            self.assertEqual(diagnostic["pace_wait_ms"], "0")

    def test_no_second_burst(self):
        with Fixture() as fixture, fixture.request() as (response, _):
            response.read(2 * RATE)
            time.sleep(10)
            start = time.monotonic()
            # The transport can already hold bytes handed out before it blocked.
            # Check that the response cannot finish in a second burst.
            self.assertLess(len(response.read()), LENGTH)
            self.assertGreater(time.monotonic() - start, 1)
            self.assertEqual(fixture.diagnostic()["paced"], "1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
