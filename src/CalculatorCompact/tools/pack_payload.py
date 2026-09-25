#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
"""Packs Calculator.exe for Setup to carry: installer/Unpack.h has the format.

    pack_payload.py <input.exe> <output>

The x86 branch filter rewrites call and jump targets from relative to absolute
before LZMA sees them; the same call target then looks the same everywhere it
is called from, which is worth about 10% on this executable. Python's lzma
module is liblzma, so this is exactly what `xz --x86 --lzma1=preset=9e` does,
minus the .xz container that Setup has no use for.
"""

import lzma
import struct
import sys
import zlib


def pack(data: bytes) -> bytes:
    # xz's own settings for -9e. A sweep of every valid lc/lp/pb and match
    # finder found nothing that beat them by more than noise.
    lc, lp, pb = 3, 0, 2
    filters = [
        {"id": lzma.FILTER_X86},
        {"id": lzma.FILTER_LZMA1, "preset": 9 | lzma.PRESET_EXTREME},
    ]
    stream = lzma.compress(data, format=lzma.FORMAT_RAW, filters=filters)
    properties = (pb * 5 + lp) * 9 + lc
    header = b"CZ1\0" + struct.pack("<B3xII", properties, len(data), zlib.crc32(data) & 0xFFFFFFFF)
    return header + stream


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as source:
        data = source.read()
    packed = pack(data)
    with open(sys.argv[2], "wb") as target:
        target.write(packed)
    print(f"    {len(data)} -> {len(packed)} bytes ({100 * len(packed) / len(data):.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
