// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// A buffer-to-buffer LZMA decoder, after the reference decoder in Igor
// Pavlov's LZMA SDK (LzmaSpec.cpp, public domain), followed by the x86 branch
// filter from xz. Everything is decoded straight into the output buffer, which
// doubles as the dictionary, so there is no window to allocate or manage.
//
// Tested against Python's lzma module, which produced the data: see
// tools/pack_payload.py, and tests/unpack_test.cpp for the round trip.

#include "Unpack.h"

#include <cstdlib>
#include <cstring>

namespace Setup
{
    namespace
    {
        constexpr unsigned kProbBits = 11;
        constexpr unsigned kProbInit = 1u << (kProbBits - 1);
        constexpr unsigned kMoveBits = 5;
        constexpr unsigned kPosBitsMax = 4;
        constexpr unsigned kStates = 12;
        constexpr unsigned kLenToPosStates = 4;
        constexpr unsigned kAlignBits = 4;
        constexpr unsigned kEndPosModelIndex = 14;
        constexpr unsigned kFullDistances = 1u << (kEndPosModelIndex >> 1);
        constexpr unsigned kMatchMinLen = 2;
        constexpr size_t kHeaderSize = 16;

        // Offsets into one flat array of probabilities, in the reference
        // decoder's order. One allocation, one initialisation loop.
        constexpr unsigned kIsMatch = 0;
        constexpr unsigned kIsRep = kIsMatch + (kStates << kPosBitsMax);
        constexpr unsigned kIsRepG0 = kIsRep + kStates;
        constexpr unsigned kIsRepG1 = kIsRepG0 + kStates;
        constexpr unsigned kIsRepG2 = kIsRepG1 + kStates;
        constexpr unsigned kIsRep0Long = kIsRepG2 + kStates;
        constexpr unsigned kPosSlot = kIsRep0Long + (kStates << kPosBitsMax);
        constexpr unsigned kPosDecoders = kPosSlot + (kLenToPosStates << 6);
        constexpr unsigned kAlign = kPosDecoders + 1 + kFullDistances - kEndPosModelIndex;
        // A length coder: two choice bits, then 3-bit low and mid trees per
        // position state, then one 8-bit high tree.
        constexpr unsigned kLenChoice = 0;
        constexpr unsigned kLenChoice2 = 1;
        constexpr unsigned kLenLow = 2;
        constexpr unsigned kLenMid = kLenLow + (1u << kPosBitsMax) * 8;
        constexpr unsigned kLenHigh = kLenMid + (1u << kPosBitsMax) * 8;
        constexpr unsigned kLenCoderSize = kLenHigh + 256;
        constexpr unsigned kLenCoder = kAlign + (1u << kAlignBits);
        constexpr unsigned kRepLenCoder = kLenCoder + kLenCoderSize;
        constexpr unsigned kLiteral = kRepLenCoder + kLenCoderSize;

        struct RangeDecoder
        {
            const uint8_t* in;
            const uint8_t* end;
            uint32_t range;
            uint32_t code;
            bool corrupted;

            uint8_t Next()
            {
                if (in == end)
                {
                    corrupted = true; // ran off the end of the input
                    return 0;
                }
                return *in++;
            }

            void Normalize()
            {
                if (range < (1u << 24))
                {
                    range <<= 8;
                    code = (code << 8) | Next();
                }
            }

            unsigned Bit(uint16_t* prob)
            {
                unsigned p = *prob;
                const uint32_t bound = (range >> kProbBits) * p;
                unsigned bit;
                if (code < bound)
                {
                    p += ((1u << kProbBits) - p) >> kMoveBits;
                    range = bound;
                    bit = 0;
                }
                else
                {
                    p -= p >> kMoveBits;
                    code -= bound;
                    range -= bound;
                    bit = 1;
                }
                *prob = static_cast<uint16_t>(p);
                Normalize();
                return bit;
            }

            uint32_t DirectBits(unsigned count)
            {
                uint32_t result = 0;
                do
                {
                    range >>= 1;
                    code -= range;
                    const uint32_t t = 0u - (code >> 31);
                    code += range & t;
                    if (code == range)
                    {
                        corrupted = true;
                    }
                    Normalize();
                    result = (result << 1) + (t + 1);
                } while (--count);
                return result;
            }

            unsigned Tree(uint16_t* probs, unsigned bits)
            {
                unsigned m = 1;
                for (unsigned i = 0; i < bits; ++i)
                {
                    m = (m << 1) + Bit(&probs[m]);
                }
                return m - (1u << bits);
            }

            unsigned ReverseTree(uint16_t* probs, unsigned bits)
            {
                unsigned m = 1;
                unsigned symbol = 0;
                for (unsigned i = 0; i < bits; ++i)
                {
                    const unsigned bit = Bit(&probs[m]);
                    m = (m << 1) + bit;
                    symbol |= bit << i;
                }
                return symbol;
            }

