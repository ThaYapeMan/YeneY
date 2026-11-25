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

SAMPLE_RATE = 44100
RATE = SAMPLE_RATE * 6
LENGTH = 44 + 32 * RATE


def length(seconds, form):
    samples = SAMPLE_RATE * seconds
    if form == "wav":
        return 44 + samples * 6
    frames = (samples + 255) // 256
    number_bytes = frames + sum(max(0, frames - limit) for limit in (128, 2048, 65536))
    return 42 + samples * 6 + frames * 11 + number_bytes


class Fixture:
    def __init__(self, small_window=False, **settings):
        self.small_window = small_window
        self.settings = settings

    def __enter__(self):
        env = dict(os.environ, YENEY_QUEUE_PACING="on", YENEY_QUEUE_PACE_RATE="2",
                   YENEY_QUEUE_PACE_FREE_MS="2000")
        env.pop("YENEY_QUEUE_PACE_TAIL_MS", None)
        env.update(self.settings)
        command = [os.environ.get("YENEY_PACING_FIXTURE", "./yeney-smoke-server"), "--pacing-fixture"]
        if self.small_window:
            command.append("--small-window")
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env,
        )
        for line in self.process.stdout:
            if line.startswith("fixture "):
                _, port, self.url, self.short_url = line.split()
                self.port = int(port)
                self.form = self.url.rsplit(".", 1)[1]
                self.length = length(32, self.form)
                self.rate = self.length / 32
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
            if self.small_window:
                client.sock = socket.socket()
                client.sock.settimeout(client.timeout)
                client.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
                client.sock.connect(("127.0.0.1", self.port))
            else:
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
                while received < fixture.length and time.monotonic() < deadline:
                    received += len(response.read1(8192))
                expected = fixture.rate * (2 + 2 * seconds)
                self.assertAlmostEqual(received, expected, delta=expected * .05,
                                       msg=f"body bytes at {seconds} s")
        with Fixture(YENEY_QUEUE_PACING="off") as fixture, fixture.request() as (response, _):
            start = time.monotonic()
            self.assertEqual(len(response.read()), fixture.length)
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
                    self.assertEqual(on.diagnostic()["paced"], "1")

    def test_tail(self):
        with Fixture() as fixture:
            tail = int(12 * fixture.rate)
            for range_value, short, count in (
                (f"bytes={fixture.length - tail}-", False, tail),
                (None, True, length(10, fixture.form)),
            ):
                with fixture.request(range_value, short) as (response, _):
                    start = time.monotonic()
                    self.assertEqual(len(response.read()), count)
                    self.assertGreaterEqual(time.monotonic() - start, (count / fixture.rate - 2) / 2 - .1)
                diagnostic = fixture.diagnostic()
                self.assertEqual(diagnostic["paced"], "1")
                self.assertGreater(int(diagnostic["pace_wait_ms"]), 0)

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
        # Keep the transport's read-ahead smaller than the initial allowance.
        with Fixture(small_window=True) as fixture:
            tick = int(fixture.rate / 20)
            with fixture.request(f"bytes=0-{200 * tick - 1}") as (response, _):
                start = time.monotonic()
                for step in range(200):
                    self.assertEqual(len(response.read(tick)), tick)
                    time.sleep(max(0, start + (step + 1) * .05 - time.monotonic()))
                diagnostic = fixture.diagnostic()
                self.assertEqual(diagnostic["paced"], "1")
                self.assertEqual(diagnostic["pace_wait_ms"], "0")

    def test_no_second_burst(self):
        with Fixture(small_window=True) as fixture, fixture.request() as (response, sock):
            initial = int(2 * fixture.rate)
            self.assertEqual(len(response.read(initial)), initial)
            time.sleep(10)
            window = .1
            deadline = time.monotonic() + window
            received = 0
            while time.monotonic() < deadline:
                received += len(response.read1(8192))
            # Account for TCP data queued before the pause and credit earned
            # during the measurement. Include one outstanding server write
            # and one final read that can straddle the deadline.
            limit = initial + int(2 * fixture.rate * window) + 65536 + 2 * 8192
            self.assertLessEqual(received, limit)
            sock.shutdown(socket.SHUT_RDWR)
            self.assertEqual(fixture.diagnostic()["paced"], "1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
