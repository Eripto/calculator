// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for the CalcManager changes made by the compact build.
//
// The engine is portable C++, so these build and run on the host toolchain --
// no Windows and no cross-compiler needed. Two things are covered:
//
//   1. A differential test of the hand-written decimal parser that replaced
//      std::wregex in scidisp.cpp, checked against the original regex over an
//      exhaustive corpus. scidisp.cpp is included directly so the file-static
//      parser is reachable; the runner links every other engine object.
//   2. End-to-end behaviour through CalculatorManager: arithmetic, scientific
//      functions, programmer radices, memory, history and error handling.

#include <iostream>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include "GraphModel.h"

#include "CalculatorManager.h"
#include "CalculatorResource.h"
#include "Command.h"
#include "EngineStringTable.h"
#include "Header Files/CalcEngine.h"
#include "Header Files/NumericString.h"
#include "Header Files/RadixType.h"

// Brings in the file-static MatchDecimalNumber under test.
#include "CEngine/scidisp.cpp"

#include "ConverterModel.h"
#include "DateCalcModel.h"

using namespace CalculationManager;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool condition, const std::string& what)
    {
        ++g_checks;
        if (!condition)
        {
            ++g_failures;
            std::cout << "  FAIL: " << what << "\n";
        }
    }

    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        for (wchar_t c : text)
        {
            out += (c < 128) ? static_cast<char>(c) : '?';
        }
        return out;
    }

    void CheckEqual(const std::wstring& actual, const std::wstring& expected, const std::string& what)
    {
        ++g_checks;
        if (actual != expected)
        {
            ++g_failures;
            std::cout << "  FAIL: " << what << " -- expected \"" << Narrow(expected) << "\", got \"" << Narrow(actual) << "\"\n";
        }
    }

    // ---------------------------------------------------------------- part 1

    // The expression scidisp.cpp used before the parser replaced it.
    bool RegexReference(const std::wstring& text, wchar_t separator, std::wstring& integerDigits, size_t& fractionLength, size_t& exponentLength)
    {
        const std::wstring pattern = std::wstring(L"[+-]?(\\d*)[") + separator + std::wstring(L"]?(\\d*)(?:e[+-]?(\\d*))?$");
        const std::wregex rx(pattern);
        std::wsmatch matches;
        if (!std::regex_match(text, matches, rx))
        {
            return false;
        }
        integerDigits = matches.str(1);
        fractionLength = static_cast<size_t>(matches.length(2));
        exponentLength = static_cast<size_t>(matches.length(3));
        return true;
    }

    void TestDecimalParser()
    {
        std::cout << "decimal parser (differential vs std::wregex)\n";

        // Characters chosen to exercise every branch: signs, digits, both
        // candidate separators, the exponent marker and a rejected character.
        const std::wstring alphabet = L"0.9e+-,x";
        const wchar_t separators[] = { L'.', L',' };

        long long compared = 0;
        long long mismatches = 0;

        for (wchar_t separator : separators)
        {
            for (int length = 0; length <= 5; ++length)
            {
                const size_t total = static_cast<size_t>(std::pow(alphabet.size(), length));
                for (size_t index = 0; index < total; ++index)
                {
                    std::wstring candidate;
                    size_t n = index;
                    for (int i = 0; i < length; ++i)
                    {
                        candidate += alphabet[n % alphabet.size()];
                        n /= alphabet.size();
                    }

                    std::wstring expectedInt, actualInt;
                    size_t expectedFrac = 0, expectedExp = 0, actualFrac = 0, actualExp = 0;
                    const bool expected = RegexReference(candidate, separator, expectedInt, expectedFrac, expectedExp);
                    const bool actual = MatchDecimalNumber(candidate, separator, actualInt, actualFrac, actualExp);

                    ++compared;
                    if (expected != actual || (expected && (expectedInt != actualInt || expectedFrac != actualFrac || expectedExp != actualExp)))
                    {
                        if (++mismatches <= 5)
                        {
                            std::cout << "  FAIL: \"" << Narrow(candidate) << "\" sep='" << static_cast<char>(separator) << "' regex="
                                      << expected << "(" << Narrow(expectedInt) << "," << expectedFrac << "," << expectedExp << ") parser=" << actual
                                      << "(" << Narrow(actualInt) << "," << actualFrac << "," << actualExp << ")\n";
                        }
                    }
                }
            }
        }

        // A few realistic long inputs the exhaustive corpus is too short to cover.
        const wchar_t* samples[] = {
            L"123456789", L"-0.000123", L"+3.5e-12", L"1e308", L"0.1e+5", L".5", L"5.", L"1,234", L"abc", L"1.2.3", L"", L"-", L"e5", L"1e", L"99999999999999999999",
        };
        for (const wchar_t* sample : samples)
        {
            const std::wstring candidate = sample;
            std::wstring expectedInt, actualInt;
            size_t expectedFrac = 0, expectedExp = 0, actualFrac = 0, actualExp = 0;
            const bool expected = RegexReference(candidate, L'.', expectedInt, expectedFrac, expectedExp);
            const bool actual = MatchDecimalNumber(candidate, L'.', actualInt, actualFrac, actualExp);
            ++compared;
            if (expected != actual || (expected && (expectedInt != actualInt || expectedFrac != actualFrac || expectedExp != actualExp)))
            {
                ++mismatches;
                std::cout << "  FAIL (sample): \"" << Narrow(candidate) << "\"\n";
            }
        }

        ++g_checks;
        if (mismatches != 0)
        {
            ++g_failures;
        }
        std::cout << "  compared " << compared << " inputs, " << mismatches << " mismatches\n";
    }
}

