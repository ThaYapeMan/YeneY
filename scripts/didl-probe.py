#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Owner-run probe 7: which queue-item metadata shape does the Sonos app display?

  python3 didl-probe.py <speaker-ip> show
      Read-only: print the metadata the speaker holds for the current track
      (what YeneY sent). Changes nothing.
  python3 didl-probe.py <speaker-ip> a|b|c|d
      Replace the room's Sonos queue with one 30 s quiet tone item (441 Hz, -30 dBFS)
      carrying a metadata variant, play it, then stop and empty the queue:
        a  title + artist, NO album element            (YeneY 27ded82 default)
        b  title + artist + EMPTY album element         (candidate fix)
        c  title + EMPTY artist + EMPTY album           (PODCAST-02 shape, displayed before)
        d  title + artist + album "BNR Nieuwsradio"     (album setting on)
      Look at the Sonos app while it plays: what does it show as title / artist / album?
YeneY sees an external takeover and stops LMS for that room; start from LMS again after.
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
RATE, BLOCK, SECONDS = 44100, 4096, 30
TITLE, ARTIST, ALBUM = "Media smullen van Trump versus Obama", "Amerika in 15 minuten", "BNR Nieuwsradio"


def wav():
    one = b"".join(struct.pack("<hh", v, v) for v in
                   (int(1000 * math.sin(2 * math.pi * 41 * k / BLOCK)) for k in range(BLOCK)))
    frames = round(SECONDS * RATE / BLOCK)
    data = one * frames
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16) + b"data" + struct.pack("<I", len(data)) + data)


def soap(ip, action, args, service=AVT, path="/MediaRenderer/AVTransport/Control"):
    body = "".join(f"<{k}>{html.escape(v, quote=False)}</{k}>" for k, v in args)
    envelope = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
                's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
                f'<u:{action} xmlns:u="{service}">{body}</u:{action}></s:Body></s:Envelope>')
    req = urllib.request.Request(f"http://{ip}:1400{path}", envelope.encode(),
                                 {"Content-Type": 'text/xml; charset="utf-8"', "SOAPACTION": f'"{service}#{action}"'})
    with urllib.request.urlopen(req, timeout=30) as r:
        root = ET.fromstring(r.read())
    return {el.tag.split("}")[-1]: el.text or "" for el in root.iter() if len(el) == 0}


def browse_queue(ip):
    cd = "urn:schemas-upnp-org:service:ContentDirectory:1"
    r = soap(ip, "Browse", [("ObjectID", "Q:0"), ("BrowseFlag", "BrowseDirectChildren"), ("Filter", "*"),
                            ("StartingIndex", "0"), ("RequestedCount", "5"), ("SortCriteria", "")],
             service=cd, path="/MediaServer/ContentDirectory/Control")
    return re.sub(r"session=[^&]+", "session=...", r.get("Result", ""))


def didl(variant, url, length):
    artist = "" if variant == "c" else ARTIST
    fields = f"<dc:title>{html.escape(TITLE)}</dc:title>"
    if variant in "abd":
        fields += f"<dc:creator>{html.escape(artist)}</dc:creator><upnp:artist>{html.escape(artist)}</upnp:artist>"
    else:
        fields += "<dc:creator></dc:creator><upnp:artist></upnp:artist>"
    if variant in "bc":
        fields += "<upnp:album></upnp:album>"
    elif variant == "d":
        fields += f"<upnp:album>{html.escape(ALBUM)}</upnp:album>"
    return ('<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
            'xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">'
            f'<item id="didl-probe-{variant}" parentID="0" restricted="true">{fields}'
            '<upnp:class>object.item.audioItem.musicTrack</upnp:class>'
            f'<res protocolInfo="http-get:*:audio/wav:*" duration="0:00:30" size="{length}">{html.escape(url)}</res>'
            '</item></DIDL-Lite>')


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("show", "a", "b", "c", "d"):
        sys.exit(__doc__)
    ip, variant = sys.argv[1], sys.argv[2]
    if variant == "show":
        pos = soap(ip, "GetPositionInfo", [("InstanceID", "0")])
        print("TrackURI:", re.sub(r"session=[^&]+", "session=...", pos.get("TrackURI", "")))
        print("TrackMetaData:")
        print(pos.get("TrackMetaData", ""))
        print("Sonos queue (Browse Q:0), what the app uses:")
        print(browse_queue(ip))
        return
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
    url = f"http://{local}:{server.server_address[1]}/probe-{variant}.wav"
    meta = didl(variant, url, len(body))
    print("sending metadata:", meta)
    try:
        soap(ip, "RemoveAllTracksFromQueue", [("InstanceID", "0")])
        soap(ip, "AddURIToQueue", [("InstanceID", "0"), ("EnqueuedURI", url), ("EnqueuedURIMetaData", meta),
                                   ("DesiredFirstTrackNumberEnqueued", "0"), ("EnqueueAsNext", "0")])
        soap(ip, "SetAVTransportURI", [("InstanceID", "0"), ("CurrentURI", f"x-rincon-queue:{rincon}#0"),
                                       ("CurrentURIMetaData", "")])
        soap(ip, "Seek", [("InstanceID", "0"), ("Unit", "TRACK_NR"), ("Target", "1")])
        soap(ip, "Play", [("InstanceID", "0"), ("Speed", "1")])
        print(f"variant {variant} playing for 25 s; look at the Sonos app now (title / artist / album)")
        time.sleep(4)
        print("speaker holds:", soap(ip, "GetPositionInfo", [("InstanceID", "0")]).get("TrackMetaData", ""))
        print("queue holds:", browse_queue(ip))
        time.sleep(21)
    finally:
        soap(ip, "Stop", [("InstanceID", "0")])
        soap(ip, "RemoveAllTracksFromQueue", [("InstanceID", "0")])
        server.shutdown()
        print("stopped, queue emptied; start from LMS again")


if __name__ == "__main__":
    main()
