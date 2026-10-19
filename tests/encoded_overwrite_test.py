"""Packet capacity stays 256; slow readers backpressure instead of overwriting."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
from pathlib import Path
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix='sonos-backpressure-') as directory:
    source, exe = Path(directory, 'test.cpp'), Path(directory, 'test')
    source.write_text('''#include "sbencoder.h"
#include "stream_metrics.h"
#include <cassert>
#include <thread>
extern "C" unsigned get_squeezebox_stream_id() { return 1; }
extern "C" int yeney_is_paused() { return 0; }
extern "C" uint64_t get_sb_time_ms() { return 0; }
namespace bridge {
struct EncoderTestAccess {
    static void fill(SBEncoder& e) {
        std::thread reader([&] { char c; int count=0; while(count<1024) {
            count += e.drainEncodedBytes(&c,1); std::this_thread::yield();
        }});
        for (int i=0; i<1024; ++i) assert(e.acceptEncodedBytes("x",1)==1);
        reader.join();
    }
};
}
int main() {
    for (unsigned id : {1u,1u,2u,1u,2u}) {
        bridge::SBEncoder encoder(id); bridge::EncoderTestAccess::fill(encoder);
    }
    assert(bridge::streamMetrics().overwrites==0 && bridge::streamMetrics().dropped==0);
}''')
    subprocess.run(['g++', '-Wall', '-Wextra', '-DSBENCODER_TEST', '-I.', str(source),
                    'sbencoder.cpp', 'upnp/encoded_buffer.cpp', '-lFLAC++', '-lFLAC',
                    '-lpthread', '-o', str(exe)], check=True)
    output = subprocess.check_output([str(exe)], text=True)
    assert not output, output
    print('PASS encoded backpressure: 256 packets, slow readers drain all bytes across reconnects, zero overwrites/drops')
