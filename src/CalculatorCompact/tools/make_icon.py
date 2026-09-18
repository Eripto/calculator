#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
"""Generates res/calculator.ico.

Draws the app icon at 4x and box-filters it down, so the small sizes stay clean
without needing an image library at build time. Sizes above 32px are stored as
PNG so the whole icon costs a few kilobytes rather than ~150KB.
"""

import os
import struct
import zlib

SS = 4  # supersampling factor

ACCENT = (0x00, 0x67, 0xC0)
ACCENT_DARK = (0x00, 0x4C, 0x94)
KEY = (0xFF, 0xFF, 0xFF)
SCREEN = (0xE8, 0xF2, 0xFC)
EQUALS = (0xFF, 0xB9, 0x00)


def blend(dst, src, alpha):
    return tuple(int(round(d + (s - d) * alpha)) for d, s in zip(dst, src))


class Canvas:
    def __init__(self, size):
        self.n = size
        self.px = [[(0, 0, 0)] * size for _ in range(size)]
        self.a = [[0.0] * size for _ in range(size)]

    def fill_round_rect(self, x0, y0, x1, y1, radius, color):
        for y in range(max(0, int(y0)), min(self.n, int(y1) + 1)):
            for x in range(max(0, int(x0)), min(self.n, int(x1) + 1)):
                # Distance into the corner circle, if any.
                cx = x0 + radius if x < x0 + radius else (x1 - radius if x > x1 - radius else x)
                cy = y0 + radius if y < y0 + radius else (y1 - radius if y > y1 - radius else y)
                dx, dy = x - cx, y - cy
                if dx * dx + dy * dy <= radius * radius:
                    self.px[y][x] = color
                    self.a[y][x] = 1.0

    def downsample(self, target):
        step = self.n // target
        out = []
        for y in range(target):
            row = []
            for x in range(target):
                r = g = b = a = 0.0
                for sy in range(y * step, (y + 1) * step):
                    for sx in range(x * step, (x + 1) * step):
                        alpha = self.a[sy][sx]
                        c = self.px[sy][sx]
                        r += c[0] * alpha
                        g += c[1] * alpha
                        b += c[2] * alpha
                        a += alpha
                count = step * step
                if a > 0:
                    row.append((int(r / a), int(g / a), int(b / a), int(round(255 * a / count))))
                else:
                    row.append((0, 0, 0, 0))
            out.append(row)
        return out


def render(size):
    n = size * SS
    c = Canvas(n)
    u = n / 32.0  # design grid is 32 units

    # Body.
    c.fill_round_rect(3 * u, 1.5 * u, 29 * u, 30.5 * u, 3.2 * u, ACCENT)
    # Screen.
    c.fill_round_rect(6 * u, 4.5 * u, 26 * u, 11 * u, 1.0 * u, SCREEN)

    # Keys: 4 columns x 3 rows.
    left, top = 6 * u, 13.5 * u
    kw, kh = 3.6 * u, 3.6 * u
    gapx, gapy = 1.2 * u, 1.2 * u
    for row in range(3):
        for col in range(4):
            x0 = left + col * (kw + gapx)
            y0 = top + row * (kh + gapy)
            color = EQUALS if (row == 2 and col == 3) else KEY
            c.fill_round_rect(x0, y0, x0 + kw, y0 + kh, 0.8 * u, color)

    return c.downsample(size)


def png_bytes(pixels):
    size = len(pixels)
    raw = b"".join(
        b"\x00" + b"".join(struct.pack("BBBB", *px) for px in row) for row in pixels
    )

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def bmp_bytes(pixels):
    size = len(pixels)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    body = bytearray()
    for row in reversed(pixels):  # BMP rows are bottom-up
        for r, g, b, a in row:
            body += struct.pack("BBBB", b, g, r, a)
    # 1bpp AND mask, rows padded to 4 bytes; fully transparent where alpha is 0.
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for row in reversed(pixels):
        bits = bytearray(stride)
        for x, (_, _, _, a) in enumerate(row):
            if a == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + bytes(body) + bytes(mask)


def main():
    # 16px stays uncompressed for the widest shell compatibility; everything
    # above is PNG-compressed (supported since Vista), which keeps the whole
    # icon at a few KB instead of ~150KB of raw bitmaps. 64 and 128 are omitted:
    # Windows scales the 256px entry down for those, and the icon is flat enough
    # that the result is indistinguishable.
    sizes = [16, 24, 32, 48, 256]
    entries = []
    for size in sizes:
        pixels = render(size)
        entries.append((size, bmp_bytes(pixels) if size <= 16 else png_bytes(pixels)))

    out = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for size, data in entries:
        out += struct.pack(
            "<BBBBHHII", size if size < 256 else 0, size if size < 256 else 0, 0, 0, 1, 32, len(data), offset
        )
        offset += len(data)
    for _, data in entries:
        out += data

    path = os.path.join(os.path.dirname(__file__), "..", "res", "calculator.ico")
    with open(os.path.normpath(path), "wb") as handle:
        handle.write(out)
    print("wrote %s (%d bytes, %d sizes)" % (os.path.normpath(path), len(out), len(entries)))


if __name__ == "__main__":
    main()
