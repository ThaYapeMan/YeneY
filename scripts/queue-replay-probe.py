#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Owner-run probe 11: replay YeneY's own queue metadata, byte for byte, from a neutral server.

The speaker holds full titles for YeneY's queue items (Browse Q:0), yet the
Sonos app shows blank rows; probes with hand-written metadata always showed
titles. This probe separates "content" from "how YeneY edits the queue":

  1. While YeneY is playing, it reads the current Sonos queue (Browse Q:0).
  2. It rebuilds each item's DIDL exactly as stored (same title, artist, album,
     artwork, item id, <res> attributes), changing only the <res> URL so it
     points at a 20 s quiet tone served by this script.
  3. It empties the queue, adds the items back in one go, in the same order,
     and plays item 1.

  python3 queue-replay-probe.py <speaker-ip> [mode]

Modes (default "batch"):
  batch      add all items at the end, one after another (probe 11 result: titles shown)
  positions  add item 1, then insert the others one at a time at explicit
             positions with DesiredFirstTrackNumberEnqueued=<n>, 1 s apart (like YeneY)
  url        like batch, but every URL looks like YeneY's:
             /music/queue.wav?session=...&epoch=1&item=<n>&revision=1
  remove     like batch with one extra item in front, which is then removed with
             RemoveTrackRangeFromQueue using the queue UpdateID (like a YeneY history trim)

