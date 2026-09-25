// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Round-trips files through tools/pack_payload.py and Setup's decoder, and
// checks that a damaged payload is refused rather than half-installed.
//
//   unpack_test <packed> <original> [<packed> <original> ...]

#include "../installer/Unpack.h"

#include <cstdio>
#include <vector>

static std::vector<uint8_t> ReadAll(const char* path)
{
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(path, "rb"))
    {
        uint8_t chunk[65536];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
        {
            data.insert(data.end(), chunk, chunk + n);
        }
        std::fclose(f);
    }
    return data;
}

int main(int argc, char** argv)
{
    int failures = 0;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        std::vector<uint8_t> packed = ReadAll(argv[i]);
        const std::vector<uint8_t> original = ReadAll(argv[i + 1]);
        const size_t size = Setup::PayloadSize(packed.data(), packed.size());
        std::vector<uint8_t> out(size ? size : 1);
        const bool ok = size == original.size() && Setup::Unpack(packed.data(), packed.size(), out.data())
            && std::equal(original.begin(), original.end(), out.begin());
        std::printf("  %s  %s (%zu -> %zu bytes)\n", ok ? "PASS" : "FAIL", argv[i + 1], packed.size(), original.size());
        failures += ok ? 0 : 1;

        // Damage: a flipped byte in the stream, and a truncation. Either must be
        // refused; neither may crash.
        if (packed.size() > 64)
        {
            std::vector<uint8_t> flipped = packed;
            flipped[flipped.size() / 2] ^= 0x40;
            const bool refused = !Setup::Unpack(flipped.data(), flipped.size(), out.data());
            std::printf("  %s  flipped byte refused\n", refused ? "PASS" : "FAIL");
            failures += refused ? 0 : 1;

            const bool truncated = !Setup::Unpack(packed.data(), packed.size() - packed.size() / 3, out.data());
            std::printf("  %s  truncated stream refused\n", truncated ? "PASS" : "FAIL");
            failures += truncated ? 0 : 1;
        }
    }
    return failures == 0 ? 0 : 1;
}
