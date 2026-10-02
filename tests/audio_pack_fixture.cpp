#include "pcm_pack.h"
extern "C" void pack_audio_test(void* destination, int32_t* source, unsigned frames, unsigned bits) {
    auto* out = static_cast<char*>(destination);
    const unsigned bytes = bits / 8;
    for (unsigned i = 0; i < frames * 2; ++i) packSonosSample(out + i * bytes, source[i], bytes);
}
