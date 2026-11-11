# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Generated ALAC fixtures with complete LMS-readable metadata and trim tables."""
import subprocess
import os
import decoders_test


def encode(source, target, index, trim=False, bits=16):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(source), '-c:a', 'alac',
                    '-sample_fmt', 's16p' if bits == 16 else 's32p', str(target)], check=True)
    cookie, packets = decoders_test.alac_parts(target.read_bytes())
    rate = int.from_bytes(cookie[20:24], 'big')
    if os.environ.get('YENEY_NAV_FAULT') == 'mismatch':
        import numpy as np
        samples = np.frombuffer(source.read_bytes(), dtype='<i2', offset=44).reshape(-1, 2)
        packets = []
        for at in range(0, len(samples), 4096):
            block = samples[at:at+4096]
            payload = block.astype('>i2').tobytes()
            header = int('001'+'0000'+'0'*12+'1001'+f'{len(block):032b}', 2)
            bits = 55 + len(payload)*8 + 3
            padding = (-bits) % 8
            packed = ((header << (len(payload)*8+3)) | (int.from_bytes(payload, 'big') << 3) | 7) << padding
            packets.append(packed.to_bytes((bits+padding)//8, 'big'))
    frames = 61 * rate
    box = decoders_test.box
    def tagged(tag, data, **kwargs):
        if tag == 'meta':
            data = data[:4] + box('hdlr', bytes(8) + b'mdir' + b'appl' + bytes(9)) + data[4:]
        if tag == 'moov':
            entries = b''.join((len(value.encode())+24).to_bytes(4, 'big') + name.encode('latin1') + box('data', bytes(3)+b'\1'+bytes(4)+value.encode())
                               for name, value in (('\xa9nam', f'Title & {index}'),
                                                   ('\xa9ART', 'Artist <one>'), ('\xa9alb', 'Album & fixture')))
            data += box('udta', box('meta', bytes(4) + box('hdlr', bytes(8) + b'mdir' + b'appl' + bytes(9)) + box('ilst', entries)))
        return box(tag, data, **kwargs)
    decoders_test.box = tagged
    try:
        target.write_bytes(decoders_test.mp4(cookie, packets, frames, complete=True,
            trim=(113, frames-332) if trim else None,
            smpb=(113, 219, frames-332) if trim else None))
    finally:
        decoders_test.box = box