            unsigned Length(uint16_t* coder, unsigned posState)
            {
                if (!Bit(&coder[kLenChoice]))
                {
                    return Tree(&coder[kLenLow + posState * 8], 3);
                }
                if (!Bit(&coder[kLenChoice2]))
                {
                    return 8 + Tree(&coder[kLenMid + posState * 8], 3);
                }
                return 16 + Tree(&coder[kLenHigh], 8);
            }
        };

        bool DecodeLzma(unsigned properties, const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize)
        {
            if (properties >= 9 * 5 * 5)
            {
                return false;
            }
            const unsigned lc = properties % 9;
            const unsigned lp = (properties / 9) % 5;
            const unsigned pb = properties / 45;
            if (lc + lp > 12)
            {
                return false;
            }

            const size_t probCount = kLiteral + (0x300u << (lc + lp));
            uint16_t* probs = static_cast<uint16_t*>(std::malloc(probCount * sizeof(uint16_t)));
            if (probs == nullptr)
            {
                return false;
            }
            for (size_t i = 0; i < probCount; ++i)
            {
                probs[i] = kProbInit;
            }

            RangeDecoder rc{ in, in + inSize, 0xFFFFFFFFu, 0, false };
            bool ok = rc.Next() == 0;
            for (int i = 0; i < 4; ++i)
            {
                rc.code = (rc.code << 8) | rc.Next();
            }
            ok = ok && rc.code != rc.range;

            size_t pos = 0;
            unsigned state = 0;
            uint32_t rep0 = 0;
            uint32_t rep1 = 0;
            uint32_t rep2 = 0;
            uint32_t rep3 = 0;
            const unsigned pbMask = (1u << pb) - 1;
            const unsigned lpMask = (1u << lp) - 1;

            while (ok && pos < outSize && !rc.corrupted)
            {
                const unsigned posState = static_cast<unsigned>(pos) & pbMask;

                if (!rc.Bit(&probs[kIsMatch + (state << kPosBitsMax) + posState]))
                {
                    // Literal, coded against the byte at rep0 after a match.
                    const unsigned previous = pos > 0 ? out[pos - 1] : 0;
                    const unsigned litState = ((static_cast<unsigned>(pos) & lpMask) << lc) + (previous >> (8 - lc));
                    uint16_t* lit = &probs[kLiteral + 0x300u * litState];
                    unsigned symbol = 1;
                    if (state >= 7 && pos > rep0)
                    {
                        unsigned matchByte = out[pos - rep0 - 1];
                        do
                        {
                            const unsigned matchBit = (matchByte >> 7) & 1;
                            matchByte <<= 1;
                            const unsigned bit = rc.Bit(&lit[((1 + matchBit) << 8) + symbol]);
                            symbol = (symbol << 1) | bit;
                            if (matchBit != bit)
                            {
                                break;
                            }
                        } while (symbol < 0x100);
                    }
                    while (symbol < 0x100)
                    {
                        symbol = (symbol << 1) | rc.Bit(&lit[symbol]);
                    }
                    out[pos++] = static_cast<uint8_t>(symbol - 0x100);
                    state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
                    continue;
                }

                unsigned length;
                if (rc.Bit(&probs[kIsRep + state]))
                {
                    if (pos == 0)
                    {
                        ok = false;
                        break;
                    }
                    if (!rc.Bit(&probs[kIsRepG0 + state]))
                    {
                        if (!rc.Bit(&probs[kIsRep0Long + (state << kPosBitsMax) + posState]))
                        {
                            // Short rep: one byte from rep0.
                            if (rep0 >= pos)
                            {
                                ok = false;
                                break;
                            }
                            state = state < 7 ? 9 : 11;
                            out[pos] = out[pos - rep0 - 1];
                            ++pos;
                            continue;
                        }
                    }
                    else
                    {
                        uint32_t distance;
                        if (!rc.Bit(&probs[kIsRepG1 + state]))
                        {
                            distance = rep1;
                        }
                        else
                        {
                            if (!rc.Bit(&probs[kIsRepG2 + state]))
                            {
                                distance = rep2;
                            }
                            else
                            {
                                distance = rep3;
                                rep3 = rep2;
                            }
                            rep2 = rep1;
                        }
                        rep1 = rep0;
                        rep0 = distance;
                    }
                    length = rc.Length(&probs[kRepLenCoder], posState);
                    state = state < 7 ? 8 : 11;
                }
                else
                {
                    rep3 = rep2;
                    rep2 = rep1;
                    rep1 = rep0;
                    length = rc.Length(&probs[kLenCoder], posState);
                    state = state < 7 ? 7 : 10;

                    const unsigned lenState = length < kLenToPosStates - 1 ? length : kLenToPosStates - 1;
                    const unsigned posSlot = rc.Tree(&probs[kPosSlot + (lenState << 6)], 6);
                    if (posSlot < 4)
                    {
                        rep0 = posSlot;
                    }
                    else
                    {
                        const unsigned directBits = (posSlot >> 1) - 1;
                        rep0 = (2u | (posSlot & 1)) << directBits;
                        if (posSlot < kEndPosModelIndex)
                        {
                            rep0 += rc.ReverseTree(&probs[kPosDecoders + rep0 - posSlot], directBits);
                        }
                        else
                        {
                            rep0 += rc.DirectBits(directBits - kAlignBits) << kAlignBits;
                            rep0 += rc.ReverseTree(&probs[kAlign], kAlignBits);
                        }
                    }
                    if (rep0 == 0xFFFFFFFFu)
                    {
                        break; // end marker, before the expected size: short
                    }
                }

                length += kMatchMinLen;
                if (rep0 >= pos || length > outSize - pos)
                {
                    ok = false;
                    break;
                }
                // Byte by byte on purpose: a match may overlap its own output.
                const uint8_t* from = out + pos - rep0 - 1;
                for (unsigned i = 0; i < length; ++i)
                {
                    out[pos + i] = from[i];
                }
                pos += length;
            }

            std::free(probs);
            return ok && !rc.corrupted && pos == outSize;
        }