Look at the Sonos app queue during the 30 s run (close and reopen the queue
view once). Titles shown  -> YeneY's metadata content is fine; the cause is in
how/when YeneY edits the queue.  Rows blank -> the content itself trips the app.
The script prints the DIDL it re-adds (session tokens masked). Afterwards it
stops and empties the queue; start from LMS again.
"""
import html
import math
import re
import socket
import struct
import sys
import threading
import time
import urllib.request
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

AVT = "urn:schemas-upnp-org:service:AVTransport:1"
CD = "urn:schemas-upnp-org:service:ContentDirectory:1"
RATE, BLOCK, SECONDS = 44100, 4096, 20
HEAD = ('<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
        'xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">')


def wav():
    one = b"".join(struct.pack("<hh", v, v) for v in
                   (int(1000 * math.sin(2 * math.pi * 41 * k / BLOCK)) for k in range(BLOCK)))
    data = one * round(SECONDS * RATE / BLOCK)
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16) + b"data" + struct.pack("<I", len(data)) + data)


def soap(ip, action, args, service=AVT, path="/MediaRenderer/AVTransport/Control"):
    body = "".join(f"<{k}>{html.escape(v, quote=False)}</{k}>" for k, v in args)
    envelope = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
                's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
                f'<u:{action} xmlns:u="{service}">{body}</u:{action}></s:Body></s:Envelope>')
    req = urllib.request.Request(f"http://{ip}:1400{path}", envelope.encode("utf-8"),
                                 {"Content-Type": 'text/xml; charset="utf-8"', "SOAPACTION": f'"{service}#{action}"'})
    with urllib.request.urlopen(req, timeout=30) as r:
        root = ET.fromstring(r.read())
    return {el.tag.split("}")[-1]: el.text or "" for el in root.iter() if len(el) == 0}


def browse_raw(ip):
    r = soap(ip, "Browse", [("ObjectID", "Q:0"), ("BrowseFlag", "BrowseDirectChildren"), ("Filter", "*"),
                            ("StartingIndex", "0"), ("RequestedCount", "10"), ("SortCriteria", "")],
             service=CD, path="/MediaServer/ContentDirectory/Control")
    return r.get("Result", "")


def rebuild(item_xml, url):
    """Turn a Browse Q:0 <item> back into the DIDL that was enqueued, with a new <res> URL."""
    orig_id = re.search(r'r:origID="([^"]*)"', item_xml)
    orig_parent = re.search(r'r:origParentID="([^"]*)"', item_xml)
    inner = re.search(r"<item[^>]*>(.*)</item>", item_xml, re.S).group(1)
    inner = re.sub(r"(<res[^>]*>)[^<]*(</res>)", lambda m: m.group(1) + html.escape(url) + m.group(2), inner)
    return (HEAD + f'<item id="{orig_id.group(1) if orig_id else "replay"}" '
            f'parentID="{orig_parent.group(1) if orig_parent else "0"}" restricted="true">'
            + inner + "</item></DIDL-Lite>")


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in ("batch", "positions", "url", "remove")):
        sys.exit(__doc__)
    ip = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) == 3 else "batch"
    result = browse_raw(ip)
    items = re.findall(r"<item .*?</item>", result, re.S)
    if not items:
        sys.exit("The Sonos queue is empty; start YeneY playback from LMS first.")
    body = wav()
    desc = urllib.request.urlopen(f"http://{ip}:1400/xml/device_description.xml", timeout=10).read().decode()
    rincon = re.search(r"<UDN>uuid:(RINCON_[0-9A-Fa-f]+)", desc).group(1)
    if soap(ip, "GetMediaInfo", [("InstanceID", "0")]).get("CurrentURI", "").startswith("x-rincon:"):
        sys.exit(f"{ip} is a group member; ungroup the room first.")

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_HEAD(self):
            self.send(False)

        def do_GET(self):
            self.send(True)

        def send(self, with_body):
            r = re.fullmatch(r"bytes=(\d+)-\d*", self.headers.get("Range") or "")
            off = int(r.group(1)) if r and int(r.group(1)) < len(body) else 0
            self.send_response(206 if r else 200)
            if r:
                self.send_header("Content-Range", f"bytes {off}-{len(body) - 1}/{len(body)}")
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(body) - off))
            self.send_header("Connection", "close")
            self.end_headers()
            if with_body:
                try:
                    self.wfile.write(body[off:])
                except (BrokenPipeError, ConnectionResetError):
                    pass

        def log_message(self, *a):
            pass

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((ip, 1400))
        local = s.getsockname()[0]
    server = ThreadingHTTPServer(("0.0.0.0", 0), H)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://{local}:{server.server_address[1]}"
    def url_for(n):
        if mode == "url":
            return f"{base}/music/queue.wav?session=6e9a9a7f94ebf828&epoch=1&item={n}&revision=1"
        return f"{base}/replay-{n}.wav"
    didls = [rebuild(x, url_for(n)) for n, x in enumerate(items, 1)]
    if mode == "remove":
        didls.insert(0, rebuild(items[0], url_for(0)))
    try:
        soap(ip, "RemoveAllTracksFromQueue", [("InstanceID", "0")])
        print(f"mode={mode}")
        for n, d in enumerate(didls, 1):
            print(f"item {n}:", re.sub(r"session=[^&<]+", "session=...", d))
            position = "0"
            if mode == "positions" and n > 1:
                position = str(n)
                time.sleep(1)
            soap(ip, "AddURIToQueue", [("InstanceID", "0"),
                                       ("EnqueuedURI", re.search(r"<res[^>]*>([^<]*)</res>", d).group(1).replace("&amp;", "&")),
                                       ("EnqueuedURIMetaData", d),
                                       ("DesiredFirstTrackNumberEnqueued", position), ("EnqueueAsNext", "0")])
        soap(ip, "SetAVTransportURI", [("InstanceID", "0"), ("CurrentURI", f"x-rincon-queue:{rincon}#0"),
                                       ("CurrentURIMetaData", "")])
        soap(ip, "Seek", [("InstanceID", "0"), ("Unit", "TRACK_NR"), ("Target", "1")])
        soap(ip, "Play", [("InstanceID", "0"), ("Speed", "1")])
        if mode == "remove":
            time.sleep(2)
            cd = soap(ip, "Browse", [("ObjectID", "Q:0"), ("BrowseFlag", "BrowseDirectChildren"), ("Filter", "*"),
                                     ("StartingIndex", "0"), ("RequestedCount", "1"), ("SortCriteria", "")],
                      service=CD, path="/MediaServer/ContentDirectory/Control")
            soap(ip, "Seek", [("InstanceID", "0"), ("Unit", "TRACK_NR"), ("Target", "2")])
            soap(ip, "Play", [("InstanceID", "0"), ("Speed", "1")])
            time.sleep(1)
            soap(ip, "RemoveTrackRangeFromQueue", [("InstanceID", "0"), ("UpdateID", cd.get("UpdateID", "0")),
                                                   ("StartingIndex", "1"), ("NumberOfTracks", "1")])
            print("removed the extra first item with RemoveTrackRangeFromQueue")
        print(f"{len(didls)} replayed items playing; look at the Sonos app queue now (30 s), "
              "close and reopen the queue view once.")
        time.sleep(30)
    finally:
        soap(ip, "Stop", [("InstanceID", "0")])
        soap(ip, "RemoveAllTracksFromQueue", [("InstanceID", "0")])
        server.shutdown()
        print("stopped, queue emptied; start from LMS again")


if __name__ == "__main__":
    main()