namespace
{
    // ---------------------------------------------------------------- part 2

    class TestResources final : public IResourceProvider
    {
    public:
        std::wstring GetCEngineString(std::wstring_view id) override
        {
            if (id == L"sDecimal") return L".";
            if (id == L"sThousand") return L",";
            if (id == L"sGrouping") return L"3;0";
            for (const auto& entry : CalcCompact::kEngineStrings)
            {
                if (id.compare(entry.id) == 0)
                {
                    return std::wstring(entry.value);
                }
            }
            return std::wstring();
        }
    };

    class TestDisplay final : public ICalcDisplay
    {
    public:
        void SetPrimaryDisplay(const std::wstring& text, bool isError) override
        {
            primary = text;
            error = isError;
        }
        void SetIsInError(bool isError) override { error = isError; }
        void SetExpressionDisplay(
            std::shared_ptr<std::vector<std::pair<std::wstring, int>>> const& tokens,
            std::shared_ptr<std::vector<std::shared_ptr<IExpressionCommand>>> const&) override
        {
            expression.clear();
            if (tokens)
            {
                for (const auto& token : *tokens)
                {
                    expression += token.first;
                }
            }
        }
        void SetParenthesisNumber(unsigned int count) override { parens = count; }
        void SetMemorizedNumbers(const std::vector<std::wstring>& numbers) override { memory = numbers; }
        void OnHistoryItemAdded(unsigned int) override {}
        void OnNoRightParenAdded() override {}
        void MaxDigitsReached() override {}
        void BinaryOperatorReceived() override {}
        void MemoryItemChanged(unsigned int) override {}
        void InputChanged() override {}

        std::wstring primary = L"0";
        std::wstring expression;
        std::vector<std::wstring> memory;
        unsigned int parens = 0;
        bool error = false;
    };

    struct Harness
    {
        TestResources resources;
        TestDisplay display;
        CalculatorManager manager{ &display, &resources };

        void Send(std::initializer_list<Command> commands)
        {
            for (Command command : commands)
            {
                manager.SendCommand(command);
            }
        }

        // Feeds a decimal literal one keystroke at a time, the way the UI does.
        void Type(const std::wstring& digits)
        {
            for (wchar_t c : digits)
            {
                if (c >= L'0' && c <= L'9')
                {
                    manager.SendCommand(static_cast<Command>(static_cast<int>(Command::Command0) + (c - L'0')));
                }
                else if (c == L'.')
                {
                    manager.SendCommand(Command::CommandPNT);
                }
            }
        }
    };

    void TestStandardArithmetic()
    {
        std::cout << "standard mode\n";
        Harness h;
        h.manager.SetStandardMode();

        h.Type(L"2");
        h.Send({ Command::CommandADD });
        h.Type(L"3");
        h.Send({ Command::CommandEQU });
        CheckEqual(h.display.primary, L"5", "2 + 3");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"10");
        h.Send({ Command::CommandDIV });
        h.Type(L"4");
        h.Send({ Command::CommandEQU });
        CheckEqual(h.display.primary, L"2.5", "10 / 4");

