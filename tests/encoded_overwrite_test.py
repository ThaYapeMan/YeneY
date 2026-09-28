"""An overflow warning is emitted once per stream, including across reconnects."""
from pathlib import Path
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix='sonos-overwrite-') as directory:
    source, exe = Path(directory, 'test.cpp'), Path(directory, 'test')
    source.write_text('''#include "sbencoder.h"
#include <cassert>
extern "C" unsigned get_squeezebox_stream_id() { return 1; }
extern "C" int sonos_lms_is_paused() { return 0; }
extern "C" uint64_t get_sb_time_ms() { return 0; }
namespace bridge {
struct EncoderTestAccess {
    static void fill(SBEncoder& e) {
        for (int i = 0; i < 1024; ++i) assert(e.acceptEncodedBytes("x", 1) == 1);
    }
};
}
int main() {
    for (unsigned id : {1u, 1u, 2u, 1u, 2u}) {
        bridge::SBEncoder encoder(id);
        bridge::EncoderTestAccess::fill(encoder);
    }
}''')
    subprocess.run(['g++', '-Wall', '-Wextra', '-DSBENCODER_TEST', '-I.', str(source),
                    'sbencoder.cpp', 'upnp/encoded_buffer.cpp', '-lFLAC++', '-lFLAC',
                    '-lpthread', '-o', str(exe)], check=True)
    output = subprocess.check_output([str(exe)], text=True)
    assert output == 'encoded buffer: overwrote oldest packet, capacity 256 packets\n' * 2, output
    print('PASS: first overwrite logs once per stream across repeated encoders; capacity remains 256 packets')
