# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Queue delivery and navigation through the Study's loopback fetch model."""
import socket
import threading
import time

from queue_sonos_fake import request
from served_queue_audio import RowHeader, RowReader


class Fetch:
    """One audio request. The renderer drains its buffer at PCM play speed."""

    def __init__(self, row, reference, offset, play_from, speaker_seek=False):
        self.row, self.reference, self.offset = row, reference, offset
        self.rate = int.from_bytes(reference[28:32], "little")
        self.play_from = play_from
        self.speaker_seek = speaker_seek
        self.opened = time.monotonic()
        self.received = 0
        self.finished = None
        self.failure = None
        self.cancelled = threading.Event()
        self.sock = None
        self.worker = threading.Thread(target=self.read, daemon=True)
        self.worker.start()

    @property
    def cursor(self):
        return self.offset + self.received

    def read(self):
        try:
            self.download()
        except (OSError, EOFError) as error:
            if not self.cancelled.is_set():
                self.failure = error
        except Exception as error:
            self.failure = error

    def download(self):
        info = None
        wire_offset = self.offset
        first = 0
        if self.offset and self.row["uri"].endswith(".flac"):
            probe, response = request(self.row["uri"], byte_range="bytes=0-41")
            try:
                assert response.status == 206
                info = RowHeader.read(response)
            finally:
                response.close()
                probe.close()
            target, remainder = divmod(self.offset - 44, 6)
            wire_offset, first = info.frame_range(target, speaker_seek=self.speaker_seek)
            if self.speaker_seek:
                self.offset = 44 + first * 6
                remainder = 0
        byte_range = f"bytes={wire_offset}-" if wire_offset else None
        client, response = request(self.row["uri"], byte_range=byte_range)
        try:
            self.sock = response.fp.raw._sock
            assert response.status == (206 if wire_offset else 200), response.status
            reader = None
            if self.row["uri"].endswith(".flac"):
                reader = RowReader(response, info, first)
                info = reader.header
                wire_length = info.frame_offset((info.samples + 255) // 256)
                if wire_offset and remainder:
                    reader.read_pcm(remainder)
            else:
                wire_length = len(self.reference)
            assert int(response.getheader("Content-Length")) == wire_length - wire_offset
            due = time.monotonic()
            while not self.cancelled.is_set() and self.cursor < len(self.reference):
                now = time.monotonic()
                consumed = max(0, now - self.play_from) * self.rate
                size = min(8192, len(self.reference) - self.cursor)
                if now < due or self.received + size > 20_000_000 + consumed:
                    self.cancelled.wait(.005)
                    continue
                if reader:
                    header = reader.header.pcm_header[self.cursor:self.cursor + size] if self.cursor < 44 else b""
                    chunk = header + reader.read_pcm(size - len(header))
                else:
                    chunk = response.read1(size)
                if not chunk:
                    if self.cancelled.is_set():
                        return
                    raise EOFError(f"short queue body at {self.cursor}")
                assert chunk == self.reference[self.cursor:self.cursor + len(chunk)], (
                    self.row["metadata"]["title"], self.cursor, "PCM or WAV header differs")
                self.received += len(chunk)
                due = time.monotonic() + len(chunk) / (6 * self.rate)
            if self.cursor == len(self.reference):
                self.finished = time.monotonic()
        finally:
            self.sock = None
            response.close()
            client.close()

    def check(self):
        if self.failure:
            raise self.failure

    def close(self):
        self.cancelled.set()
        sock = self.sock
        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.worker.join(4)
        assert not self.worker.is_alive(), "audio reader did not close"
        self.check()


def expected_wav(path):
    source = path.read_bytes()
    rate = int.from_bytes(source[24:28], "little")
    body = bytearray((len(source) - 44) * 3 // 2)
    body[1::3], body[2::3] = source[44::2], source[45::2]
    header = bytearray(source[:44])
    header[4:8] = (len(body) + 36).to_bytes(4, "little")
    header[28:32] = (rate * 6).to_bytes(4, "little")
    header[32:36] = bytes((6, 0, 24, 0))
    header[40:44] = len(body).to_bytes(4, "little")
    return header + body


def run(context, mode):
    cli = context["cli"]
    mac = context["mac"]
    base = context["base"]
    wait = context["wait"]
    sonos = context["sonos"]
    items = context["items"]
    ids = context["window_ids"]
    active = []

    def bridge_log():
        return (base / "bridge.log").read_text()

    def command(value):
        return cli(value, mac)

    def index_is(index):
        return int(command("playlist index ?")[-1]) == index

    def position(seconds, row=None):
        seconds = int(seconds)
        rel_time = f"{seconds // 3600:02}:{seconds // 60 % 60:02}:{seconds % 60:02}"
        with sonos.lock:
            if row is not None:
                sonos.position = dict(Track=str(sonos.items.index(row) + 1),
                                      TrackURI=row["uri"], RelTime=rel_time)
            else:
                sonos.position["RelTime"] = rel_time

    def load(indices):
        command("playlistcontrol cmd:load track_id:" + ",".join(ids[i] for i in indices))
        wait(lambda: len(items()) == len(indices), 15, "paced queue publication")
        context["select_item"](0)
        wait(lambda: index_is(0), 5, "initial LMS pairing")
        rows = items()
        for row, i in zip(rows, indices):
            assert row["metadata"]["title"] == f"Title & {i}"
        return rows, [expected_wav(base / f"input-{i}.wav") for i in indices]

    def fetch(row, reference, offset, play_from, speaker_seek=False):
        download = Fetch(row, reference, offset, play_from, speaker_seek)
        active.append(download)
        return download

    def finish():
        for download in active:
            download.close()
        active.clear()
        command("stop")
        wait(lambda: not items(), 10, "paced queue release")

    def play(download, start_position, play_from, stop_position):
        while True:
            download.check()
            now = time.monotonic()
            current = min(stop_position, max(start_position, start_position + now - play_from))
            needed = 44 + int(current * download.rate)
            if needed > download.cursor:
                assert now - play_from < .15, ("buffer underrun", needed, download.cursor)
            position(current)
            if current >= stop_position:
                break
            time.sleep(.02)

    def start(row, reference, seconds=0):
        play_from = time.monotonic() + .5
        rate = int.from_bytes(reference[28:32], "little")
        offset = 44 + seconds * rate if seconds else 0
        return fetch(row, reference, offset, play_from, speaker_seek=bool(seconds)), play_from

    def seek(row, seconds):
        mark = len(sonos.actions("Seek"))
        log_mark = len(bridge_log())
        command(f"time {seconds}")
        wait(lambda: any(args.get("Unit") == "REL_TIME" and
                             args.get("Target", "").endswith(f":{seconds % 60:02}")
                         for _, args, *_ in sonos.actions("Seek")[mark:]),
             10, f"renderer seek to {seconds}")
        position(seconds, row)
        wait(lambda: abs(float(command("time ?")[-1]) - seconds) <= 1, 5,
             "LMS seek landing")
        assert index_is(0)
        assert sonos.position["TrackURI"] == row["uri"]
        wait(lambda: "decision=in-item" in bridge_log()[log_mark:], 5, "retained WAV seek")

    def join(download, current_seconds, play_from, following, reference):
        duration = (len(download.reference) - 44) / download.rate
        end = play_from + duration - current_seconds
        if current_seconds == 0:
            assert end - download.opened >= 5.8 + (duration - 2) / 2
        play(download, current_seconds, play_from, duration - 5.8)
        wait(lambda: download.finished is not None, .25, "first row complete before prefetch")
        assert end - download.finished >= 5.8, (duration, end - download.finished)
        if current_seconds == 0:
            minimum = (duration - 2) / 2
            assert download.finished - download.opened >= minimum - .1, (
                "current row exceeded default delivery rate", duration, download.finished - download.opened)
        next_download = fetch(following, reference, 0, end)
        assert next_download.opened >= max(end - 5.8, download.finished)
        play(download, current_seconds, play_from, duration)
        log_mark = len(bridge_log())
        position(0, following)
        sonos.notify_transport()
        play(next_download, 0, end, 1)
        wait(lambda: index_is(1), 8, "natural successor pairing")
        wait(lambda: "kind=natural" in bridge_log()[log_mark:], 5, "natural queue transition")
        assert sonos.position["TrackURI"] == following["uri"]
        assert next_download.received >= next_download.rate
        print(f"PASS paced natural join duration={duration:.1f} complete_ahead={end-download.finished:.3f}",
              flush=True)

    try:
        if mode == 'transport':
            (first, second), (one, two) = load((0, 1))
            download, at = start(first, one)
            play(download, 0, at, 2)
            assert download.received <= 1.05 * download.rate * (
                2 + 2 * (time.monotonic() - download.opened)), download.received
            print("PASS paced start matching source PCM", flush=True)
            download.close()
            seek(first, 16)
            download, at = start(first, one, 16)
            play(download, 16, at, 18)
            print("PASS paced seek matching Range bytes and LMS landing", flush=True)
            command("pause 1")
            wait(lambda: sonos.transport_state == "PAUSED_PLAYBACK", 5, "renderer pause")
            download.close()
            cursor = download.cursor
            time.sleep(10)
            command("pause 0")
            wait(lambda: sonos.transport_state == "PLAYING", 5, "renderer resume")
            at = time.monotonic()
            resumed = fetch(first, one, cursor, at + (cursor - 44) / download.rate - 18)
            assert resumed.offset == cursor
            play(resumed, 18, at, 20)
            print("PASS paced resume matching bytes from the end of the prior buffer", flush=True)
            resumed.close()
            duration = (len(one) - 44) // download.rate
            seek(first, duration - 12)
            download, at = start(first, one, duration - 12)
            join(download, duration - 12, at, second, two)
            print("PASS paced seek 12 s before the join", flush=True)
            finish()

            (first, second), (one, two) = load((0, 1))
            download, at = start(first, one)
            play(download, 0, at, 2)
            download.close()
            log_mark = len(bridge_log())
            command("playlist index +1")
            wait(lambda: sonos.position["TrackURI"] == second["uri"], 8, "LMS Next target URI")
            assert index_is(1)
            wait(lambda: "lms_commands=0 kind=next" in bridge_log()[log_mark:], 5, "LMS Next pairing")
            next_download, at = start(second, two)
            play(next_download, 0, at, 2)
            print("PASS paced LMS Next landing and target audio bytes", flush=True)
            finish()
        else:
            for indices in ((0, 1), (2, 3)):
                (first, second), (one, two) = load(indices)
                download, at = start(first, one)
                join(download, 0, at, second, two)
                finish()

    finally:
        for download in active:
            download.close()