        // Standard mode evaluates left to right, unlike scientific.
        h.Send({ Command::CommandCLEAR });
        h.Type(L"2");
        h.Send({ Command::CommandADD });
        h.Type(L"3");
        h.Send({ Command::CommandMUL });
        h.Type(L"4");
        h.Send({ Command::CommandEQU });
        CheckEqual(h.display.primary, L"20", "2 + 3 * 4 (left to right)");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"9");
        h.Send({ Command::CommandSQRT });
        CheckEqual(h.display.primary, L"3", "sqrt(9)");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"1");
        h.Send({ Command::CommandDIV });
        h.Type(L"0");
        h.Send({ Command::CommandEQU });
        Check(h.display.error, "1 / 0 reports an error");
        CheckEqual(h.display.primary, L"Cannot divide by zero", "divide-by-zero message");

        // Digit grouping comes from the resource provider's grouping string.
        h.Send({ Command::CommandCLEAR });
        h.Type(L"1234567");
        CheckEqual(h.display.primary, L"1,234,567", "digit grouping");
    }

    void TestScientific()
    {
        std::cout << "scientific mode\n";
        Harness h;
        h.manager.SetScientificMode();
        h.Send({ Command::CommandDEG });

        h.Type(L"2");
        h.Send({ Command::CommandADD });
        h.Type(L"3");
        h.Send({ Command::CommandMUL });
        h.Type(L"4");
        h.Send({ Command::CommandEQU });
        CheckEqual(h.display.primary, L"14", "2 + 3 * 4 respects precedence");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"30");
        h.Send({ Command::CommandSIN });
        CheckEqual(h.display.primary, L"0.5", "sin(30 deg)");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"1");
        h.Send({ Command::CommandDIV });
        h.Type(L"3");
        h.Send({ Command::CommandEQU });
        CheckEqual(h.display.primary, L"0.33333333333333333333333333333333", "1/3 at scientific precision (32 digits)");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"5");
        h.Send({ Command::CommandFAC });
        CheckEqual(h.display.primary, L"120", "5!");

        // Parentheses drive the open-paren counter the UI renders.
        h.Send({ Command::CommandCLEAR });
        h.Send({ Command::CommandOPENP });
        Check(h.display.parens == 1, "open parenthesis counted");
        h.Type(L"2");
        h.Send({ Command::CommandADD });
        h.Type(L"3");
        h.Send({ Command::CommandCLOSEP, Command::CommandMUL });
        h.Type(L"4");
        h.Send({ Command::CommandEQU });
        CheckEqual(h.display.primary, L"20", "(2 + 3) * 4");
    }

    void TestProgrammer()
    {
        std::cout << "programmer mode\n";
        Harness h;
        h.manager.SetProgrammerMode();
        h.Send({ Command::CommandQword, Command::CommandDec });

        h.Type(L"255");
        CheckEqual(h.manager.GetResultForRadix(16, 64, false), L"FF", "255 as hex");
        CheckEqual(h.manager.GetResultForRadix(8, 64, false), L"377", "255 as octal");
        CheckEqual(h.manager.GetResultForRadix(2, 64, false), L"11111111", "255 as binary");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"12");
        h.Send({ Command::CommandAnd });
        h.Type(L"10");
        h.Send({ Command::CommandEQU });
        CheckEqual(h.display.primary, L"8", "12 AND 10");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"12");
        h.Send({ Command::CommandXor });
        h.Type(L"10");
        h.Send({ Command::CommandEQU });
        CheckEqual(h.display.primary, L"6", "12 XOR 10");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"1");
        h.Send({ Command::CommandLSHF });
        h.Type(L"8");
        h.Send({ Command::CommandEQU });
        CheckEqual(h.display.primary, L"256", "1 << 8");

        // Hex entry uses the A-F commands.
        h.Send({ Command::CommandHex, Command::CommandCLEAR });
        h.Send({ Command::CommandF, Command::CommandF });
        CheckEqual(h.manager.GetResultForRadix(10, 64, false), L"255", "hex FF as decimal");
    }

    void TestMemory()
    {
        std::cout << "memory\n";
        Harness h;
        h.manager.SetStandardMode();

        h.Type(L"7");
        h.manager.MemorizeNumber();
        Check(h.display.memory.size() == 1, "MS stores one slot");
        CheckEqual(h.display.memory.empty() ? L"" : h.display.memory[0], L"7", "stored value");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"3");
        h.manager.MemorizedNumberAdd(0);
        CheckEqual(h.display.memory[0], L"10", "M+ adds to the slot");

        h.manager.MemorizedNumberSubtract(0);
        CheckEqual(h.display.memory[0], L"7", "M- subtracts from the slot");

        h.Send({ Command::CommandCLEAR });
        h.manager.MemorizedNumberLoad(0);
        CheckEqual(h.display.primary, L"7", "MR recalls the slot");

        h.manager.MemorizedNumberClearAll();
        Check(h.display.memory.empty(), "MC clears all slots");
    }

    void TestHistory()
    {
        std::cout << "history\n";
        Harness h;
        h.manager.SetStandardMode();

        h.Type(L"2");
        h.Send({ Command::CommandADD });
        h.Type(L"3");
        h.Send({ Command::CommandEQU });

        const auto& items = h.manager.GetHistoryItems();
        Check(items.size() == 1, "one history entry after =");
        if (!items.empty())
        {
            CheckEqual(items[0]->historyItemVector.result, L"5", "history result");
            Check(items[0]->historyItemVector.expression.find(L"2") != std::wstring::npos, "history expression keeps the operand");
            Check(items[0]->historyItemVector.spCommands != nullptr, "history entry carries replay commands");
        }

        h.manager.ClearHistory();
        Check(h.manager.GetHistoryItems().empty(), "history cleared");
    }

    void TestOperatorNames()
    {
        // Exercises the operator-name table that became a flat constexpr array.
        std::cout << "operator name table\n";
        Harness h;
        h.manager.SetScientificMode();
        h.Send({ Command::CommandDEG });

        h.Type(L"9");
        h.Send({ Command::CommandSQRT });
        Check(h.display.expression.find(L"√") != std::wstring::npos, "sqrt token uses the root glyph");

        h.Send({ Command::CommandCLEAR });
        h.Type(L"30");
        h.Send({ Command::CommandSIN });
        Check(h.display.expression.find(L"sin") != std::wstring::npos, "sin token present in degrees");

        h.Send({ Command::CommandCLEAR, Command::CommandRAD });
        h.Type(L"1");
        h.Send({ Command::CommandSIN });
        Check(h.display.expression.find(L"sin") != std::wstring::npos, "sin token present in radians");

        h.Send({ Command::CommandCLEAR, Command::CommandDEG });
        h.Type(L"5");
        h.Send({ Command::CommandFAC });
        Check(h.display.expression.find(L"fact") != std::wstring::npos, "factorial token present");
    }

    // The rand command formats its value with TryExactFixedPointBelowOne; check
    // it against the host C library over a wide range of inputs.
    void TestExactFixedPoint()
    {
        std::cout << "exact fixed-point formatter\n";
        std::mt19937_64 rng(20240918);
        long long compared = 0, mismatches = 0;

        auto reference = [](double value, int precision) {
            std::wstring buffer(static_cast<size_t>(precision) + 340, L'\0');
            const int written = swprintf(buffer.data(), buffer.size(), L"%.*f", precision, value);
            buffer.resize(written > 0 ? static_cast<size_t>(written) : 0);
            return buffer;
        };

        for (int precision : { 0, 1, 2, 5, 16, 17, 32, 40, 53 })
        {
            for (int i = 0; i < 4000; ++i)
            {
                // Exactly the shape generate_canonical produces: k / 2^53.
                const unsigned long long k = rng() >> 11;
                const double value = static_cast<double>(k) / 9007199254740992.0;

                std::wstring actual;
                const bool ok = CalcEngine::NumericString::TryExactFixedPointBelowOne(value, precision, actual);
                ++compared;
                if (!ok)
                {
                    ++mismatches;
                    continue;
                }
                const std::wstring expected = reference(value, precision);
                if (actual != expected)
                {
                    if (++mismatches <= 5)
                    {
                        std::cout << "  FAIL: p=" << precision << " expected \"" << Narrow(expected) << "\" got \"" << Narrow(actual) << "\"\n";
                    }
                }
            }
        }

        // Carry propagation and the boundaries.
        std::wstring out;
        Check(CalcEngine::NumericString::TryExactFixedPointBelowOne(0.0, 8, out) && out == L"0.00000000", "zero");
        Check(!CalcEngine::NumericString::TryExactFixedPointBelowOne(1.0, 8, out), "1.0 is out of range");
        Check(!CalcEngine::NumericString::TryExactFixedPointBelowOne(-0.5, 8, out), "negative is out of range");
        // 1 - 2^-53 rounds up to 1 at low precision, exercising the carry.
        const double almostOne = 1.0 - 1.0 / 9007199254740992.0;
        Check(CalcEngine::NumericString::TryExactFixedPointBelowOne(almostOne, 4, out) && out == L"1.0000", "carry propagates to the integer part");

        ++g_checks;
        if (mismatches != 0)
        {
            ++g_failures;
        }
        std::cout << "  compared " << compared << " values, " << mismatches << " mismatches\n";
    }

    void TestRandom()
    {
        // The rand command formats a double with swprintf now instead of a
        // wstringstream; check the result is a usable number in [0, 1).
        std::cout << "rand\n";
        Harness h;
        h.manager.SetScientificMode();

        bool allDistinct = true;
        std::wstring previous;
        for (int i = 0; i < 8; ++i)
        {
            h.Send({ Command::CommandCLEAR, Command::CommandRand });
            const std::wstring value = h.display.primary;
            Check(!h.display.error, "rand does not error");
            Check(!value.empty() && (value[0] == L'0' || value[0] == L'1'), "rand is in [0, 1)");
            Check(value.find(L'.') != std::wstring::npos || value == L"0", "rand has a fractional part");
            if (!previous.empty() && previous == value)
            {
                allDistinct = false;
            }
            previous = value;
        }
        Check(allDistinct, "successive rand values differ");
    }
}

