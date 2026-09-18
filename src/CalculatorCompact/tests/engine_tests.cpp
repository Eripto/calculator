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
#include <regex>
#include <string>
#include <vector>

#include "CalculatorManager.h"
#include "CalculatorResource.h"
#include "Command.h"
#include "EngineStringTable.h"
#include "Header Files/CalcEngine.h"
#include "Header Files/RadixType.h"

// Brings in the file-static MatchDecimalNumber under test.
#include "CEngine/scidisp.cpp"

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
                if (entry.id == id)
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
