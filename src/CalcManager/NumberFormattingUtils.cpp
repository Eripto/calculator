// Deliberately not including pch.h: it pulls <regex>, <sstream> and <iostream>,
// and with libstdc++ those alone add several hundred KB of locale and stream
// machinery to anything that links the engine.
#include <algorithm> // for std::max
#include <cmath>     // for log10, abs
#include <string>

#include "Header Files/NumericString.h"
#include "NumberFormattingUtils.h"

using namespace std;

namespace UnitConversionManager::NumberFormattingUtils
{
    /// <summary>
    /// Trims out any trailing zeros or decimals in the given input string
    /// </summary>
    /// <param name="number">number to trim</param>
    void TrimTrailingZeros(_Inout_ wstring& number)
    {
        if (number.find(L'.') == wstring::npos)
        {
            return;
        }

        if (auto i = number.find_last_not_of(L'0'); i != wstring::npos)
        {
            number.erase(number.cbegin() + i + 1, number.cend());
        }

        if (number.back() == L'.')
        {
            number.pop_back();
        }
    }

    /// <summary>
    /// Get number of digits (whole number part + decimal part)</summary>
    /// <param name="value">the number</param>
    unsigned int GetNumberDigits(wstring value)
    {
        TrimTrailingZeros(value);
        unsigned int numberSignificantDigits = static_cast<unsigned int>(value.size());
        if (value.find(L'.') != wstring::npos)
        {
            --numberSignificantDigits;
        }
        if (value.find(L'-') != wstring::npos)
        {
            --numberSignificantDigits;
        }
        return numberSignificantDigits;
    }

    /// <summary>
    /// Get number of digits (whole number part only)</summary>
    /// <param name="value">the number</param>
    unsigned int GetNumberDigitsWholeNumberPart(double value)
    {
        return value == 0 ? 1u : static_cast<unsigned int>(1 + max(0.0, log10(abs(value))));
    }

    /// <summary>
    /// Rounds the given double to the given number of significant digits
    /// </summary>
    /// <param name="num">input double</param>
    /// <param name="numSignificant">unsigned int number of significant digits to round to</param>
    wstring RoundSignificantDigits(double num, unsigned int numSignificant)
    {
        return CalcEngine::NumericString::FixedPoint(num, static_cast<int>(numSignificant));
    }

    /// <summary>
    ///  Convert a Number to Scientific Notation
    /// </summary>
    /// <param name="number">number to convert</param>
    wstring ToScientificNumber(double number)
    {
        return CalcEngine::NumericString::Scientific(number);
    }
}
