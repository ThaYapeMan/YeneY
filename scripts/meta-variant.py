#!/usr/bin/env python3
# Device-test aid: re-send the active stream to inspect Sonos app metadata.
"""Try metadata variants on the stream a Sonos is currently playing.

Reads the current stream URI and metadata from the speaker, changes the DIDL
according to one variant, and sends SetAVTransportURI + Play with the SAME
stream URL. The bridge serves that request like any reconnect; LMS is not
touched. Look at the Sonos app after each run.

Usage (on the bridge host, while the room plays from LMS):
  python3 meta-variant.py <speaker-ip> <variant>

Variants:
  show      print the current metadata only, change nothing
  original  resend the current metadata unchanged (control run)
  title     title becomes "Artist - Title"
  show-md   artist in r:radioShowMd (the radio "show name" line)
  track     upnp:class object.item.audioItem.musicTrack
  broadcast upnp:class object.item.audioItem.audioBroadcast
  album     artist in upnp:album (album line), album text dropped
"""
import html
import re
import sys
import urllib.request
import xml.etree.ElementTree as ET

AVT = "urn:schemas-upnp-org:service:AVTransport:1"


def soap(ip, action, args):
    body = "".join(f"<{k}>{html.escape(v, quote=False)}</{k}>" for k, v in args)
    envelope = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
                's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
                f'<u:{action} xmlns:u="{AVT}">{body}</u:{action}></s:Body></s:Envelope>')
    req = urllib.request.Request(
        f"http://{ip}:1400/MediaRenderer/AVTransport/Control", envelope.encode(),
        {"Content-Type": 'text/xml; charset="utf-8"', "SOAPACTION": f'"{AVT}#{action}"'})
    with urllib.request.urlopen(req, timeout=20) as r:
        root = ET.fromstring(r.read())
    out = {}
    for el in root.iter():
        if el.tag.split("}")[-1] and el.text is not None and len(el) == 0:
            out[el.tag.split("}")[-1]] = el.text
    return out


def tag(didl, name):
    m = re.search(rf"<{name}>(.*?)</{name}>", didl, re.S)
    return html.unescape(m.group(1)) if m else ""


def set_tag(didl, name, value, after="</dc:title>"):
    value = html.escape(value, quote=False)
    if re.search(rf"<{name}>.*?</{name}>", didl, re.S):
        return re.sub(rf"<{name}>.*?</{name}>", lambda _: f"<{name}>{value}</{name}>", didl, count=1, flags=re.S)
    if re.search(rf"<{name}\s*/>", didl):
        return re.sub(rf"<{name}\s*/>", lambda _: f"<{name}>{value}</{name}>", didl, count=1)
    return didl.replace(after, after + f"<{name}>{value}</{name}>", 1)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ip, variant = sys.argv[1], sys.argv[2]
    info = soap(ip, "GetMediaInfo", [("InstanceID", "0")])
    uri, didl = info.get("CurrentURI", ""), info.get("CurrentURIMetaData", "")
    if "squeezebox.flac" not in uri:
        sys.exit(f"Not playing the bridge stream (CurrentURI={uri!r}). Ungroup the room and start LMS first.")
    title, artist, album = tag(didl, "dc:title"), tag(didl, "dc:creator"), tag(didl, "upnp:album")
    print(f"current: title={title!r} artist={artist!r} album={album!r}")
    print(f"         class={tag(didl, 'upnp:class')!r} streamContent={tag(didl, 'r:streamContent')!r}")
    if variant == "show":
        return
    if not artist and variant not in ("original", "track", "broadcast"):
        sys.exit("No dc:creator in the current metadata; nothing to move around.")
    if variant == "title":
        didl = set_tag(didl, "dc:title", f"{artist} - {title}")
    elif variant == "show-md":
        didl = set_tag(didl, "r:radioShowMd", artist)
    elif variant == "track":
        didl = set_tag(didl, "upnp:class", "object.item.audioItem.musicTrack")
    elif variant == "broadcast":
        didl = set_tag(didl, "upnp:class", "object.item.audioItem.audioBroadcast")
    elif variant == "album":
        didl = set_tag(didl, "upnp:album", artist)
    elif variant != "original":
        sys.exit(f"Unknown variant {variant!r}.\n{__doc__}")
    soap(ip, "SetAVTransportURI", [("InstanceID", "0"), ("CurrentURI", uri), ("CurrentURIMetaData", didl)])
    soap(ip, "Play", [("InstanceID", "0"), ("Speed", "1")])
    print(f"sent variant {variant!r}; look at the Sonos app now (give it ~5 s)")


if __name__ == "__main__":
    main()