namespace
{
    void TestConverter()
    {
        std::cout << "unit converter\n";
        CalcCompact::ConverterModel model;

        Check(model.Categories().size() == 12, "twelve converter categories");

        auto typeInto = [&model](const wchar_t* digits) {
            model.Clear();
            for (const wchar_t* p = digits; *p; ++p)
            {
                model.Send(CalcCompact::ConverterCommandForChar(*p, L'.'));
            }
        };

        // Volume: 1 US gallon is 3.785411784 litres.
        model.SetCategory(4);
        const auto& volumeUnits = model.Units();
        Check(!volumeUnits.empty(), "volume has units");
        int gallon = 0, litre = 0;
        for (const auto& u : volumeUnits)
        {
            if (u.name == L"Gallons (US)") gallon = u.id;
            if (u.name == L"Liters") litre = u.id;
        }
        Check(gallon != 0 && litre != 0, "found gallon and litre");
        model.SetUnits(gallon, litre);
        typeInto(L"1");
        // The converter shows seven significant digits, as the shipping app does.
        CheckEqual(model.ToValue(), L"3.785412", "1 gal (US) -> L");

        // Length: 1 mile is 1.609344 km.
        model.SetCategory(5);
        int mile = 0, kilometre = 0;
        for (const auto& u : model.Units())
        {
            if (u.name == L"Miles") mile = u.id;
            if (u.name == L"Kilometers") kilometre = u.id;
        }
        model.SetUnits(mile, kilometre);
        typeInto(L"1");
        CheckEqual(model.ToValue(), L"1.609344", "1 mile -> km");

        // Temperature uses the explicit ratio/offset table.
        model.SetCategory(7);
        int celsius = 0, fahrenheit = 0, kelvin = 0;
        for (const auto& u : model.Units())
        {
            if (u.name == L"Celsius") celsius = u.id;
            if (u.name == L"Fahrenheit") fahrenheit = u.id;
            if (u.name == L"Kelvin") kelvin = u.id;
        }
        model.SetUnits(celsius, fahrenheit);
        typeInto(L"100");
        CheckEqual(model.ToValue(), L"212", "100 C -> F");
        typeInto(L"0");
        CheckEqual(model.ToValue(), L"32", "0 C -> F");
        model.SetUnits(celsius, kelvin);
        typeInto(L"0");
        CheckEqual(model.ToValue(), L"273.15", "0 C -> K");
        model.SetUnits(fahrenheit, celsius);
        typeInto(L"212");
        CheckEqual(model.ToValue(), L"100", "212 F -> C");
        Check(model.SupportsNegative(), "temperature supports negative values");

        // Data: 1 GB is 1000 MB in the shipping table.
        model.SetCategory(13);
        int gigabyte = 0, megabyte = 0;
        for (const auto& u : model.Units())
        {
            if (u.name == L"Gigabytes") gigabyte = u.id;
            if (u.name == L"Megabytes") megabyte = u.id;
        }
        model.SetUnits(gigabyte, megabyte);
        typeInto(L"1");
        CheckEqual(model.ToValue(), L"1000", "1 GB -> MB");

        // Angle: 180 degrees is pi radians.
        model.SetCategory(15);
        int degree = 0, radian = 0;
        for (const auto& u : model.Units())
        {
            if (u.name == L"Degrees") degree = u.id;
            if (u.name == L"Radians") radian = u.id;
        }
        model.SetUnits(degree, radian);
        typeInto(L"180");
        Check(model.ToValue().substr(0, 7) == L"3.14159", "180 deg -> rad");

        // Editing the second field converts backwards.
        model.SetCategory(5);
        model.SetUnits(mile, kilometre);
        model.Clear();
        model.SetActiveField(true);
        model.Send(CalcCompact::ConverterCommandForChar(L'1', L'.'));
        CheckEqual(model.FromValue(), L"0.621371", "1 km -> miles (editing the second field)");
        CheckEqual(model.ToValue(), L"1", "second field holds what was typed");
        model.SetActiveField(false);

        // Every category must expose units and a usable default pair.
        for (const auto& category : model.Categories())
        {
            model.SetCategory(category.id);
            Check(!model.Units().empty(), "category has units");
            Check(model.FromUnitId() != model.ToUnitId() || model.Units().size() == 1, "distinct default units");
        }
    }
}

