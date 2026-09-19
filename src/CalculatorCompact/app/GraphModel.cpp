// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "GraphModel.h"

#include <cmath>

namespace Graphing
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kE = 2.71828182845904523536;

        struct FunctionName
        {
            const wchar_t* name;
            Op op;
        };

        // Longest name first, so "sinh" is not mistaken for "sin" and the
        // inverse forms are matched before their bases. The set is the one the
        // shipping graphing keypad offers.
        constexpr FunctionName kFunctions[] = {
            { L"arcsinh", Op::Asinh }, { L"arccosh", Op::Acosh }, { L"arctanh", Op::Atanh },
            { L"arcsin", Op::Asin },   { L"arccos", Op::Acos },   { L"arctan", Op::Atan },
            { L"arcsec", Op::Asec },   { L"arccsc", Op::Acsc },   { L"arccot", Op::Acot },
            { L"asinh", Op::Asinh },   { L"acosh", Op::Acosh },   { L"atanh", Op::Atanh },
            { L"sinh", Op::Sinh },     { L"cosh", Op::Cosh },     { L"tanh", Op::Tanh },
            { L"sech", Op::Sech },     { L"csch", Op::Csch },     { L"coth", Op::Coth },
            { L"asin", Op::Asin },     { L"acos", Op::Acos },     { L"atan", Op::Atan },
            { L"asec", Op::Asec },     { L"acsc", Op::Acsc },     { L"acot", Op::Acot },
            { L"sqrt", Op::Sqrt },     { L"cbrt", Op::Cbrt },
            { L"sin", Op::Sin },       { L"cos", Op::Cos },       { L"tan", Op::Tan },
            { L"sec", Op::Sec },       { L"csc", Op::Csc },       { L"cot", Op::Cot },
            { L"log", Op::Log },       { L"exp", Op::Exp },       { L"abs", Op::Abs },
            { L"ln", Op::Ln },
        };

        // The keypad writes its inverse keys as "sin" followed by U+207B U+00B9.
        constexpr wchar_t kSuperscriptMinus = 0x207B;
        constexpr wchar_t kSuperscriptOne = 0x00B9;

        Op InverseOf(Op op)
        {
            switch (op)
            {
            case Op::Sin: return Op::Asin;
            case Op::Cos: return Op::Acos;
            case Op::Tan: return Op::Atan;
            case Op::Sec: return Op::Asec;
            case Op::Csc: return Op::Acsc;
            case Op::Cot: return Op::Acot;
            case Op::Sinh: return Op::Asinh;
            case Op::Cosh: return Op::Acosh;
            case Op::Tanh: return Op::Atanh;
            default: return op;
            }
        }

        bool IsDigit(wchar_t c)
        {
            return c >= L'0' && c <= L'9';
        }

        // A recursive-descent parser emitting postfix as it goes.
        class Parser
        {
        public:
            Parser(const std::wstring& text, std::vector<Instruction>& program)
                : m_text(text)
                , m_program(program)
            {
            }

            bool Run()
            {
                SkipSpace();
                if (!ParseExpression())
                {
                    return false;
                }
                SkipSpace();
                return m_position == m_text.size();
            }

        private:
            void SkipSpace()
            {
                while (m_position < m_text.size() && m_text[m_position] == L' ')
                {
                    ++m_position;
                }
            }

            wchar_t Peek() const
            {
                return (m_position < m_text.size()) ? m_text[m_position] : L'\0';
            }

            void Emit(Op op, double value = 0.0)
            {
                m_program.push_back({ op, value });
            }

            bool ParseExpression()
            {
                if (!ParseTerm())
                {
                    return false;
                }
                for (;;)
                {
                    SkipSpace();
                    const wchar_t c = Peek();
                    if (c != L'+' && c != L'-' && c != 0x2212)
                    {
                        return true;
                    }
                    ++m_position;
                    if (!ParseTerm())
                    {
                        return false;
                    }
                    Emit(c == L'+' ? Op::Add : Op::Subtract);
                }
            }

            bool ParseTerm()
            {
                if (!ParseUnary())
                {
                    return false;
                }
                for (;;)
                {
                    SkipSpace();
                    const wchar_t c = Peek();
                    if (c == L'*' || c == 0x00D7 || c == 0x22C5)
                    {
                        ++m_position;
                        if (!ParseUnary())
                        {
                            return false;
                        }
                        Emit(Op::Multiply);
                    }
                    else if (c == L'/' || c == 0x00F7)
                    {
                        ++m_position;
                        if (!ParseUnary())
                        {
                            return false;
                        }
                        Emit(Op::Divide);
                    }
                    else if (StartsPrimary(c))
                    {
                        // Implicit multiplication: 2x, 3sin(x), (x+1)(x-1).
                        if (!ParseUnary())
                        {
                            return false;
                        }
                        Emit(Op::Multiply);
                    }
                    else
                    {
                        return true;
                    }
                }
            }

            bool StartsPrimary(wchar_t c) const
            {
                if (IsDigit(c) || c == L'.' || c == L'(')
                {
                    return true;
                }
                // Inside |...| the next bar is the closing one, not the start of
                // another absolute value.
                if (c == L'|')
                {
                    return m_absDepth == 0;
                }
                if (c == L'x' || c == L'X' || c == 0x03C0 || c == L'e')
                {
                    return true;
                }
                return MatchFunction(m_position) != nullptr;
            }

            bool ParseUnary()
            {
                SkipSpace();
                const wchar_t c = Peek();
                if (c == L'-' || c == 0x2212)
                {
                    ++m_position;
                    if (!ParseUnary())
                    {
                        return false;
                    }
                    Emit(Op::Negate);
                    return true;
                }
                if (c == L'+')
                {
                    ++m_position;
                    return ParseUnary();
                }
                return ParsePower();
            }

            bool ParsePower()
            {
                if (!ParsePrimary())
                {
                    return false;
                }
                SkipSpace();
                const wchar_t c = Peek();
                if (c == L'^')
                {
                    ++m_position;
                    // Right associative, and the exponent may be signed.
                    if (!ParseUnary())
                    {
                        return false;
                    }
                    Emit(Op::Power);
                }
                else if (c == 0x00B2 || c == 0x00B3)
                {
                    // The squared and cubed superscripts the keypads produce.
                    ++m_position;
                    Emit(Op::Number, (c == 0x00B2) ? 2.0 : 3.0);
                    Emit(Op::Power);
                }
                return true;
            }

            const FunctionName* MatchFunction(size_t at) const
            {
                for (const FunctionName& entry : kFunctions)
                {
                    size_t length = 0;
                    while (entry.name[length] != L'\0')
                    {
                        ++length;
                    }
                    if (at + length > m_text.size())
                    {
                        continue;
                    }
                    bool same = true;
                    for (size_t i = 0; i < length; ++i)
                    {
                        if (m_text[at + i] != entry.name[i])
                        {
                            same = false;
                            break;
                        }
                    }
                    if (same)
                    {
                        return &entry;
                    }
                }
                return nullptr;
            }

            bool ParsePrimary()
            {
                SkipSpace();
                const wchar_t c = Peek();
                if (c == L'\0')
                {
                    return false;
                }

                if (IsDigit(c) || c == L'.')
                {
                    return ParseNumber();
                }

                if (c == L'(')
                {
                    ++m_position;
                    if (!ParseExpression())
                    {
                        return false;
                    }
                    SkipSpace();
                    if (Peek() != L')')
                    {
                        return false;
                    }
                    ++m_position;
                    return true;
                }

                if (c == L'|')
                {
                    ++m_position;
                    ++m_absDepth;
                    const bool parsed = ParseExpression();
                    --m_absDepth;
                    if (!parsed)
                    {
                        return false;
                    }
                    SkipSpace();
                    if (Peek() != L'|')
                    {
                        return false;
                    }
                    ++m_position;
                    Emit(Op::Abs);
                    return true;
                }

                if (c == 0x03C0)
                {
                    ++m_position;
                    Emit(Op::Number, kPi);
                    return true;
                }

                if (c == L'x' || c == L'X')
                {
                    ++m_position;
                    Emit(Op::Variable);
                    return true;
                }

                if (const FunctionName* function = MatchFunction(m_position); function != nullptr)
                {
                    size_t length = 0;
                    while (function->name[length] != L'\0')
                    {
                        ++length;
                    }
                    m_position += length;

                    Op op = function->op;
                    if (m_position + 1 < m_text.size() && m_text[m_position] == kSuperscriptMinus
                        && m_text[m_position + 1] == kSuperscriptOne)
                    {
                        m_position += 2;
                        op = InverseOf(op);
                    }

                    // The argument may be parenthesised or not: sin(x) and sin x
                    // both read the same way, and sin x^2 binds as sin(x^2) the
                    // way the shipping app shows it.
                    if (!ParseUnary())
                    {
                        return false;
                    }
                    Emit(op);
                    return true;
                }

                if (c == L'e')
                {
                    ++m_position;
                    Emit(Op::Number, kE);
                    return true;
                }

                return false;
            }

            bool ParseNumber()
            {
                const size_t start = m_position;
                while (m_position < m_text.size() && IsDigit(m_text[m_position]))
                {
                    ++m_position;
                }
                if (m_position < m_text.size() && m_text[m_position] == L'.')
                {
                    ++m_position;
                    while (m_position < m_text.size() && IsDigit(m_text[m_position]))
                    {
                        ++m_position;
                    }
                }
                if (m_position == start)
                {
                    return false;
                }

                // Built by hand rather than through the CRT: the parse is exact
                // for the digit counts an equation carries, and it keeps the
                // floating-point conversion code out of the binary.
                double value = 0.0;
                size_t i = start;
                for (; i < m_position && m_text[i] != L'.'; ++i)
                {
                    value = value * 10.0 + static_cast<double>(m_text[i] - L'0');
                }
                if (i < m_position && m_text[i] == L'.')
                {
                    double scale = 0.1;
                    for (++i; i < m_position; ++i)
                    {
                        value += static_cast<double>(m_text[i] - L'0') * scale;
                        scale *= 0.1;
                    }
                }
                Emit(Op::Number, value);
                return true;
            }

            const std::wstring& m_text;
            std::vector<Instruction>& m_program;
            size_t m_position = 0;
            int m_absDepth = 0;
        };
    }

    bool Expression::Compile(const std::wstring& text)
    {
        m_program.clear();

        // Equation rows are written "y = f(x)"; the left side is implied.
        std::wstring body;
        body.reserve(text.size());
        size_t start = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == L'=')
            {
                start = i + 1;
                break;
            }
        }
        for (size_t i = start; i < text.size(); ++i)
        {
            body.push_back(text[i]);
        }

        bool blank = true;
        for (const wchar_t c : body)
        {
            if (c != L' ')
            {
                blank = false;
                break;
            }
        }
        if (blank)
        {
            return false;
        }

        Parser parser(body, m_program);
        if (!parser.Run())
        {
            m_program.clear();
            return false;
        }
        return true;
    }

    double Expression::Evaluate(double x) const
    {
        // A fixed stack: the parser cannot emit a program that needs more than
        // its own nesting depth, and an equation that deep is not worth a heap
        // allocation per sample.
        constexpr int kStackSize = 64;
        double stack[kStackSize];
        int top = 0;

        const auto push = [&](double value) {
            if (top < kStackSize)
            {
                stack[top++] = value;
            }
        };

        for (const Instruction& instruction : m_program)
        {
            switch (instruction.op)
            {
            case Op::Number:
                push(instruction.value);
                continue;
            case Op::Variable:
                push(x);
                continue;
            default:
                break;
            }

            if (top < 1)
            {
                return std::nan("");
            }

            // Binary operators take the top two, everything else the top one.
            if (instruction.op == Op::Add || instruction.op == Op::Subtract || instruction.op == Op::Multiply
                || instruction.op == Op::Divide || instruction.op == Op::Power)
            {
                if (top < 2)
                {
                    return std::nan("");
                }
                const double b = stack[--top];
                const double a = stack[--top];
                switch (instruction.op)
                {
                case Op::Add: push(a + b); break;
                case Op::Subtract: push(a - b); break;
                case Op::Multiply: push(a * b); break;
                case Op::Divide: push(a / b); break;
                default: push(std::pow(a, b)); break;
                }
                continue;
            }

            const double a = stack[--top];
            switch (instruction.op)
            {
            case Op::Negate: push(-a); break;
            case Op::Sin: push(std::sin(a)); break;
            case Op::Cos: push(std::cos(a)); break;
            case Op::Tan: push(std::tan(a)); break;
            case Op::Sec: push(1.0 / std::cos(a)); break;
            case Op::Csc: push(1.0 / std::sin(a)); break;
            case Op::Cot: push(1.0 / std::tan(a)); break;
            case Op::Asin: push(std::asin(a)); break;
            case Op::Acos: push(std::acos(a)); break;
            case Op::Atan: push(std::atan(a)); break;
            case Op::Asec: push(std::acos(1.0 / a)); break;
            case Op::Acsc: push(std::asin(1.0 / a)); break;
            case Op::Acot: push(std::atan(1.0 / a)); break;
            case Op::Sinh: push(std::sinh(a)); break;
            case Op::Cosh: push(std::cosh(a)); break;
            case Op::Tanh: push(std::tanh(a)); break;
            case Op::Sech: push(1.0 / std::cosh(a)); break;
            case Op::Csch: push(1.0 / std::sinh(a)); break;
            case Op::Coth: push(1.0 / std::tanh(a)); break;
            case Op::Asinh: push(std::asinh(a)); break;
            case Op::Acosh: push(std::acosh(a)); break;
            case Op::Atanh: push(std::atanh(a)); break;
            case Op::Log: push(std::log10(a)); break;
            case Op::Ln: push(std::log(a)); break;
            case Op::Sqrt: push(std::sqrt(a)); break;
            case Op::Cbrt: push(std::cbrt(a)); break;
            case Op::Abs: push(std::fabs(a)); break;
            case Op::Exp: push(std::exp(a)); break;
            default: return std::nan("");
            }
        }

        return (top == 1) ? stack[0] : std::nan("");
    }
}
