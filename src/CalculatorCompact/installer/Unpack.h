// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// Unpacks the calculator Setup carries.
//
// Stored compressed, it takes Setup from 449KB to under 225KB. The format is
// the one tools/pack_payload.py writes: a 16-byte header, then raw LZMA of the
// executable after the x86 branch filter -- the pair xz uses for executables,
// taken from Python's standard library at build time so nothing extra has to
// be installed to build.
//
//   offset 0  "CZ1\0"
//   offset 4  LZMA properties byte, (pb * 5 + lp) * 9 + lc
//   offset 5  three reserved bytes, zero
//   offset 8  unpacked size, little-endian
//   offset 12 CRC-32 of the unpacked executable, little-endian
//   offset 16 raw LZMA1 stream

#pragma once

#include <cstddef>
#include <cstdint>

namespace Setup
{
    // Returns the unpacked size, or 0 if the data is not a payload this can
    // read. `out` may be null to ask for the size alone.
    size_t PayloadSize(const uint8_t* packed, size_t packedSize);

    // Unpacks into `out`, which must hold PayloadSize() bytes. False on any
    // corruption; nothing it writes is trusted until this returns true.
    bool Unpack(const uint8_t* packed, size_t packedSize, uint8_t* out);
}
