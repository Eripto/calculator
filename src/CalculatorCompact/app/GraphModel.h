// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// Plotting for the graphing mode.
//
// The shipping app hands its equations to Graphing::IMathSolver, which this
// repository does not contain: src/GraphingImpl holds only MockGraphingImpl,
// whose solver returns an empty graph. The real one is a closed-source
// component shipped separately. So the parsing and evaluation here are written
// rather than ported; what is taken from the repository is the shape of the
// feature -- the function set in GraphingNumPad.xaml and the equation colours
// in App.xaml.
//
// Expressions compile to a flat postfix program rather than a node tree: no
// per-node allocation, no pointer chasing, and a good deal less code.

#pragma once

#include <string>
#include <vector>

namespace Graphing
{
    enum class Op : unsigned char
    {
        Number,
        Variable,

        Add,
        Subtract,
        Multiply,
        Divide,
        Power,
        Negate,

        Sin,
        Cos,
        Tan,
        Sec,
        Csc,
        Cot,

        Asin,
        Acos,
        Atan,
        Asec,
        Acsc,
        Acot,

        Sinh,
        Cosh,
        Tanh,
        Sech,
        Csch,
        Coth,

        Asinh,
        Acosh,
        Atanh,

        Log,
        Ln,
        Sqrt,
        Cbrt,
        Abs,
        Exp,
    };

    // The graph options pane offers the same three angle units the scientific
    // keypad does; trigonometric arguments and inverse results are converted
    // against whichever is chosen.
    enum class AngleMode
    {
        Radians,
        Degrees,
        Gradians,
    };

    struct Instruction
    {
        Op op;
        double value; // only meaningful for Op::Number
    };

    // A compiled expression in x. Evaluate returns NaN where the function is
    // undefined, which the renderer treats as a break in the curve.
    class Expression
    {
    public:
        // Accepts an optional leading "y=", as the equation rows are written.
        // Returns false and leaves the program empty if the text does not parse.
        bool Compile(const std::wstring& text);

        double Evaluate(double x, AngleMode angle = AngleMode::Radians) const;

        bool Empty() const
        {
            return m_program.empty();
        }

        void Clear()
        {
            m_program.clear();
        }

    private:
        std::vector<Instruction> m_program;
    };
}