namespace
{
    void TestDateCalculation()
    {
        std::cout << "date calculation\n";
        using namespace CalcCompact;

        // Round-trip the civil-date conversion across a wide span, including
        // every leap-year rule boundary.
        long long checked = 0, bad = 0;
        for (long long day = -60000; day <= 40000; day += 7)
        {
            const CivilDate date = DateMath::FromDayNumber(day);
            if (DateMath::ToDayNumber(date) != day)
            {
                ++bad;
            }
            ++checked;
        }
        Check(bad == 0, "day-number round trip");

        Check(DateMath::IsLeapYear(2000), "2000 is a leap year");
        Check(!DateMath::IsLeapYear(1900), "1900 is not a leap year");
        Check(DateMath::IsLeapYear(2024), "2024 is a leap year");
        Check(DateMath::DaysInMonth(2024, 2) == 29, "February 2024 has 29 days");
        Check(DateMath::DaysInMonth(2023, 2) == 28, "February 2023 has 28 days");

        // Known weekday anchors.
        CheckEqual(DateMath::WeekdayName(CivilDate{ 1970, 1, 1 }), L"Thursday", "1970-01-01 was a Thursday");
        CheckEqual(DateMath::WeekdayName(CivilDate{ 2000, 1, 1 }), L"Saturday", "2000-01-01 was a Saturday");
        CheckEqual(DateMath::WeekdayName(CivilDate{ 2024, 2, 29 }), L"Thursday", "2024-02-29 was a Thursday");

        // End-of-month clamping, as the calendar does it.
        Check(DateMath::AddMonths(CivilDate{ 2024, 1, 31 }, 1) == (CivilDate{ 2024, 2, 29 }), "31 Jan + 1 month clamps to 29 Feb");
        Check(DateMath::AddMonths(CivilDate{ 2023, 1, 31 }, 1) == (CivilDate{ 2023, 2, 28 }), "31 Jan + 1 month clamps to 28 Feb");
        Check(DateMath::AddMonths(CivilDate{ 2023, 12, 15 }, 1) == (CivilDate{ 2024, 1, 15 }), "month addition crosses the year");
        Check(DateMath::AddMonths(CivilDate{ 2024, 1, 15 }, -1) == (CivilDate{ 2023, 12, 15 }), "month subtraction crosses the year");

        Check(DateMath::AddDays(CivilDate{ 2024, 2, 28 }, 1) == (CivilDate{ 2024, 2, 29 }), "leap day follows 28 Feb 2024");
        Check(DateMath::AddDays(CivilDate{ 2023, 2, 28 }, 1) == (CivilDate{ 2023, 3, 1 }), "1 March follows 28 Feb 2023");

        // Differences.
        auto diff = DateMath::Difference(CivilDate{ 2024, 1, 1 }, CivilDate{ 2024, 12, 31 });
        Check(diff.totalDays == 365, "2024-01-01 to 2024-12-31 is 365 days");
        Check(diff.years == 0 && diff.months == 11, "...which is 11 months and change");

        diff = DateMath::Difference(CivilDate{ 2000, 1, 1 }, CivilDate{ 2025, 6, 15 });
        Check(diff.years == 25 && diff.months == 5 && diff.weeks == 2, "25 years 5 months 2 weeks");
        Check(diff.totalDays == 9297, "total days across 25 years");

        // Order must not matter.
        const auto forward = DateMath::Difference(CivilDate{ 2020, 3, 1 }, CivilDate{ 2021, 5, 20 });
        const auto backward = DateMath::Difference(CivilDate{ 2021, 5, 20 }, CivilDate{ 2020, 3, 1 });
        Check(forward.totalDays == backward.totalDays && forward.years == backward.years, "difference is symmetric");

        // Same date.
        diff = DateMath::Difference(CivilDate{ 2024, 5, 5 }, CivilDate{ 2024, 5, 5 });
        Check(diff.totalDays == 0 && diff.years == 0 && diff.months == 0 && diff.weeks == 0 && diff.days == 0, "same date is zero");
        CheckEqual(DateMath::DescribeDifference(diff), L"Same dates", "same-date wording");

        // Reconstructing the end date from the decomposition must land exactly.
        long long mismatches = 0;
        for (int i = 0; i < 400; ++i)
        {
            const CivilDate a = DateMath::FromDayNumber(-20000 + i * 137);
            const CivilDate b = DateMath::FromDayNumber(-20000 + i * 137 + i * 29);
            const auto d = DateMath::Difference(a, b);
            CivilDate rebuilt = DateMath::AddMonths(a, static_cast<long long>(d.years) * 12 + d.months);
            rebuilt = DateMath::AddDays(rebuilt, static_cast<long long>(d.weeks) * 7 + d.days);
            if (!(rebuilt == b))
            {
                ++mismatches;
            }
        }
        Check(mismatches == 0, "difference decomposition rebuilds the end date");

        // The add/subtract mode.
        DateCalcModel model;
        model.SetMode(DateCalcModel::Mode::AddSubtract);
        model.Start() = CivilDate{ 2024, 1, 31 };
        model.SetAdding(true);
        model.OffsetMonths() = 1;
        Check(model.Result() == (CivilDate{ 2024, 2, 29 }), "add one month clamps");
        model.OffsetMonths() = 0;
        model.OffsetDays() = 30;
        Check(model.Result() == (CivilDate{ 2024, 3, 1 }), "add 30 days");
        model.SetAdding(false);
        Check(model.Result() == (CivilDate{ 2024, 1, 1 }), "subtract 30 days");

        std::cout << "  round-tripped " << checked << " day numbers\n";
    }
}

