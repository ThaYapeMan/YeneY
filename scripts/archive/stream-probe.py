#!/usr/bin/env python3
"""stream-probe.py -- test how a Sonos shows now-playing metadata, without the bridge.

Serves one audio source to a Sonos from a tiny HTTP server on this host, points
the speaker at it with a chosen DIDL/URI variant, and changes the ICY
StreamTitle every TITLE_SECS seconds. Every stream request's headers are printed,
so you can see whether the Sonos asks for ICY (Icy-MetaData: 1).

Stop the room's bridge first (it would fight over the speaker):
  systemctl stop 'yeney@Study'
and start it again afterwards:
  systemctl start 'yeney@Study'

Usage:
  python3 stream-probe.py <speaker-ip> <variant> [source-url]

The source URL is fetched as supplied, including /music/yeney.flac and the
pre-upgrade /music/squeezebox.flac path with their session and stream queries.

Variants (source defaults to an LMS FLAC track; MP3 variants need an MP3 URL):
  flac-radio        like the bridge: x-rincon-mp3radio, audio/flac; ICY only if asked
  flac-radio-mpeg   x-rincon-mp3radio announced as audio/mpeg, body is FLAC; ICY if asked
  flac-radio-force  like flac-radio, but ICY headers and blocks even if not asked
  flac-radio-noicy  like flac-radio, radio-server style answer, but ICY requests are
                    ignored (no icy-metaint, no blocks): can the radio client play FLAC?
  mp3-radio         x-rincon-mp3radio, audio/mpeg, MP3 source; ICY if asked (control)
  flac-track        plain http:// URI, http-get protocolInfo, musicTrack with
                    artist/album (the "track instead of radio" route)

Every 5 s it prints what the speaker itself stores (title, artist, album,
streamContent, duration, position). Ctrl+C stops the speaker and the server.
"""
import html
import re
import xml.etree.ElementTree as ET
import socket
import socketserver
import sys
import threading
import time
import urllib.request

PORT = 18080
METAINT = 16000
TITLE_SECS = 15
RATE = 180_000            # bytes/s throttle for file sources (~1.4 Mbit/s)
DEFAULT_SOURCE = "http://192.168.178.23:9000/music/44436/download"
ARTIST, TITLE, ALBUM = "Trijntje Oosterhuis", "Probe Song", "Probe Album"
AVT = "urn:schemas-upnp-org:service:AVTransport:1"


def now():
    return time.strftime("%H:%M:%S")


def current_title():
    n = int(time.time() // TITLE_SECS) % 4 + 1
    return f"{ARTIST} - {TITLE} {n}"


def icy_block(text):
    if text is None:
        return b"\x00"
    payload = f"StreamTitle='{text.replace(chr(39), '')}';".encode("utf-8")
    length = (len(payload) + 15) // 16
    return bytes([length]) + payload.ljust(length * 16, b"\x00")


class Handler(socketserver.StreamRequestHandler):
    variant = ""
    source = ""
    requests = 0
    lock = threading.Lock()

    def handle(self):
        with Handler.lock:
            Handler.requests += 1
            number = Handler.requests
        line = self.rfile.readline(4096).decode("latin-1").strip()
        headers = {}
        while True:
            h = self.rfile.readline(4096).decode("latin-1").strip()
            if not h:
                break
            k, _, v = h.partition(":")
            headers[k.strip().lower()] = v.strip()
        asked = headers.get("icy-metadata") == "1"
        shown = "; ".join(f"{k}={v[:60]}" for k, v in headers.items())
        print(f"[{now()}] request #{number}: {line} | icy-asked={'YES' if asked else 'no'} | {shown}", flush=True)
        mp3 = self.variant == "mp3-radio"
        use_icy = (asked and self.variant != "flac-radio-noicy") or self.variant == "flac-radio-force"
        ctype = "audio/mpeg" if mp3 else "audio/flac"
        head = f"HTTP/1.0 200 OK\r\nContent-Type: {ctype}\r\nConnection: close\r\n"
        if use_icy:
            head += f"icy-metaint: {METAINT}\r\nicy-name: YeneY probe\r\n"
        self.wfile.write((head + "\r\n").encode())
        if line.startswith("HEAD"):
            return
        try:
            src = urllib.request.urlopen(self.source, timeout=10)
        except OSError as e:
            print(f"[{now()}] request #{number}: source failed: {e}", flush=True)
            return
        throttle = not mp3
        sent_title, until_meta, start, total = None, METAINT, time.monotonic(), 0
        try:
            while True:
                chunk = src.read(4096)
                if not chunk:
                    print(f"[{now()}] request #{number}: source ended", flush=True)
                    return
                while chunk:
                    part = chunk[:until_meta] if use_icy else chunk
                    chunk = chunk[len(part):]
                    self.wfile.write(part)
                    total += len(part)
                    if use_icy:
                        until_meta -= len(part)
                        if until_meta == 0:
                            title = current_title()
                            self.wfile.write(icy_block(title if title != sent_title else None))
                            if title != sent_title:
                                print(f"[{now()}] request #{number}: StreamTitle -> {title}", flush=True)
                                sent_title = title
                            until_meta = METAINT
                if throttle:
                    ahead = total / RATE - (time.monotonic() - start)
                    if ahead > 0:
                        time.sleep(ahead)
        except (BrokenPipeError, ConnectionResetError):
            print(f"[{now()}] request #{number}: closed by speaker after {total} bytes", flush=True)


def soap_fields(ip, action, args):
    body = "".join(f"<{k}>{html.escape(v, quote=False)}</{k}>" for k, v in args)
    env = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
           f'<u:{action} xmlns:u="{AVT}">{body}</u:{action}></s:Body></s:Envelope>')
    req = urllib.request.Request(f"http://{ip}:1400/MediaRenderer/AVTransport/Control", env.encode(),
                                 {"Content-Type": 'text/xml; charset="utf-8"', "SOAPACTION": f'"{AVT}#{action}"'})
    with urllib.request.urlopen(req, timeout=5) as r:
        root = ET.fromstring(r.read())
    return {el.tag.split("}")[-1]: (el.text or "") for el in root.iter() if len(el) == 0}


