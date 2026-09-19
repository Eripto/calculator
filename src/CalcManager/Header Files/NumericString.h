// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <string>
#include <string_view>

#if defined(_WIN32)
#include <cstdio>  // for _snwprintf
#include <cstdlib> // for _wtof
#else
#include <cwchar>
#endif

// Small number<->string helpers used across the engine.
//
// These exist so the engine does not reach for std::to_wstring, std::stod or
// the <iostream> manipulators. Each of those routes through a C99-conformant
// printf/strtod in the C++ runtime, and with libstdc++ that pulls a private
// copy of the floating-point conversion code (~31KB) into every binary that
// links CalcManager -- even binaries that only ever format integers. On Windows
// the CRT already exports the same conversions, so calling them directly costs
// nothing beyond an import.
namespace CalcEngine::NumericString
{
    // Replaces std::to_wstring for integral values.
    inline std::wstring FromInteger(long long value)
    {
        wchar_t buffer[24];
        wchar_t* const end = buffer + (sizeof(buffer) / sizeof(buffer[0]));
        wchar_t* cursor = end;

        unsigned long long magnitude = (value < 0) ? (0ull - static_cast<unsigned long long>(value)) : static_cast<unsigned long long>(value);
        do
        {
            *--cursor = static_cast<wchar_t>(L'0' + (magnitude % 10));
            magnitude /= 10;
        } while (magnitude != 0);

        if (value < 0)
        {
            *--cursor = L'-';
        }
        return std::wstring(cursor, end);
    }

    // Replaces std::stod for the well-formed decimal strings the engine
    // produces. Unlike std::stod this returns 0 rather than throwing on input
    // that is not a number, which suits every current caller.
    inline double ToDouble(const std::wstring& text)
    {
        if (text.empty())
        {
            return 0.0;
        }
#if defined(_WIN32)
        return _wtof(text.c_str());
#else
        return wcstod(text.c_str(), nullptr);
#endif
    }

    // Equivalent to `stream << std::fixed << std::setprecision(precision)`.
    inline std::wstring FixedPoint(double value, int precision)
    {
        if (precision < 0)
        {
            precision = 0;
        }
        // Room for the sign, the integer part of any finite double, the point,
        // the requested fraction digits and a terminator.
        std::wstring result(static_cast<size_t>(precision) + 340, L'\0');
#if defined(_WIN32)
        const int written = _snwprintf(result.data(), result.size(), L"%.*f", precision, value);
#else
        const int written = swprintf(result.data(), result.size(), L"%.*f", precision, value);
#endif
        result.resize(written > 0 ? static_cast<size_t>(written) : 0);
        return result;
    }

    // Exact fixed-point rendering of a value in [0, 1), for callers that need
    // digits beyond the 17 significant ones a CRT may stop at.
    //
    // std::uniform_real_distribution<double> over [0, 1) produces k / 2^53, and
    // for such a value the decimal expansion can be peeled off with 64-bit
    // integers alone: each step multiplies the remainder by ten, which needs at
    // most 57 bits. Rounding of the final digit is to nearest with ties to even,
    // matching printf under the default rounding mode.
    //
    // Returns false when `value` is not an exact multiple of 2^-53 in [0, 1),
    // which cannot arise from generate_canonical but leaves the caller a path
    // back to FixedPoint().
    inline bool TryExactFixedPointBelowOne(double value, int precision, std::wstring& result)
    {
        constexpr double twoPow53 = 9007199254740992.0; // 2^53
        constexpr unsigned long long mask = (1ull << 53) - 1;

        if (!(value >= 0.0) || value >= 1.0 || precision < 0)
        {
            return false;
        }

        const double scaled = value * twoPow53;
        const unsigned long long mantissa = static_cast<unsigned long long>(scaled);
        if (static_cast<double>(mantissa) != scaled)
        {
            return false;
        }

        std::wstring digits;
        digits.reserve(static_cast<size_t>(precision));
        unsigned long long remainder = mantissa;
        for (int i = 0; i < precision; ++i)
        {
            remainder *= 10;
            digits += static_cast<wchar_t>(L'0' + static_cast<wchar_t>(remainder >> 53));
            remainder &= mask;
        }

        // What is left is remainder / 2^53 of the last emitted place.
        bool roundUp = false;
        const unsigned long long twiceRemainder = remainder * 2;
        if (twiceRemainder > (1ull << 53))
        {
            roundUp = true;
        }
        else if (twiceRemainder == (1ull << 53))
        {
            // Exact tie: round to even.
            const bool lastDigitOdd = !digits.empty() && (((digits.back() - L'0') & 1) != 0);
            roundUp = digits.empty() ? false : lastDigitOdd;
        }

        int integerPart = 0;
        if (roundUp)
        {
            int index = static_cast<int>(digits.size()) - 1;
            while (index >= 0)
            {
                if (digits[static_cast<size_t>(index)] != L'9')
                {
                    ++digits[static_cast<size_t>(index)];
                    break;
                }
                digits[static_cast<size_t>(index)] = L'0';
                --index;
            }
            if (index < 0)
            {
                integerPart = 1;
            }
        }

        result.assign(1, static_cast<wchar_t>(L'0' + integerPart));
        if (precision > 0)
        {
            result += L'.';
            result += digits;
        }
        return true;
    }

    // Equivalent to `stream << std::scientific`, whose default precision is six
    // fraction digits.
    inline std::wstring Scientific(double value)
    {
        wchar_t buffer[64] = {};
        const size_t capacity = sizeof(buffer) / sizeof(buffer[0]);
#if defined(_WIN32)
        const int written = _snwprintf(buffer, capacity, L"%e", value);
#else
        const int written = swprintf(buffer, capacity, L"%e", value);
#endif
        if (written <= 0)
        {
            return std::wstring();
        }
        return std::wstring(buffer, static_cast<size_t>(written));
    }
}