namespace
{
    void TestGraphing()
    {
        std::cout << "graphing\n";

        const auto value = [](const wchar_t* text, double x) {
            Graphing::Expression expression;
            if (!expression.Compile(text))
            {
                return std::nan("");
            }
            return expression.Evaluate(x);
        };
        const auto close = [](double a, double b) {
            return std::fabs(a - b) <= 1e-9 * (1.0 + std::fabs(b));
        };
        const auto parses = [](const wchar_t* text) {
            Graphing::Expression expression;
            return expression.Compile(text);
        };

        // Shape of the grammar.
        Check(close(value(L"x", 3.0), 3.0), "x");
        Check(close(value(L"y=x^2", 4.0), 16.0), "leading y= is stripped");
        Check(close(value(L"2x", 5.0), 10.0), "implicit multiplication by a number");
        Check(close(value(L"(x+1)(x-1)", 4.0), 15.0), "implicit multiplication of groups");
        Check(close(value(L"3sin(x)", 0.0), 0.0), "implicit multiplication of a call");
        Check(close(value(L"x^2+2x+1", 3.0), 16.0), "polynomial");

        // Precedence and associativity.
        Check(close(value(L"2^3^2", 0.0), 512.0), "power is right associative");
        Check(close(value(L"-2^2", 0.0), -4.0), "unary minus binds looser than power");
        Check(close(value(L"-x^2", 3.0), -9.0), "negated square");
        Check(close(value(L"1-2-3", 0.0), -4.0), "subtraction is left associative");
        Check(close(value(L"8/4/2", 0.0), 1.0), "division is left associative");

        // Functions, constants and the forms the keypads produce.
        Check(close(value(L"sqrt(x)", 9.0), 3.0), "sqrt");
        Check(close(value(L"cbrt(27)", 0.0), 3.0), "cbrt");
        Check(close(value(L"|x|", -5.0), 5.0), "absolute value bars");
        Check(close(value(L"abs(x)", -5.0), 5.0), "abs");
        Check(close(value(L"ln(e)", 0.0), 1.0), "ln and e");
        Check(close(value(L"log(100)", 0.0), 2.0), "log base 10");
        Check(close(value(L"sin(π/2)", 0.0), 1.0), "pi");
        Check(close(value(L"x²", 5.0), 25.0), "superscript two");
        Check(close(value(L"x³", 2.0), 8.0), "superscript three");
        Check(close(value(L"sin⁻¹(1)", 0.0), 3.14159265358979323846 / 2.0), "keypad inverse form");
        Check(close(value(L"asin(1)", 0.0), 3.14159265358979323846 / 2.0), "written inverse form");
        Check(close(value(L"sin x", 0.0), 0.0), "unparenthesised argument");
        Check(close(value(L"tanh(0)", 0.0), 0.0), "hyperbolic");
        Check(close(value(L"1.5x", 2.0), 3.0), "decimal literal");
        Check(close(value(L"x×2", 3.0), 6.0), "typographic multiplication sign");
        Check(close(value(L"x÷2", 6.0), 3.0), "typographic division sign");
        Check(close(value(L"x−4", 6.0), 2.0), "typographic minus sign");

        // Undefined points come back as NaN, which the renderer breaks the
        // curve at rather than drawing a line through.
        Check(std::isnan(value(L"sqrt(x)", -1.0)), "sqrt of a negative is undefined");
        Check(std::isnan(value(L"ln(x)", -1.0)), "ln of a negative is undefined");
        Check(!std::isfinite(value(L"1/x", 0.0)), "1/x at the asymptote is not finite");

        // Things that must not parse, rather than parse into something wrong.
        Check(!parses(L""), "empty is rejected");
        Check(!parses(L"   "), "blank is rejected");
        Check(!parses(L"y="), "bare y= is rejected");
        Check(!parses(L"x+"), "trailing operator is rejected");
        Check(!parses(L"(x"), "unclosed group is rejected");
        Check(!parses(L"x)"), "unopened group is rejected");
        Check(!parses(L"sin()"), "empty call is rejected");
        Check(!parses(L"|x"), "unclosed bars are rejected");
        Check(!parses(L"@"), "unknown character is rejected");

        // A compiled expression samples cleanly across a range, which is what
        // the renderer does a few hundred times per frame.
        {
            Graphing::Expression expression;
            Check(expression.Compile(L"sin(x)/x"), "compiles a quotient");
            int finite = 0;
            for (int i = -200; i <= 200; ++i)
            {
                const double x = static_cast<double>(i) * 0.05;
                if (std::isfinite(expression.Evaluate(x)))
                {
                    ++finite;
                }
            }
            Check(finite >= 395, "sin(x)/x is finite away from the origin");
        }

        Graphing::Expression cleared;
        Check(cleared.Empty(), "a fresh expression is empty");
        Check(cleared.Compile(L"x") && !cleared.Empty(), "compiling fills it");
        cleared.Clear();
        Check(cleared.Empty(), "Clear empties it");
    }
}

int main()
{
    std::cout << "CalcManager compact-build tests\n\n";
    TestDecimalParser();
    TestStandardArithmetic();
    TestScientific();
    TestProgrammer();
    TestMemory();
    TestHistory();
    TestOperatorNames();
    TestConverter();
    TestDateCalculation();
    TestGraphing();
    TestExactFixedPoint();
    TestRandom();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures != 0)
    {
        std::cout << g_failures << " FAILED\n";
        return 1;
    }
    std::cout << "all good\n";
    return 0;
}