def watch(ip):
    """Print what the speaker itself stores for the current track, every 5 s."""
    last = None
    while True:
        try:
            pos = soap_fields(ip, "GetPositionInfo", [("InstanceID", "0")])
            meta = pos.get("TrackMetaData", "")
            get = lambda t: html.unescape(m.group(1)) if (m := re.search(rf"<{t}>(.*?)</{t}>", meta, re.S)) else "-"
            line = (f"speaker stores: title={get('dc:title')!r} artist={get('dc:creator')!r} "
                    f"album={get('upnp:album')!r} streamContent={get('r:streamContent')!r} "
                    f"duration={pos.get('TrackDuration', '-')} position={pos.get('RelTime', '-')}")
        except Exception as e:
            line = f"speaker stores: (no answer: {e})"
        if line != last:
            print(f"[{now()}] {line}", flush=True)
            last = line
        time.sleep(5)


def soap(ip, action, args):
    body = "".join(f"<{k}>{html.escape(v, quote=False)}</{k}>" for k, v in args)
    env = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
           f'<u:{action} xmlns:u="{AVT}">{body}</u:{action}></s:Body></s:Envelope>')
    req = urllib.request.Request(f"http://{ip}:1400/MediaRenderer/AVTransport/Control", env.encode(),
                                 {"Content-Type": 'text/xml; charset="utf-8"', "SOAPACTION": f'"{AVT}#{action}"'})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.status


def didl(variant, url):
    e = lambda s: html.escape(s, quote=False)
    if variant == "flac-track":
        cls, proto, uri = "object.item.audioItem.musicTrack", "http-get:*:audio/flac:*", url
    else:
        mime = "audio/flac" if variant in ("flac-radio", "flac-radio-force", "flac-radio-noicy") else "audio/mpeg"
        cls, proto = "object.item.audioItem", f"x-rincon-mp3radio:*:{mime}:*"
        uri = "x-rincon-mp3radio" + url[url.index(":"):]
    meta = ('<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
            'xmlns:r="urn:schemas-rinconnetworks-com:metadata-1-0/" xmlns:dc="http://purl.org/dc/elements/1.1/" '
            'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/"><item id="probe" parentID="-1" restricted="true">'
            f"<upnp:class>{cls}</upnp:class><dc:title>{e(TITLE)}</dc:title><dc:creator>{e(ARTIST)}</dc:creator>"
            f"<upnp:album>{e(ALBUM)}</upnp:album><r:streamContent></r:streamContent>"
            f'<res protocolInfo="{proto}">{e(uri)}</res></item></DIDL-Lite>')
    return uri, meta


def local_ip(speaker):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((speaker, 1400))
    ip = s.getsockname()[0]
    s.close()
    return ip


def main():
    variants = ("flac-radio", "flac-radio-mpeg", "flac-radio-force", "flac-radio-noicy", "mp3-radio", "flac-track")
    if len(sys.argv) < 3 or sys.argv[2] not in variants:
        sys.exit(__doc__)
    speaker, variant = sys.argv[1], sys.argv[2]
    source = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_SOURCE
    if variant == "mp3-radio" and len(sys.argv) < 4:
        sys.exit("mp3-radio needs an MP3 source URL, e.g. an internet radio MP3 stream.")
    Handler.variant, Handler.source = variant, source
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    server = socketserver.ThreadingTCPServer(("0.0.0.0", PORT), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    ext = "mp3" if variant == "mp3-radio" else "flac"
    url = f"http://{local_ip(speaker)}:{PORT}/probe.{ext}"
    uri, meta = didl(variant, url)
    print(f"[{now()}] variant {variant}: {uri}")
    print(f"[{now()}] source {source}")
    soap(speaker, "SetAVTransportURI", [("InstanceID", "0"), ("CurrentURI", uri), ("CurrentURIMetaData", meta)])
    soap(speaker, "Play", [("InstanceID", "0"), ("Speed", "1")])
    print(f"[{now()}] playing; title changes every {TITLE_SECS} s when ICY is used. Watch the Sonos app. Ctrl+C to stop.")
    threading.Thread(target=watch, args=(speaker,), daemon=True).start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            soap(speaker, "Stop", [("InstanceID", "0"), ("Speed", "1")])
        except (OSError, KeyboardInterrupt):
            pass
        server.shutdown()
        print(f"\n[{now()}] stopped")


if __name__ == "__main__":
    main()