        // The reverse of xz's x86 BCJ filter: E8 (call) and E9 (jmp) operands
        // were stored as absolute addresses, which repeat far more than the
        // relative ones in the program and so compress better.
        void UnfilterX86(uint8_t* buffer, size_t size)
        {
            static const bool kAllowed[8] = { true, true, true, false, true, false, false, false };
            static const unsigned kBitNumber[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
            auto msByte = [](uint8_t b) { return b == 0 || b == 0xFF; };

            if (size < 5)
            {
                return;
            }
            uint32_t prevMask = 0;
            uint32_t prevPos = 0u - 5u;
            const size_t limit = size - 5;
            size_t i = 0;
            while (i <= limit)
            {
                uint8_t b = buffer[i];
                if (b != 0xE8 && b != 0xE9)
                {
                    ++i;
                    continue;
                }
                const uint32_t offset = static_cast<uint32_t>(i) - prevPos;
                prevPos = static_cast<uint32_t>(i);
                if (offset > 5)
                {
                    prevMask = 0;
                }
                else
                {
                    for (uint32_t k = 0; k < offset; ++k)
                    {
                        prevMask &= 0x77;
                        prevMask <<= 1;
                    }
                }

                b = buffer[i + 4];
                if (msByte(b) && kAllowed[(prevMask >> 1) & 0x7] && (prevMask >> 1) < 0x10)
                {
                    uint32_t src = (static_cast<uint32_t>(b) << 24) | (static_cast<uint32_t>(buffer[i + 3]) << 16)
                        | (static_cast<uint32_t>(buffer[i + 2]) << 8) | buffer[i + 1];
                    uint32_t dest;
                    for (;;)
                    {
                        dest = src - (static_cast<uint32_t>(i) + 5);
                        if (prevMask == 0)
                        {
                            break;
                        }
                        const uint32_t index = kBitNumber[prevMask >> 1];
                        b = static_cast<uint8_t>(dest >> (24 - index * 8));
                        if (!msByte(b))
                        {
                            break;
                        }
                        src = dest ^ ((1u << (32 - index * 8)) - 1);
                    }
                    buffer[i + 4] = static_cast<uint8_t>(~(((dest >> 24) & 1) - 1));
                    buffer[i + 3] = static_cast<uint8_t>(dest >> 16);
                    buffer[i + 2] = static_cast<uint8_t>(dest >> 8);
                    buffer[i + 1] = static_cast<uint8_t>(dest);
                    i += 5;
                    prevMask = 0;
                }
                else
                {
                    ++i;
                    prevMask |= 1;
                    if (msByte(b))
                    {
                        prevMask |= 0x10;
                    }
                }
            }
        }

        uint32_t Read32(const uint8_t* p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16)
                | (static_cast<uint32_t>(p[3]) << 24);
        }

        // Table-free CRC-32 (the zlib polynomial). Slow per byte, but it runs
        // once over a few hundred KB and costs a few dozen bytes of code.
        uint32_t Crc32(const uint8_t* data, size_t size)
        {
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = 0; i < size; ++i)
            {
                crc ^= data[i];
                for (int k = 0; k < 8; ++k)
                {
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
                }
            }
            return ~crc;
        }
    }

    size_t PayloadSize(const uint8_t* packed, size_t packedSize)
    {
        if (packed == nullptr || packedSize < kHeaderSize || std::memcmp(packed, "CZ1", 4) != 0)
        {
            return 0;
        }
        const uint32_t size = Read32(packed + 8);
        return size <= (64u << 20) ? size : 0; // anything larger is not ours
    }

    bool Unpack(const uint8_t* packed, size_t packedSize, uint8_t* out)
    {
        const size_t size = PayloadSize(packed, packedSize);
        if (size == 0 || out == nullptr)
        {
            return false;
        }
        if (!DecodeLzma(packed[4], packed + kHeaderSize, packedSize - kHeaderSize, out, size))
        {
            return false;
        }
        UnfilterX86(out, size);
        // The decoder catches most corruption on its own; the checksum
        // catches the rest, and any mistake in the filter.
        return Crc32(out, size) == Read32(packed + 12);
    }
}
