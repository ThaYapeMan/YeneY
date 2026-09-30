#pragma once
#include "third_party/yeney-core/core/decoder.h"
std::unique_ptr<yeney::Decoder> makeCompatibilityDecoder(const yeney::DecoderConfig&);
