// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Calculator (compact build)
// -------------------------
// A native Win32 front end for the Windows Calculator engine that ships in this
// repository (src/CalcManager). The engine is used verbatim, so arithmetic,
// precision, operator precedence, history and memory semantics are identical to
// the shipping app; only the presentation layer is replaced. XAML, WinUI and the
// packaged-app machinery are traded for direct GDI/GDI+ drawing, which is what
// keeps the binary small.

#include "version.h"
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <new>
#include <memory>
#include <string>
#include <vector>

#include "CalculatorManager.h"
#include "ConverterModel.h"
#include "GraphModel.h"
#include "DateCalcModel.h"
#include "ConverterData.generated.h"
#include "CalculatorResource.h"
#include "Command.h"
#include "EngineStringTable.h"
#include "Header Files/CalcEngine.h"
#include "Header Files/EngineStrings.h"
#include "Header Files/NumericString.h"
#include "Header Files/RadixType.h"

using namespace CalculationManager;
using namespace CalcCompact;

// libstdc++ funnels every internal error -- allocation failure, container
// length and range violations -- through the five helpers below, which live in
// functexcept.o. Linking that object pulls std::logic_error, which stores a
// std::string, which drags in the narrow std::string and std::random_device
// instantiations behind it: roughly 50KB for diagnostic text this app never
// reads. Defining the helpers here keeps functexcept.o off the link line.
//
// Nothing in the engine or the UI catches a std:: exception type, and
// CalcApp::Send() catches (...), so throwing a lightweight type preserves both
// stack unwinding and the existing catch behaviour.
namespace CalcCompact
{
    struct StandardLibraryError
    {
        const char* reason;
    };

}

namespace std
{
    void __throw_bad_alloc()
    {
        throw CalcCompact::StandardLibraryError{ "bad_alloc" };
    }

    void __throw_bad_array_new_length()
    {
        throw CalcCompact::StandardLibraryError{ "bad_array_new_length" };
    }

    void __throw_logic_error(const char* reason)
    {
        throw CalcCompact::StandardLibraryError{ reason };
    }

    void __throw_length_error(const char* reason)
    {
        throw CalcCompact::StandardLibraryError{ reason };
    }

    void __throw_out_of_range(const char* reason)
    {
        throw CalcCompact::StandardLibraryError{ reason };
    }

    void __throw_out_of_range_fmt(const char* reason, ...)
    {
        throw CalcCompact::StandardLibraryError{ reason };
    }

    void __throw_invalid_argument(const char* reason)
    {
        throw CalcCompact::StandardLibraryError{ reason };
    }
}

// libstdc++'s operator new reports failure by throwing std::bad_alloc, and the
// reference to that type's typeinfo is enough to pull functexcept.o back in.
// Replacing the global allocation functions -- a standard customization point --
// closes that last path. Behaviour is unchanged: allocation failure still
// unwinds, it just carries the lightweight type above.
void* operator new(std::size_t size)
{
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr)
    {
        std::__throw_bad_alloc();
    }
    return memory;
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return std::malloc(size != 0 ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return std::malloc(size != 0 ? size : 1);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
    std::free(memory);
}

// libstdc++'s default terminate handler prints a demangled exception name,
// which drags the whole C++ symbol demangler (~48KB) into the binary for a
// diagnostic a windowed app never shows. Replace it with a silent abort.
namespace __gnu_cxx
{
    void __verbose_terminate_handler()
    {
        ::ExitProcess(3);
    }
}

namespace
{
    // ---------------------------------------------------------------- theming

    // Colours follow the Windows 11 Calculator: a neutral page behind two button
    // tiers (numbers sit slightly proud of operators) with the system accent on
    // the equals key.
    struct Theme
    {
        COLORREF page;
        COLORREF card;
        COLORREF primaryText;
        COLORREF secondaryText;
        COLORREF numberFill;
        COLORREF numberHover;
        COLORREF numberPress;
        COLORREF operatorFill;
        COLORREF operatorHover;
        COLORREF operatorPress;
        COLORREF flatHover;
        COLORREF flatPress;
        COLORREF accent;
        COLORREF accentHover;
        COLORREF accentPress;
        COLORREF accentText;
        COLORREF divider;
        COLORREF disabledText;
        bool dark;
    };

    Theme g_theme{};

    // 0 follows the system, 1 forces light, 2 forces dark. Set from Settings.
    int g_themeOverride = 0;

    COLORREF Mix(COLORREF a, COLORREF b, double t)
    {
        auto lerp = [t](int x, int y) { return static_cast<int>(x + (y - x) * t + 0.5); };
        return RGB(
            lerp(GetRValue(a), GetRValue(b)),
            lerp(GetGValue(a), GetGValue(b)),
            lerp(GetBValue(a), GetBValue(b)));
    }

    bool SystemUsesDarkTheme()
    {
        DWORD value = 1;
        DWORD size = sizeof(value);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme",
                RRF_RT_REG_DWORD,
                nullptr,
                &value,
                &size)
            != ERROR_SUCCESS)
        {
            return false;
        }
        return value == 0;
    }

    // The accent is stored as 0xAABBGGRR under the DWM key. Fall back to the
    // Windows 11 default blue when it is unreadable.
    COLORREF SystemAccent()
    {
        DWORD value = 0;
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &value, &size)
            == ERROR_SUCCESS)
        {
            return RGB(value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF);
        }
        return RGB(0, 120, 212);
    }

    void LoadTheme()
    {
        const bool dark = (g_themeOverride == 0) ? SystemUsesDarkTheme() : (g_themeOverride == 2);
        const COLORREF accent = SystemAccent();
        Theme t{};
        t.dark = dark;
        if (dark)
        {
            t.page = RGB(32, 32, 32);
            t.card = RGB(39, 39, 39);
            t.primaryText = RGB(255, 255, 255);
            t.secondaryText = RGB(163, 163, 163);
            t.numberFill = RGB(59, 59, 59);
            t.numberHover = RGB(70, 70, 70);
            t.numberPress = RGB(52, 52, 52);
            t.operatorFill = RGB(50, 50, 50);
            t.operatorHover = RGB(61, 61, 61);
            t.operatorPress = RGB(44, 44, 44);
            t.flatHover = RGB(48, 48, 48);
            t.flatPress = RGB(42, 42, 42);
            // Accents are brightened on dark backgrounds the way Fluent does, so
            // the equals key keeps its contrast against the page.
            t.accent = Mix(accent, RGB(255, 255, 255), 0.45);
            t.accentHover = Mix(accent, RGB(255, 255, 255), 0.55);
            t.accentPress = Mix(accent, RGB(255, 255, 255), 0.32);
            t.accentText = RGB(0, 0, 0);
            t.divider = RGB(55, 55, 55);
            t.disabledText = RGB(115, 115, 115);
        }
        else
        {
            t.page = RGB(243, 243, 243);
            t.card = RGB(255, 255, 255);
            t.primaryText = RGB(0, 0, 0);
            t.secondaryText = RGB(95, 95, 95);
            t.numberFill = RGB(255, 255, 255);
            t.numberHover = RGB(246, 246, 246);
            t.numberPress = RGB(237, 237, 237);
            t.operatorFill = RGB(249, 249, 249);
            t.operatorHover = RGB(240, 240, 240);
            t.operatorPress = RGB(232, 232, 232);
            t.flatHover = RGB(234, 234, 234);
            t.flatPress = RGB(225, 225, 225);
            t.accent = Mix(accent, RGB(0, 0, 0), 0.12);
            t.accentHover = Mix(accent, RGB(0, 0, 0), 0.22);
            t.accentPress = Mix(accent, RGB(0, 0, 0), 0.30);
            t.accentText = RGB(255, 255, 255);
            t.divider = RGB(224, 224, 224);
            t.disabledText = RGB(160, 160, 160);
        }
        g_theme = t;
    }

    // ------------------------------------------------------------------- DPI

    int g_dpi = 96;

    int Dp(int dip)
    {
        return MulDiv(dip, g_dpi, 96);
    }

    // ---------------------------------------------------------------- motion
    //
    // Durations and curves follow the WinUI motion guidance the shipping app is
    // built on: 167ms for a small state change, 250ms for a surface moving onto
    // the screen, and a decelerating curve for anything entering. Values are
    // computed from the clock on demand rather than stepped, so a dropped frame
    // never desynchronises an animation from where it should be.
    constexpr int kMotionFastMs = 167;
    constexpr int kMotionNormalMs = 250;

    // Approximates the WinUI "decelerate" curve, cubic-bezier(0, 0, 0, 1).
    inline float EaseDecelerate(float t)
    {
        const float inverse = 1.0f - t;
        return 1.0f - inverse * inverse * inverse * inverse;
    }

    // The mirror of decelerate: barely moves at first, then goes. Used for
    // something that should sit still for a moment before it leaves.
    inline float EaseAccelerate(float t)
    {
        return t * t * t;
    }

    // Approximates "point to point", cubic-bezier(0.55, 0.55, 0, 1).
    inline float EaseStandard(float t)
    {
        const float inverse = 1.0f - t;
        return 1.0f - inverse * inverse * inverse;
    }

    class Anim
    {
    public:
        // Retargets from wherever the value currently is, so an interrupted
        // animation continues smoothly instead of jumping.
        void To(float target, int durationMs, float (*easing)(float) = EaseDecelerate)
        {
            if (m_target == target && (m_startTick != 0 || m_value == target))
            {
                return;
            }
            m_value = Value();
            m_from = m_value;
            m_target = target;
            m_duration = (durationMs > 0) ? durationMs : 1;
            m_easing = easing;
            m_startTick = GetTickCount64();
        }

        void Set(float value)
        {
            m_value = value;
            m_from = value;
            m_target = value;
            m_startTick = 0;
        }

        float Value() const
        {
            if (m_startTick == 0)
            {
                return m_target;
            }
            const ULONGLONG elapsed = GetTickCount64() - m_startTick;
            if (elapsed >= static_cast<ULONGLONG>(m_duration))
            {
                return m_target;
            }
            const float t = static_cast<float>(elapsed) / static_cast<float>(m_duration);
            return m_from + (m_target - m_from) * m_easing(t);
        }

        bool Active() const
        {
            return m_startTick != 0 && (GetTickCount64() - m_startTick) < static_cast<ULONGLONG>(m_duration);
        }

        void Settle()
        {
            if (m_startTick != 0 && !Active())
            {
                m_value = m_target;
                m_startTick = 0;
            }
        }

    private:
        ULONGLONG m_startTick = 0;
        int m_duration = 1;
        float m_from = 0.0f;
        float m_target = 0.0f;
        float m_value = 0.0f;
        float (*m_easing)(float) = EaseDecelerate;
    };

    // Blends two colours; used to fade hover and press states in rather than
    // switching them on a frame boundary.
    COLORREF Blend(COLORREF from, COLORREF to, float t)
    {
        if (t <= 0.0f)
        {
            return from;
        }
        if (t >= 1.0f)
        {
            return to;
        }
        return Mix(from, to, static_cast<double>(t));
    }

    // ------------------------------------------------------- engine plumbing

    // Serves the engine the same CEngineStrings values the shipping app uses,
    // plus the number-format characters taken from the user's locale.
    class ResourceProvider final : public IResourceProvider
    {
    public:
        ResourceProvider()
        {
            m_decimal = LocaleInfo(LOCALE_SDECIMAL, L".");
            m_thousand = LocaleInfo(LOCALE_STHOUSAND, L",");
            // CalcEngine expects the Win32 "grouping" form, e.g. "3;0".
            std::wstring grouping = LocaleInfo(LOCALE_SGROUPING, L"3;0");
            m_grouping = grouping.empty() ? L"3;0" : grouping;
        }

        std::wstring GetCEngineString(std::wstring_view id) override
        {
            if (id == L"sDecimal")
            {
                return m_decimal;
            }
            if (id == L"sThousand")
            {
                return m_thousand;
            }
            if (id == L"sGrouping")
            {
                return m_grouping;
            }
            for (const auto& entry : CalcCompact::kEngineStrings)
            {
                if (id.compare(entry.id) == 0)
                {
                    return std::wstring(entry.value);
                }
            }
            return std::wstring();
        }

        wchar_t Decimal() const
        {
            return m_decimal.empty() ? L'.' : m_decimal[0];
        }

    private:
        static std::wstring LocaleInfo(LCTYPE type, const wchar_t* fallback)
        {
            wchar_t buffer[16] = {};
            const int written = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, buffer, ARRAYSIZE(buffer));
            if (written <= 1)
            {
                return fallback;
            }
            return buffer;
        }

        std::wstring m_decimal;
        std::wstring m_thousand;
        std::wstring m_grouping;
    };
}

namespace
{
    // ------------------------------------------------------------ UI model

    enum class Mode
    {
        Standard,
        Scientific,
        Graphing,
        Programmer,
        Date,
        Converter
    };

    enum class Style
    {
        Number,     // raised tile, the digits and decimal point
        Operator,   // recessed tile, everything else on the keypad
        Accent,     // the equals key
        Flat,       // chrome: nav, memory strip, panel commands
        Toggle,     // flat, but drawn "checked" when active
        Card,       // settings surface: filled, outlined, larger radius
        Bit         // programmer bit-flip cell
    };

    // Actions are either a direct engine command or a front-end action. Engine
    // commands are offset so the two ranges cannot collide.
    constexpr int kCommandBase = 100000;

    constexpr int Cmd(Command c)
    {
        return kCommandBase + static_cast<int>(c);
    }

    enum Action
    {
        ACT_NONE = 0,
        ACT_NAV_MENU,
        ACT_MODE_STANDARD,
        ACT_MODE_SCIENTIFIC,
        ACT_MODE_PROGRAMMER,
        ACT_TOGGLE_PANEL,
        ACT_PANEL_HISTORY,
        ACT_PANEL_MEMORY,
        ACT_HISTORY_CLEAR,
        ACT_MEMORY_CLEAR_ALL,
        ACT_MEMORY_PANEL,
        ACT_SECOND,
        ACT_ANGLE_CYCLE,
        ACT_HYP,
        ACT_FE,
        ACT_TRIG_MENU,
        ACT_FUNC_MENU,
        ACT_BITWISE_MENU,
        ACT_SHIFT_MENU,
        ACT_WORDSIZE_CYCLE,
        ACT_BITBOARD,
        ACT_RADIX_HEX,
        ACT_RADIX_DEC,
        ACT_RADIX_OCT,
        ACT_RADIX_BIN,
        ACT_MODE_DATE,
        ACT_MODE_GRAPHING,
        ACT_GRAPH_ZOOM_IN,
        ACT_GRAPH_ZOOM_OUT,
        ACT_GRAPH_RESET,
        ACT_GRAPH_ADD,
        ACT_GRAPH_BACKSPACE,
        ACT_GRAPH_CLEAR,
        ACT_GRAPH_NEGATE,
        ACT_GRAPH_TAB_PLOT,
        ACT_GRAPH_TAB_EQUATIONS,
        ACT_GRAPH_TRACE,
        ACT_GRAPH_SHARE,
        ACT_GRAPH_OPTIONS,
        ACT_INEQ_MENU,
        ACT_GRAPH_ANGLE_RAD,
        ACT_GRAPH_ANGLE_DEG,
        ACT_GRAPH_ANGLE_GRAD,
        ACT_GRAPH_THEME_LIGHT,
        ACT_GRAPH_THEME_APP,
        ACT_GRAPH_THICKNESS,
        ACT_ALWAYS_ON_TOP,
        ACT_APP_THEME,
        ACT_SETTINGS,
        ACT_CONV_FROM_UNIT,
        ACT_CONV_TO_UNIT,
        ACT_CONV_FIELD_FROM,
        ACT_CONV_FIELD_TO,
        ACT_DATE_MODE_DIFF,
        ACT_DATE_MODE_OFFSET,
        ACT_DATE_ADD_SUBTRACT,
        ACT_MEM_STORE,
        ACT_MEM_RECALL,
        ACT_MEM_ADD,
        ACT_MEM_SUB,
        ACT_ABOUT_EXPAND,
        ACT_THEME_LIGHT,
        ACT_THEME_DARK,
        ACT_THEME_SYSTEM,
        // Ranged actions carry an index in their low digits. Execute() tests them
        // from the highest base down, so the ranges must not overlap AND the
        // tests must stay in descending order -- a lower base tested first
        // swallows every action above it.
        ACT_BIT_BASE = 2000,       // + bit index 0..63
        ACT_MEM_LOAD_BASE = 3000,  // + memory slot
        ACT_MEM_CLEAR_BASE = 3200, // + memory slot
        ACT_MEM_ADD_BASE = 3400,   // + memory slot
        ACT_MEM_SUB_BASE = 3600,   // + memory slot
        ACT_HIST_ITEM_BASE = 3800, // + history row
        ACT_CONV_CATEGORY_BASE = 4000, // + converter category id
        ACT_CONV_UNIT_FROM_BASE = 4100, // + unit index
        ACT_CONV_UNIT_TO_BASE = 4400, // + unit index
        ACT_DATE_FIELD_BASE = 4700,  // + date field index
        // Values chosen from a date picker. Encoded as
        //   offsets: +which*1000 + value
        //   month:   +3000 + dateIndex*100 + month
        //   day:     +4000 + dateIndex*100 + day
        //   year:    +5000 + dateIndex*200 + (year - m_dateYearBase[dateIndex])
        ACT_DATE_VALUE_BASE = 10000,
        ACT_CAL_PREV = 20000,
        ACT_CAL_NEXT,
        ACT_CAL_DAY_BASE = 20100, // + day of month
        ACT_GRAPH_SELECT_BASE = 21000, // + equation index
        ACT_GRAPH_REMOVE_BASE = 21100, // + equation index
        ACT_GRAPH_TEXT_BASE = 21200,   // + index into the keypad's character table
        ACT_GRAPH_FIELD_BASE = 21400,  // + window field 0..3
    };

    static_assert(ACT_MEM_SUB < ACT_BIT_BASE, "scalar actions must stay below the ranged actions");
    static_assert(ACT_BIT_BASE + 64 <= ACT_MEM_LOAD_BASE, "bit indices must not reach the memory range");
    static_assert(ACT_CONV_UNIT_FROM_BASE + 300 <= ACT_CONV_UNIT_TO_BASE, "unit lists must not overlap");
    static_assert(ACT_DATE_VALUE_BASE + 16000 < kCommandBase, "date values must stay below the engine commands");

    enum VIcon
    {
        VIcon_None = 0,
        VIcon_Menu,
        VIcon_Back,
        VIcon_History,
        VIcon_KeepOnTop,
        VIcon_KeepOnTopOn,
        VIcon_Backspace,
        VIcon_ChevronDown,
        VIcon_ChevronUp,
        VIcon_ChevronLeft,
        VIcon_ChevronRight,
        VIcon_Calendar,
        VIcon_Gear,
        VIcon_RadioOn,
        VIcon_RadioOff,
        VIcon_Palette,
        VIcon_Calculator,
        VIcon_ZoomIn,
        VIcon_ZoomOut,
        VIcon_ZoomReset,
        VIcon_Plus,
        VIcon_Minus,
        // Navigation category icons. The icon fonts do not carry every one of
        // these codepoints on every machine -- several showed as empty boxes --
        // so they are paths like the rest of the chrome.
        VIcon_Flask,
        VIcon_Curve,
        VIcon_Code,
        VIcon_Cup,
        VIcon_Ruler,
        VIcon_Scales,
        VIcon_Thermometer,
        VIcon_Flame,
        VIcon_AreaGrid,
        VIcon_Speed,
        VIcon_Clock,
        VIcon_Plug,
        VIcon_Data,
        VIcon_Gauge,
        VIcon_Angle,
        // Graphing chrome.
        VIcon_Share,
        VIcon_Trace,
        VIcon_GraphSettings,
        VIcon_Recenter,
        VIcon_Fx,
        VIcon_Enter,
        VIcon_Close,
    };

    int MeasureTextWidth(std::wstring_view text, int dip, int weight, int face);
    int VIconForGlyph(std::wstring_view label);
    void DrawVectorIcon(Gdiplus::Graphics& g, const RECT& rc, int id, COLORREF color, int sizeDip);

    struct Btn
    {
        std::wstring_view label;
        int action = ACT_NONE;
        Style style = Style::Operator;
        RECT rc{};
        bool icon = false;     // render with the Fluent icon font
        bool enabled = true;
        bool checked = false;
        bool leftAlign = false;  // nav rows and list entries read left-to-right
        bool dateField = false;  // draws a trailing calendar glyph and a rule
        bool combo = false;      // combo box: leading text, trailing chevron
        int vicon = 0;           // VIcon_*: drawn as a path instead of a glyph
        bool radio = false;      // draws a leading radio button
        bool accentText = false; // link-coloured label
        int textDip = 0;        // 0 = use the style default
    };

    struct KeyDef
    {
        const wchar_t* label;
        int action;
        Style style;
        bool icon;
        int textDip;
    };

    // ------------------------------------------------------- keypad layouts

    // Standard: 4 columns x 6 rows, matching the shipping app key for key.
    constexpr KeyDef kStandardKeys[] = {
        { L"%",           Cmd(Command::CommandPERCENT), Style::Operator, false, 0 },
        { L"CE",          Cmd(Command::CommandCENTR),   Style::Operator, false, 0 },
        { L"C",           Cmd(Command::CommandCLEAR),   Style::Operator, false, 0 },
        { L"",      Cmd(Command::CommandBACK),    Style::Operator, true,  0 },

        { L"¹⁄ₓ", Cmd(Command::CommandREC),  Style::Operator, false, 0 },
        { L"x²",     Cmd(Command::CommandSQR),     Style::Operator, false, 0 },
        { L"²√x", Cmd(Command::CommandSQRT),  Style::Operator, false, 0 },
        { L"÷",      Cmd(Command::CommandDIV),     Style::Operator, false, 22 },

        { L"7", Cmd(Command::Command7), Style::Number, false, 0 },
        { L"8", Cmd(Command::Command8), Style::Number, false, 0 },
        { L"9", Cmd(Command::Command9), Style::Number, false, 0 },
        { L"×", Cmd(Command::CommandMUL), Style::Operator, false, 22 },

        { L"4", Cmd(Command::Command4), Style::Number, false, 0 },
        { L"5", Cmd(Command::Command5), Style::Number, false, 0 },
        { L"6", Cmd(Command::Command6), Style::Number, false, 0 },
        { L"−", Cmd(Command::CommandSUB), Style::Operator, false, 22 },

        { L"1", Cmd(Command::Command1), Style::Number, false, 0 },
        { L"2", Cmd(Command::Command2), Style::Number, false, 0 },
        { L"3", Cmd(Command::Command3), Style::Number, false, 0 },
        { L"+", Cmd(Command::CommandADD), Style::Operator, false, 22 },

        { L"+/−", Cmd(Command::CommandSIGN), Style::Number, false, 0 },
        { L"0", Cmd(Command::Command0), Style::Number, false, 0 },
        { L".", Cmd(Command::CommandPNT), Style::Number, false, 0 },
        { L"=", Cmd(Command::CommandEQU), Style::Accent, false, 22 },
    };

    // Scientific: 5 columns x 7 rows. The first column carries the functions
    // that the "2nd" key swaps for their inverses.
    constexpr KeyDef kScientificKeys[] = {
        { L"2ⁿᵈ",  ACT_SECOND,                   Style::Toggle,   false, 15 },
        { L"π",         Cmd(Command::CommandPI),      Style::Operator, false, 0 },
        { L"e",              Cmd(Command::CommandEuler),   Style::Operator, false, 0 },
        { L"C",              Cmd(Command::CommandCLEAR),   Style::Operator, false, 0 },
        { L"",         Cmd(Command::CommandBACK),    Style::Operator, true,  0 },

        { L"x²",        Cmd(Command::CommandSQR),     Style::Operator, false, 0 },
        { L"¹⁄ₓ", Cmd(Command::CommandREC), Style::Operator, false, 0 },
        { L"|x|",            Cmd(Command::CommandAbs),     Style::Operator, false, 0 },
        { L"exp",            Cmd(Command::CommandEXP),     Style::Operator, false, 15 },
        { L"mod",            Cmd(Command::CommandMOD),     Style::Operator, false, 15 },

        { L"²√x",  Cmd(Command::CommandSQRT),    Style::Operator, false, 0 },
        { L"(",              Cmd(Command::CommandOPENP),   Style::Operator, false, 0 },
        { L")",              Cmd(Command::CommandCLOSEP),  Style::Operator, false, 0 },
        { L"n!",             Cmd(Command::CommandFAC),     Style::Operator, false, 0 },
        { L"÷",         Cmd(Command::CommandDIV),     Style::Operator, false, 22 },

        { L"xʸ",        Cmd(Command::CommandPWR),     Style::Operator, false, 0 },
        { L"7", Cmd(Command::Command7), Style::Number, false, 0 },
        { L"8", Cmd(Command::Command8), Style::Number, false, 0 },
        { L"9", Cmd(Command::Command9), Style::Number, false, 0 },
        { L"×", Cmd(Command::CommandMUL), Style::Operator, false, 22 },

        { L"10ˣ",       Cmd(Command::CommandPOW10),   Style::Operator, false, 0 },
        { L"4", Cmd(Command::Command4), Style::Number, false, 0 },
        { L"5", Cmd(Command::Command5), Style::Number, false, 0 },
        { L"6", Cmd(Command::Command6), Style::Number, false, 0 },
        { L"−", Cmd(Command::CommandSUB), Style::Operator, false, 22 },

        { L"log",            Cmd(Command::CommandLOG),     Style::Operator, false, 15 },
        { L"1", Cmd(Command::Command1), Style::Number, false, 0 },
        { L"2", Cmd(Command::Command2), Style::Number, false, 0 },
        { L"3", Cmd(Command::Command3), Style::Number, false, 0 },
        { L"+", Cmd(Command::CommandADD), Style::Operator, false, 22 },

        { L"ln",             Cmd(Command::CommandLN),      Style::Operator, false, 15 },
        { L"+/−", Cmd(Command::CommandSIGN), Style::Number, false, 0 },
        { L"0", Cmd(Command::Command0), Style::Number, false, 0 },
        { L".", Cmd(Command::CommandPNT), Style::Number, false, 0 },
        { L"=", Cmd(Command::CommandEQU), Style::Accent, false, 22 },
    };

    // The "2nd" replacements for the scientific function column.
    struct SecondSwap
    {
        int primary;
        const wchar_t* label;
        int action;
        int textDip;
    };

    constexpr SecondSwap kSecondSwaps[] = {
        { Cmd(Command::CommandSQR),   L"x³",             Cmd(Command::CommandCUB),      0 },
        { Cmd(Command::CommandSQRT),  L"³√x",       Cmd(Command::CommandCUBEROOT), 0 },
        { Cmd(Command::CommandPWR),   L"ʸ√x",       Cmd(Command::CommandROOT),     0 },
        { Cmd(Command::CommandPOW10), L"2ˣ",             Cmd(Command::CommandPOW2),     0 },
        { Cmd(Command::CommandLOG),   L"logᵧx",          Cmd(Command::CommandLogBaseY), 13 },
        { Cmd(Command::CommandLN),    L"eˣ",             Cmd(Command::CommandPOWE),     0 },
    };

    // Programmer: 5 columns x 6 rows, with the hex digits in the first column.
    constexpr KeyDef kProgrammerKeys[] = {
        { L"A", Cmd(Command::CommandA), Style::Number, false, 0 },
        { L"«", Cmd(Command::CommandLSHF), Style::Operator, false, 20 },
        { L"»", Cmd(Command::CommandRSHF), Style::Operator, false, 20 },
        { L"C",  Cmd(Command::CommandCLEAR), Style::Operator, false, 0 },
        { L"", Cmd(Command::CommandBACK), Style::Operator, true, 0 },

        { L"B", Cmd(Command::CommandB), Style::Number, false, 0 },
        { L"(", Cmd(Command::CommandOPENP), Style::Operator, false, 0 },
        { L")", Cmd(Command::CommandCLOSEP), Style::Operator, false, 0 },
        { L"%", Cmd(Command::CommandPERCENT), Style::Operator, false, 0 },
        { L"÷", Cmd(Command::CommandDIV), Style::Operator, false, 22 },

        { L"C", Cmd(Command::CommandC), Style::Number, false, 0 },
        { L"7", Cmd(Command::Command7), Style::Number, false, 0 },
        { L"8", Cmd(Command::Command8), Style::Number, false, 0 },
        { L"9", Cmd(Command::Command9), Style::Number, false, 0 },
        { L"×", Cmd(Command::CommandMUL), Style::Operator, false, 22 },

        { L"D", Cmd(Command::CommandD), Style::Number, false, 0 },
        { L"4", Cmd(Command::Command4), Style::Number, false, 0 },
        { L"5", Cmd(Command::Command5), Style::Number, false, 0 },
        { L"6", Cmd(Command::Command6), Style::Number, false, 0 },
        { L"−", Cmd(Command::CommandSUB), Style::Operator, false, 22 },

        { L"E", Cmd(Command::CommandE), Style::Number, false, 0 },
        { L"1", Cmd(Command::Command1), Style::Number, false, 0 },
        { L"2", Cmd(Command::Command2), Style::Number, false, 0 },
        { L"3", Cmd(Command::Command3), Style::Number, false, 0 },
        { L"+", Cmd(Command::CommandADD), Style::Operator, false, 22 },

        { L"F", Cmd(Command::CommandF), Style::Number, false, 0 },
        { L"+/−", Cmd(Command::CommandSIGN), Style::Number, false, 0 },
        { L"0", Cmd(Command::Command0), Style::Number, false, 0 },
        { L".", Cmd(Command::CommandPNT), Style::Number, false, 0 },
        { L"=", Cmd(Command::CommandEQU), Style::Accent, false, 22 },
    };

    struct MenuItem
    {
        std::wstring_view label;
        int action;
    };

    constexpr MenuItem kTrigMenu[] = {
        { L"sin",  Cmd(Command::CommandSIN) },   { L"cos",  Cmd(Command::CommandCOS) },
        { L"tan",  Cmd(Command::CommandTAN) },   { L"sec",  Cmd(Command::CommandSEC) },
        { L"csc",  Cmd(Command::CommandCSC) },   { L"cot",  Cmd(Command::CommandCOT) },
    };

    constexpr MenuItem kTrigMenuInverse[] = {
        { L"sin⁻¹", Cmd(Command::CommandASIN) }, { L"cos⁻¹", Cmd(Command::CommandACOS) },
        { L"tan⁻¹", Cmd(Command::CommandATAN) }, { L"sec⁻¹", Cmd(Command::CommandASEC) },
        { L"csc⁻¹", Cmd(Command::CommandACSC) }, { L"cot⁻¹", Cmd(Command::CommandACOT) },
    };

    constexpr MenuItem kHypMenu[] = {
        { L"sinh", Cmd(Command::CommandSINH) }, { L"cosh", Cmd(Command::CommandCOSH) },
        { L"tanh", Cmd(Command::CommandTANH) }, { L"sech", Cmd(Command::CommandSECH) },
        { L"csch", Cmd(Command::CommandCSCH) }, { L"coth", Cmd(Command::CommandCOTH) },
    };

    constexpr MenuItem kHypMenuInverse[] = {
        { L"sinh⁻¹", Cmd(Command::CommandASINH) }, { L"cosh⁻¹", Cmd(Command::CommandACOSH) },
        { L"tanh⁻¹", Cmd(Command::CommandATANH) }, { L"sech⁻¹", Cmd(Command::CommandASECH) },
        { L"csch⁻¹", Cmd(Command::CommandACSCH) }, { L"coth⁻¹", Cmd(Command::CommandACOTH) },
    };

    constexpr MenuItem kFuncMenu[] = {
        { L"|x|",      Cmd(Command::CommandAbs) },
        { L"⌊x⌋", Cmd(Command::CommandFloor) },
        { L"⌈x⌉", Cmd(Command::CommandCeil) },
        { L"rand",     Cmd(Command::CommandRand) },
        { L"dms",      Cmd(Command::CommandDMS) },
        { L"degrees",  Cmd(Command::CommandDegrees) },
    };

    constexpr MenuItem kBitwiseMenu[] = {
        { L"AND",  Cmd(Command::CommandAnd) },  { L"OR",   Cmd(Command::CommandOR) },
        { L"NOT",  Cmd(Command::CommandNot) },  { L"XOR",  Cmd(Command::CommandXor) },
        { L"NAND", Cmd(Command::CommandNand) }, { L"NOR",  Cmd(Command::CommandNor) },
    };

    constexpr MenuItem kShiftMenu[] = {
        { L"Arithmetic shift «", Cmd(Command::CommandLSHF) },
        { L"Arithmetic shift »", Cmd(Command::CommandRSHF) },
        { L"Logical shift »",    Cmd(Command::CommandRSHFL) },
        { L"Rotate circular «",  Cmd(Command::CommandROL) },
        { L"Rotate circular »",  Cmd(Command::CommandROR) },
        { L"Rotate through carry «", Cmd(Command::CommandROLC) },
        { L"Rotate through carry »", Cmd(Command::CommandRORC) },
    };
}

namespace
{
    // ------------------------------------------------------------- the app

    constexpr int kNavRowDip = 48;   // HamburgerHeight in App.xaml
    constexpr int kTitleDip = 20;    // SubtitleTextBlockStyle
    constexpr int kNavPaneDip = 256; // SplitViewOpenPaneLength in App.xaml
    constexpr int kExprRowDip = 22;
    constexpr int kResultRowDip = 62;
    constexpr int kStripRowDip = 32;
    constexpr int kMemRowDip = 34;
    // The shipping app leaves roughly 6px between keys and the same again around
    // the keypad, rather than running it to the window edge. AddGrid insets each
    // cell by kGapDip on every side, so adjacent keys end up 2 * kGapDip apart.
    constexpr int kGapDip = 3;
    constexpr int kPadDip = 3;
    constexpr int kRadiusDip = 4;
    constexpr int kPanelWidthDip = 300;
    constexpr int kDockThresholdDip = 640;

    enum class Panel
    {
        None,
        History,
        Memory
    };

    class CalcApp final : public ICalcDisplay
    {
    public:
        // ---- ICalcDisplay -------------------------------------------------
        void SetPrimaryDisplay(const std::wstring& text, bool isError) override
        {
            m_primary = text;
            m_isError = isError;
            Invalidate();
        }

        void SetIsInError(bool isError) override
        {
            m_isError = isError;
            Invalidate();
        }

        void SetExpressionDisplay(
            std::shared_ptr<std::vector<std::pair<std::wstring, int>>> const& tokens,
            std::shared_ptr<std::vector<std::shared_ptr<IExpressionCommand>>> const&) override
        {
            m_expression.clear();
            if (tokens)
            {
                for (const auto& token : *tokens)
                {
                    m_expression += token.first;
                }
            }
            Invalidate();
        }

        void SetParenthesisNumber(unsigned int count) override
        {
            m_openParens = count;
            Invalidate();
        }

        void SetMemorizedNumbers(const std::vector<std::wstring>& numbers) override
        {
            m_memory = numbers;
            Invalidate();
        }

        void OnHistoryItemAdded(unsigned int) override
        {
            m_historyScroll = 0;
            Invalidate();
        }

        void OnNoRightParenAdded() override
        {
        }

        void MaxDigitsReached() override
        {
            MessageBeep(MB_OK);
        }

        void BinaryOperatorReceived() override
        {
        }

        void MemoryItemChanged(unsigned int) override
        {
            Invalidate();
        }

        void InputChanged() override
        {
        }

        // ---- lifetime -----------------------------------------------------
        void Attach(HWND hwnd)
        {
            m_hwnd = hwnd;
            m_provider = std::make_unique<ResourceProvider>();
            m_manager = std::make_unique<CalculatorManager>(this, m_provider.get());
            m_manager->SetStandardMode();
            m_manager->SendCommand(Command::CommandDEG);
            m_manager->SetMemorizedNumbersString();
            // The first frame is already "arrived": only a mode change replays
            // the entrance.
            m_contentAnim.Set(1.0f);
        }

        Mode CurrentMode() const
        {
            return m_mode;
        }

        // Hands back whatever the mode being left was holding. The converter
        // is the only one that owns anything substantial -- its engine keeps
        // the category's unit list and the suggestion set -- but the layout
        // vectors have grown to whichever mode needed the most buttons, and
        // there is no reason to carry that into a mode that needs fewer.
        void ReleaseModeState(Mode leaving, Mode entering)
        {
            if (leaving == entering)
            {
                return;
            }
            if (leaving == Mode::Converter && entering != Mode::Converter)
            {
                m_converterModel.reset();
            }
            if (leaving == Mode::Graphing && entering != Mode::Graphing)
            {
                // The compiled programs go; the text stays, so the equations
                // come back as they were and recompile on their next paint.
                for (auto& equation : m_equations)
                {
                    equation.expression.Clear();
                    equation.valid = false;
                }
                m_graphOptionsOpen = false;
                m_graphField = -1;
            }
            m_buttons.clear();
            m_buttons.shrink_to_fit();
            m_menuButtons.clear();
            m_menuButtons.shrink_to_fit();
            m_menuItemStorage.clear();
            m_menuItemStorage.shrink_to_fit();
        }

        void SetMode(Mode mode)
        {
            if (m_mode == mode)
            {
                m_navOpen = false;
                m_navAnim.To(0.0f, kMotionNormalMs);
                EnsureFrameTimer();
                Relayout();
                return;
            }
            ReleaseModeState(m_mode, mode);
            m_mode = mode;
            m_navOpen = false;
            m_navAnim.To(0.0f, kMotionNormalMs);
            StartContentEnter();
            m_menuOpen = false;
            m_second = false;
            m_hyp = false;
            switch (mode)
            {
            case Mode::Standard:
                m_manager->SetStandardMode();
                break;
            case Mode::Scientific:
                m_manager->SetScientificMode();
                m_manager->SendCommand(m_angle);
                break;
            case Mode::Programmer:
                m_manager->SetProgrammerMode();
                m_manager->SendCommand(m_wordSize);
                ApplyRadix();
                break;
            case Mode::Graphing:
                EnsureGraphing();
                for (int i = 0; i < static_cast<int>(m_equations.size()); ++i)
                {
                    if (m_equations[i].expression.Empty() && !m_equations[i].text.empty())
                    {
                        RecompileEquation(i);
                    }
                }
                break;
            case Mode::Date:
                break;
            case Mode::Converter:
                EnsureConverter();
                m_converterModel->SetCategory(m_converterCategory);
                break;
            }
            if (mode != Mode::Programmer)
            {
                m_panel = (m_panel == Panel::None) ? Panel::None : m_panel;
            }
            Relayout();
        }

        void EnsureGraphing()
        {
            if (m_equations.empty())
            {
                m_equations.push_back(GraphEquation{});
                m_activeEquation = 0;
            }
        }

        void AppendToEquation(wchar_t c)
        {
            if (c == L'\0')
            {
                return;
            }
            EnsureGraphing();
            m_equations[m_activeEquation].text.push_back(c);
            RecompileEquation(m_activeEquation);
        }

        void BackspaceEquation()
        {
            EnsureGraphing();
            std::wstring& text = m_equations[m_activeEquation].text;
            if (!text.empty())
            {
                text.pop_back();
            }
            RecompileEquation(m_activeEquation);
        }

        void RecompileEquation(int index)
        {
            if (index < 0 || index >= static_cast<int>(m_equations.size()))
            {
                return;
            }
            GraphEquation& equation = m_equations[index];
            equation.valid = equation.expression.Compile(equation.text);
        }

        // Zooms about the centre of the view, as the zoom buttons do.
        void ScaleGraph(double factor)
        {
            const double centreX = (m_graphXMin + m_graphXMax) * 0.5;
            const double centreY = (m_graphYMin + m_graphYMax) * 0.5;
            const double halfX = (m_graphXMax - m_graphXMin) * 0.5 * factor;
            const double halfY = (m_graphYMax - m_graphYMin) * 0.5 * factor;
            // Stop short of the range where the tick arithmetic loses meaning.
            if (halfX < 1e-6 || halfX > 1e6)
            {
                return;
            }
            m_graphXMin = centreX - halfX;
            m_graphXMax = centreX + halfX;
            m_graphYMin = centreY - halfY;
            m_graphYMax = centreY + halfY;
        }

        // The four window fields show the live view unless one is being typed
        // into, in which case that one shows what has been typed so far.
        std::wstring GraphFieldText(int index) const
        {
            if (index == m_graphField && !m_graphFieldText[index].empty())
            {
                return m_graphFieldText[index];
            }
            const double value = (index == 0) ? m_graphXMin : (index == 1) ? m_graphXMax
                                 : (index == 2) ? m_graphYMin : m_graphYMax;
            std::wstring text = CalcEngine::NumericString::FixedPoint(value, 4);
            while (!text.empty() && text.back() == L'0')
            {
                text.pop_back();
            }
            if (!text.empty() && text.back() == L'.')
            {
                text.pop_back();
            }
            return text;
        }

        // Applies whatever was typed into a field, ignoring anything that does
        // not leave a usable range.
        void CommitGraphField()
        {
            if (m_graphField < 0 || m_graphFieldText[m_graphField].empty())
            {
                m_graphField = -1;
                return;
            }
            const double value = CalcEngine::NumericString::ToDouble(m_graphFieldText[m_graphField]);
            double xMin = m_graphXMin, xMax = m_graphXMax, yMin = m_graphYMin, yMax = m_graphYMax;
            switch (m_graphField)
            {
            case 0: xMin = value; break;
            case 1: xMax = value; break;
            case 2: yMin = value; break;
            default: yMax = value; break;
            }
            if (xMax - xMin > 1e-6 && yMax - yMin > 1e-6 && (xMax - xMin) < 1e9 && (yMax - yMin) < 1e9)
            {
                m_graphXMin = xMin;
                m_graphXMax = xMax;
                m_graphYMin = yMin;
                m_graphYMax = yMax;
            }
            m_graphFieldText[m_graphField].clear();
            m_graphField = -1;
        }

        void ResetGraph()
        {
            m_graphXMin = -10.0;
            m_graphXMax = 10.0;
            m_graphYMin = -10.0;
            m_graphYMax = 10.0;
        }

        std::wstring_view ModeName() const
        {
            switch (m_mode)
            {
            case Mode::Scientific:
                return L"Scientific";
            case Mode::Graphing:
                return L"Graphing";
            case Mode::Programmer:
                return L"Programmer";
            case Mode::Date:
                return L"Date calculation";
            case Mode::Converter:
                return m_converterModel ? std::wstring_view(m_converterModel->CategoryName()) : std::wstring_view(L"Converter");
            default:
                return L"Standard";
            }
        }

        void EnsureConverter()
        {
            if (!m_converterModel)
            {
                m_converterModel = std::make_unique<ConverterModel>();
            }
        }

        void SetConverterCategory(int categoryId)
        {
            EnsureConverter();
            m_converterCategory = categoryId;
            m_mode = Mode::Converter;
            m_converterModel->SetCategory(categoryId);
            m_navOpen = false;
            m_menuOpen = false;
            ForceClosePanel();
            m_navAnim.To(0.0f, kMotionNormalMs);
            StartContentEnter();
            Relayout();
        }

        bool NavVisible() const
        {
            return m_navOpen || m_navAnim.Value() > 0.002f;
        }

        bool SettingsVisible() const
        {
            return m_settingsOpen || m_settingsAnim.Value() > 0.002f;
        }

        bool AnimationsRunning() const
        {
            return m_navAnim.Active() || m_settingsAnim.Active() || m_panelAnim.Active() || m_menuAnim.Active() || m_contentAnim.Active()
                || m_themeAnim.Active() || m_toastAnim.Active()
                || m_pressAnim.Active() || m_hoverIn.Active() || m_hoverOut.Active();
        }

        // Frames are paced by the message loop against the compositor, so this
        // only has to wake it: starting an animation from a message handler
        // means the loop re-checks AnimationsRunning() on its next pass.
        void EnsureFrameTimer()
        {
            if (m_hwnd && AnimationsRunning())
            {
                Invalidate();
            }
        }

        // One animation frame: anything that moves a control moves its hit
        // target too, so those frames relayout rather than just repaint.
        void FrameTick()
        {
            if (!m_hwnd)
            {
                return;
            }
            if (m_panelClosing && !m_panelAnim.Active())
            {
                m_panel = Panel::None;
                m_panelClosing = false;
                Relayout();
            }
            if (m_navAnim.Active() || m_settingsAnim.Active() || m_panelAnim.Active())
            {
                Relayout();
            }
            else
            {
                Invalidate();
            }
            UpdateWindow(m_hwnd);
        }

        // Cross-fades the highlight from the previously hovered key to the new
        // one, so neither snaps.
        void SetHover(int action)
        {
            if (action == m_hoverInAction)
            {
                return;
            }
            m_hoverOutAction = m_hoverInAction;
            m_hoverOut.Set(m_hoverIn.Value());
            if (m_hoverOutAction != ACT_NONE)
            {
                m_hoverOut.To(0.0f, kMotionFastMs, EaseStandard);
            }
            m_hoverInAction = action;
            m_hoverIn.Set(0.0f);
            if (action != ACT_NONE)
            {
                m_hoverIn.To(1.0f, kMotionFastMs, EaseStandard);
            }
            EnsureFrameTimer();
        }

        // The history/memory panel opens and closes with the same slide. On
        // the way out it has to stay in the layout until the slide finishes,
        // so m_panel is held and m_panelClosing marks it as on its way.
        void OpenPanel(Panel panel)
        {
            const bool hidden = (m_panel == Panel::None);
            m_panel = panel;
            m_panelClosing = false;
            m_historyScroll = 0;
            if (hidden)
            {
                m_panelAnim.Set(0.0f);
            }
            m_panelAnim.To(1.0f, kMotionNormalMs);
            EnsureFrameTimer();
        }

        void ClosePanel()
        {
            if (m_panel == Panel::None || m_panelClosing)
            {
                return;
            }
            m_panelClosing = true;
            m_panelAnim.To(0.0f, kMotionNormalMs, EaseStandard);
            EnsureFrameTimer();
        }

        // Runs once, on the frame after everything has come to rest. A panel
        // on its way out is only dropped from the layout here, which is what
        // keeps it on screen for the whole of its closing slide.
        void OnAnimationsSettled()
        {
            if (m_panelClosing)
            {
                m_panel = Panel::None;
                m_panelClosing = false;
                Relayout();
                // The keypad is not laid out while the panel covers it, so it
                // has nothing on screen to come back from. Bring it in the way
                // any other page arrives rather than letting it appear whole.
                StartContentEnter();
            }
            Invalidate();
        }

        // Drops the panel without the slide, for the cases where the whole
        // frame is being rebuilt anyway and there would be nothing to watch.
        void ForceClosePanel()
        {
            m_panel = Panel::None;
            m_panelClosing = false;
            m_panelAnim.Set(0.0f);
        }

        // A short confirmation over the plot. It holds for most of its life
        // and then goes, rather than fading from the moment it appears.
        void ShowToast(std::wstring text)
        {
            m_toastText = std::move(text);
            m_toastAnim.Set(1.0f);
            m_toastAnim.To(0.0f, 1800, EaseAccelerate);
            EnsureFrameTimer();
        }

        // Content entering the frame rises slightly as it fades in.
        void StartContentEnter()
        {
            m_contentAnim.Set(0.0f);
            m_contentAnim.To(1.0f, kMotionNormalMs);
            EnsureFrameTimer();
        }

        void Invalidate()
        {
            if (m_hwnd)
            {
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        }

        // ---- engine dispatch ----------------------------------------------
        void Send(Command command)
        {
            try
            {
                m_manager->SendCommand(command);
            }
            catch (...)
            {
                // The engine signals domain errors by throwing; it has already
                // pushed an error string to the display by this point.
            }
            Invalidate();
        }

        void ApplyRadix()
        {
            switch (m_radix)
            {
            case RadixType::Hex:
                Send(Command::CommandHex);
                break;
            case RadixType::Decimal:
                Send(Command::CommandDec);
                break;
            case RadixType::Octal:
                Send(Command::CommandOct);
                break;
            case RadixType::Binary:
                Send(Command::CommandBin);
                break;
            }
        }

        std::wstring RadixText(uint32_t radix) const
        {
            try
            {
                return m_manager->GetResultForRadix(radix, 64, true);
            }
            catch (...)
            {
                return std::wstring();
            }
        }

        int WordSizeBits() const
        {
            switch (m_wordSize)
            {
            case Command::CommandByte:
                return 8;
            case Command::CommandWord:
                return 16;
            case Command::CommandDword:
                return 32;
            default:
                return 64;
            }
        }

        const wchar_t* WordSizeName() const
        {
            switch (m_wordSize)
            {
            case Command::CommandByte:
                return L"BYTE";
            case Command::CommandWord:
                return L"WORD";
            case Command::CommandDword:
                return L"DWORD";
            default:
                return L"QWORD";
            }
        }

        void Relayout()
        {
            if (!m_hwnd)
            {
                return;
            }
            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            BuildLayout(rc.right - rc.left, rc.bottom - rc.top);
            Invalidate();
        }

        // Members are reached by the window procedure and the painter.
        HWND m_hwnd = nullptr;
        std::unique_ptr<ResourceProvider> m_provider;
        std::unique_ptr<CalculatorManager> m_manager;

        std::wstring m_primary = L"0";
        std::wstring m_expression;
        std::vector<std::wstring> m_memory;
        bool m_isError = false;
        unsigned int m_openParens = 0;

        Mode m_mode = Mode::Standard;
        std::unique_ptr<ConverterModel> m_converterModel;
        DateCalcModel m_dateModel;
        int m_converterCategory = 4; // Volume, the first converter category
        bool m_alwaysOnTop = false;
        int m_themeChoice = 0; // 0 system, 1 light, 2 dark
        bool m_themeExpanded = false;
        bool m_aboutExpanded = false;
        bool m_settingsOpen = false;
        bool m_second = false;
        bool m_hyp = false;
        bool m_fe = false;
        Command m_angle = Command::CommandDEG;
        Command m_wordSize = Command::CommandQword;
        RadixType m_radix = RadixType::Decimal;
        bool m_bitBoard = false;

        Panel m_panel = Panel::None;
        bool m_panelClosing = false; // panel is sliding back out but still laid out
        bool m_docked = false;
        bool m_navOpen = false;
        bool m_menuOpen = false;
        std::vector<Btn> m_buttons;
        // The navigation pane and the settings page are overlay surfaces drawn
        // above the mode content, which is what lets them slide over it.
        std::vector<Btn> m_navButtons;

        Anim m_navAnim;      // navigation pane, 0 closed .. 1 open
        Anim m_settingsAnim; // settings page
        Anim m_panelAnim;    // history / memory panel
        Anim m_themeAnim;    // outgoing theme, fading out over the new one
        Anim m_toastAnim;    // the confirmation over the plot, holding then fading
        Anim m_menuAnim;     // dropdown flyout
        Anim m_contentAnim;  // mode content entering
        Anim m_pressAnim;    // pointer-down shrink on the pressed key
        Anim m_hoverIn;
        Anim m_hoverOut;
        int m_hoverInAction = ACT_NONE;
        int m_hoverOutAction = ACT_NONE;
        std::vector<Btn> m_menuButtons;
        std::vector<MenuItem> m_menuItemStorage;
        std::vector<std::wstring> m_menuLabelStorage;
        RECT m_menuAnchor{};
        int m_menuScroll = 0;
        int m_menuContentHeight = 0;
        RECT m_menuRect{};
        RECT m_panelRect{};
        int m_panelTop = 0; // where the panel sits with the slide finished
        RECT m_displayRect{};
        RECT m_exprRect{};
        RECT m_titleRect{};
        int m_navPaneWidth = 0;

        // ---------------------------------------------------------- graphing
        struct GraphEquation
        {
            std::wstring text;
            Graphing::Expression expression;
            int colour = 0;   // index into the equation palette
            bool visible = true;
            bool valid = false;
        };

        std::vector<GraphEquation> m_equations;
        int m_activeEquation = 0;
        // The view window. The shipping app opens on a symmetric range and
        // keeps the axes square, which is what makes a circle look round.
        double m_graphXMin = -10.0;
        double m_graphXMax = 10.0;
        double m_graphYMin = -10.0;
        double m_graphYMax = 10.0;
        RECT m_graphRect{};
        RECT m_graphEquationRect{};
        std::wstring m_toastText;
        // The shipping app shows either the plot or the equation editor, with a
        // two-tab toggle in the title bar between them, rather than stacking
        // both. It opens on the plot.
        bool m_graphEquationsView = false;
        // Graph options, as the shipping pane offers them.
        bool m_graphOptionsOpen = false;
        Graphing::AngleMode m_graphAngle = Graphing::AngleMode::Radians;
        int m_graphThickness = 1;      // 0 thin, 1 medium, 2 thick
        bool m_graphAlwaysLight = true;
        int m_graphField = -1;         // which window field is being edited
        std::wstring m_graphFieldText[4];
        RECT m_graphOptionsRect{};
        bool m_graphDragging = false;
        POINT m_graphDragFrom{};
        double m_graphDragXMin = 0.0;
        double m_graphDragYMin = 0.0;
        RECT m_settingsAppearanceRect{};
        RECT m_settingsAboutRect{};
        RECT m_settingsThemePanel{};
        RECT m_settingsAboutPanel{};
        RECT m_settingsFeedbackRect{};
        RECT m_settingsContributeRect{};
        struct NavGlyph
        {
            std::wstring_view glyph;
            RECT rc;
            bool isHeader;
        };
        std::vector<NavGlyph> m_navGlyphs;
        int m_navContentHeight = 0;
        int m_dateYearBase[3] = { 1970, 1970, 1970 };
        // The date picker reuses the flyout surface, laid out as a month grid.
        bool m_menuIsCalendar = false;
        CivilDate m_calendarMonth;
        int m_calendarField = 0;
        int m_navScroll = 0;
        int m_hot = -1;
        int m_pressed = -1;
        int m_hotNav = -1;
        int m_pressedNav = -1;
        int m_navSlide = 0;
        int m_hotMenu = -1;
        int m_historyScroll = 0;
        std::vector<RECT> m_historyRects;
        std::vector<RECT> m_memoryRects;

        void BuildLayout(int width, int height);

        // Geometry and text the converter and date painters need. The date
        // segment labels are held here because Btn::label is a view.
        std::wstring m_dateFieldText[16];
        std::wstring_view m_dateCaptions[3];
        RECT m_dateCaptionRects[3]{};
        RECT m_convFromValue{};
        RECT m_convToValue{};
        RECT m_convSuggestions{};
        RECT m_dateResultRect{};
        RECT m_dateSecondaryRect{};

    private:
        void AddGrid(const KeyDef* keys, size_t count, int cols, int rows, RECT area);
        void BuildConverterLayout(int contentRight, int height, int y);
        void BuildDateLayout(int contentRight, int height, int y);
        void BuildGraphingLayout(int contentRight, int height, int y);
        void BuildGraphOptions(int contentRight, int height);
        void BuildNavLayout(int width, int height);
        void BuildSettingsLayout(int width, int height);

        // The overlay surfaces are independent of which mode is showing, so
        // every exit path from BuildLayout ends here.
        void BuildOverlayLayout(int width, int height)
        {
            if (NavVisible())
            {
                BuildNavLayout(width, height);
            }
            else if (SettingsVisible())
            {
                BuildSettingsLayout(width, height);
            }
        }

        // Adds an overlay row, shifted by the pane's current slide offset.
        void PushNav(Btn button)
        {
            button.rc.left -= m_navSlide;
            button.rc.right -= m_navSlide;
            m_navButtons.push_back(std::move(button));
        }



    public:
        void OpenDateFieldMenu(int fieldIndex);
        void OpenCalendar(int dateIndex);
        void LayoutCalendar();
        void LayoutMenu();

        CivilDate& DateForIndex(int index)
        {
            return (index == 0) ? m_dateModel.From() : (index == 1) ? m_dateModel.To() : m_dateModel.Start();
        }
    };

    CalcApp g_app;
}

namespace
{
    void CalcApp::AddGrid(const KeyDef* keys, size_t count, int cols, int rows, RECT area)
    {
        const int gap = Dp(kGapDip);
        const int w = area.right - area.left;
        const int h = area.bottom - area.top;
        for (size_t i = 0; i < count; ++i)
        {
            const int r = static_cast<int>(i) / cols;
            const int c = static_cast<int>(i) % cols;
            Btn b;
            b.label = keys[i].label;
            b.action = keys[i].action;
            b.style = keys[i].style;
            b.icon = keys[i].icon;
            b.textDip = keys[i].textDip;

            // "2nd" swaps the scientific function column for its inverses.
            if (m_second && m_mode == Mode::Scientific)
            {
                for (const auto& swap : kSecondSwaps)
                {
                    if (swap.primary == b.action)
                    {
                        b.label = swap.label;
                        b.action = swap.action;
                        if (swap.textDip)
                        {
                            b.textDip = swap.textDip;
                        }
                        break;
                    }
                }
            }
            if (b.action == ACT_SECOND)
            {
                b.checked = m_second;
            }

            // Hex digits are only meaningful while the radix can represent them.
            if (m_mode == Mode::Programmer)
            {
                const int digit = b.action - kCommandBase;
                if (digit >= static_cast<int>(Command::CommandA) && digit <= static_cast<int>(Command::CommandF))
                {
                    b.enabled = (m_radix == RadixType::Hex);
                }
                else if (digit >= static_cast<int>(Command::Command2) && digit <= static_cast<int>(Command::Command9))
                {
                    const int value = digit - static_cast<int>(Command::Command0);
                    const int base = (m_radix == RadixType::Hex) ? 16 : (m_radix == RadixType::Decimal) ? 10 : (m_radix == RadixType::Octal) ? 8 : 2;
                    b.enabled = value < base;
                }
                else if (digit == static_cast<int>(Command::Command1))
                {
                    b.enabled = true;
                }
                else if (digit == static_cast<int>(Command::CommandPNT))
                {
                    b.enabled = false; // programmer mode is integer-only
                }
            }

            b.rc.left = area.left + MulDiv(w, c, cols) + gap;
            b.rc.right = area.left + MulDiv(w, c + 1, cols) - gap;
            b.rc.top = area.top + MulDiv(h, r, rows) + gap;
            b.rc.bottom = area.top + MulDiv(h, r + 1, rows) - gap;
            m_buttons.push_back(std::move(b));
        }
    }

    void CalcApp::BuildLayout(int width, int height)
    {
        m_buttons.clear();
        m_historyRects.clear();
        m_memoryRects.clear();

        m_navButtons.clear();

        const int pad = Dp(kPadDip);
        const int gap = Dp(kGapDip);

        m_docked = (m_panel != Panel::None) && (width >= Dp(kDockThresholdDip));
        const int contentRight = m_docked ? width - Dp(kPanelWidthDip) : width;
        m_panelRect = m_docked ? RECT{ contentRight, Dp(kNavRowDip), width, height } : RECT{ 0, 0, width, height };

        // The panel slides in: from the right edge when docked beside the
        // keypad, upward from below when it covers it.
        const int panelSlide = static_cast<int>((1.0f - m_panelAnim.Value()) * static_cast<float>(Dp(m_docked ? kPanelWidthDip : 28)));

        int y = 0;

        // Title bar row, laid out as MainPage.xaml has it: a 48px hamburger, the
        // mode name, and the keep-on-top button immediately after the title
        // rather than off at the window edge. History is the only trailing item.
        const int navH = Dp(kNavRowDip);
        {
            const int centre = navH / 2;
            const int iconHalf = Dp(16); // SquareIconButtonStyle is 32x32

            Btn menu;
            menu.action = ACT_NAV_MENU;
            menu.style = Style::Flat;
            menu.vicon = VIcon_Menu;
            menu.textDip = 16;
            menu.rc = { Dp(2), centre - Dp(22), Dp(46), centre + Dp(22) };
            m_buttons.push_back(menu);

            const int titleLeft = Dp(48);
            const int titleWidth = MeasureTextWidth(ModeName(), kTitleDip, FW_SEMIBOLD, 0);
            m_titleRect = { titleLeft, 0, titleLeft + titleWidth, navH };

            int right = contentRight - Dp(6);

            if (m_mode == Mode::Graphing)
            {
                // A segmented pair of tabs, the plot on the left and the
                // equation editor on the right, as the shipping title bar has.
                const int tabWidth = Dp(38);
                const int tabHeight = Dp(30);
                const int tabTop = centre - tabHeight / 2;
                for (int i = 0; i < 2; ++i)
                {
                    Btn tab;
                    tab.action = (i == 0) ? ACT_GRAPH_TAB_PLOT : ACT_GRAPH_TAB_EQUATIONS;
                    tab.style = Style::Toggle;
                    tab.vicon = (i == 0) ? VIcon_Curve : VIcon_Fx;
                    tab.textDip = 15;
                    tab.checked = (i == 0) ? !m_graphEquationsView : m_graphEquationsView;
                    const int tabLeft = right - tabWidth * (2 - i);
                    tab.rc = { tabLeft, tabTop, tabLeft + tabWidth, tabTop + tabHeight };
                    m_buttons.push_back(tab);
                }
                right -= tabWidth * 2 + Dp(6);
            }

            const bool calculatorMode = (m_mode == Mode::Standard || m_mode == Mode::Scientific || m_mode == Mode::Programmer);
            if (calculatorMode)
            {
                Btn hist;
                hist.action = ACT_TOGGLE_PANEL;
                hist.style = Style::Flat;
                hist.vicon = VIcon_History;
                hist.textDip = 16;
                hist.checked = (m_panel != Panel::None);
                hist.rc = { right - iconHalf * 2, centre - iconHalf, right, centre + iconHalf };
                m_buttons.push_back(hist);
                right -= iconHalf * 2 + Dp(4);
            }

            // 10px after the title, per SquareIconButtonStyle's margin, but never
            // so far right that it collides with the history toggle.
            Btn onTop;
            onTop.action = ACT_ALWAYS_ON_TOP;
            onTop.style = Style::Flat;
            onTop.vicon = m_alwaysOnTop ? VIcon_KeepOnTopOn : VIcon_KeepOnTop;
            onTop.textDip = 16;
            onTop.checked = m_alwaysOnTop;
            const int onTopLeft = (std::min)(titleLeft + titleWidth + Dp(10), right - iconHalf * 2);
            onTop.rc = { onTopLeft, centre - iconHalf, onTopLeft + iconHalf * 2, centre + iconHalf };
            m_buttons.push_back(onTop);
        }
        y += navH;

        if (m_mode == Mode::Converter || m_mode == Mode::Date || m_mode == Mode::Graphing)
        {
            if (m_mode == Mode::Converter)
            {
                BuildConverterLayout(contentRight, height, y);
            }
            else if (m_mode == Mode::Graphing)
            {
                BuildGraphingLayout(contentRight, height, y);
            }
            else
            {
                BuildDateLayout(contentRight, height, y);
            }
            BuildOverlayLayout(width, height);
            return;
        }

        // Display.
        m_exprRect = { pad, y, contentRight - Dp(12), y + Dp(kExprRowDip) };
        y += Dp(kExprRowDip);
        m_displayRect = { pad, y, contentRight - Dp(12), y + Dp(kResultRowDip) };
        y += Dp(kResultRowDip) + Dp(4);

        // When the panel cannot dock beside the keypad it covers it instead,
        // so the hidden chrome is not laid out or hit tested at all.
        const bool overlay = (m_panel != Panel::None) && !m_docked;
        m_panelTop = y;
        if (overlay)
        {
            m_panelRect = { 0, y + panelSlide, width, height };
        }
        else if (m_docked)
        {
            m_panelRect.left += panelSlide;
            m_panelRect.right += panelSlide;
        }
        else
        {
            // Programmer radix readouts.
            if (m_mode == Mode::Programmer)
            {
                // The readouts are plain rows: the active radix is marked by a short
                // accent bar at the leading edge, not by filling the whole row.
                const int rowH = Dp(26);
                const RadixType radices[] = { RadixType::Hex, RadixType::Decimal, RadixType::Octal, RadixType::Binary };
                const int actions[] = { ACT_RADIX_HEX, ACT_RADIX_DEC, ACT_RADIX_OCT, ACT_RADIX_BIN };
                for (int i = 0; i < 4; ++i)
                {
                    Btn b;
                    b.action = actions[i];
                    b.style = Style::Bit; // hover-only fill
                    b.checked = (m_radix == radices[i]);
                    b.rc = { pad + Dp(2), y, contentRight - Dp(4), y + rowH - Dp(2) };
                    m_buttons.push_back(b);
                    y += rowH;
                }
                y += Dp(6);

                // Word size and the bit-flip board toggle.
                const int stripH = Dp(kStripRowDip);
                Btn word;
                word.label = WordSizeName();
                word.action = ACT_WORDSIZE_CYCLE;
                word.style = Style::Flat;
                word.textDip = 13;
                word.rc = { pad + Dp(2), y, pad + Dp(80), y + stripH - gap };
                m_buttons.push_back(word);

                Btn bits;
                bits.label = L"";
                bits.action = ACT_BITBOARD;
                bits.style = Style::Flat;
                bits.icon = true;
                bits.checked = m_bitBoard;
                bits.textDip = 14;
                bits.rc = { pad + Dp(86), y, pad + Dp(126), y + stripH - gap };
                m_buttons.push_back(bits);

                Btn bitwise;
                bitwise.label = L"Bitwise ";
                bitwise.action = ACT_BITWISE_MENU;
                bitwise.style = Style::Flat;
                bitwise.textDip = 13;
                bitwise.rc = { pad + Dp(132), y, pad + Dp(214), y + stripH - gap };
                m_buttons.push_back(bitwise);

                Btn shift;
                shift.label = L"Bit shift ";
                shift.action = ACT_SHIFT_MENU;
                shift.style = Style::Flat;
                shift.textDip = 13;
                shift.rc = { pad + Dp(220), y, (std::min)(contentRight - Dp(4), pad + Dp(310)), y + stripH - gap };
                m_buttons.push_back(shift);
                y += stripH;

                if (m_bitBoard)
                {
                    // Four rows of sixteen bits, most-significant first, trimmed to
                    // the active word size.
                    const int bitRows = 4;
                    const int boardH = Dp(18) * bitRows;
                    const int bits64 = WordSizeBits();
                    const std::wstring binary = RadixText(2);
                    std::wstring digits;
                    for (wchar_t ch : binary)
                    {
                        if (ch == L'0' || ch == L'1')
                        {
                            digits += ch;
                        }
                    }
                    for (int row = 0; row < bitRows; ++row)
                    {
                        for (int col = 0; col < 16; ++col)
                        {
                            const int index = 63 - (row * 16 + col);
                            Btn b;
                            b.action = ACT_BIT_BASE + index;
                            b.style = Style::Bit;
                            b.enabled = index < bits64;
                            const size_t fromEnd = static_cast<size_t>(index);
                            const bool on = fromEnd < digits.size() && digits[digits.size() - 1 - fromEnd] == L'1';
                            b.label = on ? L"1" : L"0";
                            b.checked = on;
                            b.textDip = 11;
                            b.rc.left = pad + MulDiv(contentRight - pad * 2, col, 16);
                            b.rc.right = pad + MulDiv(contentRight - pad * 2, col + 1, 16);
                            b.rc.top = y + Dp(18) * row;
                            b.rc.bottom = b.rc.top + Dp(18);
                            m_buttons.push_back(b);
                        }
                    }
                    y += boardH + Dp(2);
                }
            }

            // Scientific control strip. The shipping app puts the angle unit and F-E
            // above the memory row, and the two function menus below it.
            if (m_mode == Mode::Scientific)
            {
                const int stripH = Dp(kStripRowDip);
            
                Btn angle;
                angle.label = (m_angle == Command::CommandDEG) ? L"DEG" : (m_angle == Command::CommandRAD) ? L"RAD" : L"GRAD";
                angle.action = ACT_ANGLE_CYCLE;
                angle.style = Style::Flat;
                angle.textDip = 13;
                angle.rc = { pad + Dp(8), y, pad + Dp(64), y + stripH - gap };
                m_buttons.push_back(angle);
            
                Btn fe;
                fe.label = L"F-E";
                fe.action = ACT_FE;
                fe.style = Style::Toggle;
                fe.checked = m_fe;
                fe.textDip = 13;
                fe.rc = { pad + Dp(70), y, pad + Dp(126), y + stripH - gap };
                m_buttons.push_back(fe);
                y += stripH;
            }

            // Memory strip.
            {
                const int memH = Dp(kMemRowDip);
                const bool hasMemory = !m_memory.empty();
                // The memory strip drives the manager's slot list: each button acts on
                // the newest slot, exactly as the shipping app does.
                const KeyDef memKeys[] = {
                    { L"MC", ACT_MEMORY_CLEAR_ALL,  Style::Flat, false, 13 },
                    { L"MR", ACT_MEM_RECALL,        Style::Flat, false, 13 },
                    { L"M+", ACT_MEM_ADD,           Style::Flat, false, 13 },
                    { L"M−", ACT_MEM_SUB,      Style::Flat, false, 13 },
                    { L"MS", ACT_MEM_STORE,         Style::Flat, false, 13 },
                    { L"M", ACT_MEMORY_PANEL, Style::Flat, false, 13 },
                };
                const int usable = contentRight - pad * 2;
                for (int i = 0; i < 6; ++i)
                {
                    Btn b;
                    b.label = memKeys[i].label;
                    b.action = memKeys[i].action;
                    b.style = Style::Flat;
                    b.textDip = memKeys[i].textDip;
                    // Everything except MS needs a stored slot to act on.
                    if (i != 4)
                    {
                        b.enabled = hasMemory;
                    }
                    b.rc.left = pad + MulDiv(usable, i, 6) + gap;
                    b.rc.right = pad + MulDiv(usable, i + 1, 6) - gap;
                    b.rc.top = y;
                    b.rc.bottom = y + memH - gap;
                    m_buttons.push_back(b);
                }
                y += memH;
            }

            // Trigonometry and Function menus sit below the memory row.
            if (m_mode == Mode::Scientific)
            {
                const int stripH = Dp(kStripRowDip);
                Btn trig;
                trig.label = L"Trigonometry ";
                trig.action = ACT_TRIG_MENU;
                trig.style = Style::Flat;
                trig.textDip = 13;
                trig.rc = { pad + Dp(8), y + gap, pad + Dp(152), y + stripH - gap };
                m_buttons.push_back(trig);

                Btn func;
                func.label = L"Function ";
                func.action = ACT_FUNC_MENU;
                func.style = Style::Flat;
                func.textDip = 13;
                func.rc = { pad + Dp(158), y + gap, pad + Dp(268), y + stripH - gap };
                m_buttons.push_back(func);
                y += stripH + Dp(2);
            }

            // Keypad fills the rest.
            RECT keypad{ pad, y, contentRight - pad, height - pad };
            if (keypad.bottom < keypad.top)
            {
                keypad.bottom = keypad.top;
            }
            switch (m_mode)
            {
            case Mode::Standard:
                AddGrid(kStandardKeys, ARRAYSIZE(kStandardKeys), 4, 6, keypad);
                break;
            case Mode::Scientific:
                AddGrid(kScientificKeys, ARRAYSIZE(kScientificKeys), 5, 7, keypad);
                break;
            case Mode::Programmer:
                AddGrid(kProgrammerKeys, ARRAYSIZE(kProgrammerKeys), 5, 6, keypad);
                break;
            }

        }

        // History / memory panel: tabs, rows and the trailing clear command.
        if (m_panel != Panel::None)
        {
            const RECT area = m_panelRect;
            const int halfW = (area.right - area.left) / 2;

            Btn tabHistory;
            tabHistory.label = L"History";
            tabHistory.action = ACT_PANEL_HISTORY;
            tabHistory.style = Style::Toggle;
            tabHistory.checked = (m_panel == Panel::History);
            tabHistory.textDip = 13;
            tabHistory.rc = { area.left + Dp(8), area.top + Dp(4), area.left + halfW - Dp(2), area.top + Dp(34) };
            m_buttons.push_back(tabHistory);

            Btn tabMemory;
            tabMemory.label = L"Memory";
            tabMemory.action = ACT_PANEL_MEMORY;
            tabMemory.style = Style::Toggle;
            tabMemory.checked = (m_panel == Panel::Memory);
            tabMemory.textDip = 13;
            tabMemory.rc = { area.left + halfW + Dp(2), area.top + Dp(4), area.right - Dp(8), area.top + Dp(34) };
            m_buttons.push_back(tabMemory);

            RECT rows = area;
            rows.top += Dp(38);
            rows.bottom -= Dp(46);

            const int rowH = (m_panel == Panel::History) ? Dp(60) : Dp(52);
            const auto& history = m_manager->GetHistoryItems();
            const size_t count = (m_panel == Panel::History) ? history.size() : m_memory.size();

            const int contentH = static_cast<int>(count) * rowH;
            const int maxScroll = (std::max)(0, contentH - static_cast<int>(rows.bottom - rows.top));
            m_historyScroll = (std::max)(0, (std::min)(m_historyScroll, maxScroll));

            int top = rows.top - m_historyScroll;
            for (size_t i = 0; i < count; ++i)
            {
                RECT r{ rows.left + Dp(8), top, rows.right - Dp(8), top + rowH - Dp(4) };
                if (m_panel == Panel::History)
                {
                    m_historyRects.push_back(r);
                    Btn item;
                    item.action = ACT_HIST_ITEM_BASE + static_cast<int>(i);
                    item.style = Style::Bit; // hover-only fill
                    item.rc = r;
                    m_buttons.push_back(item);
                }
                else
                {
                    m_memoryRects.push_back(r);
                    Btn item;
                    item.action = ACT_MEM_LOAD_BASE + static_cast<int>(i);
                    item.style = Style::Bit;
                    item.rc = r;
                    m_buttons.push_back(item);

                    // Per-slot commands, mirroring the shipping memory list.
                    const wchar_t* labels[] = { L"MC", L"M+", L"M-" };
                    const int bases[] = { ACT_MEM_CLEAR_BASE, ACT_MEM_ADD_BASE, ACT_MEM_SUB_BASE };
                    for (int k = 0; k < 3; ++k)
                    {
                        Btn cmd;
                        cmd.label = labels[k];
                        cmd.action = bases[k] + static_cast<int>(i);
                        cmd.style = Style::Flat;
                        cmd.textDip = 12;
                        cmd.rc = { r.right - Dp(38) * (3 - k) - Dp(4), r.bottom - Dp(26), r.right - Dp(38) * (2 - k) - Dp(6), r.bottom - Dp(2) };
                        m_buttons.push_back(cmd);
                    }
                }
                top += rowH;
            }

            Btn clear;
            clear.label = L"";
            clear.action = (m_panel == Panel::History) ? ACT_HISTORY_CLEAR : ACT_MEMORY_CLEAR_ALL;
            clear.style = Style::Flat;
            clear.icon = true;
            clear.textDip = 14;
            clear.enabled = (count > 0);
            clear.rc = { area.right - Dp(50), area.bottom - Dp(42), area.right - Dp(10), area.bottom - Dp(6) };
            m_buttons.push_back(clear);
        }

        // Overlay surfaces sit above the content and slide in over it.
        BuildOverlayLayout(width, height);
    }
}

namespace
{
    // ------------------------------------------------------------- painting

    // Must be CALLBACK (stdcall): a plain lambda would use the wrong calling
    // convention on x86.
    int CALLBACK FontFoundProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param)
    {
        *reinterpret_cast<bool*>(param) = true;
        return 0;
    }

    bool FontInstalled(const wchar_t* name)
    {
        LOGFONTW lf{};
        lstrcpynW(lf.lfFaceName, name, LF_FACESIZE);
        lf.lfCharSet = DEFAULT_CHARSET;
        bool found = false;
        HDC hdc = GetDC(nullptr);
        EnumFontFamiliesExW(hdc, &lf, FontFoundProc, reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, hdc);
        return found;
    }

    const wchar_t* UiFontFace()
    {
        static const wchar_t* face = FontInstalled(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        return face;
    }

    const wchar_t* DisplayFontFace()
    {
        static const wchar_t* face = FontInstalled(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
        return face;
    }

    const wchar_t* IconFontFace()
    {
        static const wchar_t* face = FontInstalled(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        return face;
    }

    bool IconFontAvailable()
    {
        static const bool available = FontInstalled(L"Segoe Fluent Icons") || FontInstalled(L"Segoe MDL2 Assets");
        return available;
    }

    // Windows 10 and 11 always ship an icon font, so this is a safety net rather
    // than a normal path: without it a missing font would render the chrome as
    // empty boxes.
    // A label that is nothing but one of these glyphs is drawn as a path
    // instead. Keeps the chrome monochrome and independent of which icon font
    // the machine happens to have.
    int VIconForGlyph(std::wstring_view label)
    {
        if (label.size() != 1)
        {
            return VIcon_None;
        }
        switch (label[0])
        {
        case 0xE700: return VIcon_Menu;
        case 0xE72B: return VIcon_Back;
        case 0xE81C: return VIcon_History;
        case 0xE750: return VIcon_Backspace;
        case 0xE70D: return VIcon_ChevronDown;
        case 0xE70E: return VIcon_ChevronUp;
        case 0xE76B: return VIcon_ChevronLeft;
        case 0xE76C: return VIcon_ChevronRight;
        case 0xE74D: return VIcon_Close;
        case 0xE787: return VIcon_Calendar;
        case 0xE713: return VIcon_Gear;
        // The navigation categories, keyed by the codepoints NavCategory.cs
        // assigns them.
        case 0xE8EF: return VIcon_Calculator;    // Standard
        case 0xF196: return VIcon_Flask;         // Scientific
        case 0xF770: return VIcon_Curve;         // Graphing
        case 0xECCE: return VIcon_Code;          // Programmer
        case 0xF1AA: return VIcon_Cup;           // Volume
        case 0xECC6: return VIcon_Ruler;         // Length
        case 0xF4C1: return VIcon_Scales;        // Weight and mass
        case 0xE7A3: return VIcon_Thermometer;   // Temperature
        case 0xECAD: return VIcon_Flame;         // Energy
        case 0xE809: return VIcon_AreaGrid;      // Area
        case 0xEADA: return VIcon_Speed;         // Speed
        case 0xE917: return VIcon_Clock;         // Time
        case 0xE945: return VIcon_Plug;          // Power
        case 0xF20F: return VIcon_Data;          // Data
        case 0xEC4A: return VIcon_Gauge;         // Pressure
        case 0xF515: return VIcon_Angle;         // Angle
        default: return VIcon_None;
        }
    }

    std::wstring_view IconFallback(std::wstring_view glyph)
    {
        if (glyph == L"") return L"☰"; // hamburger
        if (glyph == L"") return L"↺"; // history
        if (glyph == L"") return L"⌫"; // backspace
        if (glyph == L"") return L"˅"; // chevron down
        if (glyph == L"") return L"✕"; // delete
        if (glyph == L"") return L"01";  // bit-flip board
        return glyph;
    }

    struct FontKey
    {
        int height;
        int weight;
        int face; // 0 = ui, 1 = display, 2 = icon
        bool italic;
        HFONT font;
    };

    std::vector<FontKey> g_fonts;

    HFONT GetFont(int dip, int weight = FW_NORMAL, int face = 0, bool italic = false)
    {
        const int height = -Dp(dip);
        for (const auto& entry : g_fonts)
        {
            if (entry.height == height && entry.weight == weight && entry.face == face && entry.italic == italic)
            {
                return entry.font;
            }
        }
        const wchar_t* name = (face == 2) ? IconFontFace() : (face == 1) ? DisplayFontFace() : UiFontFace();
        HFONT font = CreateFontW(
            height, 0, 0, 0, weight, italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH, name);
        g_fonts.push_back({ height, weight, face, italic, font });
        return font;
    }

    // Laying the title bar out needs the title's width before painting starts.
    int MeasureTextWidth(std::wstring_view text, int dip, int weight, int face)
    {
        if (text.empty())
        {
            return 0;
        }
        HDC hdc = GetDC(nullptr);
        HGDIOBJ previous = SelectObject(hdc, GetFont(dip, weight, face));
        SIZE size{};
        GetTextExtentPoint32W(hdc, text.data(), static_cast<int>(text.size()), &size);
        SelectObject(hdc, previous);
        ReleaseDC(nullptr, hdc);
        return size.cx;
    }

    void ClearFontCache()
    {
        for (auto& entry : g_fonts)
        {
            DeleteObject(entry.font);
        }
        g_fonts.clear();
    }

    Gdiplus::Color ToGp(COLORREF c, BYTE alpha = 255)
    {
        return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
    }

    void FillRounded(Gdiplus::Graphics& g, const RECT& rc, int radius, COLORREF color, BYTE alpha = 255)
    {
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0)
        {
            return;
        }
        const int r = (std::min)(radius, (std::min)(w, h) / 2);
        Gdiplus::GraphicsPath path;
        if (r <= 0)
        {
            path.AddRectangle(Gdiplus::Rect(rc.left, rc.top, w, h));
        }
        else
        {
            const int d = r * 2;
            path.AddArc(rc.left, rc.top, d, d, 180.0f, 90.0f);
            path.AddArc(rc.right - d, rc.top, d, d, 270.0f, 90.0f);
            path.AddArc(rc.right - d, rc.bottom - d, d, d, 0.0f, 90.0f);
            path.AddArc(rc.left, rc.bottom - d, d, d, 90.0f, 90.0f);
            path.CloseFigure();
        }
        Gdiplus::SolidBrush brush(ToGp(color, alpha));
        g.FillPath(&brush, &path);
    }

    // Hover and press are blended in rather than switched, so a key lights up
    // and settles the way a Fluent control does.
    COLORREF ButtonFill(const Btn& b, float hot, float pressed, bool& painted)
    {
        painted = true;
        if (b.style == Style::Card)
        {
            // Cards keep their surface whether or not they are interactive.
            return Blend(Blend(g_theme.card, g_theme.flatHover, b.enabled ? hot * 0.6f : 0.0f), g_theme.flatPress, pressed * 0.6f);
        }
        if (!b.enabled)
        {
            painted = (b.style == Style::Number || b.style == Style::Operator);
            return Mix(g_theme.operatorFill, g_theme.page, 0.5);
        }

        const float active = (hot > pressed) ? hot : pressed;
        switch (b.style)
        {
        case Style::Number:
            return Blend(Blend(g_theme.numberFill, g_theme.numberHover, hot), g_theme.numberPress, pressed);
        case Style::Operator:
            return Blend(Blend(g_theme.operatorFill, g_theme.operatorHover, hot), g_theme.operatorPress, pressed);
        case Style::Accent:
            return Blend(Blend(g_theme.accent, g_theme.accentHover, hot), g_theme.accentPress, pressed);
        case Style::Toggle:
            if (b.checked)
            {
                return Blend(Blend(g_theme.accent, g_theme.accentHover, hot), g_theme.accentPress, pressed);
            }
            painted = active > 0.004f;
            return Blend(g_theme.flatHover, g_theme.flatPress, pressed);
        case Style::Bit:
            painted = active > 0.004f;
            return Blend(g_theme.flatHover, g_theme.flatPress, pressed);
        case Style::Card:
            return Blend(Blend(g_theme.card, g_theme.flatHover, hot * 0.6f), g_theme.flatPress, pressed * 0.6f);
        case Style::Flat:
        default:
            if (b.checked)
            {
                painted = true;
                return Blend(g_theme.flatHover, g_theme.flatPress, pressed);
            }
            painted = active > 0.004f;
            return Blend(g_theme.flatHover, g_theme.flatPress, pressed);
        }
    }

    // How far through its hover and press animations a given button is.
    void ButtonMotion(const CalcApp& app, const Btn& b, int index, int hotIndex, int pressedIndex, float& hot, float& pressed);

    int ButtonRadius(const Btn& b)
    {
        return (b.style == Style::Card) ? Dp(6) : Dp(kRadiusDip);
    }

    // Pointer-down shrinks the key slightly, as Fluent's pressed state does.
    RECT PressedRect(const RECT& rc, float pressed)
    {
        if (pressed <= 0.001f)
        {
            return rc;
        }
        RECT r = rc;
        const int dx = static_cast<int>((r.right - r.left) * 0.018f * pressed);
        const int dy = static_cast<int>((r.bottom - r.top) * 0.018f * pressed);
        InflateRect(&r, -dx, -dy);
        return r;
    }

    COLORREF ButtonText(const Btn& b)
    {
        if (b.accentText)
        {
            return g_theme.accent;
        }

        if (b.style == Style::Card)
        {
            return g_theme.primaryText; // cards carry content even when inert
        }
        if (!b.enabled)
        {
            return g_theme.disabledText;
        }
        if (b.style == Style::Accent || (b.style == Style::Toggle && b.checked))
        {
            return g_theme.accentText;
        }
        if (b.style == Style::Bit)
        {
            // Date fields and list rows use this style for their hover fill but
            // carry ordinary content.
            if (b.dateField || !b.label.empty())
            {
                return g_theme.primaryText;
            }
            return b.checked ? g_theme.primaryText : g_theme.disabledText;
        }
        return g_theme.primaryText;
    }

    // The shipping app sets mathematical variables in italic: x squared, one
    // over x, the roots, n factorial and so on. Operators and named functions
    // (exp, mod, log, ln) stay upright.
    bool IsVariableKey(int action)
    {
        if (action < kCommandBase)
        {
            return false;
        }
        switch (static_cast<Command>(action - kCommandBase))
        {
        case Command::CommandSQR:
        case Command::CommandCUB:
        case Command::CommandREC:
        case Command::CommandSQRT:
        case Command::CommandCUBEROOT:
        case Command::CommandPWR:
        case Command::CommandROOT:
        case Command::CommandPOW10:
        case Command::CommandPOW2:
        case Command::CommandFAC:
        case Command::CommandAbs:
        case Command::CommandLogBaseY:
        case Command::CommandPOWE:
            return true;
        default:
            return false;
        }
    }

    int DefaultTextDip(const Btn& b)
    {
        if (b.textDip)
        {
            return b.textDip;
        }
        switch (b.style)
        {
        case Style::Number:
            return 20;
        case Style::Accent:
            return 20;
        case Style::Operator:
            return 17;
        default:
            return 13;
        }
    }

    void DrawLabel(
        HDC hdc,
        const RECT& rc,
        std::wstring_view text,
        int dip,
        COLORREF color,
        bool icon,
        UINT flags,
        int weight = FW_NORMAL,
        int face = 0,
        bool italic = false)
    {
        if (icon && !IconFontAvailable())
        {
            text = IconFallback(text);
            icon = false;
        }
        SelectObject(hdc, GetFont(dip, weight, icon ? 2 : face, italic));
        SetTextColor(hdc, color);
        RECT r = rc;
        DrawTextW(hdc, text.data(), static_cast<int>(text.size()), &r, flags);
    }

    // A label such as "M" mixes body text with an icon glyph; draw the two
    // runs side by side so neither font has to carry the other's characters.
    void DrawMixedLabel(HDC hdc, const RECT& rc, std::wstring_view text, int dip, COLORREF color)
    {
        size_t split = std::wstring::npos;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] >= 0xE000 && text[i] <= 0xF8FF)
            {
                split = i;
                break;
            }
        }
        if (split == std::wstring::npos)
        {
            DrawLabel(hdc, rc, text, dip, color, false, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            return;
        }
        if (split == 0)
        {
            DrawLabel(hdc, rc, text, dip, color, true, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            return;
        }

        const std::wstring_view head = text.substr(0, split);
        SelectObject(hdc, GetFont(dip));
        SIZE headSize{};
        GetTextExtentPoint32W(hdc, head.data(), static_cast<int>(head.size()), &headSize);

        // A trailing chevron is a path, so it matches the standalone ones and
        // cannot be substituted by a colour font.
        const int vectorTail = VIconForGlyph(text.substr(split));
        if (vectorTail != VIcon_None)
        {
            const int tailWidth = Dp(dip);
            const int total = headSize.cx + Dp(1) + tailWidth;
            const int x = rc.left + ((rc.right - rc.left) - total) / 2;
            RECT r = rc;
            r.left = x;
            DrawLabel(hdc, r, head, dip, color, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
            RECT icon{ x + headSize.cx + Dp(1), rc.top, x + headSize.cx + Dp(1) + tailWidth, rc.bottom };
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            DrawVectorIcon(g, icon, vectorTail, color, dip);
            return;
        }

        const bool iconFont = IconFontAvailable();
        const std::wstring_view tail = iconFont ? text.substr(split) : IconFallback(text.substr(split));
        SelectObject(hdc, GetFont(dip - 2, FW_NORMAL, iconFont ? 2 : 0));
        SIZE tailSize{};
        GetTextExtentPoint32W(hdc, tail.data(), static_cast<int>(tail.size()), &tailSize);

        const int total = headSize.cx + Dp(3) + tailSize.cx;
        int x = rc.left + ((rc.right - rc.left) - total) / 2;
        RECT r = rc;
        r.left = x;
        DrawLabel(hdc, r, head, dip, color, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        r.left = x + headSize.cx + Dp(3);
        DrawLabel(hdc, r, tail, dip - 2, color, iconFont, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    }

    // The chrome icons are drawn as paths rather than font glyphs. Segoe's icon
    // fonts are not guaranteed to carry every codepoint, and the text-presentation
    // characters we used to fall back on can be substituted by a colour emoji
    // font, which puts blue and orange into a title bar that should be
    // monochrome. Paths are immune to both.

    // Every icon is described on a 16x16 grid and scaled to sizeDip, the way the
    // icon fonts are drawn from a 16px em box.
    void DrawVectorIcon(Gdiplus::Graphics& g, const RECT& rc, int id, COLORREF color, int sizeDip)
    {
        if (id == VIcon_None || sizeDip <= 0)
        {
            return;
        }

        const float s = static_cast<float>(Dp(sizeDip)) / 16.0f;
        const float ox = static_cast<float>(rc.left + rc.right) * 0.5f - 8.0f * s;
        const float oy = static_cast<float>(rc.top + rc.bottom) * 0.5f - 8.0f * s;
        const auto P = [&](float x, float y) { return Gdiplus::PointF(ox + x * s, oy + y * s); };

        const Gdiplus::Color gp(GetRValue(color), GetGValue(color), GetBValue(color));
        Gdiplus::Pen pen(gp, 1.1f * s);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush brush(gp);

        switch (id)
        {
        case VIcon_Menu:
            g.DrawLine(&pen, P(2.5f, 4.0f), P(13.5f, 4.0f));
            g.DrawLine(&pen, P(2.5f, 8.0f), P(13.5f, 8.0f));
            g.DrawLine(&pen, P(2.5f, 12.0f), P(13.5f, 12.0f));
            break;

        case VIcon_Back:
            g.DrawLine(&pen, P(3.2f, 8.0f), P(13.0f, 8.0f));
            g.DrawLine(&pen, P(7.6f, 3.6f), P(3.2f, 8.0f));
            g.DrawLine(&pen, P(3.2f, 8.0f), P(7.6f, 12.4f));
            break;

        case VIcon_History:
        {
            // An open circle with an arrowhead at the break, plus clock hands.
            const Gdiplus::RectF dial(ox + 2.8f * s, oy + 3.3f * s, 10.4f * s, 10.4f * s);
            g.DrawArc(&pen, dial, 250.0f, 290.0f);
            const Gdiplus::PointF head[3] = { P(7.4f, 1.7f), P(7.4f, 5.7f), P(4.5f, 3.7f) };
            g.FillPolygon(&brush, head, 3);
            g.DrawLine(&pen, P(8.0f, 5.6f), P(8.0f, 8.6f));
            g.DrawLine(&pen, P(8.0f, 8.6f), P(10.5f, 9.9f));
            break;
        }

        case VIcon_KeepOnTop:
        case VIcon_KeepOnTopOn:
        {
            // The shipping "keep on top" glyph: a window with a second, smaller
            // window pinned inside its trailing corner.
            Gdiplus::RectF outer(ox + 2.0f * s, oy + 3.4f * s, 12.0f * s, 9.2f * s);
            g.DrawRectangle(&pen, outer);
            Gdiplus::RectF inner(ox + 8.0f * s, oy + 7.2f * s, 4.6f * s, 3.6f * s);
            if (id == VIcon_KeepOnTopOn)
            {
                g.FillRectangle(&brush, inner);
            }
            else
            {
                g.DrawRectangle(&pen, inner);
            }
            break;
        }

        case VIcon_Backspace:
        {
            const Gdiplus::PointF body[5] = { P(5.6f, 3.4f), P(14.4f, 3.4f), P(14.4f, 12.6f), P(5.6f, 12.6f), P(1.4f, 8.0f) };
            g.DrawPolygon(&pen, body, 5);
            g.DrawLine(&pen, P(7.9f, 6.4f), P(11.7f, 9.6f));
            g.DrawLine(&pen, P(11.7f, 6.4f), P(7.9f, 9.6f));
            break;
        }

        case VIcon_ChevronDown:
            g.DrawLine(&pen, P(4.5f, 6.5f), P(8.0f, 10.0f));
            g.DrawLine(&pen, P(8.0f, 10.0f), P(11.5f, 6.5f));
            break;

        case VIcon_ChevronUp:
            g.DrawLine(&pen, P(4.5f, 9.5f), P(8.0f, 6.0f));
            g.DrawLine(&pen, P(8.0f, 6.0f), P(11.5f, 9.5f));
            break;

        case VIcon_ChevronLeft:
            g.DrawLine(&pen, P(9.5f, 4.5f), P(6.0f, 8.0f));
            g.DrawLine(&pen, P(6.0f, 8.0f), P(9.5f, 11.5f));
            break;

        case VIcon_ChevronRight:
            g.DrawLine(&pen, P(6.5f, 4.5f), P(10.0f, 8.0f));
            g.DrawLine(&pen, P(10.0f, 8.0f), P(6.5f, 11.5f));
            break;

        case VIcon_Calendar:
        {
            Gdiplus::RectF body(ox + 2.0f * s, oy + 3.6f * s, 12.0f * s, 10.4f * s);
            g.DrawRectangle(&pen, body);
            g.DrawLine(&pen, P(2.0f, 6.8f), P(14.0f, 6.8f));
            g.DrawLine(&pen, P(5.4f, 2.0f), P(5.4f, 5.0f));
            g.DrawLine(&pen, P(10.6f, 2.0f), P(10.6f, 5.0f));
            break;
        }

        case VIcon_Gear:
        {
            // Eight teeth around a hub, with the bore punched out by the
            // alternating fill rule.
            Gdiplus::GraphicsPath path;
            path.SetFillMode(Gdiplus::FillModeAlternate);
            Gdiplus::PointF teeth[16];
            for (int i = 0; i < 16; ++i)
            {
                const float angle = (3.14159265f / 8.0f) * static_cast<float>(i);
                const float radius = (i % 2 == 0) ? 7.0f : 5.2f;
                teeth[i] = P(8.0f + radius * cosf(angle), 8.0f + radius * sinf(angle));
            }
            path.AddPolygon(teeth, 16);
            path.AddEllipse(ox + 5.6f * s, oy + 5.6f * s, 4.8f * s, 4.8f * s);
            g.FillPath(&brush, &path);
            break;
        }

        case VIcon_Close:
            g.DrawLine(&pen, P(4.2f, 4.2f), P(11.8f, 11.8f));
            g.DrawLine(&pen, P(11.8f, 4.2f), P(4.2f, 11.8f));
            break;

        case VIcon_ZoomIn:
        case VIcon_ZoomOut:
            g.DrawEllipse(&pen, ox + 2.2f * s, oy + 2.2f * s, 9.2f * s, 9.2f * s);
            g.DrawLine(&pen, P(11.0f, 11.0f), P(14.0f, 14.0f));
            g.DrawLine(&pen, P(4.4f, 6.8f), P(9.2f, 6.8f));
            if (id == VIcon_ZoomIn)
            {
                g.DrawLine(&pen, P(6.8f, 4.4f), P(6.8f, 9.2f));
            }
            break;

        case VIcon_ZoomReset:
            // Reset view: a frame with arrows pushing out to its corners.
            g.DrawRectangle(&pen, Gdiplus::RectF(ox + 2.6f * s, oy + 2.6f * s, 10.8f * s, 10.8f * s));
            g.DrawLine(&pen, P(6.0f, 6.0f), P(10.0f, 10.0f));
            g.DrawLine(&pen, P(10.0f, 6.0f), P(6.0f, 10.0f));
            break;

        case VIcon_Plus:
            g.DrawLine(&pen, P(8.0f, 3.5f), P(8.0f, 12.5f));
            g.DrawLine(&pen, P(3.5f, 8.0f), P(12.5f, 8.0f));
            break;

        case VIcon_Minus:
            g.DrawLine(&pen, P(3.5f, 8.0f), P(12.5f, 8.0f));
            break;

        case VIcon_Flask:
            // Scientific: a conical flask.
            g.DrawLine(&pen, P(6.4f, 2.2f), P(6.4f, 6.6f));
            g.DrawLine(&pen, P(9.6f, 2.2f), P(9.6f, 6.6f));
            g.DrawLine(&pen, P(5.4f, 2.2f), P(10.6f, 2.2f));
            g.DrawLine(&pen, P(6.4f, 6.6f), P(3.2f, 12.4f));
            g.DrawLine(&pen, P(9.6f, 6.6f), P(12.8f, 12.4f));
            g.DrawLine(&pen, P(3.2f, 12.4f), P(12.8f, 12.4f));
            g.DrawLine(&pen, P(5.0f, 9.2f), P(11.0f, 9.2f));
            break;

        case VIcon_Curve:
        {
            // Graphing: axes with a curve rising across them.
            g.DrawLine(&pen, P(2.6f, 2.6f), P(2.6f, 13.4f));
            g.DrawLine(&pen, P(2.6f, 13.4f), P(13.4f, 13.4f));
            const Gdiplus::PointF curve[4] = { P(3.4f, 11.6f), P(6.4f, 10.4f), P(8.4f, 4.6f), P(12.8f, 3.4f) };
            g.DrawCurve(&pen, curve, 4);
            break;
        }

        case VIcon_Code:
            // Programmer: angle brackets around a slash.
            g.DrawLine(&pen, P(5.6f, 4.4f), P(2.4f, 8.0f));
            g.DrawLine(&pen, P(2.4f, 8.0f), P(5.6f, 11.6f));
            g.DrawLine(&pen, P(10.4f, 4.4f), P(13.6f, 8.0f));
            g.DrawLine(&pen, P(13.6f, 8.0f), P(10.4f, 11.6f));
            g.DrawLine(&pen, P(9.2f, 3.4f), P(6.8f, 12.6f));
            break;

        case VIcon_Cup:
            // Volume: a measuring cup with a level line.
            g.DrawLine(&pen, P(3.4f, 3.4f), P(4.6f, 13.0f));
            g.DrawLine(&pen, P(12.6f, 3.4f), P(11.4f, 13.0f));
            g.DrawLine(&pen, P(4.6f, 13.0f), P(11.4f, 13.0f));
            g.DrawLine(&pen, P(3.4f, 3.4f), P(12.6f, 3.4f));
            g.DrawLine(&pen, P(4.1f, 8.6f), P(11.9f, 8.6f));
            break;

        case VIcon_Ruler:
            // Length: a rule with graduations.
            g.DrawRectangle(&pen, Gdiplus::RectF(ox + 1.8f * s, oy + 5.4f * s, 12.4f * s, 5.2f * s));
            g.DrawLine(&pen, P(4.4f, 5.4f), P(4.4f, 8.0f));
            g.DrawLine(&pen, P(6.8f, 5.4f), P(6.8f, 9.0f));
            g.DrawLine(&pen, P(9.2f, 5.4f), P(9.2f, 8.0f));
            g.DrawLine(&pen, P(11.6f, 5.4f), P(11.6f, 9.0f));
            break;

        case VIcon_Scales:
            // Weight and mass: a balance.
            g.DrawLine(&pen, P(8.0f, 3.0f), P(8.0f, 13.0f));
            g.DrawLine(&pen, P(3.0f, 5.0f), P(13.0f, 5.0f));
            g.DrawLine(&pen, P(5.0f, 13.0f), P(11.0f, 13.0f));
            g.DrawLine(&pen, P(3.0f, 5.0f), P(1.8f, 8.6f));
            g.DrawLine(&pen, P(1.8f, 8.6f), P(4.2f, 8.6f));
            g.DrawLine(&pen, P(4.2f, 8.6f), P(3.0f, 5.0f));
            g.DrawLine(&pen, P(13.0f, 5.0f), P(11.8f, 8.6f));
            g.DrawLine(&pen, P(11.8f, 8.6f), P(14.2f, 8.6f));
            g.DrawLine(&pen, P(14.2f, 8.6f), P(13.0f, 5.0f));
            break;

        case VIcon_Thermometer:
            // Temperature: a bulb thermometer.
            g.DrawLine(&pen, P(6.6f, 3.0f), P(6.6f, 9.4f));
            g.DrawLine(&pen, P(9.4f, 3.0f), P(9.4f, 9.4f));
            g.DrawArc(&pen, ox + 6.6f * s, oy + 2.0f * s, 2.8f * s, 2.8f * s, 180.0f, 180.0f);
            g.DrawEllipse(&pen, ox + 5.4f * s, oy + 9.0f * s, 5.2f * s, 5.2f * s);
            g.FillEllipse(&brush, ox + 6.6f * s, oy + 10.2f * s, 2.8f * s, 2.8f * s);
            break;

        case VIcon_Flame:
        {
            // Energy: a flame.
            Gdiplus::GraphicsPath flame;
            const Gdiplus::PointF body[6] = { P(8.0f, 1.8f),  P(11.8f, 5.6f), P(12.6f, 9.6f),
                                              P(8.0f, 14.2f), P(3.4f, 9.6f),  P(5.6f, 6.0f) };
            flame.AddClosedCurve(body, 6);
            g.DrawPath(&pen, &flame);
            break;
        }

        case VIcon_AreaGrid:
            // Area: a square divided into cells.
            g.DrawRectangle(&pen, Gdiplus::RectF(ox + 2.4f * s, oy + 2.4f * s, 11.2f * s, 11.2f * s));
            g.DrawLine(&pen, P(8.0f, 2.4f), P(8.0f, 13.6f));
            g.DrawLine(&pen, P(2.4f, 8.0f), P(13.6f, 8.0f));
            break;

        case VIcon_Speed:
            // Speed: a dial with a needle.
            g.DrawArc(&pen, ox + 1.8f * s, oy + 3.0f * s, 12.4f * s, 12.4f * s, 180.0f, 180.0f);
            g.DrawLine(&pen, P(8.0f, 9.2f), P(11.0f, 5.6f));
            g.FillEllipse(&brush, ox + 7.2f * s, oy + 8.4f * s, 1.6f * s, 1.6f * s);
            g.DrawLine(&pen, P(1.8f, 9.2f), P(3.2f, 9.2f));
            g.DrawLine(&pen, P(12.8f, 9.2f), P(14.2f, 9.2f));
            break;

        case VIcon_Clock:
            // Time: a clock face.
            g.DrawEllipse(&pen, ox + 2.2f * s, oy + 2.2f * s, 11.6f * s, 11.6f * s);
            g.DrawLine(&pen, P(8.0f, 4.8f), P(8.0f, 8.0f));
            g.DrawLine(&pen, P(8.0f, 8.0f), P(10.6f, 9.4f));
            break;

        case VIcon_Plug:
            // Power: the standard power symbol.
            g.DrawArc(&pen, ox + 3.0f * s, oy + 3.6f * s, 10.0f * s, 10.0f * s, 300.0f, 300.0f);
            g.DrawLine(&pen, P(8.0f, 1.8f), P(8.0f, 7.4f));
            break;

        case VIcon_Data:
            // Data: stacked platters.
            g.DrawEllipse(&pen, ox + 2.6f * s, oy + 2.4f * s, 10.8f * s, 3.4f * s);
            g.DrawLine(&pen, P(2.6f, 4.1f), P(2.6f, 11.9f));
            g.DrawLine(&pen, P(13.4f, 4.1f), P(13.4f, 11.9f));
            g.DrawArc(&pen, ox + 2.6f * s, oy + 6.4f * s, 10.8f * s, 3.4f * s, 0.0f, 180.0f);
            g.DrawArc(&pen, ox + 2.6f * s, oy + 10.2f * s, 10.8f * s, 3.4f * s, 0.0f, 180.0f);
            break;

        case VIcon_Gauge:
            // Pressure: a gauge with a needle and a stem.
            g.DrawEllipse(&pen, ox + 2.6f * s, oy + 1.8f * s, 10.8f * s, 10.8f * s);
            g.DrawLine(&pen, P(8.0f, 7.2f), P(10.4f, 4.8f));
            g.DrawLine(&pen, P(6.6f, 12.4f), P(9.4f, 12.4f));
            g.DrawLine(&pen, P(7.0f, 12.4f), P(7.0f, 14.2f));
            g.DrawLine(&pen, P(9.0f, 12.4f), P(9.0f, 14.2f));
            break;

        case VIcon_Angle:
            // Angle: two rays with an arc between them.
            g.DrawLine(&pen, P(2.6f, 12.4f), P(13.4f, 12.4f));
            g.DrawLine(&pen, P(2.6f, 12.4f), P(12.0f, 4.2f));
            g.DrawArc(&pen, ox + 2.6f * s, oy + 7.6f * s, 9.6f * s, 9.6f * s, 270.0f, 49.0f);
            break;

        case VIcon_Share:
            // Three nodes joined by two links.
            g.DrawEllipse(&pen, ox + 10.2f * s, oy + 1.8f * s, 3.6f * s, 3.6f * s);
            g.DrawEllipse(&pen, ox + 2.2f * s, oy + 6.2f * s, 3.6f * s, 3.6f * s);
            g.DrawEllipse(&pen, ox + 10.2f * s, oy + 10.6f * s, 3.6f * s, 3.6f * s);
            g.DrawLine(&pen, P(5.6f, 7.0f), P(10.4f, 4.2f));
            g.DrawLine(&pen, P(5.6f, 9.0f), P(10.4f, 11.8f));
            break;

        case VIcon_Trace:
        {
            // Trace: a play triangle.
            const Gdiplus::PointF play[3] = { P(5.0f, 3.2f), P(12.4f, 8.0f), P(5.0f, 12.8f) };
            g.DrawPolygon(&pen, play, 3);
            break;
        }

        case VIcon_GraphSettings:
            // A plot with a gear in its corner.
            g.DrawLine(&pen, P(2.4f, 2.4f), P(2.4f, 12.0f));
            g.DrawLine(&pen, P(2.4f, 12.0f), P(12.0f, 12.0f));
            g.DrawLine(&pen, P(3.4f, 9.6f), P(7.0f, 5.2f));
            g.DrawEllipse(&pen, ox + 9.4f * s, oy + 2.2f * s, 4.4f * s, 4.4f * s);
            g.DrawLine(&pen, P(11.6f, 1.2f), P(11.6f, 7.6f));
            g.DrawLine(&pen, P(8.4f, 4.4f), P(14.8f, 4.4f));
            break;

        case VIcon_Recenter:
            // Recentre: a crosshair over the origin.
            g.DrawEllipse(&pen, ox + 4.6f * s, oy + 4.6f * s, 6.8f * s, 6.8f * s);
            g.DrawLine(&pen, P(8.0f, 1.6f), P(8.0f, 4.0f));
            g.DrawLine(&pen, P(8.0f, 12.0f), P(8.0f, 14.4f));
            g.DrawLine(&pen, P(1.6f, 8.0f), P(4.0f, 8.0f));
            g.DrawLine(&pen, P(12.0f, 8.0f), P(14.4f, 8.0f));
            g.FillEllipse(&brush, ox + 7.0f * s, oy + 7.0f * s, 2.0f * s, 2.0f * s);
            break;

        case VIcon_Enter:
            // The return arrow on the graphing keypad's accent key.
            g.DrawLine(&pen, P(12.2f, 3.6f), P(12.2f, 9.6f));
            g.DrawLine(&pen, P(12.2f, 9.6f), P(4.6f, 9.6f));
            g.DrawLine(&pen, P(7.6f, 6.6f), P(4.2f, 9.6f));
            g.DrawLine(&pen, P(4.2f, 9.6f), P(7.6f, 12.6f));
            break;

        case VIcon_Fx:
            // The equations tab: a lowercase f beside an x.
            g.DrawArc(&pen, ox + 4.4f * s, oy + 2.4f * s, 4.0f * s, 4.0f * s, 180.0f, 140.0f);
            g.DrawLine(&pen, P(5.6f, 4.4f), P(5.6f, 13.2f));
            g.DrawLine(&pen, P(3.4f, 7.2f), P(8.0f, 7.2f));
            g.DrawLine(&pen, P(9.6f, 8.4f), P(13.6f, 13.2f));
            g.DrawLine(&pen, P(13.6f, 8.4f), P(9.6f, 13.2f));
            break;

        case VIcon_RadioOff:
            g.DrawEllipse(&pen, ox + 2.6f * s, oy + 2.6f * s, 10.8f * s, 10.8f * s);
            break;

        case VIcon_RadioOn:
        {
            // A filled accent ring with a white dot, as WinUI draws a checked
            // radio button.
            g.FillEllipse(&brush, ox + 2.6f * s, oy + 2.6f * s, 10.8f * s, 10.8f * s);
            Gdiplus::SolidBrush centre(ToGp(g_theme.card));
            g.FillEllipse(&centre, ox + 5.6f * s, oy + 5.6f * s, 4.8f * s, 4.8f * s);
            break;
        }

        case VIcon_Palette:
        {
            // The app-theme setting: a circle with one half filled, the usual
            // shorthand for light against dark.
            g.DrawEllipse(&pen, ox + 2.4f * s, oy + 2.4f * s, 11.2f * s, 11.2f * s);
            Gdiplus::GraphicsPath half;
            half.AddArc(ox + 2.4f * s, oy + 2.4f * s, 11.2f * s, 11.2f * s, 90.0f, 180.0f);
            half.CloseFigure();
            g.FillPath(&brush, &half);
            break;
        }

        case VIcon_Calculator:
        {
            // The app's own icon: a keypad with a display across the top.
            Gdiplus::RectF body(ox + 3.0f * s, oy + 1.6f * s, 10.0f * s, 12.8f * s);
            g.DrawRectangle(&pen, body);
            g.DrawLine(&pen, P(5.0f, 5.4f), P(11.0f, 5.4f));
            for (int row = 0; row < 2; ++row)
            {
                for (int col = 0; col < 3; ++col)
                {
                    g.FillEllipse(
                        &brush, ox + (4.6f + static_cast<float>(col) * 2.4f) * s,
                        oy + (7.8f + static_cast<float>(row) * 2.6f) * s, 1.4f * s, 1.4f * s);
                }
            }
            break;
        }

        default:
            break;
        }
    }
}

namespace
{
    // Shrinks the result until it fits the display.
    //
    // The floor has to go low enough for the longest value the app can produce:
    // a 64-bit binary word is 64 digits plus 15 nibble separators, and
    // scientific notation carries 32 significant digits plus an exponent. The
    // old floor of 16 could not fit either, and since the text is right
    // aligned what did not fit was clipped off the *front* -- a result reading
    // ".333...e-1" with its leading digit missing, or a binary word showing
    // eight nibbles of sixteen. Shrinking further is worse typography than
    // clipping is wrong.
    void DrawAutoFitNumber(HDC hdc, const RECT& rc, std::wstring_view text, COLORREF color)
    {
        constexpr int kMaxDip = 46;
        constexpr int kMinDip = 8;

        const int maxWidth = rc.right - rc.left;
        SIZE size{};
        int dip = kMaxDip;
        for (;;)
        {
            SelectObject(hdc, GetFont(dip, FW_SEMIBOLD, 1));
            GetTextExtentPoint32W(hdc, text.data(), static_cast<int>(text.size()), &size);
            if (size.cx <= maxWidth || dip <= kMinDip)
            {
                break; // the floor itself is measured, rather than stepped past
            }
            dip -= 2;
        }
        SetTextColor(hdc, color);
        RECT r = rc;
        DrawTextW(hdc, text.data(), static_cast<int>(text.size()), &r, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX);
    }

    // Right-aligns long text by clipping from the left, the way the shipping
    // app keeps the tail of an expression visible.
    void DrawTail(HDC hdc, const RECT& rc, std::wstring_view text, int dip, COLORREF color)
    {
        SelectObject(hdc, GetFont(dip));
        SetTextColor(hdc, color);
        RECT r = rc;
        DrawTextW(hdc, text.data(), static_cast<int>(text.size()), &r, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    void PaintPanelContents(HDC hdc, CalcApp& app)
    {
        RECT rows = app.m_panelRect;
        rows.top += Dp(38);
        rows.bottom -= Dp(46);

        HRGN clip = CreateRectRgn(rows.left, rows.top, rows.right, rows.bottom);
        SelectClipRgn(hdc, clip);

        if (app.m_panel == Panel::History)
        {
            const auto& items = app.m_manager->GetHistoryItems();
            if (items.empty())
            {
                RECT r = rows;
                DrawLabel(hdc, r, L"There's no history yet.", 13, g_theme.secondaryText, false, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            }
            else
            {
                // The engine stores oldest first; show newest at the top.
                for (size_t i = 0; i < app.m_historyRects.size() && i < items.size(); ++i)
                {
                    const auto& item = items[items.size() - 1 - i]->historyItemVector;
                    RECT r = app.m_historyRects[i];
                    if (r.bottom < rows.top || r.top > rows.bottom)
                    {
                        continue;
                    }
                    RECT top = r;
                    top.bottom = top.top + Dp(22);
                    top.right -= Dp(10);
                    DrawTail(hdc, top, item.expression, 12, g_theme.secondaryText);

                    RECT bottom = r;
                    bottom.top = top.bottom;
                    bottom.right -= Dp(10);
                    SelectObject(hdc, GetFont(20, FW_SEMIBOLD, 1));
                    SetTextColor(hdc, g_theme.primaryText);
                    DrawTextW(hdc, item.result.data(), static_cast<int>(item.result.size()), &bottom, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                }
            }
        }
        else
        {
            if (app.m_memory.empty())
            {
                RECT r = rows;
                DrawLabel(hdc, r, L"There's nothing saved in memory.", 13, g_theme.secondaryText, false, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            }
            else
            {
                for (size_t i = 0; i < app.m_memoryRects.size() && i < app.m_memory.size(); ++i)
                {
                    RECT r = app.m_memoryRects[i];
                    if (r.bottom < rows.top || r.top > rows.bottom)
                    {
                        continue;
                    }
                    RECT value = r;
                    value.bottom = value.top + Dp(28);
                    value.right -= Dp(10);
                    SelectObject(hdc, GetFont(20, FW_SEMIBOLD, 1));
                    SetTextColor(hdc, g_theme.primaryText);
                    DrawTextW(hdc, app.m_memory[i].data(), static_cast<int>(app.m_memory[i].size()), &value, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                }
            }
        }

        SelectClipRgn(hdc, nullptr);
        DeleteObject(clip);
    }


    // EquationBrush1..14 from App.xaml, in both theme variants. The shipping
    // app hands these out in order as equations are added.
    COLORREF EquationColour(int index)
    {
        static constexpr COLORREF light[] = {
            RGB(0x00, 0x63, 0xB1), RGB(0x00, 0xB7, 0xC3), RGB(0x66, 0x00, 0xCC), RGB(0x10, 0x7C, 0x10),
            RGB(0x00, 0xCC, 0x6A), RGB(0x00, 0x80, 0x55), RGB(0x58, 0x59, 0x5B), RGB(0xE8, 0x11, 0x23),
            RGB(0xE3, 0x00, 0x8C), RGB(0xB3, 0x15, 0x64), RGB(0xFF, 0xB9, 0x00), RGB(0xF7, 0x63, 0x0C),
            RGB(0x8E, 0x56, 0x2E), RGB(0x00, 0x00, 0x00),
        };
        static constexpr COLORREF dark[] = {
            RGB(0x4D, 0x92, 0xC8), RGB(0x4D, 0xCD, 0xD5), RGB(0xA3, 0x66, 0xE0), RGB(0x58, 0xA3, 0x58),
            RGB(0x4D, 0xDB, 0x97), RGB(0x4D, 0xA6, 0x88), RGB(0x8A, 0x8B, 0x8C), RGB(0xEF, 0x58, 0x65),
            RGB(0xEB, 0x4D, 0xAF), RGB(0xCA, 0x5B, 0x93), RGB(0xFF, 0xCE, 0x4D), RGB(0xF9, 0x92, 0x55),
            RGB(0xB0, 0x89, 0x6D), RGB(0xFF, 0xFF, 0xFF),
        };
        constexpr int count = static_cast<int>(sizeof(light) / sizeof(light[0]));
        const int slot = ((index % count) + count) % count;
        return g_theme.dark ? dark[slot] : light[slot];
    }

    // The step between gridlines: 1, 2 or 5 times a power of ten, whichever
    // puts roughly the requested number of lines across the range.
    double GridStep(double range, int target)
    {
        if (range <= 0.0 || target <= 0)
        {
            return 1.0;
        }
        const double rough = range / static_cast<double>(target);
        const double magnitude = std::pow(10.0, std::floor(std::log10(rough)));
        const double normalised = rough / magnitude;
        double step = 10.0;
        if (normalised < 1.5)
        {
            step = 1.0;
        }
        else if (normalised < 3.5)
        {
            step = 2.0;
        }
        else if (normalised < 7.5)
        {
            step = 5.0;
        }
        return step * magnitude;
    }

    // Caption above a field, in the app's small secondary style.
    void DrawCaption(HDC hdc, RECT rc, std::wstring_view text)
    {
        DrawLabel(hdc, rc, text, 12, g_theme.secondaryText, false, DT_SINGLELINE | DT_LEFT | DT_BOTTOM);
    }

    void PaintConverter(HDC hdc, CalcApp& app)
    {
        if (!app.m_converterModel)
        {
            return;
        }
        const auto& model = *app.m_converterModel;
        const bool secondActive = model.IsSecondFieldActive();

        // The field being edited is the brighter of the two, as in the app.
        DrawAutoFitNumber(hdc, app.m_convFromValue, model.FromValue(), secondActive ? g_theme.secondaryText : g_theme.primaryText);
        DrawAutoFitNumber(hdc, app.m_convToValue, model.ToValue(), secondActive ? g_theme.primaryText : g_theme.secondaryText);

        const auto& suggestions = model.Suggestions();
        if (suggestions.empty())
        {
            return;
        }

        RECT caption = app.m_convSuggestions;
        caption.bottom = caption.top + Dp(16);
        DrawLabel(hdc, caption, L"About equal to", 12, g_theme.secondaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        // Two columns of "value abbreviation", as the shipping layout shows.
        const int columnWidth = (app.m_convSuggestions.right - app.m_convSuggestions.left) / 2;
        for (size_t i = 0; i < suggestions.size() && i < 4; ++i)
        {
            RECT cell;
            cell.left = app.m_convSuggestions.left + static_cast<int>(i % 2) * columnWidth;
            cell.right = cell.left + columnWidth - Dp(8);
            cell.top = app.m_convSuggestions.top + Dp(18) + static_cast<int>(i / 2) * Dp(18);
            cell.bottom = cell.top + Dp(18);

            std::wstring text = suggestions[i].value;
            text += L' ';
            text += suggestions[i].abbreviation;
            DrawLabel(hdc, cell, text, 12, g_theme.primaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    // Formats a tick label: enough decimals for the step in use, no trailing
    // zeros, and a real minus sign.
    std::wstring TickLabel(double value, double step)
    {
        int decimals = 0;
        double scaled = step;
        while (scaled < 1.0 && decimals < 6)
        {
            scaled *= 10.0;
            ++decimals;
        }
        // Snap away the error that repeated addition of the step leaves behind.
        if (std::fabs(value) < step * 0.5)
        {
            value = 0.0;
        }
        std::wstring text = CalcEngine::NumericString::FixedPoint(value, decimals);
        if (decimals > 0)
        {
            while (!text.empty() && text.back() == L'0')
            {
                text.pop_back();
            }
            if (!text.empty() && text.back() == L'.')
            {
                text.pop_back();
            }
        }
        for (wchar_t& c : text)
        {
            if (c == L'-')
            {
                c = 0x2212;
            }
        }
        return text;
    }

    // The plot itself, drawn before the buttons so the zoom controls sit on
    // top of it rather than under it.
    // The text a shared flyout command contributes to an equation. Anything the
    // plotter has no meaning for -- rand, dms, degrees -- returns nullptr and is
    // ignored rather than written in as something that will not parse.
    const wchar_t* GraphingFunctionName(Command command)
    {
        switch (command)
        {
        case Command::CommandSIN: return L"sin(";
        case Command::CommandCOS: return L"cos(";
        case Command::CommandTAN: return L"tan(";
        case Command::CommandSEC: return L"sec(";
        case Command::CommandCSC: return L"csc(";
        case Command::CommandCOT: return L"cot(";
        case Command::CommandASIN: return L"asin(";
        case Command::CommandACOS: return L"acos(";
        case Command::CommandATAN: return L"atan(";
        case Command::CommandASEC: return L"asec(";
        case Command::CommandACSC: return L"acsc(";
        case Command::CommandACOT: return L"acot(";
        case Command::CommandSINH: return L"sinh(";
        case Command::CommandCOSH: return L"cosh(";
        case Command::CommandTANH: return L"tanh(";
        case Command::CommandSECH: return L"sech(";
        case Command::CommandCSCH: return L"csch(";
        case Command::CommandCOTH: return L"coth(";
        case Command::CommandASINH: return L"asinh(";
        case Command::CommandACOSH: return L"acosh(";
        case Command::CommandATANH: return L"atanh(";
        case Command::CommandAbs: return L"abs(";
        case Command::CommandLOG: return L"log(";
        case Command::CommandLN: return L"ln(";
        case Command::CommandSQRT: return L"sqrt(";
        case Command::CommandCUBEROOT: return L"cbrt(";
        default: return nullptr;
        }
    }

    void PaintGraphPlot(HDC hdc, CalcApp& app)
    {
        const RECT& plot = app.m_graphRect;
        const int width = plot.right - plot.left;
        const int height = plot.bottom - plot.top;
        if (width <= 0 || height <= 0)
        {
            return;
        }

        const double xMin = app.m_graphXMin;
        const double xMax = app.m_graphXMax;
        const double yMin = app.m_graphYMin;
        const double yMax = app.m_graphYMax;
        const double xSpan = xMax - xMin;
        const double ySpan = yMax - yMin;
        if (xSpan <= 0.0 || ySpan <= 0.0)
        {
            return;
        }

        const auto toScreenX = [&](double x) {
            return static_cast<double>(plot.left) + (x - xMin) / xSpan * static_cast<double>(width);
        };
        const auto toScreenY = [&](double y) {
            return static_cast<double>(plot.bottom) - (y - yMin) / ySpan * static_cast<double>(height);
        };

        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // Surface, clipped so nothing can spill into the equation list.
        FillRounded(g, plot, Dp(4), g_theme.card);
        g.SetClip(Gdiplus::Rect(plot.left, plot.top, width, height));

        const double xStep = GridStep(xSpan, 8);
        const double yStep = GridStep(ySpan, 8);

        // Gridlines.
        {
            Gdiplus::Pen grid(ToGp(g_theme.divider));
            for (double x = std::ceil(xMin / xStep) * xStep; x <= xMax; x += xStep)
            {
                const auto sx = static_cast<Gdiplus::REAL>(toScreenX(x));
                g.DrawLine(&grid, sx, static_cast<Gdiplus::REAL>(plot.top), sx, static_cast<Gdiplus::REAL>(plot.bottom));
            }
            for (double y = std::ceil(yMin / yStep) * yStep; y <= yMax; y += yStep)
            {
                const auto sy = static_cast<Gdiplus::REAL>(toScreenY(y));
                g.DrawLine(&grid, static_cast<Gdiplus::REAL>(plot.left), sy, static_cast<Gdiplus::REAL>(plot.right), sy);
            }
        }

        // Axes: heavier than the grid, and tipped with arrowheads the way the
        // shipping plot draws them.
        {
            Gdiplus::Pen axis(ToGp(g_theme.primaryText), 1.2f);
            Gdiplus::SolidBrush head(ToGp(g_theme.primaryText));
            const float arrow = static_cast<float>(Dp(5));
            if (xMin <= 0.0 && xMax >= 0.0)
            {
                const auto sx = static_cast<Gdiplus::REAL>(toScreenX(0.0));
                g.DrawLine(&axis, sx, static_cast<Gdiplus::REAL>(plot.top + Dp(6)), sx,
                           static_cast<Gdiplus::REAL>(plot.bottom));
                const Gdiplus::PointF tip[3] = { { sx, static_cast<Gdiplus::REAL>(plot.top) },
                                                 { sx - arrow * 0.6f, static_cast<Gdiplus::REAL>(plot.top) + arrow },
                                                 { sx + arrow * 0.6f, static_cast<Gdiplus::REAL>(plot.top) + arrow } };
                g.FillPolygon(&head, tip, 3);
            }
            if (yMin <= 0.0 && yMax >= 0.0)
            {
                const auto sy = static_cast<Gdiplus::REAL>(toScreenY(0.0));
                g.DrawLine(&axis, static_cast<Gdiplus::REAL>(plot.left), sy,
                           static_cast<Gdiplus::REAL>(plot.right - Dp(6)), sy);
                const Gdiplus::PointF tip[3] = { { static_cast<Gdiplus::REAL>(plot.right), sy },
                                                 { static_cast<Gdiplus::REAL>(plot.right) - arrow, sy - arrow * 0.6f },
                                                 { static_cast<Gdiplus::REAL>(plot.right) - arrow, sy + arrow * 0.6f } };
                g.FillPolygon(&head, tip, 3);
            }
        }

        // The curves. One sample per pixel column, with the polyline broken
        // wherever the function is undefined or jumps the way it does either
        // side of an asymptote.
        {
            // Held across paints rather than rebuilt each time: the plot
            // repaints on every hover and every pan, and this is one allocation
            // of a few thousand points in the hottest path in the app.
            static std::vector<Gdiplus::PointF> points;
            points.clear();
            points.reserve(static_cast<size_t>(width) + 2);
            const double breakThreshold = ySpan * 4.0;

            for (const auto& equation : app.m_equations)
            {
                if (!equation.visible || !equation.valid || equation.expression.Empty())
                {
                    continue;
                }
                Gdiplus::Pen curve(ToGp(EquationColour(equation.colour)), 1.0f + static_cast<float>(app.m_graphThickness));
                curve.SetLineJoin(Gdiplus::LineJoinRound);

                points.clear();
                double previous = 0.0;
                bool havePrevious = false;

                for (int px = 0; px <= width; ++px)
                {
                    const double x = xMin + xSpan * static_cast<double>(px) / static_cast<double>(width);
                    const double y = equation.expression.Evaluate(x, app.m_graphAngle);

                    const bool usable = std::isfinite(y);
                    const bool jumped = havePrevious && std::fabs(y - previous) > breakThreshold;
                    if (!usable || jumped)
                    {
                        if (points.size() > 1)
                        {
                            g.DrawLines(&curve, points.data(), static_cast<INT>(points.size()));
                        }
                        points.clear();
                        havePrevious = usable;
                        previous = y;
                        if (!usable)
                        {
                            continue;
                        }
                    }

                    // Clamp far off-screen values rather than drop them, so a
                    // steep curve still enters and leaves the view correctly.
                    const double clamped = (std::min)((std::max)(y, yMin - ySpan), yMax + ySpan);
                    points.push_back(Gdiplus::PointF(
                        static_cast<Gdiplus::REAL>(toScreenX(x)), static_cast<Gdiplus::REAL>(toScreenY(clamped))));
                    previous = y;
                    havePrevious = true;
                }
                if (points.size() > 1)
                {
                    g.DrawLines(&curve, points.data(), static_cast<INT>(points.size()));
                }
            }
        }

        g.ResetClip();

        // The plot's controls sit on rounded cards, as they do in the shipping
        // graph view. Drawn here so the buttons themselves land on top.
        {
            const auto cardFor = [&](std::initializer_list<int> actions) {
                RECT box{ 0, 0, 0, 0 };
                bool first = true;
                for (const Btn& b : app.m_buttons)
                {
                    bool wanted = false;
                    for (const int action : actions)
                    {
                        if (b.action == action)
                        {
                            wanted = true;
                            break;
                        }
                    }
                    if (!wanted)
                    {
                        continue;
                    }
                    if (first)
                    {
                        box = b.rc;
                        first = false;
                    }
                    else
                    {
                        box.left = (std::min)(box.left, b.rc.left);
                        box.top = (std::min)(box.top, b.rc.top);
                        box.right = (std::max)(box.right, b.rc.right);
                        box.bottom = (std::max)(box.bottom, b.rc.bottom);
                    }
                }
                if (!first)
                {
                    box.left -= Dp(3);
                    box.top -= Dp(3);
                    box.right += Dp(3);
                    box.bottom += Dp(3);
                    FillRounded(g, box, Dp(6), g_theme.dark ? Mix(g_theme.card, RGB(255, 255, 255), 0.06) : g_theme.page);
                }
            };
            cardFor({ ACT_GRAPH_TRACE, ACT_GRAPH_SHARE, ACT_GRAPH_OPTIONS });
            cardFor({ ACT_GRAPH_ZOOM_IN, ACT_GRAPH_ZOOM_OUT, ACT_GRAPH_RESET });
        }

        // The shipping plot labels the axes themselves rather than the ticks:
        // an italic x and y at the arrow ends, and a single 0 at the origin.
        {
            const bool haveY = (xMin <= 0.0 && xMax >= 0.0);
            const bool haveX = (yMin <= 0.0 && yMax >= 0.0);
            if (haveY)
            {
                const int sx = static_cast<int>(toScreenX(0.0));
                RECT label{ sx + Dp(6), plot.top + Dp(2), sx + Dp(28), plot.top + Dp(20) };
                DrawLabel(hdc, label, L"y", 13, g_theme.primaryText, false,
                          DT_SINGLELINE | DT_LEFT | DT_VCENTER, FW_NORMAL, 0, true);
            }
            if (haveX)
            {
                const int sy = static_cast<int>(toScreenY(0.0));
                RECT label{ plot.right - Dp(22), sy + Dp(4), plot.right - Dp(4), sy + Dp(22) };
                DrawLabel(hdc, label, L"x", 13, g_theme.primaryText, false,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER, FW_NORMAL, 0, true);
            }
            if (haveX && haveY)
            {
                const int sx = static_cast<int>(toScreenX(0.0));
                const int sy = static_cast<int>(toScreenY(0.0));
                RECT label{ sx - Dp(22), sy + Dp(2), sx - Dp(4), sy + Dp(20) };
                DrawLabel(hdc, label, L"0", 12, g_theme.secondaryText, false,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
            }
        }

        // The confirmation sits low over the plot, out of the way of the
        // command bar in the opposite corner.
        const float toast = app.m_toastAnim.Value();
        if (toast > 0.004f && !app.m_toastText.empty())
        {
            const int textWidth = MeasureTextWidth(app.m_toastText, 12, FW_NORMAL, 0);
            const int pillWidth = textWidth + Dp(24);
            const int pillHeight = Dp(28);
            const int cx = (plot.left + plot.right) / 2;
            RECT pill{ cx - pillWidth / 2, plot.bottom - Dp(20) - pillHeight, cx + pillWidth / 2, plot.bottom - Dp(20) };

            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const int alpha = static_cast<int>(230.0f * toast);
            FillRounded(g, pill, pillHeight / 2, g_theme.dark ? RGB(60, 60, 60) : RGB(44, 44, 44), alpha);
            DrawLabel(hdc, pill, app.m_toastText, 12, Blend(g_theme.page, RGB(255, 255, 255), toast), false,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
    }

    void EnsureGraphingSafe(CalcApp& app)
    {
        if (app.m_equations.empty())
        {
            app.EnsureGraphing();
        }
        if (app.m_activeEquation < 0 || app.m_activeEquation >= static_cast<int>(app.m_equations.size()))
        {
            app.m_activeEquation = 0;
        }
    }

    // The graph options pane, drawn over the plot.
    void PaintGraphOptions(HDC hdc, CalcApp& app)
    {
        if (!app.m_graphOptionsOpen || app.m_graphEquationsView)
        {
            return;
        }
        const RECT& panel = app.m_graphOptionsRect;
        if (panel.right <= panel.left)
        {
            return;
        }

        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        FillRounded(g, panel, Dp(8), g_theme.card);
        {
            Gdiplus::Pen outline(ToGp(g_theme.divider));
            Gdiplus::GraphicsPath path;
            const int r = Dp(8);
            path.AddArc(panel.left, panel.top, r * 2, r * 2, 180.0f, 90.0f);
            path.AddArc(panel.right - r * 2, panel.top, r * 2, r * 2, 270.0f, 90.0f);
            path.AddArc(panel.right - r * 2, panel.bottom - r * 2, r * 2, r * 2, 0.0f, 90.0f);
            path.AddArc(panel.left, panel.bottom - r * 2, r * 2, r * 2, 90.0f, 90.0f);
            path.CloseFigure();
            g.DrawPath(&outline, &path);
        }

        const int left = panel.left + Dp(12);
        RECT title{ left, panel.top + Dp(10), panel.right - Dp(12), panel.top + Dp(42) };
        DrawLabel(hdc, title, L"Graph options", 20, g_theme.primaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER,
                  FW_SEMIBOLD);

        const auto section = [&](const wchar_t* text, int top) {
            RECT rc{ left, top, panel.right - Dp(12), top + Dp(18) };
            DrawLabel(hdc, rc, text, 12, g_theme.secondaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        };

        // Each section header sits just above the controls the layout placed.
        const Btn* firstField = nullptr;
        const Btn* firstUnit = nullptr;
        const Btn* thickness = nullptr;
        const Btn* firstTheme = nullptr;
        for (const Btn& b : app.m_buttons)
        {
            if (b.action == ACT_GRAPH_FIELD_BASE && firstField == nullptr) firstField = &b;
            if (b.action == ACT_GRAPH_ANGLE_RAD && firstUnit == nullptr) firstUnit = &b;
            if (b.action == ACT_GRAPH_THICKNESS && thickness == nullptr) thickness = &b;
            if (b.action == ACT_GRAPH_THEME_LIGHT && firstTheme == nullptr) firstTheme = &b;
        }

        if (firstField != nullptr)
        {
            section(L"Window", firstField->rc.top - Dp(38));
            static constexpr const wchar_t* kFieldLabels[4] = { L"X-Min", L"X-Max", L"Y-Min", L"Y-Max" };
            for (int i = 0; i < 4; ++i)
            {
                const Btn* field = nullptr;
                for (const Btn& b : app.m_buttons)
                {
                    if (b.action == ACT_GRAPH_FIELD_BASE + i)
                    {
                        field = &b;
                        break;
                    }
                }
                if (field == nullptr)
                {
                    continue;
                }
                RECT caption{ field->rc.left, field->rc.top - Dp(18), field->rc.right, field->rc.top - Dp(2) };
                DrawLabel(hdc, caption, kFieldLabels[i], 12, g_theme.secondaryText, false,
                          DT_SINGLELINE | DT_LEFT | DT_VCENTER);

                Gdiplus::Pen border(ToGp(app.m_graphField == i ? g_theme.accent : g_theme.divider));
                g.DrawRectangle(&border, Gdiplus::Rect(field->rc.left, field->rc.top,
                                                       field->rc.right - field->rc.left - 1,
                                                       field->rc.bottom - field->rc.top - 1));
                std::wstring value = app.GraphFieldText(i);
                if (app.m_graphField == i)
                {
                    value += L"|";
                }
                RECT text{ field->rc.left + Dp(8), field->rc.top, field->rc.right - Dp(8), field->rc.bottom };
                DrawLabel(hdc, text, value, 14, g_theme.primaryText, false,
                          DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }
        if (firstUnit != nullptr)
        {
            section(L"Units", firstUnit->rc.top - Dp(20));
        }
        if (thickness != nullptr)
        {
            section(L"Line thickness", thickness->rc.top - Dp(20));
            // A sample of the width the curves are drawn at.
            Gdiplus::Pen sample(ToGp(g_theme.primaryText), 1.0f + static_cast<float>(app.m_graphThickness));
            const int centre = (thickness->rc.top + thickness->rc.bottom) / 2;
            g.DrawLine(&sample, thickness->rc.left + Dp(10), centre, thickness->rc.right - Dp(40), centre);
        }
        if (firstTheme != nullptr)
        {
            section(L"Graph theme", firstTheme->rc.top - Dp(20));
        }
    }

    // The equation editor: the f badge the shipping app puts beside the field,
    // and the expression being typed.
    void PaintGraphing(HDC hdc, CalcApp& app)
    {
        if (!app.m_graphEquationsView)
        {
            return;
        }
        const RECT& field = app.m_graphEquationRect;
        if (field.right <= field.left)
        {
            return;
        }

        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        const int badge = field.bottom - field.top - Dp(12);
        RECT badgeRect{ field.left + Dp(6), field.top + Dp(6), field.left + Dp(6) + badge, field.top + Dp(6) + badge };
        FillRounded(g, badgeRect, Dp(4), g_theme.flatHover);
        DrawLabel(
            hdc, badgeRect, L"f", 18, g_theme.primaryText, false, DT_SINGLELINE | DT_CENTER | DT_VCENTER, FW_NORMAL, 0,
            true);

        EnsureGraphingSafe(app);
        const auto& equation = app.m_equations[app.m_activeEquation];
        RECT text{ badgeRect.right + Dp(12), field.top, field.right - Dp(10), field.bottom };
        if (equation.text.empty())
        {
            DrawLabel(hdc, text, L"Enter an expression", 14, g_theme.disabledText, false,
                      DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
        else
        {
            std::wstring shown = equation.text;
            shown += L"|"; // the caret
            DrawLabel(hdc, text, shown, 14, equation.valid ? g_theme.primaryText : g_theme.disabledText, false,
                      DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }

        // The colour of the equation being edited, on the leading edge.
        RECT swatch{ field.left, field.top + Dp(10), field.left + Dp(3), field.bottom - Dp(10) };
        HBRUSH brush = CreateSolidBrush(EquationColour(equation.colour));
        FillRect(hdc, &swatch, brush);
        DeleteObject(brush);
    }

    void PaintDate(HDC hdc, CalcApp& app)
    {
        const bool isDifference = app.m_dateModel.CurrentMode() == DateCalcModel::Mode::Difference;

        // Captions sit just above each row of date segments.
        for (const Btn& b : app.m_buttons)
        {
            if (b.action < ACT_DATE_FIELD_BASE || b.action > ACT_DATE_FIELD_BASE + 2)
            {
                continue;
            }
            const int field = b.action - ACT_DATE_FIELD_BASE;
            RECT caption{ b.rc.left, b.rc.top - Dp(18), b.rc.right, b.rc.top - Dp(2) };
            DrawCaption(hdc, caption, (field == 1) ? L"To" : L"From");
        }

        if (isDifference)
        {
            const DateDifference difference = app.m_dateModel.Difference();

            RECT caption = app.m_dateResultRect;
            caption.top -= Dp(20);
            caption.bottom = app.m_dateResultRect.top - Dp(2);
            DrawCaption(hdc, caption, L"Difference");

            DrawLabel(
                hdc,
                app.m_dateResultRect,
                DateMath::DescribeDifference(difference),
                15,
                g_theme.primaryText,
                false,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

            std::wstring total = CalcEngine::NumericString::FromInteger(difference.totalDays);
            total += (difference.totalDays == 1) ? L" day" : L" days";
            DrawLabel(hdc, app.m_dateSecondaryRect, total, 13, g_theme.secondaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        }
        else
        {
            for (int i = 0; i < 3; ++i)
            {
                DrawCaption(hdc, app.m_dateCaptionRects[i], app.m_dateCaptions[i]);
            }

            RECT caption = app.m_dateResultRect;
            caption.top -= Dp(20);
            caption.bottom = app.m_dateResultRect.top - Dp(2);
            DrawCaption(hdc, caption, L"Date");

            DrawLabel(
                hdc,
                app.m_dateResultRect,
                DateMath::FormatLong(app.m_dateModel.Result()),
                15,
                g_theme.primaryText,
                false,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }


    // The navigation pane and settings page are surfaces above the content: an
    // opaque panel with a soft edge, sliding in from the left.
    void ButtonMotion(const CalcApp& app, const Btn& b, int index, int hotIndex, int pressedIndex, float& hot, float& pressed)
    {
        if (index == hotIndex)
        {
            hot = app.m_hoverIn.Value();
        }
        else if (b.action != ACT_NONE && b.action == app.m_hoverOutAction)
        {
            hot = app.m_hoverOut.Value();
        }
        if (index == pressedIndex)
        {
            pressed = app.m_pressAnim.Value();
        }
    }

    // A cached off-screen surface.
    //
    // Every frame used to create and destroy a full-window bitmap: at 360x620
    // that is 892KB allocated and freed, and an animating frame wanted two or
    // three of them at once. Holding one and recreating it only when the client
    // area changes size draws exactly the same pixels while allocating nothing
    // per frame, and lets the surfaces be handed back entirely when the app is
    // idle.
    class Surface
    {
    public:
        HDC Acquire(HDC reference, int width, int height)
        {
            if (m_dc != nullptr && (m_width != width || m_height != height))
            {
                Release();
            }
            if (m_dc == nullptr)
            {
                if (width <= 0 || height <= 0)
                {
                    return nullptr;
                }
                m_dc = CreateCompatibleDC(reference);
                if (m_dc == nullptr)
                {
                    return nullptr;
                }
                m_bitmap = CreateCompatibleBitmap(reference, width, height);
                if (m_bitmap == nullptr)
                {
                    DeleteDC(m_dc);
                    m_dc = nullptr;
                    return nullptr;
                }
                m_previous = SelectObject(m_dc, m_bitmap);
                m_width = width;
                m_height = height;
            }
            return m_dc;
        }

        void Release()
        {
            if (m_dc == nullptr)
            {
                return;
            }
            SelectObject(m_dc, m_previous);
            DeleteObject(m_bitmap);
            DeleteDC(m_dc);
            m_dc = nullptr;
            m_bitmap = nullptr;
            m_previous = nullptr;
            m_width = 0;
            m_height = 0;
        }

        bool Held() const
        {
            return m_dc != nullptr;
        }

        HDC Dc() const
        {
            return m_dc;
        }

        bool Matches(int width, int height) const
        {
            return m_dc != nullptr && m_width == width && m_height == height;
        }

    private:
        HDC m_dc = nullptr;
        HBITMAP m_bitmap = nullptr;
        HGDIOBJ m_previous = nullptr;
        int m_width = 0;
        int m_height = 0;
    };

    // The window's double buffer, and one scratch layer shared by the two
    // transitions that composite through AlphaBlend. They never overlap within
    // a frame: the content layer is blended and finished with before the
    // overlays are drawn.
    // Hands back the pages an animation touched. The working set is what Task
    // Manager reports as an app's memory, and a burst of compositing leaves a
    // lot of it resident that nothing will read again. Pages fault back in on
    // demand, so this changes the footprint and nothing else.
    // Empties the working set and decommits the heap's free blocks.
    //
    // Only worth doing when the window is minimised. Run on a short idle timer
    // instead -- which is what this used to do -- it sets up a sawtooth: the
    // trim drops the working set, the very next paint reallocates the back
    // buffer and faults every page back in, and two seconds later it all
    // happens again. Task Manager shows that as memory jumping about, and the
    // page faults make the paint slower into the bargain. The average was no
    // better than simply holding one buffer steady.
    void TrimWorkingSet()
    {
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));

        // Also decommit the heap's free blocks. Laying out a mode churns a lot
        // of small allocations; once they are freed the heap keeps the pages
        // unless it is asked to give them back. Resolved at runtime because the
        // mingw headers predate the flag.
        struct OptimizeResources
        {
            DWORD version;
            DWORD flags;
        };
        using HeapSetInformationFn = BOOL(WINAPI*)(HANDLE, int, PVOID, SIZE_T);
        static const auto heapSetInformation = reinterpret_cast<HeapSetInformationFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSetInformation")));
        if (heapSetInformation != nullptr)
        {
            OptimizeResources optimize{ 1, 0 };
            heapSetInformation(nullptr, 3 /* HeapOptimizeResources */, &optimize, sizeof(optimize));
        }
    }

    // Never composite more often than this, whatever DwmFlush does.
    constexpr ULONGLONG kMinFrameMs = 16; // 60fps; the compositor will not show more
    ULONGLONG g_lastFrameTick = 0;

    Surface g_backBuffer;
    Surface g_scratch;

    // Holds the frame as it looked under the outgoing theme, for the length of
    // the cross-fade and no longer.
    Surface g_themeLayer;

    // Set while g_scratch holds an outer composite, so a nested fade knows to
    // draw straight to its target instead of stealing the layer.
    bool g_scratchHeld = false;

    // Switching theme repaints every surface in the window at once, which is
    // far too much to change in a single frame. Grabbing the frame as it is
    // now, before the palette is swapped, lets the new one fade up underneath
    // it -- and at 450ms it is a deliberate, unhurried change rather than a
    // flash. Nothing is kept afterwards: the copy goes as soon as it ends.
    constexpr int kThemeFadeMs = 450;

    // Share, by the only route a desktop process has.
    //
    // The shipping app hands the plot to the Windows share sheet, which is a
    // WinRT surface and out of reach from here. The clipboard carries the same
    // two payloads to the same places: the plot as a bitmap, for anything that
    // takes an image, and the equations as text for anything that does not.
    bool CopyGraphToClipboard(CalcApp& app)
    {
        const RECT plot = app.m_graphRect;
        const int width = plot.right - plot.left;
        const int height = plot.bottom - plot.top;
        HDC source = g_backBuffer.Dc();
        if (source == nullptr || width <= 0 || height <= 0)
        {
            return false;
        }

        HDC target = CreateCompatibleDC(source);
        if (target == nullptr)
        {
            return false;
        }
        HBITMAP bitmap = CreateCompatibleBitmap(source, width, height);
        if (bitmap == nullptr)
        {
            DeleteDC(target);
            return false;
        }
        HGDIOBJ previousBitmap = SelectObject(target, bitmap);
        BitBlt(target, 0, 0, width, height, source, plot.left, plot.top, SRCCOPY);
        // The clipboard takes ownership of a bitmap that is not selected
        // anywhere, so put the DC back the way it was before handing it over.
        SelectObject(target, previousBitmap);
        DeleteDC(target);

        std::wstring equations;
        for (const auto& equation : app.m_equations)
        {
            if (equation.text.empty())
            {
                continue;
            }
            if (!equations.empty())
            {
                equations.append(L"\r\n");
            }
            equations.append(L"y=");
            equations.append(equation.text);
        }

        HGLOBAL text = nullptr;
        if (!equations.empty())
        {
            text = GlobalAlloc(GMEM_MOVEABLE, (equations.size() + 1) * sizeof(wchar_t));
            if (text != nullptr)
            {
                void* buffer = GlobalLock(text);
                if (buffer != nullptr)
                {
                    memcpy(buffer, equations.c_str(), (equations.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(text);
                }
                else
                {
                    GlobalFree(text);
                    text = nullptr;
                }
            }
        }

        if (!OpenClipboard(app.m_hwnd))
        {
            DeleteObject(bitmap);
            if (text != nullptr)
            {
                GlobalFree(text);
            }
            return false;
        }
        EmptyClipboard();
        const bool placed = SetClipboardData(CF_BITMAP, bitmap) != nullptr;
        if (!placed)
        {
            DeleteObject(bitmap);
        }
        if (text != nullptr && SetClipboardData(CF_UNICODETEXT, text) == nullptr)
        {
            GlobalFree(text);
        }
        CloseClipboard();
        return placed;
    }

    void BeginThemeFade(CalcApp& app)
    {
        if (app.m_hwnd == nullptr)
        {
            return;
        }
        RECT client{};
        GetClientRect(app.m_hwnd, &client);
        const int width = client.right;
        const int height = client.bottom;
        HDC previous = g_backBuffer.Dc();
        if (width <= 0 || height <= 0 || !g_backBuffer.Matches(width, height))
        {
            return;
        }
        HDC layer = g_themeLayer.Acquire(previous, width, height);
        if (layer == nullptr)
        {
            return;
        }
        BitBlt(layer, 0, 0, width, height, previous, 0, 0, SRCCOPY);
        app.m_themeAnim.Set(1.0f);
        app.m_themeAnim.To(0.0f, kThemeFadeMs, EaseStandard);
        app.EnsureFrameTimer();
    }

    void PaintNavOverlay(HDC hdc, CalcApp& app, int width, int height)
    {
        const bool nav = app.NavVisible();
        const bool settings = !nav && app.SettingsVisible();
        if (!nav && !settings)
        {
            return;
        }

        const float progress = nav ? app.m_navAnim.Value() : app.m_settingsAnim.Value();
        const int slide = app.m_navSlide;

        {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            // A scrim over the content, deepening as the pane comes in.
            const BYTE scrim = static_cast<BYTE>(70.0f * progress);
            if (scrim != 0)
            {
                Gdiplus::SolidBrush shade(ToGp(g_theme.dark ? RGB(0, 0, 0) : RGB(60, 60, 60), scrim));
                g.FillRectangle(&shade, 0, 0, width, height);
            }

            const int paneWidth = (app.m_navPaneWidth > 0) ? app.m_navPaneWidth : width;
            RECT surface{ -slide, 0, paneWidth - slide, height };
            Gdiplus::SolidBrush panel(ToGp(g_theme.dark ? Mix(g_theme.page, RGB(255, 255, 255), 0.04) : g_theme.card));
            g.FillRectangle(
                &panel,
                static_cast<INT>(surface.left),
                static_cast<INT>(surface.top),
                static_cast<INT>(surface.right - surface.left),
                static_cast<INT>(surface.bottom - surface.top));

            Gdiplus::Pen edge(ToGp(g_theme.divider));
            g.DrawLine(
                &edge,
                static_cast<INT>(surface.right),
                static_cast<INT>(surface.top),
                static_cast<INT>(surface.right),
                static_cast<INT>(surface.bottom));

            if (nav)
            {
                // The WinUI selection indicator: a rounded accent bar on the
                // leading edge of the selected item.
                Gdiplus::SolidBrush accent(ToGp(g_theme.accent));
                for (const Btn& b : app.m_navButtons)
                {
                    if (!b.checked || b.style != Style::Flat)
                    {
                        continue;
                    }
                    const int barHeight = Dp(16);
                    const int centre = (b.rc.top + b.rc.bottom) / 2;
                    const int left = surface.left + Dp(4);
                    FillRounded(
                        g, RECT{ left, centre - barHeight / 2, left + Dp(3), centre + barHeight / 2 }, Dp(1),
                        g_theme.accent);
                }

                // And a scrollbar when the list is taller than the pane.
                const int listTop = Dp(kNavRowDip);
                const int listBottom = height - Dp(52);
                const int visible = listBottom - listTop;
                if (visible > 0 && app.m_navContentHeight > visible)
                {
                    const float span = static_cast<float>(app.m_navContentHeight);
                    const int thumbHeight = (std::max)(Dp(24), static_cast<int>(static_cast<float>(visible) * static_cast<float>(visible) / span));
                    const float maxScroll = span - static_cast<float>(visible);
                    const float ratio = (maxScroll > 0.0f) ? (static_cast<float>(app.m_navScroll) / maxScroll) : 0.0f;
                    const int travel = visible - thumbHeight;
                    const int thumbTop = listTop + static_cast<int>(ratio * static_cast<float>(travel));
                    const int right = surface.right - Dp(3);
                    FillRounded(
                        g, RECT{ right - Dp(2), thumbTop, right, thumbTop + thumbHeight }, Dp(1),
                        g_theme.secondaryText);
                }
            }

            if (settings)
            {
                // An open expander's panel is a card butted under its header.
                const RECT panels[2] = { app.m_settingsThemePanel, app.m_settingsAboutPanel };
                for (const RECT& panel : panels)
                {
                    if (panel.right <= panel.left)
                    {
                        continue;
                    }
                    RECT r = panel;
                    r.left -= slide;
                    r.right -= slide;
                    FillRounded(g, r, Dp(6), g_theme.card);
                    Gdiplus::Pen outline(ToGp(g_theme.divider));
                    Gdiplus::GraphicsPath path;
                    const int radius = Dp(6);
                    path.AddArc(r.left, r.top, radius * 2, radius * 2, 180.0f, 90.0f);
                    path.AddArc(r.right - radius * 2, r.top, radius * 2, radius * 2, 270.0f, 90.0f);
                    path.AddArc(r.right - radius * 2, r.bottom - radius * 2, radius * 2, radius * 2, 0.0f, 90.0f);
                    path.AddArc(r.left, r.bottom - radius * 2, radius * 2, radius * 2, 90.0f, 90.0f);
                    path.CloseFigure();
                    g.DrawPath(&outline, &path);
                }
            }

            for (size_t i = 0; i < app.m_navButtons.size(); ++i)
            {
                const Btn& b = app.m_navButtons[i];
                float hot = 0.0f;
                float pressed = 0.0f;
                ButtonMotion(app, b, static_cast<int>(i), app.m_hotNav, app.m_pressedNav, hot, pressed);
                bool painted = false;
                const COLORREF fill = ButtonFill(b, hot, pressed, painted);
                if (painted)
                {
                    const RECT rc = PressedRect(b.rc, pressed);
                    FillRounded(g, rc, ButtonRadius(b), fill);
                    if (b.style == Style::Card)
                    {
                        Gdiplus::Pen outline(ToGp(g_theme.divider));
                        Gdiplus::GraphicsPath path;
                        const int r = Dp(6);
                        path.AddArc(rc.left, rc.top, r * 2, r * 2, 180.0f, 90.0f);
                        path.AddArc(rc.right - r * 2, rc.top, r * 2, r * 2, 270.0f, 90.0f);
                        path.AddArc(rc.right - r * 2, rc.bottom - r * 2, r * 2, r * 2, 0.0f, 90.0f);
                        path.AddArc(rc.left, rc.bottom - r * 2, r * 2, r * 2, 90.0f, 90.0f);
                        path.CloseFigure();
                        g.DrawPath(&outline, &path);
                    }
                }
            }
        }

        {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            for (const Btn& b : app.m_navButtons)
            {
                if (b.radio)
                {
                    RECT box{ b.rc.left + Dp(2), b.rc.top, b.rc.left + Dp(34), b.rc.bottom };
                    DrawVectorIcon(g, box, b.checked ? VIcon_RadioOn : VIcon_RadioOff, b.checked ? g_theme.accent : g_theme.secondaryText, 19);
                    continue;
                }
                const int id = (b.vicon != VIcon_None) ? b.vicon : VIconForGlyph(b.label);
                if (id == VIcon_None)
                {
                    continue;
                }
                // An expander header carries its icon on the leading edge.
                RECT box = b.rc;
                if (b.style == Style::Card)
                {
                    box = RECT{ b.rc.left + Dp(14), b.rc.top, b.rc.left + Dp(46), b.rc.bottom };
                }
                DrawVectorIcon(g, box, id, ButtonText(b), (b.textDip > 0) ? b.textDip : 16);
            }
        }

        for (const Btn& b : app.m_navButtons)
        {
            // A card or radio row draws its icon beside the label; everything
            // else with an icon is icon-only, so its label is the glyph.
            const bool iconOnly = (b.style != Style::Card && !b.radio)
                && (b.vicon != VIcon_None || VIconForGlyph(b.label) != VIcon_None);
            if (b.label.empty() || iconOnly)
            {
                continue;
            }
            const COLORREF color = ButtonText(b);
            if (b.icon)
            {
                DrawLabel(hdc, b.rc, b.label, DefaultTextDip(b), color, true, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            }
            else if (b.style == Style::Card)
            {
                RECT text{ b.rc.left + Dp(52), b.rc.top + Dp(12), b.rc.right - Dp(44), b.rc.top + Dp(34) };
                DrawLabel(hdc, text, b.label, 14, color, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            }
            else if (b.radio)
            {
                RECT text = b.rc;
                text.left += Dp(38);
                DrawLabel(hdc, text, b.label, 14, color, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            }
            else
            {
                RECT text = b.rc;
                if (b.leftAlign)
                {
                    text.left += Dp(44);
                }
                DrawLabel(
                    hdc, text, b.label, DefaultTextDip(b), color, false,
                    DT_SINGLELINE | (b.leftAlign ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }

        if (nav)
        {
            for (const auto& entry : app.m_navGlyphs)
            {
                if (entry.rc.right == entry.rc.left || entry.glyph.empty())
                {
                    continue;
                }
                RECT glyph = entry.rc;
                glyph.left += Dp(10);
                glyph.right = glyph.left + Dp(28);
                const int id = VIconForGlyph(entry.glyph);
                if (id != VIcon_None)
                {
                    Gdiplus::Graphics g(hdc);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    DrawVectorIcon(g, glyph, id, g_theme.primaryText, 16);
                }
                else
                {
                    DrawLabel(hdc, glyph, entry.glyph, 15, g_theme.primaryText, true, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                }
            }
            return;
        }

        RECT title{ Dp(24) - slide, Dp(48), width - Dp(24) - slide, Dp(96) };
        DrawLabel(hdc, title, L"Settings", kTitleDip, g_theme.primaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER, FW_SEMIBOLD, 1);

        const auto shifted = [&](RECT r) {
            r.left -= slide;
            r.right -= slide;
            return r;
        };

        // BodyStrongTextBlockStyle: body size, semibold.
        DrawLabel(
            hdc, shifted(app.m_settingsAppearanceRect), L"Appearance", 14, g_theme.primaryText, false,
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, FW_SEMIBOLD);
        DrawLabel(
            hdc, shifted(app.m_settingsAboutRect), L"About", 14, g_theme.primaryText, false,
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, FW_SEMIBOLD);

        // Each expander header carries a description under its title and a
        // chevron that flips when the panel is open.
        for (const Btn& b : app.m_navButtons)
        {
            if (b.style != Style::Card)
            {
                continue;
            }
            const bool isTheme = (b.action == ACT_APP_THEME);
            const wchar_t* subtitle = isTheme
                ? L"Select which app theme to display"
                : L"\u00a9 Microsoft Corporation. MIT Licensed.";

            RECT line{ b.rc.left + Dp(52), b.rc.top + Dp(33), b.rc.right - Dp(44), b.rc.top + Dp(53) };
            DrawLabel(hdc, line, subtitle, 12, g_theme.secondaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

            const bool open = isTheme ? app.m_themeExpanded : app.m_aboutExpanded;
            RECT chevron{ b.rc.right - Dp(38), b.rc.top, b.rc.right - Dp(12), b.rc.bottom };
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            DrawVectorIcon(g, chevron, open ? VIcon_ChevronUp : VIcon_ChevronDown, g_theme.secondaryText, 14);
        }

        // The version sits where the About expander's own content does.
        for (const Btn& b : app.m_navButtons)
        {
            if (b.style == Style::Card && b.action == ACT_ABOUT_EXPAND)
            {
                RECT version{ b.rc.left + Dp(52), b.rc.top + Dp(51), b.rc.right - Dp(44), b.rc.top + Dp(71) };
                DrawLabel(hdc, version, L"" CALC_VERSION_FULL, 12, g_theme.secondaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
            }
        }

        DrawLabel(
            hdc, shifted(app.m_settingsFeedbackRect), L"Send feedback", 14, g_theme.accent, false,
            DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        // The closing paragraph wraps, with GitHub set as a link on its last line.
        RECT contribute = shifted(app.m_settingsContributeRect);
        DrawLabel(
            hdc, contribute, L"To learn how you can contribute to Windows Calculator, check out the project on", 14,
            g_theme.primaryText, false, DT_WORDBREAK | DT_LEFT | DT_TOP);
        RECT linkRect = contribute;
        linkRect.top = contribute.bottom;
        linkRect.bottom = linkRect.top + Dp(22);
        DrawLabel(hdc, linkRect, L"GitHub.", 14, g_theme.accent, false, DT_SINGLELINE | DT_LEFT | DT_TOP);
    }

    // Everything below the overlay surfaces. Split out so a mode change can
    // render it to its own layer and fade that in.
    void PaintContent(HDC hdc, int width, int height)
    {
        CalcApp& app = g_app;

        RECT full{ 0, 0, width, height };
        HBRUSH background = CreateSolidBrush(g_theme.page);
        FillRect(hdc, &full, background);
        DeleteObject(background);

        SetBkMode(hdc, TRANSPARENT);

        if (app.m_mode == Mode::Graphing)
        {
            PaintGraphPlot(hdc, app);
            PaintGraphOptions(hdc, app);
        }

        // Nothing panel-shaped has been drawn yet, so this is the frame the
        // panel fades up from -- and back down to when it is dismissed.
        HDC panelLayer = nullptr;
        if (app.m_panel != Panel::None && !app.m_docked && !g_scratchHeld && width > 0 && height > 0
            && app.m_panelAnim.Value() < 0.995f)
        {
            panelLayer = g_scratch.Acquire(hdc, width, height);
            if (panelLayer != nullptr)
            {
                g_scratchHeld = true;
                BitBlt(panelLayer, 0, 0, width, height, hdc, 0, 0, SRCCOPY);
            }
        }

        // --- shapes -------------------------------------------------------
        {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            if (app.m_panel != Panel::None)
            {
                FillRounded(g, app.m_panelRect, Dp(8), g_theme.card);
            }


            for (size_t i = 0; i < app.m_buttons.size(); ++i)
            {
                const Btn& b = app.m_buttons[i];
                float hot = 0.0f;
                float pressed = 0.0f;
                ButtonMotion(app, b, static_cast<int>(i), app.m_hot, app.m_pressed, hot, pressed);
                bool painted = false;
                const COLORREF fill = ButtonFill(b, hot, pressed, painted);
                if (painted)
                {
                    FillRounded(g, PressedRect(b.rc, pressed), Dp(kRadiusDip), fill);
                }
            }
        }

        // --- vector icons --------------------------------------------------
        {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            for (size_t i = 0; i < app.m_buttons.size(); ++i)
            {
                const Btn& b = app.m_buttons[i];
                if (b.radio)
                {
                    RECT box{ b.rc.left + Dp(2), b.rc.top, b.rc.left + Dp(34), b.rc.bottom };
                    DrawVectorIcon(g, box, b.checked ? VIcon_RadioOn : VIcon_RadioOff,
                                   b.checked ? g_theme.accent : g_theme.secondaryText, 19);
                    continue;
                }
                const int id = (b.vicon != VIcon_None) ? b.vicon : VIconForGlyph(b.label);
                if (id == VIcon_None)
                {
                    continue;
                }
                float hot = 0.0f;
                float pressed = 0.0f;
                ButtonMotion(app, b, static_cast<int>(i), app.m_hot, app.m_pressed, hot, pressed);
                RECT box = PressedRect(b.rc, pressed);
                if (b.leftAlign)
                {
                    // A row with both an icon and a label puts the icon in the
                    // leading box the label is indented past.
                    box = RECT{ box.left + Dp(8), box.top, box.left + Dp(36), box.bottom };
                }
                DrawVectorIcon(g, box, id, ButtonText(b), (b.textDip > 0) ? b.textDip : 16);
            }
        }

        // --- text ---------------------------------------------------------
        for (size_t i = 0; i < app.m_buttons.size(); ++i)
        {
            const Btn& b = app.m_buttons[i];
            const bool iconOnly = (b.style != Style::Card && !b.radio && !b.leftAlign)
                && (b.vicon != VIcon_None || VIconForGlyph(b.label) != VIcon_None);
            if (b.label.empty() || iconOnly)
            {
                continue;
            }
            float hot = 0.0f;
            float pressed = 0.0f;
            ButtonMotion(app, b, static_cast<int>(i), app.m_hot, app.m_pressed, hot, pressed);
            const RECT rc = PressedRect(b.rc, pressed);
            const int dip = DefaultTextDip(b);
            const COLORREF color = ButtonText(b);
            if (b.combo)
            {
                // A combo box anchors its text to the leading edge and parks
                // the chevron at the trailing one, rather than centring both.
                RECT text = rc;
                text.left += Dp(10);
                text.right -= Dp(30);
                DrawLabel(hdc, text, b.label, dip, color, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
                RECT chevron{ rc.right - Dp(28), rc.top, rc.right - Dp(8), rc.bottom };
                Gdiplus::Graphics g(hdc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                DrawVectorIcon(g, chevron, VIcon_ChevronDown, g_theme.secondaryText, 14);
            }
            else if (b.dateField)
            {
                RECT text = rc;
                text.left += Dp(10);
                text.right -= Dp(36);
                DrawLabel(hdc, text, b.label, dip, color, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
                RECT glyph{ rc.right - Dp(32), rc.top, rc.right - Dp(6), rc.bottom };
                Gdiplus::Graphics g(hdc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                DrawVectorIcon(g, glyph, VIcon_Calendar, g_theme.secondaryText, 16);
            }
            else if (b.icon)
            {
                DrawLabel(hdc, rc, b.label, dip, color, true, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            }
            else if (b.leftAlign)
            {
                RECT text = rc;
                text.left += Dp(44); // clear of the leading glyph
                DrawLabel(hdc, text, b.label, dip, color, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            }
            else if (b.radio)
            {
                RECT text = rc;
                text.left += Dp(38);
                DrawLabel(hdc, text, b.label, dip, color, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            }
            else if (IsVariableKey(b.action))
            {
                DrawLabel(hdc, rc, b.label, dip, color, false, DT_SINGLELINE | DT_CENTER | DT_VCENTER, FW_NORMAL, 0, true);
            }
            else
            {
                DrawMixedLabel(hdc, rc, b.label, dip, color);
            }
        }

        // Mode name beside the navigation button.
        {
            DrawLabel(
                hdc, app.m_titleRect, app.ModeName(), kTitleDip, g_theme.primaryText, false,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER, FW_SEMIBOLD);
        }

        if (app.m_mode == Mode::Converter)
        {
            PaintConverter(hdc, app);
        }
        else if (app.m_mode == Mode::Graphing)
        {
            PaintGraphing(hdc, app);
        }
        else if (app.m_mode == Mode::Date)
        {
            PaintDate(hdc, app);
        }
        else
        {
        // Expression line, with the open-parenthesis count when one is pending.
        {
            if (app.m_openParens > 0)
            {
                wchar_t parenText[16] = L"(";
                unsigned int value = app.m_openParens;
                wchar_t digits[12];
                int count = 0;
                do
                {
                    digits[count++] = static_cast<wchar_t>(L'0' + value % 10);
                    value /= 10;
                } while (value != 0 && count < 11);
                for (int i = 0; i < count; ++i)
                {
                    parenText[1 + i] = digits[count - 1 - i];
                }
                parenText[1 + count] = L'\0';

                RECT parens = app.m_exprRect;
                parens.left += Dp(8);
                DrawLabel(hdc, parens, parenText, 12, g_theme.secondaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
            }
            DrawTail(hdc, app.m_exprRect, app.m_expression, 12, g_theme.secondaryText);
        }

        DrawAutoFitNumber(hdc, app.m_displayRect, app.m_primary, app.m_isError ? g_theme.primaryText : g_theme.primaryText);

        // Programmer readouts: an accent bar marks the active row, then the radix
        // name and its value, both left aligned.
        if (app.m_mode == Mode::Programmer && app.m_panel == Panel::None)
        {
            static constexpr uint32_t radices[] = { 16, 10, 8, 2 };
            static constexpr const wchar_t* names[] = { L"HEX", L"DEC", L"OCT", L"BIN" };
            int found = 0;
            for (const Btn& b : app.m_buttons)
            {
                if (b.action < ACT_RADIX_HEX || b.action > ACT_RADIX_BIN)
                {
                    continue;
                }
                const int index = b.action - ACT_RADIX_HEX;

                if (b.checked)
                {
                    const int barHeight = Dp(14);
                    const int centre = (b.rc.top + b.rc.bottom) / 2;
                    RECT bar{ b.rc.left, centre - barHeight / 2, b.rc.left + Dp(3), centre + barHeight / 2 };
                    HBRUSH brush = CreateSolidBrush(g_theme.accent);
                    FillRect(hdc, &bar, brush);
                    DeleteObject(brush);
                }

                RECT name = b.rc;
                name.left += Dp(14);
                name.right = name.left + Dp(46);
                DrawLabel(
                    hdc, name, names[index], 12, b.checked ? g_theme.primaryText : g_theme.secondaryText, false,
                    DT_SINGLELINE | DT_LEFT | DT_VCENTER, b.checked ? FW_SEMIBOLD : FW_NORMAL);

                RECT value = b.rc;
                value.left += Dp(64);
                value.right -= Dp(8);
                DrawLabel(
                    hdc, value, app.RadixText(radices[index]), 13, b.checked ? g_theme.primaryText : g_theme.secondaryText, false,
                    DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

                if (++found == 4)
                {
                    break;
                }
            }
        }
        } // calculator-mode display

        if (app.m_panel != Panel::None)
        {
            PaintPanelContents(hdc, app);
        }

        // The panel cross-fades as well as slides, both ways. Its card, its
        // buttons and its rows are drawn by three separate passes above, so
        // rather than divert each one through a layer, the frame was copied
        // before the first of them: blending that copy back over the finished
        // frame is the same fade, and leaves everything outside the panel
        // untouched because there it is blending identical pixels.
        if (panelLayer != nullptr)
        {
            // Only the band the panel can occupy: the title row and the
            // display are drawn after the copy was taken, so blending over
            // them would wash them out along with it.
            const int top = (std::max)(0, app.m_panelTop);
            if (height > top)
            {
                BLENDFUNCTION blend{};
                blend.BlendOp = AC_SRC_OVER;
                blend.SourceConstantAlpha = static_cast<BYTE>(255.0f * (1.0f - app.m_panelAnim.Value()));
                AlphaBlend(hdc, 0, top, width, height - top, panelLayer, 0, top, width, height - top, blend);
            }
            g_scratchHeld = false;
        }
    }

    // The overlay surfaces: navigation pane or settings page, then any dropdown.
    void PaintFlyout(HDC hdc)
    {
        CalcApp& app = g_app;
        {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            RECT shadow = app.m_menuRect;
            InflateRect(&shadow, Dp(2), Dp(2));
            FillRounded(g, shadow, Dp(9), g_theme.dark ? RGB(0, 0, 0) : RGB(120, 120, 120), 60);
            FillRounded(g, app.m_menuRect, Dp(8), g_theme.dark ? RGB(45, 45, 45) : RGB(252, 252, 252));

            for (size_t i = 0; i < app.m_menuButtons.size(); ++i)
            {
                if (static_cast<int>(i) == app.m_hotMenu)
                {
                    FillRounded(g, app.m_menuButtons[i].rc, Dp(4), g_theme.flatHover);
                }
                else if (app.m_menuButtons[i].checked)
                {
                    FillRounded(g, app.m_menuButtons[i].rc, Dp(4), g_theme.flatPress);
                }
            }
        }
        if (app.m_menuIsCalendar)
        {
            std::wstring title = DateMath::MonthName(app.m_calendarMonth.month);
            title += L' ';
            title += CalcEngine::NumericString::FromInteger(app.m_calendarMonth.year);
            RECT header{ app.m_menuRect.left + Dp(14), app.m_menuRect.top + Dp(6), app.m_menuRect.right - Dp(76), app.m_menuRect.top + Dp(34) };
            DrawLabel(hdc, header, title, 14, g_theme.primaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER, FW_SEMIBOLD);

            static constexpr const wchar_t* weekdays[] = { L"S", L"M", L"T", L"W", L"T", L"F", L"S" };
            const int cell = Dp(34);
            for (int i = 0; i < 7; ++i)
            {
                RECT day{ app.m_menuRect.left + Dp(8) + i * cell,
                          app.m_menuRect.top + Dp(40),
                          app.m_menuRect.left + Dp(8) + (i + 1) * cell,
                          app.m_menuRect.top + Dp(62) };
                DrawLabel(hdc, day, weekdays[i], 11, g_theme.secondaryText, false, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            }

            for (const Btn& b : app.m_menuButtons)
            {
                if (b.icon)
                {
                    DrawLabel(hdc, b.rc, b.label, b.textDip, g_theme.primaryText, true, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                }
                else
                {
                    DrawLabel(
                        hdc, b.rc, b.label, 13, b.enabled ? (b.checked ? g_theme.accent : g_theme.primaryText) : g_theme.disabledText, false,
                        DT_SINGLELINE | DT_CENTER | DT_VCENTER, b.checked ? FW_SEMIBOLD : FW_NORMAL);
                }
            }
            return;
        }

        for (const Btn& b : app.m_menuButtons)
        {
            RECT r = b.rc;
            r.left += Dp(12);
            DrawLabel(hdc, r, b.label, 13, g_theme.primaryText, false, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        }
    }

    void PaintOverlays(HDC hdc, int width, int height)
    {
        CalcApp& app = g_app;
        SetBkMode(hdc, TRANSPARENT);
        PaintNavOverlay(hdc, app, width, height);

        const float menu = app.m_menuAnim.Value();
        if (app.m_menuButtons.empty() || menu <= 0.004f)
        {
            return;
        }
        if (menu >= 0.999f)
        {
            PaintFlyout(hdc);
            return;
        }

        // Fading the flyout means compositing it over a copy of the frame, since
        // GDI text has no alpha of its own.
        const int lift = static_cast<int>(Dp(10) * (1.0f - menu));
        HDC layer = g_scratch.Acquire(hdc, width, height);
        if (layer == nullptr)
        {
            PaintFlyout(hdc);
            return;
        }
        g_scratchHeld = true;
        BitBlt(layer, 0, 0, width, height, hdc, 0, 0, SRCCOPY);
        SetBkMode(layer, TRANSPARENT);

        // The lift belongs to the flyout, so it is applied to the flyout's own
        // drawing inside the layer. Blending the layer at an offset instead --
        // which is what this used to do -- slid the entire window down by up to
        // 10px for the length of the fade.
        POINT previousOrigin{};
        SetViewportOrgEx(layer, 0, lift, &previousOrigin);
        PaintFlyout(layer);
        SetViewportOrgEx(layer, previousOrigin.x, previousOrigin.y, nullptr);
        g_scratchHeld = false;

        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = static_cast<BYTE>(255.0f * menu);
        AlphaBlend(hdc, 0, 0, width, height, layer, 0, 0, width, height, blend);
    }

    // Composites the frame: content (faded and lifted while a mode is entering)
    // with the overlay surfaces on top.
    void PaintApp(HDC hdc, int width, int height)
    {
        CalcApp& app = g_app;
        const float enter = app.m_contentAnim.Value();

        if (enter >= 0.999f || width <= 0 || height <= 0)
        {
            PaintContent(hdc, width, height);
            PaintOverlays(hdc, width, height);
            return;
        }

        // Content rises into place as it fades in, matching the app's
        // page-transition motion.
        const int rise = static_cast<int>(Dp(16) * (1.0f - enter));

        RECT full{ 0, 0, width, height };
        HBRUSH background = CreateSolidBrush(g_theme.page);
        FillRect(hdc, &full, background);
        DeleteObject(background);

        HDC layer = g_scratch.Acquire(hdc, width, height);
        if (layer == nullptr)
        {
            // Out of GDI memory: draw straight to the target rather than lose
            // the frame. The transition is the only thing that suffers.
            PaintContent(hdc, width, height);
            PaintOverlays(hdc, width, height);
            return;
        }

        g_scratchHeld = true;
        PaintContent(layer, width, height);
        g_scratchHeld = false;

        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = static_cast<BYTE>(255.0f * enter);
        AlphaBlend(hdc, 0, rise, width, height - rise, layer, 0, 0, width, height - rise, blend);

        PaintOverlays(hdc, width, height);
    }
}

namespace
{
    // ------------------------------------------------------------ interaction

    // Lays the open menu out from the stored items, clamped to the window and
    // scrolled by m_menuScroll. Long lists (38 data units, 31 days, a century of
    // years) are taller than the window, so they scroll rather than overflow.
    void CalcApp::LayoutMenu()
    {
        if (m_menuIsCalendar)
        {
            LayoutCalendar();
            return;
        }
        m_menuButtons.clear();

        RECT client{};
        GetClientRect(m_hwnd, &client);

        const int itemHeight = Dp(34);
        const int count = static_cast<int>(m_menuItemStorage.size());
        const int width = (std::max)(Dp(170), static_cast<int>(m_menuAnchor.right - m_menuAnchor.left));
        const int maxHeight = (client.bottom - client.top) - Dp(16);
        const int wantedHeight = count * itemHeight + Dp(8);
        const int height = (std::min)(wantedHeight, maxHeight);

        m_menuContentHeight = (std::max)(0, wantedHeight - height);
        m_menuScroll = (std::max)(0, (std::min)(m_menuScroll, m_menuContentHeight));

        RECT menu;
        menu.left = m_menuAnchor.left;
        menu.top = m_menuAnchor.bottom + Dp(2);
        menu.right = menu.left + width;
        menu.bottom = menu.top + height;
        if (menu.right > client.right - Dp(4))
        {
            const int shift = menu.right - (client.right - Dp(4));
            menu.left -= shift;
            menu.right -= shift;
        }
        if (menu.left < Dp(4))
        {
            menu.right += Dp(4) - menu.left;
            menu.left = Dp(4);
        }
        if (menu.bottom > client.bottom - Dp(4))
        {
            const int shift = menu.bottom - (client.bottom - Dp(4));
            menu.top -= shift;
            menu.bottom -= shift;
        }
        if (menu.top < Dp(4))
        {
            menu.top = Dp(4);
            menu.bottom = (std::min)(client.bottom - Dp(4), menu.top + height);
        }
        m_menuRect = menu;

        for (int i = 0; i < count; ++i)
        {
            const int top = menu.top + Dp(4) + i * itemHeight - m_menuScroll;
            if (top + itemHeight < menu.top || top > menu.bottom)
            {
                continue; // outside the visible strip
            }
            Btn b;
            b.label = m_menuItemStorage[static_cast<size_t>(i)].label;
            b.action = m_menuItemStorage[static_cast<size_t>(i)].action;
            b.rc = { menu.left + Dp(4),
                     (std::max)(top, static_cast<int>(menu.top)),
                     menu.right - Dp(4),
                     (std::min)(top + itemHeight, static_cast<int>(menu.bottom)) };
            if (b.rc.bottom - b.rc.top < Dp(10))
            {
                continue;
            }
            m_menuButtons.push_back(b);
        }
    }

    void OpenMenu(const MenuItem* items, size_t count, const RECT& anchor)
    {
        CalcApp& app = g_app;
        if (items != app.m_menuItemStorage.data())
        {
            app.m_menuItemStorage.assign(items, items + count);
        }
        app.m_menuIsCalendar = false;
        app.m_menuAnchor = anchor;
        app.m_menuScroll = 0;
        app.m_hotMenu = -1;
        app.LayoutMenu();
        app.m_menuOpen = true;
        app.m_menuAnim.Set(0.0f);
        app.m_menuAnim.To(1.0f, kMotionFastMs);
        app.EnsureFrameTimer();
        app.Invalidate();
    }

    // Builds the month / day / year and offset pickers for Date Calculation.

    // A month grid on the flyout surface, so it inherits the card, the fade and
    // the hit testing the dropdowns already use.
    void CalcApp::LayoutCalendar()
    {
        m_menuButtons.clear();
        m_menuLabelStorage.clear();

        RECT client{};
        GetClientRect(m_hwnd, &client);

        const int cell = Dp(34);
        const int width = cell * 7 + Dp(16);
        const int header = Dp(40);
        const int weekdays = Dp(22);
        const int height = header + weekdays + cell * 6 + Dp(12);

        RECT menu;
        menu.left = m_menuAnchor.left;
        menu.top = m_menuAnchor.bottom + Dp(2);
        menu.right = menu.left + width;
        menu.bottom = menu.top + height;
        if (menu.right > client.right - Dp(4))
        {
            const int shift = menu.right - (client.right - Dp(4));
            menu.left -= shift;
            menu.right -= shift;
        }
        if (menu.left < Dp(4))
        {
            menu.right += Dp(4) - menu.left;
            menu.left = Dp(4);
        }
        if (menu.bottom > client.bottom - Dp(4))
        {
            const int shift = menu.bottom - (client.bottom - Dp(4));
            menu.top -= shift;
            menu.bottom -= shift;
        }
        if (menu.top < Dp(4))
        {
            menu.top = Dp(4);
        }
        m_menuRect = menu;
        m_menuContentHeight = 0;
        m_menuScroll = 0;

        Btn prev;
        prev.label = L"";
        prev.action = ACT_CAL_PREV;
        prev.style = Style::Bit;
        prev.icon = true;
        prev.textDip = 11;
        prev.rc = { menu.right - Dp(72), menu.top + Dp(6), menu.right - Dp(42), menu.top + Dp(34) };
        m_menuButtons.push_back(prev);

        Btn next;
        next.label = L"";
        next.action = ACT_CAL_NEXT;
        next.style = Style::Bit;
        next.icon = true;
        next.textDip = 11;
        next.rc = { menu.right - Dp(38), menu.top + Dp(6), menu.right - Dp(8), menu.top + Dp(34) };
        m_menuButtons.push_back(next);

        // The grid starts on the Sunday on or before the first of the month.
        CivilDate first = m_calendarMonth;
        first.day = 1;
        long long firstDay = DateMath::ToDayNumber(first);
        long long weekday = (firstDay + 4) % 7; // 1970-01-01 was a Thursday
        if (weekday < 0)
        {
            weekday += 7;
        }
        const long long gridStart = firstDay - weekday;
        const int daysInMonth = DateMath::DaysInMonth(m_calendarMonth.year, m_calendarMonth.month);

        m_menuLabelStorage.reserve(42);
        for (int index = 0; index < 42; ++index)
        {
            const CivilDate day = DateMath::FromDayNumber(gridStart + index);
            m_menuLabelStorage.push_back(CalcEngine::NumericString::FromInteger(day.day));

            const bool inMonth = (day.year == m_calendarMonth.year) && (day.month == m_calendarMonth.month);
            Btn cellButton;
            cellButton.label = m_menuLabelStorage.back();
            cellButton.action = inMonth ? (ACT_CAL_DAY_BASE + day.day) : ACT_NONE;
            cellButton.style = Style::Bit;
            cellButton.enabled = inMonth;
            cellButton.checked = inMonth && day.day == DateForIndex(m_calendarField).day;
            cellButton.textDip = 13;
            const int column = index % 7;
            const int row = index / 7;
            cellButton.rc = { menu.left + Dp(8) + column * cell + Dp(2),
                              menu.top + header + weekdays + row * cell + Dp(2),
                              menu.left + Dp(8) + (column + 1) * cell - Dp(2),
                              menu.top + header + weekdays + (row + 1) * cell - Dp(2) };
            m_menuButtons.push_back(cellButton);
        }
        (void)daysInMonth;
    }

    void CalcApp::OpenCalendar(int dateIndex)
    {
        m_calendarField = dateIndex;
        m_calendarMonth = DateForIndex(dateIndex);
        m_menuIsCalendar = true;

        RECT anchor{ 0, 0, 0, 0 };
        for (const auto& b : m_buttons)
        {
            if (b.action == ACT_DATE_FIELD_BASE + dateIndex)
            {
                anchor = b.rc;
                break;
            }
        }
        m_menuAnchor = anchor;
        LayoutCalendar();
        m_menuOpen = true;
        m_menuAnim.Set(0.0f);
        m_menuAnim.To(1.0f, kMotionFastMs);
        EnsureFrameTimer();
        Invalidate();
    }

    void CalcApp::OpenDateFieldMenu(int fieldIndex)
    {
        m_menuLabelStorage.clear();
        m_menuItemStorage.clear();

        const auto pushNumbers = [this](int first, int last, int actionBase) {
            for (int value = first; value <= last; ++value)
            {
                m_menuLabelStorage.push_back(CalcEngine::NumericString::FromInteger(value));
                m_menuItemStorage.push_back({ std::wstring_view(), actionBase + value });
            }
        };

        int selected = 0;
        if (fieldIndex >= 10)
        {
            // Offset steppers: years, months and days, as the app allows 0-999.
            const int which = fieldIndex - 10;
            const int limit = (which == 0) ? 100 : (which == 1) ? 11 : 365;
            pushNumbers(0, limit, ACT_DATE_VALUE_BASE + which * 1000);
            selected = (which == 0) ? m_dateModel.OffsetYears() : (which == 1) ? m_dateModel.OffsetMonths() : m_dateModel.OffsetDays();
        }
        else
        {
            const int dateIndex = fieldIndex / 3;
            const int segment = fieldIndex % 3;
            CivilDate& date = (dateIndex == 0) ? m_dateModel.From() : (dateIndex == 1) ? m_dateModel.To() : m_dateModel.Start();

            if (segment == 0)
            {
                for (int month = 1; month <= 12; ++month)
                {
                    m_menuLabelStorage.push_back(DateMath::MonthName(month));
                    m_menuItemStorage.push_back({ std::wstring_view(), ACT_DATE_VALUE_BASE + 3000 + dateIndex * 100 + month });
                }
                selected = date.month - 1;
            }
            else if (segment == 1)
            {
                const int days = DateMath::DaysInMonth(date.year, date.month);
                for (int day = 1; day <= days; ++day)
                {
                    m_menuLabelStorage.push_back(CalcEngine::NumericString::FromInteger(day));
                    m_menuItemStorage.push_back({ std::wstring_view(), ACT_DATE_VALUE_BASE + 4000 + dateIndex * 100 + day });
                }
                selected = date.day - 1;
            }
            else
            {
                const int firstYear = date.year - 50;
                for (int year = firstYear; year <= date.year + 50; ++year)
                {
                    m_menuLabelStorage.push_back(CalcEngine::NumericString::FromInteger(year));
                    m_menuItemStorage.push_back({ std::wstring_view(), ACT_DATE_VALUE_BASE + 5000 + dateIndex * 200 + (year - firstYear) });
                }
                m_dateYearBase[dateIndex] = firstYear;
                selected = 50;
            }
        }

        for (size_t i = 0; i < m_menuItemStorage.size(); ++i)
        {
            m_menuItemStorage[i].label = m_menuLabelStorage[i];
        }

        RECT anchor{ 0, 0, 0, 0 };
        for (const auto& b : m_buttons)
        {
            if (b.action == ACT_DATE_FIELD_BASE + fieldIndex)
            {
                anchor = b.rc;
                break;
            }
        }
        OpenMenu(m_menuItemStorage.data(), m_menuItemStorage.size(), anchor);

        // Scroll the current value into view.
        const int itemHeight = Dp(34);
        m_menuScroll = (std::max)(0, selected * itemHeight - Dp(100));
        LayoutMenu();
        Invalidate();
    }

    void CloseMenus()
    {
        CalcApp& app = g_app;
        if (app.m_menuOpen)
        {
            app.m_menuOpen = false;
            app.m_menuIsCalendar = false;
            app.m_menuButtons.clear();
            app.m_menuAnim.To(0.0f, kMotionFastMs, EaseStandard);
            app.EnsureFrameTimer();
            app.Invalidate();
        }
    }

    // Flattens a stored expression into the raw engine commands that rebuild it,
    // mirroring the shipping ViewModel's replay.
    std::vector<int> FlattenCommands(const std::vector<std::shared_ptr<IExpressionCommand>>& commands)
    {
        std::vector<int> result;
        for (const auto& command : commands)
        {
            if (!command)
            {
                continue;
            }
            switch (command->GetCommandType())
            {
            case CommandType::UnaryCommand:
            {
                auto unary = static_cast<IUnaryCommand*>(command.get());
                if (const auto& codes = unary->GetCommands())
                {
                    for (int code : *codes)
                    {
                        result.push_back(code);
                    }
                }
                break;
            }
            case CommandType::BinaryCommand:
                result.push_back(static_cast<IBinaryCommand*>(command.get())->GetCommand());
                break;
            case CommandType::Parentheses:
                result.push_back(static_cast<IParenthesisCommand*>(command.get())->GetCommand());
                break;
            case CommandType::OperandCommand:
            {
                auto operand = static_cast<IOpndCommand*>(command.get());
                if (const auto& codes = operand->GetCommands())
                {
                    bool needSign = operand->IsNegative();
                    for (int code : *codes)
                    {
                        result.push_back(code);
                        if (needSign && code != static_cast<int>(Command::Command0))
                        {
                            result.push_back(static_cast<int>(Command::CommandSIGN));
                            needSign = false;
                        }
                    }
                }
                break;
            }
            }
        }
        return result;
    }

    void LoadHistoryItem(size_t displayIndex)
    {
        CalcApp& app = g_app;
        const auto& items = app.m_manager->GetHistoryItems();
        if (displayIndex >= items.size())
        {
            return;
        }
        const auto& item = items[items.size() - 1 - displayIndex]->historyItemVector;
        if (!item.spCommands)
        {
            return;
        }
        const std::vector<int> commands = FlattenCommands(*item.spCommands);

        app.m_manager->SetInHistoryItemLoadMode(true);
        app.m_manager->Reset(false);
        if (app.m_mode == Mode::Scientific)
        {
            app.Send(Command::ModeScientific);
        }
        if (app.m_fe)
        {
            app.Send(Command::CommandFE);
        }
        app.Send(app.m_angle);
        for (int code : commands)
        {
            app.Send(static_cast<Command>(code));
        }
        // The shipping app round-trips F-E to force the display back to its
        // canonical format after a replay.
        app.Send(Command::CommandFE);
        app.Send(Command::CommandFE);
        app.m_manager->SetInHistoryItemLoadMode(false);

        app.ForceClosePanel();
        app.Relayout();
    }

    UnitConversionManager::Command ConverterCommandForCalcCommand(Command command);
    void ApplyTitleBarTheme(HWND hwnd);

    void Execute(int action)
    {
        CalcApp& app = g_app;

        if (action >= kCommandBase)
        {
            const Command command = static_cast<Command>(action - kCommandBase);
            if (app.m_mode == Mode::Graphing)
            {
                // The trig and function flyouts are shared with the scientific
                // keypad, so in graphing mode their commands are written into
                // the equation instead of being sent to the engine.
                if (const wchar_t* name = GraphingFunctionName(command); name != nullptr)
                {
                    for (const wchar_t* c = name; *c != L'\0'; ++c)
                    {
                        app.AppendToEquation(*c);
                    }
                }
                app.Relayout();
                return;
            }
            if (app.m_mode == Mode::Converter)
            {
                app.EnsureConverter();
                app.m_converterModel->Send(ConverterCommandForCalcCommand(command));
                app.Relayout();
                return;
            }
            app.Send(command);
            app.Relayout();
            return;
        }

        if (action >= ACT_GRAPH_FIELD_BASE)
        {
            const int index = action - ACT_GRAPH_FIELD_BASE;
            if (index >= 0 && index < 4)
            {
                if (app.m_graphField != index)
                {
                    app.CommitGraphField();
                }
                app.m_graphField = index;
            }
            app.Relayout();
            return;
        }
        if (action >= ACT_GRAPH_TEXT_BASE)
        {
            // The keypad's characters, indexed the way BuildGraphingLayout
            // numbers them.
            // Single characters keep their slot; the keys that insert a whole
            // function name are listed after them.
            static constexpr wchar_t kCharacters[] = {
                L'x',  L'(', L')', L'^',
                L'7',  L'8', L'9', 0x00F7, 0x03C0,
                L'4',  L'5', L'6', 0x00D7, L'e',
                L'1',  L'2', L'3', 0x2212, L'|',
                L'0',  L'.', L'+', 0x03C0,
            };
            static constexpr const wchar_t* kSequences[] = {
                L"^2",      // 23: x squared
                L"^(-1)",   // 24: one over x
                L"abs(",    // 25
                L"y",       // 26
                L"sqrt(",   // 27
                L"=",       // 28
                L"10^",     // 29
                L"log(",    // 30
                L"ln(",     // 31
                L"<",       // 32, from the inequalities flyout
                L">",       // 33
                L"\u2264",  // 34
                L"\u2265",  // 35
            };
            constexpr int kCharacterCount = static_cast<int>(sizeof(kCharacters) / sizeof(kCharacters[0]));
            const int index = action - ACT_GRAPH_TEXT_BASE;
            if (index >= 0 && index < kCharacterCount)
            {
                app.AppendToEquation(kCharacters[index]);
            }
            else if (index >= kCharacterCount
                     && index < kCharacterCount + static_cast<int>(sizeof(kSequences) / sizeof(kSequences[0])))
            {
                for (const wchar_t* c = kSequences[index - kCharacterCount]; *c != L'\0'; ++c)
                {
                    app.AppendToEquation(*c);
                }
            }
            app.Relayout();
            return;
        }
        if (action >= ACT_GRAPH_REMOVE_BASE)
        {
            const int index = action - ACT_GRAPH_REMOVE_BASE;
            if (index >= 0 && index < static_cast<int>(app.m_equations.size()))
            {
                if (app.m_equations.size() == 1)
                {
                    // The list always keeps one row; emptying it is the same as
                    // removing the only equation.
                    app.m_equations[0].text.clear();
                    app.m_equations[0].expression.Clear();
                    app.m_equations[0].valid = false;
                }
                else
                {
                    app.m_equations.erase(app.m_equations.begin() + index);
                    if (app.m_activeEquation >= static_cast<int>(app.m_equations.size()))
                    {
                        app.m_activeEquation = static_cast<int>(app.m_equations.size()) - 1;
                    }
                }
            }
            app.Relayout();
            return;
        }
        if (action >= ACT_GRAPH_SELECT_BASE)
        {
            const int index = action - ACT_GRAPH_SELECT_BASE;
            if (index >= 0 && index < static_cast<int>(app.m_equations.size()))
            {
                app.m_activeEquation = index;
            }
            app.Relayout();
            return;
        }
        if (action >= ACT_CAL_DAY_BASE)
        {
            CivilDate& date = app.DateForIndex(app.m_calendarField);
            date.year = app.m_calendarMonth.year;
            date.month = app.m_calendarMonth.month;
            date.day = action - ACT_CAL_DAY_BASE;
            CloseMenus();
            app.Relayout();
            return;
        }
        if (action == ACT_CAL_PREV || action == ACT_CAL_NEXT)
        {
            app.m_calendarMonth = DateMath::AddMonths(app.m_calendarMonth, (action == ACT_CAL_NEXT) ? 1 : -1);
            app.LayoutCalendar();
            app.Invalidate();
            return;
        }
        if (action >= ACT_DATE_VALUE_BASE)
        {
            int value = action - ACT_DATE_VALUE_BASE;
            const auto dateAt = [&app](int index) -> CivilDate& {
                return (index == 0) ? app.m_dateModel.From() : (index == 1) ? app.m_dateModel.To() : app.m_dateModel.Start();
            };

            if (value >= 5000)
            {
                value -= 5000;
                const int dateIndex = value / 200;
                CivilDate& date = dateAt(dateIndex);
                date.year = app.m_dateYearBase[dateIndex] + (value % 200);
                date.day = (std::min)(date.day, DateMath::DaysInMonth(date.year, date.month));
            }
            else if (value >= 4000)
            {
                value -= 4000;
                dateAt(value / 100).day = value % 100;
            }
            else if (value >= 3000)
            {
                value -= 3000;
                const int dateIndex = value / 100;
                CivilDate& date = dateAt(dateIndex);
                date.month = value % 100;
                date.day = (std::min)(date.day, DateMath::DaysInMonth(date.year, date.month));
            }
            else
            {
                const int which = value / 1000;
                const int amount = value % 1000;
                if (which == 0)
                {
                    app.m_dateModel.OffsetYears() = amount;
                }
                else if (which == 1)
                {
                    app.m_dateModel.OffsetMonths() = amount;
                }
                else
                {
                    app.m_dateModel.OffsetDays() = amount;
                }
            }
            app.Relayout();
            return;
        }
        if (action >= ACT_DATE_FIELD_BASE)
        {
            const int field = action - ACT_DATE_FIELD_BASE;
            if (field < 3)
            {
                app.OpenCalendar(field);
            }
            else
            {
                app.OpenDateFieldMenu(field);
            }
            return;
        }
        if (action >= ACT_CONV_UNIT_TO_BASE)
        {
            app.EnsureConverter();
            const auto& units = app.m_converterModel->Units();
            const size_t index = static_cast<size_t>(action - ACT_CONV_UNIT_TO_BASE);
            if (index < units.size())
            {
                app.m_converterModel->SetUnits(app.m_converterModel->FromUnitId(), units[index].id);
            }
            app.Relayout();
            return;
        }
        if (action >= ACT_CONV_UNIT_FROM_BASE)
        {
            app.EnsureConverter();
            const auto& units = app.m_converterModel->Units();
            const size_t index = static_cast<size_t>(action - ACT_CONV_UNIT_FROM_BASE);
            if (index < units.size())
            {
                app.m_converterModel->SetUnits(units[index].id, app.m_converterModel->ToUnitId());
            }
            app.Relayout();
            return;
        }
        if (action >= ACT_CONV_CATEGORY_BASE)
        {
            app.SetConverterCategory(action - ACT_CONV_CATEGORY_BASE);
            return;
        }
        if (action >= ACT_HIST_ITEM_BASE)
        {
            LoadHistoryItem(static_cast<size_t>(action - ACT_HIST_ITEM_BASE));
            return;
        }
        if (action >= ACT_MEM_SUB_BASE)
        {
            app.m_manager->MemorizedNumberSubtract(static_cast<unsigned int>(action - ACT_MEM_SUB_BASE));
            app.Relayout();
            return;
        }
        if (action >= ACT_MEM_ADD_BASE)
        {
            app.m_manager->MemorizedNumberAdd(static_cast<unsigned int>(action - ACT_MEM_ADD_BASE));
            app.Relayout();
            return;
        }
        if (action >= ACT_MEM_CLEAR_BASE)
        {
            app.m_manager->MemorizedNumberClear(static_cast<unsigned int>(action - ACT_MEM_CLEAR_BASE));
            app.m_manager->SetMemorizedNumbersString();
            app.Relayout();
            return;
        }
        if (action >= ACT_MEM_LOAD_BASE)
        {
            app.m_manager->MemorizedNumberLoad(static_cast<unsigned int>(action - ACT_MEM_LOAD_BASE));
            app.Relayout();
            return;
        }
        if (action >= ACT_BIT_BASE)
        {
            const int bit = action - ACT_BIT_BASE;
            if (bit < app.WordSizeBits())
            {
                app.Send(static_cast<Command>(static_cast<int>(Command::CommandBINPOS0) + bit));
                app.Relayout();
            }
            return;
        }

        switch (action)
        {
        case ACT_NAV_MENU:
            app.m_navOpen = !app.m_navOpen;
            app.m_settingsOpen = false;
            app.m_navScroll = 0;
            app.m_navAnim.To(app.m_navOpen ? 1.0f : 0.0f, kMotionNormalMs);
            app.m_settingsAnim.To(0.0f, kMotionNormalMs);
            app.EnsureFrameTimer();
            app.Relayout();
            break;
        case ACT_SETTINGS:
            app.m_settingsOpen = !app.m_settingsOpen;
            app.m_navOpen = false;
            app.m_settingsAnim.To(app.m_settingsOpen ? 1.0f : 0.0f, kMotionNormalMs);
            app.m_navAnim.To(0.0f, kMotionNormalMs);
            app.EnsureFrameTimer();
            app.Relayout();
            break;
        case ACT_APP_THEME:
            // The shipping settings page expands to show the choices rather than
            // cycling the theme on every click.
            app.m_themeExpanded = !app.m_themeExpanded;
            app.Relayout();
            break;
        case ACT_ABOUT_EXPAND:
            app.m_aboutExpanded = !app.m_aboutExpanded;
            app.Relayout();
            break;
        case ACT_THEME_LIGHT:
        case ACT_THEME_DARK:
        case ACT_THEME_SYSTEM:
        {
            const bool wasDark = g_theme.dark;
            app.m_themeChoice = (action == ACT_THEME_LIGHT) ? 1 : (action == ACT_THEME_DARK) ? 2 : 0;
            g_themeOverride = app.m_themeChoice;
            BeginThemeFade(app);
            LoadTheme();
            if (wasDark == g_theme.dark)
            {
                // Same palette after all -- only the radio moved, so there is
                // nothing to cross-fade.
                app.m_themeAnim.Set(0.0f);
            }
            ApplyTitleBarTheme(app.m_hwnd);
            app.Relayout();
            break;
        }
        case ACT_ALWAYS_ON_TOP:
            app.m_alwaysOnTop = !app.m_alwaysOnTop;
            SetWindowPos(app.m_hwnd, app.m_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            app.Relayout();
            break;
        case ACT_MODE_DATE:
            app.SetMode(Mode::Date);
            break;
        case ACT_MODE_GRAPHING:
            app.SetMode(Mode::Graphing);
            break;
        case ACT_GRAPH_ZOOM_IN:
            app.ScaleGraph(1.0 / 1.4);
            app.Relayout();
            break;
        case ACT_GRAPH_ZOOM_OUT:
            app.ScaleGraph(1.4);
            app.Relayout();
            break;
        case ACT_GRAPH_RESET:
            app.ResetGraph();
            app.Relayout();
            break;
        case ACT_GRAPH_TAB_PLOT:
        case ACT_GRAPH_TAB_EQUATIONS:
            app.m_graphEquationsView = (action == ACT_GRAPH_TAB_EQUATIONS);
            app.StartContentEnter();
            app.Relayout();
            break;
        case ACT_GRAPH_OPTIONS:
            app.CommitGraphField();
            app.m_graphOptionsOpen = !app.m_graphOptionsOpen;
            app.Relayout();
            break;
        case ACT_GRAPH_ANGLE_RAD:
        case ACT_GRAPH_ANGLE_DEG:
        case ACT_GRAPH_ANGLE_GRAD:
            app.m_graphAngle = (action == ACT_GRAPH_ANGLE_DEG)    ? Graphing::AngleMode::Degrees
                               : (action == ACT_GRAPH_ANGLE_GRAD) ? Graphing::AngleMode::Gradians
                                                                  : Graphing::AngleMode::Radians;
            app.Relayout();
            break;
        case ACT_GRAPH_THICKNESS:
            app.m_graphThickness = (app.m_graphThickness + 1) % 3;
            app.Relayout();
            break;
        case ACT_GRAPH_THEME_LIGHT:
        case ACT_GRAPH_THEME_APP:
            app.m_graphAlwaysLight = (action == ACT_GRAPH_THEME_LIGHT);
            app.Relayout();
            break;
        case ACT_GRAPH_SHARE:
            app.ShowToast(CopyGraphToClipboard(app) ? L"Graph copied to clipboard" : L"Could not copy the graph");
            break;
        case ACT_GRAPH_TRACE:
            // Laid out for fidelity; tracing needs the analysis the closed
            // solver would provide.
            break;
        case ACT_GRAPH_ADD:
        {
            CalcApp::GraphEquation added;
            added.colour = static_cast<int>(app.m_equations.size());
            app.m_equations.push_back(std::move(added));
            app.m_activeEquation = static_cast<int>(app.m_equations.size()) - 1;
            app.Relayout();
            break;
        }
        case ACT_GRAPH_BACKSPACE:
            app.BackspaceEquation();
            app.Relayout();
            break;
        case ACT_GRAPH_CLEAR:
            app.EnsureGraphing();
            app.m_equations[app.m_activeEquation].text.clear();
            app.RecompileEquation(app.m_activeEquation);
            app.Relayout();
            break;
        case ACT_GRAPH_NEGATE:
            app.AppendToEquation(L'-');
            app.Relayout();
            break;
        case ACT_CONV_FIELD_FROM:
            app.EnsureConverter();
            app.m_converterModel->SetActiveField(false);
            app.Relayout();
            break;
        case ACT_CONV_FIELD_TO:
            app.EnsureConverter();
            app.m_converterModel->SetActiveField(true);
            app.Relayout();
            break;
        case ACT_CONV_FROM_UNIT:
        case ACT_CONV_TO_UNIT:
        {
            app.EnsureConverter();
            const bool fromSide = (action == ACT_CONV_FROM_UNIT);
            const auto& units = app.m_converterModel->Units();
            app.m_menuItemStorage.clear();
            app.m_menuItemStorage.reserve(units.size());
            for (size_t i = 0; i < units.size(); ++i)
            {
                app.m_menuItemStorage.push_back(
                    { units[i].name, static_cast<int>((fromSide ? ACT_CONV_UNIT_FROM_BASE : ACT_CONV_UNIT_TO_BASE) + i) });
            }
            RECT anchor{ 0, 0, 0, 0 };
            for (const auto& b : app.m_buttons)
            {
                if (b.action == action)
                {
                    anchor = b.rc;
                    break;
                }
            }
            OpenMenu(app.m_menuItemStorage.data(), app.m_menuItemStorage.size(), anchor);
            const int currentId = fromSide ? app.m_converterModel->FromUnitId() : app.m_converterModel->ToUnitId();
            for (size_t i = 0; i < app.m_menuButtons.size() && i < units.size(); ++i)
            {
                app.m_menuButtons[i].checked = (units[i].id == currentId);
            }
            break;
        }
        case ACT_DATE_MODE_DIFF:
            app.m_dateModel.SetMode(DateCalcModel::Mode::Difference);
            app.Relayout();
            break;
        case ACT_DATE_MODE_OFFSET:
            app.m_dateModel.SetMode(DateCalcModel::Mode::AddSubtract);
            app.Relayout();
            break;
        case ACT_DATE_ADD_SUBTRACT:
            app.m_dateModel.SetAdding(!app.m_dateModel.IsAdding());
            app.Relayout();
            break;
        case ACT_MODE_STANDARD:
            app.SetMode(Mode::Standard);
            break;
        case ACT_MODE_SCIENTIFIC:
            app.SetMode(Mode::Scientific);
            break;
        case ACT_MODE_PROGRAMMER:
            app.SetMode(Mode::Programmer);
            break;
        case ACT_TOGGLE_PANEL:
            if (app.m_panel == Panel::None || app.m_panelClosing)
            {
                app.OpenPanel(app.m_mode == Mode::Programmer ? Panel::Memory : Panel::History);
            }
            else
            {
                app.ClosePanel();
            }
            app.Relayout();
            break;
        case ACT_PANEL_HISTORY:
            app.OpenPanel(Panel::History);
            app.Relayout();
            break;
        case ACT_PANEL_MEMORY:
        case ACT_MEMORY_PANEL:
            app.OpenPanel(Panel::Memory);
            app.Relayout();
            break;
        case ACT_HISTORY_CLEAR:
            app.m_manager->ClearHistory();
            app.Relayout();
            break;
        case ACT_MEMORY_CLEAR_ALL:
            app.m_manager->MemorizedNumberClearAll();
            app.Relayout();
            break;
        case ACT_MEM_STORE:
            app.m_manager->MemorizeNumber();
            app.Relayout();
            break;
        case ACT_MEM_RECALL:
            if (!app.m_memory.empty())
            {
                app.m_manager->MemorizedNumberLoad(0);
                app.Relayout();
            }
            break;
        case ACT_MEM_ADD:
            if (!app.m_memory.empty())
            {
                app.m_manager->MemorizedNumberAdd(0);
                app.Relayout();
            }
            break;
        case ACT_MEM_SUB:
            if (!app.m_memory.empty())
            {
                app.m_manager->MemorizedNumberSubtract(0);
                app.Relayout();
            }
            break;
        case ACT_SECOND:
            app.m_second = !app.m_second;
            app.Relayout();
            break;
        case ACT_HYP:
            app.m_hyp = !app.m_hyp;
            app.Relayout();
            break;
        case ACT_FE:
            app.m_fe = !app.m_fe;
            app.Send(Command::CommandFE);
            app.Relayout();
            break;
        case ACT_ANGLE_CYCLE:
            app.m_angle = (app.m_angle == Command::CommandDEG) ? Command::CommandRAD
                : (app.m_angle == Command::CommandRAD)         ? Command::CommandGRAD
                                                               : Command::CommandDEG;
            app.Send(app.m_angle);
            app.Relayout();
            break;
        case ACT_INEQ_MENU:
        {
            static constexpr MenuItem kInequalityMenu[] = {
                { L"<", ACT_GRAPH_TEXT_BASE + 32 },  { L">", ACT_GRAPH_TEXT_BASE + 33 },
                { L"\u2264", ACT_GRAPH_TEXT_BASE + 34 }, { L"\u2265", ACT_GRAPH_TEXT_BASE + 35 },
            };
            app.m_menuItemStorage.clear();
            app.m_menuItemStorage.reserve(4);
            for (const MenuItem& item : kInequalityMenu)
            {
                app.m_menuItemStorage.push_back(item);
            }
            RECT anchor{ 0, 0, 0, 0 };
            for (const auto& b : app.m_buttons)
            {
                if (b.action == ACT_INEQ_MENU)
                {
                    anchor = b.rc;
                    break;
                }
            }
            OpenMenu(app.m_menuItemStorage.data(), app.m_menuItemStorage.size(), anchor);
            break;
        }
        case ACT_TRIG_MENU:
        {
            // One flyout lists the circular functions and then the hyperbolic
            // ones, with "2nd" swapping both sets for their inverses.
            const MenuItem* circular = app.m_second ? kTrigMenuInverse : kTrigMenu;
            const MenuItem* hyperbolic = app.m_second ? kHypMenuInverse : kHypMenu;
            app.m_menuItemStorage.clear();
            app.m_menuItemStorage.reserve(12);
            for (int i = 0; i < 6; ++i)
            {
                app.m_menuItemStorage.push_back(circular[i]);
            }
            for (int i = 0; i < 6; ++i)
            {
                app.m_menuItemStorage.push_back(hyperbolic[i]);
            }

            RECT anchor{ 0, 0, 0, 0 };
            for (const auto& b : app.m_buttons)
            {
                if (b.action == ACT_TRIG_MENU)
                {
                    anchor = b.rc;
                    break;
                }
            }
            OpenMenu(app.m_menuItemStorage.data(), app.m_menuItemStorage.size(), anchor);
            break;
        }
        case ACT_FUNC_MENU:
        {
            RECT anchor{ 0, 0, 0, 0 };
            for (const auto& b : app.m_buttons)
            {
                if (b.action == ACT_FUNC_MENU)
                {
                    anchor = b.rc;
                    break;
                }
            }
            OpenMenu(kFuncMenu, ARRAYSIZE(kFuncMenu), anchor);
            break;
        }
        case ACT_BITWISE_MENU:
        case ACT_SHIFT_MENU:
        {
            RECT anchor{ 0, 0, 0, 0 };
            for (const auto& b : app.m_buttons)
            {
                if (b.action == action)
                {
                    anchor = b.rc;
                    break;
                }
            }
            if (action == ACT_BITWISE_MENU)
            {
                OpenMenu(kBitwiseMenu, ARRAYSIZE(kBitwiseMenu), anchor);
            }
            else
            {
                OpenMenu(kShiftMenu, ARRAYSIZE(kShiftMenu), anchor);
            }
            break;
        }
        case ACT_WORDSIZE_CYCLE:
            app.m_wordSize = (app.m_wordSize == Command::CommandQword) ? Command::CommandDword
                : (app.m_wordSize == Command::CommandDword)            ? Command::CommandWord
                : (app.m_wordSize == Command::CommandWord)             ? Command::CommandByte
                                                                       : Command::CommandQword;
            app.Send(app.m_wordSize);
            app.Relayout();
            break;
        case ACT_BITBOARD:
            app.m_bitBoard = !app.m_bitBoard;
            app.Relayout();
            break;
        case ACT_RADIX_HEX:
        case ACT_RADIX_DEC:
        case ACT_RADIX_OCT:
        case ACT_RADIX_BIN:
            app.m_radix = static_cast<RadixType>(action - ACT_RADIX_HEX);
            app.ApplyRadix();
            app.Relayout();
            break;
        default:
            break;
        }
    }

    UnitConversionManager::Command ConverterCommandForCalcCommand(Command command)
    {
        using UnitConversionManager::Command;
        const int code = static_cast<int>(command);
        if (code >= static_cast<int>(CalculationManager::Command::Command0) && code <= static_cast<int>(CalculationManager::Command::Command9))
        {
            return static_cast<Command>(static_cast<int>(Command::Zero) + (code - static_cast<int>(CalculationManager::Command::Command0)));
        }
        switch (command)
        {
        case CalculationManager::Command::CommandPNT:
            return Command::Decimal;
        case CalculationManager::Command::CommandSIGN:
            return Command::Negate;
        case CalculationManager::Command::CommandBACK:
            return Command::Backspace;
        case CalculationManager::Command::CommandCENTR:
        case CalculationManager::Command::CommandCLEAR:
            return Command::Clear;
        default:
            return Command::None;
        }
    }

    int HitTest(const std::vector<Btn>& buttons, POINT pt)
    {
        for (size_t i = 0; i < buttons.size(); ++i)
        {
            if (buttons[i].enabled && PtInRect(&buttons[i].rc, pt))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
}

namespace
{
    // ------------------------------------------------------------- keyboard

    // Shortcuts follow the published Windows Calculator key map.
    int ActionForChar(wchar_t ch)
    {
        CalcApp& app = g_app;
        const bool scientific = app.m_mode == Mode::Scientific;
        const bool programmer = app.m_mode == Mode::Programmer;

        if (ch >= L'0' && ch <= L'9')
        {
            return Cmd(static_cast<Command>(static_cast<int>(Command::Command0) + (ch - L'0')));
        }
        if (programmer && app.m_radix == RadixType::Hex)
        {
            const wchar_t lower = static_cast<wchar_t>(towlower(ch));
            if (lower >= L'a' && lower <= L'f')
            {
                return Cmd(static_cast<Command>(static_cast<int>(Command::CommandA) + (lower - L'a')));
            }
        }
        if (ch == app.m_provider->Decimal() || ch == L'.')
        {
            return programmer ? ACT_NONE : Cmd(Command::CommandPNT);
        }

        switch (ch)
        {
        case L'+': return Cmd(Command::CommandADD);
        case L'-': return Cmd(Command::CommandSUB);
        case L'*': return Cmd(Command::CommandMUL);
        case L'/': return Cmd(Command::CommandDIV);
        case L'=': return Cmd(Command::CommandEQU);
        case L'\r': return Cmd(Command::CommandEQU);
        case L'%': return Cmd(Command::CommandPERCENT);
        case L'(': return Cmd(Command::CommandOPENP);
        case L')': return Cmd(Command::CommandCLOSEP);
        case L'@': return Cmd(Command::CommandSQRT);
        case L'r': case L'R': return Cmd(Command::CommandREC);
        case L'!': return scientific ? Cmd(Command::CommandFAC) : ACT_NONE;
        default: break;
        }

        if (programmer)
        {
            switch (ch)
            {
            case L'&': return Cmd(Command::CommandAnd);
            case L'|': return Cmd(Command::CommandOR);
            case L'^': return Cmd(Command::CommandXor);
            case L'~': return Cmd(Command::CommandNot);
            case L'<': return Cmd(Command::CommandLSHF);
            case L'>': return Cmd(Command::CommandRSHF);
            default: break;
            }
        }
        else if (scientific)
        {
            switch (ch)
            {
            case L'n': case L'N': return Cmd(Command::CommandLN);
            case L'l': case L'L': return Cmd(Command::CommandLOG);
            case L's': case L'S': return Cmd(Command::CommandSIN);
            case L'o': case L'O': return Cmd(Command::CommandCOS);
            case L't': case L'T': return Cmd(Command::CommandTAN);
            case L'e': case L'E': return Cmd(Command::CommandEXP);
            case L'p': case L'P': return Cmd(Command::CommandPI);
            case L'y': case L'Y': case L'^': return Cmd(Command::CommandPWR);
            case L'i': case L'I': return ACT_SECOND;
            default: break;
            }
        }
        return ACT_NONE;
    }

    int ActionForKey(WPARAM vk, bool ctrl, bool alt)
    {
        CalcApp& app = g_app;
        if (alt)
        {
            switch (vk)
            {
            // Numbered as NavCategory.cs numbers them.
            case '1': return ACT_MODE_STANDARD;
            case '2': return ACT_MODE_SCIENTIFIC;
            case '3': return ACT_MODE_GRAPHING;
            case '4': return ACT_MODE_PROGRAMMER;
            case '5': return ACT_MODE_DATE;
            default: return ACT_NONE;
            }
        }
        if (ctrl)
        {
            switch (vk)
            {
            case 'M': return ACT_MEM_STORE;
            case 'R': return ACT_MEM_RECALL;
            case 'P': return ACT_MEM_ADD;
            case 'Q': return ACT_MEM_SUB;
            case 'L': return ACT_MEMORY_CLEAR_ALL;
            case 'H': return ACT_TOGGLE_PANEL;
            default: return ACT_NONE;
            }
        }
        switch (vk)
        {
        case VK_RETURN: return Cmd(Command::CommandEQU);
        case VK_BACK: return Cmd(Command::CommandBACK);
        case VK_DELETE: return Cmd(Command::CommandCENTR);
        case VK_ESCAPE: return Cmd(Command::CommandCLEAR);
        case VK_F9: return Cmd(Command::CommandSIGN);
        default: break;
        }
        if (app.m_mode == Mode::Scientific)
        {
            switch (vk)
            {
            case VK_F3: return Cmd(Command::CommandDEG);
            case VK_F4: return Cmd(Command::CommandRAD);
            case VK_F5: return Cmd(Command::CommandGRAD);
            default: break;
            }
        }
        else if (app.m_mode == Mode::Programmer)
        {
            switch (vk)
            {
            case VK_F5: return ACT_RADIX_HEX;
            case VK_F6: return ACT_RADIX_DEC;
            case VK_F7: return ACT_RADIX_OCT;
            case VK_F8: return ACT_RADIX_BIN;
            default: break;
            }
        }
        return ACT_NONE;
    }

    // Briefly light up the on-screen key a shortcut maps to.
    void FlashAction(HWND hwnd, int action)
    {
        CalcApp& app = g_app;
        for (size_t i = 0; i < app.m_buttons.size(); ++i)
        {
            if (app.m_buttons[i].action == action && app.m_buttons[i].enabled)
            {
                app.m_pressed = static_cast<int>(i);
                SetTimer(hwnd, 1, 90, nullptr);
                break;
            }
        }
    }

    void RunAction(HWND hwnd, int action)
    {
        if (action == ACT_NONE)
        {
            return;
        }
        FlashAction(hwnd, action);
        const int pressed = g_app.m_pressed;
        Execute(action);
        // Relayout() rebuilds the button list, so restore the flash index.
        if (pressed >= 0 && pressed < static_cast<int>(g_app.m_buttons.size()))
        {
            g_app.m_pressed = pressed;
        }
        g_app.Invalidate();
    }

    void ApplyTitleBarTheme(HWND hwnd)
    {
        const BOOL dark = g_theme.dark ? TRUE : FALSE;
        // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE on current Windows; 19 was the
        // pre-20H1 spelling. Both are harmless no-ops where unsupported.
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        CalcApp& app = g_app;

        switch (message)
        {
        case WM_CREATE:
            g_dpi = static_cast<int>(GetDpiForWindow(hwnd));
            LoadTheme();
            ApplyTitleBarTheme(hwnd);
            app.Attach(hwnd);
            app.Relayout();
            return 0;

        case WM_CAPTURECHANGED:
            // Losing capture another way would otherwise leave the pan latched
            // on, and with it every mouse move swallowed.
            app.m_graphDragging = false;
            return 0;

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED)
            {
                // Nothing is on screen to hold a buffer for.
                g_backBuffer.Release();
                g_scratch.Release();
                TrimWorkingSet();
            }
            app.Relayout();
            return 0;

        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const int minHeight = (app.m_mode == Mode::Programmer) ? 600 : 500;
            RECT frame{ 0, 0, Dp(320), Dp(minHeight) };
            AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, static_cast<UINT>(g_dpi));
            info->ptMinTrackSize.x = frame.right - frame.left;
            info->ptMinTrackSize.y = frame.bottom - frame.top;
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int width = client.right;
            const int height = client.bottom;

            // PaintApp fills the whole client area before it draws anything,
            // so the retained buffer never shows stale pixels.
            HDC memDC = g_backBuffer.Acquire(hdc, width, height);
            if (memDC != nullptr)
            {
                PaintApp(memDC, width, height);

                const float outgoing = app.m_themeAnim.Value();
                if (outgoing > 0.004f && g_themeLayer.Matches(width, height))
                {
                    BLENDFUNCTION blend{};
                    blend.BlendOp = AC_SRC_OVER;
                    blend.SourceConstantAlpha = static_cast<BYTE>(255.0f * outgoing);
                    AlphaBlend(memDC, 0, 0, width, height, g_themeLayer.Dc(), 0, 0, width, height, blend);
                }
                else if (g_themeLayer.Held())
                {
                    g_themeLayer.Release();
                }

                BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
            }
            else
            {
                PaintApp(hdc, width, height);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&track);

            if (app.m_graphDragging)
            {
                // Pan by however far the pointer has moved since the press,
                // converted back into graph units.
                const int width = app.m_graphRect.right - app.m_graphRect.left;
                const int height = app.m_graphRect.bottom - app.m_graphRect.top;
                if (width > 0 && height > 0)
                {
                    const double xSpan = app.m_graphXMax - app.m_graphXMin;
                    const double ySpan = app.m_graphYMax - app.m_graphYMin;
                    const double dx = static_cast<double>(pt.x - app.m_graphDragFrom.x) / width * xSpan;
                    const double dy = static_cast<double>(pt.y - app.m_graphDragFrom.y) / height * ySpan;
                    app.m_graphXMin = app.m_graphDragXMin - dx;
                    app.m_graphXMax = app.m_graphXMin + xSpan;
                    app.m_graphYMin = app.m_graphDragYMin + dy;
                    app.m_graphYMax = app.m_graphYMin + ySpan;
                    app.Relayout();
                }
                return 0;
            }

            int hotMenu = -1;
            int hot = -1;
            int hotNav = -1;
            if (app.m_menuOpen)
            {
                hotMenu = HitTest(app.m_menuButtons, pt);
            }
            else if (app.NavVisible() || app.SettingsVisible())
            {
                hotNav = HitTest(app.m_navButtons, pt);
            }
            else
            {
                hot = HitTest(app.m_buttons, pt);
            }

            if (hot != app.m_hot || hotMenu != app.m_hotMenu || hotNav != app.m_hotNav)
            {
                app.m_hot = hot;
                app.m_hotMenu = hotMenu;
                app.m_hotNav = hotNav;
                const int action = (hot >= 0) ? app.m_buttons[static_cast<size_t>(hot)].action
                    : (hotNav >= 0)           ? app.m_navButtons[static_cast<size_t>(hotNav)].action
                                              : ACT_NONE;
                app.SetHover(action);
                app.Invalidate();
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            app.m_hot = -1;
            app.m_hotMenu = -1;
            app.m_hotNav = -1;
            app.SetHover(ACT_NONE);
            app.Invalidate();
            return 0;

        case WM_LBUTTONDOWN:
        {
            SetFocus(hwnd);
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            if (app.m_menuOpen)
            {
                const int index = HitTest(app.m_menuButtons, pt);
                if (index < 0)
                {
                    CloseMenus();
                }
                else
                {
                    const int action = app.m_menuButtons[static_cast<size_t>(index)].action;
                    // The month arrows keep the picker open; everything else
                    // dismisses it.
                    if (action != ACT_CAL_PREV && action != ACT_CAL_NEXT)
                    {
                        CloseMenus();
                    }
                    Execute(action);
                    app.Invalidate();
                }
                return 0;
            }

            app.m_pressed = -1;
            app.m_pressedNav = -1;
            if (app.NavVisible() || app.SettingsVisible())
            {
                app.m_pressedNav = HitTest(app.m_navButtons, pt);
            }
            else
            {
                app.m_pressed = HitTest(app.m_buttons, pt);
            }

            // A press inside the plot that did not land on a zoom control starts
            // a pan -- but only when the plot is actually what was clicked. The
            // navigation pane, the settings page and the flyouts all cover the
            // plot rect, and while one of those is up m_pressed stays -1, so
            // without this a click on a navigation item began a pan and the
            // button-up that should have switched modes was swallowed by it.
            const bool plotIsFrontmost = !app.NavVisible() && !app.SettingsVisible() && !app.m_menuOpen;
            if (app.m_mode == Mode::Graphing && plotIsFrontmost && app.m_pressed < 0 && app.m_pressedNav < 0
                && PtInRect(&app.m_graphRect, pt))
            {
                app.m_graphDragging = true;
                app.m_graphDragFrom = pt;
                app.m_graphDragXMin = app.m_graphXMin;
                app.m_graphDragYMin = app.m_graphYMin;
                SetCapture(hwnd);
            }

            if (app.m_pressed >= 0 || app.m_pressedNav >= 0)
            {
                app.m_pressAnim.Set(0.0f);
                app.m_pressAnim.To(1.0f, kMotionFastMs, EaseStandard);
                app.EnsureFrameTimer();
            }
            app.Invalidate();
            return 0;
        }

        case WM_LBUTTONUP:
        {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (app.m_graphDragging)
            {
                app.m_graphDragging = false;
                ReleaseCapture();
                app.Invalidate();
                return 0;
            }
            const int pressed = app.m_pressed;
            const int pressedNav = app.m_pressedNav;
            app.m_pressAnim.To(0.0f, kMotionFastMs, EaseStandard);
            app.EnsureFrameTimer();

            int action = ACT_NONE;
            if (pressedNav >= 0 && pressedNav < static_cast<int>(app.m_navButtons.size())
                && PtInRect(&app.m_navButtons[static_cast<size_t>(pressedNav)].rc, pt))
            {
                action = app.m_navButtons[static_cast<size_t>(pressedNav)].action;
            }
            else if (pressed >= 0 && pressed < static_cast<int>(app.m_buttons.size())
                     && PtInRect(&app.m_buttons[static_cast<size_t>(pressed)].rc, pt))
            {
                action = app.m_buttons[static_cast<size_t>(pressed)].action;
            }

            app.m_pressed = -1;
            app.m_pressedNav = -1;
            if (action != ACT_NONE)
            {
                Execute(action);
            }
            app.Invalidate();
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam) / 2;
            if (app.m_menuOpen)
            {
                app.m_menuScroll -= delta;
                app.LayoutMenu();
                app.Invalidate();
            }
            else if (app.m_navOpen)
            {
                app.m_navScroll = (std::max)(0, app.m_navScroll - delta);
                app.Relayout();
            }
            else if (app.m_panel != Panel::None)
            {
                app.m_historyScroll -= delta;
                app.Relayout();
            }
            else if (app.m_mode == Mode::Graphing)
            {
                app.ScaleGraph((delta > 0) ? (1.0 / 1.15) : 1.15);
                app.Relayout();
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == 1)
            {
                KillTimer(hwnd, 1);
                app.m_pressed = -1;
                app.m_pressAnim.To(0.0f, kMotionFastMs, EaseStandard);
                app.EnsureFrameTimer();
                app.Invalidate();
            }
            return 0;

        case WM_CHAR:
            if (!app.m_navOpen && !app.m_menuOpen && !app.m_settingsOpen)
            {
                if (app.m_mode == Mode::Graphing)
                {
                    const auto c = static_cast<wchar_t>(wParam);
                    if (app.m_graphField >= 0)
                    {
                        // A window bound is being edited, so it takes the keys.
                        std::wstring& value = app.m_graphFieldText[app.m_graphField];
                        if (c == L'\b')
                        {
                            if (value.empty())
                            {
                                value = app.GraphFieldText(app.m_graphField);
                            }
                            if (!value.empty())
                            {
                                value.pop_back();
                            }
                        }
                        else if (c == L'\r')
                        {
                            app.CommitGraphField();
                        }
                        else if ((c >= L'0' && c <= L'9') || c == L'.' || c == L'-')
                        {
                            value.push_back(c);
                        }
                        app.Relayout();
                        return 0;
                    }
                    // The equation rows take typed text directly; the keypad is
                    // a convenience, not the only way in.
                    if (c == L'\b')
                    {
                        app.BackspaceEquation();
                        app.Relayout();
                    }
                    else if (c >= L' ')
                    {
                        app.AppendToEquation(c);
                        app.Relayout();
                    }
                    return 0;
                }
                RunAction(hwnd, ActionForChar(static_cast<wchar_t>(wParam)));
            }
            return 0;

        case WM_SYSKEYDOWN:
        {
            const int action = ActionForKey(wParam, false, true);
            if (action != ACT_NONE)
            {
                RunAction(hwnd, action);
                return 0;
            }
            break;
        }

        case WM_KEYDOWN:
        {
            if (wParam == VK_ESCAPE && app.m_menuOpen)
            {
                CloseMenus();
                return 0;
            }
            if (wParam == VK_ESCAPE && (app.m_navOpen || app.m_settingsOpen))
            {
                app.m_navOpen = false;
                app.m_settingsOpen = false;
                app.Relayout();
                return 0;
            }
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const int action = ActionForKey(wParam, ctrl, false);
            if (action != ACT_NONE)
            {
                RunAction(hwnd, action);
                return 0;
            }
            if (app.m_panel != Panel::None && (wParam == VK_UP || wParam == VK_DOWN))
            {
                app.m_historyScroll += (wParam == VK_DOWN) ? Dp(40) : -Dp(40);
                app.Relayout();
                return 0;
            }
            return 0;
        }

        case WM_DPICHANGED:
        {
            g_dpi = HIWORD(wParam);
            ClearFontCache();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(
                hwnd,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            app.Relayout();
            return 0;
        }

        case WM_SETTINGCHANGE:
            if (lParam && lstrcmpiW(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0)
            {
                const bool wasDark = g_theme.dark;
                BeginThemeFade(app);
                LoadTheme();
                if (wasDark == g_theme.dark)
                {
                    app.m_themeAnim.Set(0.0f);
                }
                ApplyTitleBarTheme(hwnd);
                app.Invalidate();
            }
            return 0;

        case WM_DESTROY:
            g_backBuffer.Release();
            g_scratch.Release();
            g_themeLayer.Release();
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

namespace
{
    // A Setup.exe beside the calculator means Setup installed it, and Setup
    // knows how to ask GitHub for a newer release. It is asked at most once a
    // day, in its own process, so nothing here waits on the network; it exits
    // without showing anything unless it has an update to offer. The switch
    // for it is on that update page, and stored where Setup keeps its own.
    void StartDailyUpdateCheck()
    {
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = (length > 0 && length < MAX_PATH) ? wcsrchr(path, L'\\') : nullptr;
        if (slash == nullptr || (slash - path) + 11 >= MAX_PATH)
        {
            return;
        }
        lstrcpyW(slash + 1, L"Setup.exe");
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        {
            return;
        }

        constexpr wchar_t key[] = L"Software\\CompactCalculator";
        DWORD enabled = 1;
        DWORD size = sizeof(enabled);
        RegGetValueW(HKEY_CURRENT_USER, key, L"UpdateChecks", RRF_RT_REG_DWORD, nullptr, &enabled, &size);
        ULONGLONG last = 0;
        size = sizeof(last);
        RegGetValueW(HKEY_CURRENT_USER, key, L"LastUpdateCheck", RRF_RT_REG_QWORD, nullptr, &last, &size);
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const ULONGLONG ticks = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        constexpr ULONGLONG kDay = 24ull * 60 * 60 * 10000000; // FILETIME counts 100ns
        if (enabled == 0 || (ticks >= last && ticks - last < kDay))
        {
            return;
        }
        // Stamped before starting, so a check that fails still waits a day.
        RegSetKeyValueW(HKEY_CURRENT_USER, key, L"LastUpdateCheck", REG_QWORD, &ticks, sizeof(ticks));

        wchar_t command[MAX_PATH + 32];
        wsprintfW(command, L"\"%s\" /checkupdate", path);
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        if (CreateProcessW(path, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand)
{
    // GDI+ normally spins up a background thread to watch for display changes.
    // This app redraws on WM_DISPLAYCHANGE and DPI changes anyway, so suppress
    // it and pump the notification hook ourselves: one fewer thread, and its
    // stack with it.
    Gdiplus::GdiplusStartupInput startupInput;
    startupInput.SuppressBackgroundThread = TRUE;
    Gdiplus::GdiplusStartupOutput startupOutput{};
    ULONG_PTR gdiplusToken = 0;
    ULONG_PTR gdiplusHook = 0;
    bool gdiplusHooked = false;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, &startupOutput) == Gdiplus::Ok
        && startupOutput.NotificationHook != nullptr)
    {
        gdiplusHooked = (startupOutput.NotificationHook(&gdiplusHook) == Gdiplus::Ok);
    }

    LoadTheme();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"CalculatorCompactWindow";
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    const int dpi = static_cast<int>(GetDpiForSystem());
    RECT frame{ 0, 0, MulDiv(360, dpi, 96), MulDiv(620, dpi, 96) };
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, static_cast<UINT>(dpi));

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Calculator",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        frame.right - frame.left,
        frame.bottom - frame.top,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!hwnd)
    {
        return 1;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);
    StartDailyUpdateCheck();

    // While something is animating, frames are paced against the compositor
    // rather than by WM_TIMER, whose 15.6ms granularity, low priority and
    // coalescing are what make timer-driven motion look like it stutters. The
    // loop goes back to blocking on GetMessage as soon as everything settles.
    MSG message{};
    bool running = true;
    bool wasAnimating = false;
    while (running)
    {
        const bool animating = g_app.AnimationsRunning();
        if (wasAnimating && !animating)
        {
            // The scratch layer only exists for transitions, so let it go as
            // soon as one ends. The back buffer stays: the next paint needs it,
            // and releasing it only guarantees an allocation to get it back.
            g_scratch.Release();
            g_themeLayer.Release();
            g_app.OnAnimationsSettled();
        }
        wasAnimating = animating;

        if (!animating)
        {
            if (GetMessageW(&message, nullptr, 0, 0) <= 0)
            {
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }

        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running)
        {
            break;
        }

        // A frame-rate floor, independent of DwmFlush.
        //
        // DwmFlush is meant to block until the next composition pass, but it
        // returns immediately when the window is occluded, when composition is
        // off, and on some drivers under load. Without a floor the loop then
        // repaints as fast as the machine allows -- a full-window composite per
        // iteration, which pegs a core and churns the allocator hard enough to
        // look like a leak.
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG sinceLastFrame = now - g_lastFrameTick;
        if (sinceLastFrame < kMinFrameMs)
        {
            Sleep(static_cast<DWORD>(kMinFrameMs - sinceLastFrame));
        }
        g_lastFrameTick = GetTickCount64();

        g_app.FrameTick();
        if (FAILED(DwmFlush()))
        {
            Sleep(8); // composition is off; fall back to a fixed cadence
        }
    }

    ClearFontCache();
    g_backBuffer.Release();
    g_scratch.Release();
    g_themeLayer.Release();
    if (gdiplusHooked && startupOutput.NotificationUnhook != nullptr)
    {
        startupOutput.NotificationUnhook(gdiplusHook);
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return static_cast<int>(message.wParam);
}

namespace
{
    // ------------------------------------------------- converter / date layout

    void CalcApp::BuildConverterLayout(int contentRight, int height, int y)
    {
        EnsureConverter();
        const int pad = Dp(kPadDip);
        const int gap = Dp(kGapDip);

        // Two readouts, each a large value with the unit picker beneath it.
        for (int field = 0; field < 2; ++field)
        {
            const bool isFrom = (field == 0);
            RECT value{ pad + Dp(10), y, contentRight - Dp(14), y + Dp(48) };
            (isFrom ? m_convFromValue : m_convToValue) = value;

            Btn hit;
            hit.action = isFrom ? ACT_CONV_FIELD_FROM : ACT_CONV_FIELD_TO;
            hit.style = Style::Bit; // hover-only fill
            hit.rc = value;
            m_buttons.push_back(hit);
            y += Dp(48);

            Btn unit;
            unit.label = isFrom ? std::wstring_view(m_converterModel->FromUnitName()) : std::wstring_view(m_converterModel->ToUnitName());
            unit.action = isFrom ? ACT_CONV_FROM_UNIT : ACT_CONV_TO_UNIT;
            unit.style = Style::Flat;
            unit.textDip = 13;
            unit.checked = (m_converterModel->IsSecondFieldActive() != isFrom);
            unit.combo = true;
            unit.rc = { pad + Dp(8), y, (std::min)(contentRight - Dp(8), pad + Dp(260)), y + Dp(30) };
            m_buttons.push_back(unit);
            y += Dp(38);
        }

        // "About equal to" block.
        m_convSuggestions = { pad + Dp(10), y, contentRight - Dp(10), y + Dp(54) };
        if (!m_converterModel->Suggestions().empty())
        {
            y += Dp(58);
        }

        // Keypad: a clear/backspace row above the digit grid, as the shipping
        // converter lays it out.
        RECT keypad{ pad, y, contentRight - pad, height - pad };
        if (keypad.bottom < keypad.top + Dp(80))
        {
            keypad.top = (std::max)(y, static_cast<int>(keypad.bottom) - Dp(80));
        }

        const int rows = 5;
        const int rowHeight = (keypad.bottom - keypad.top) / rows;
        const int usable = keypad.right - keypad.left;

        Btn clearEntry;
        clearEntry.label = L"CE";
        clearEntry.action = Cmd(Command::CommandCENTR);
        clearEntry.style = Style::Operator;
        clearEntry.rc = { keypad.left + gap, keypad.top + gap, keypad.left + usable / 2 - gap, keypad.top + rowHeight - gap };
        m_buttons.push_back(clearEntry);

        Btn back;
        back.label = L"";
        back.action = Cmd(Command::CommandBACK);
        back.style = Style::Operator;
        back.icon = true;
        back.rc = { keypad.left + usable / 2 + gap, keypad.top + gap, keypad.right - gap, keypad.top + rowHeight - gap };
        m_buttons.push_back(back);

        static constexpr KeyDef digits[] = {
            { L"7", Cmd(Command::Command7), Style::Number, false, 0 },
            { L"8", Cmd(Command::Command8), Style::Number, false, 0 },
            { L"9", Cmd(Command::Command9), Style::Number, false, 0 },
            { L"4", Cmd(Command::Command4), Style::Number, false, 0 },
            { L"5", Cmd(Command::Command5), Style::Number, false, 0 },
            { L"6", Cmd(Command::Command6), Style::Number, false, 0 },
            { L"1", Cmd(Command::Command1), Style::Number, false, 0 },
            { L"2", Cmd(Command::Command2), Style::Number, false, 0 },
            { L"3", Cmd(Command::Command3), Style::Number, false, 0 },
            { L"+/−", Cmd(Command::CommandSIGN), Style::Number, false, 0 },
            { L"0", Cmd(Command::Command0), Style::Number, false, 0 },
            { L".", Cmd(Command::CommandPNT), Style::Number, false, 0 },
        };

        RECT grid{ keypad.left, keypad.top + rowHeight, keypad.right, keypad.bottom };
        const size_t firstDigit = m_buttons.size();
        AddGrid(digits, ARRAYSIZE(digits), 3, 4, grid);
        // Negation only applies where the category allows it.
        if (!m_converterModel->SupportsNegative())
        {
            for (size_t i = firstDigit; i < m_buttons.size(); ++i)
            {
                if (m_buttons[i].action == Cmd(Command::CommandSIGN))
                {
                    m_buttons[i].enabled = false;
                }
            }
        }
    }

    // Graphing: the plot on top, the equation list under it, and a keypad
    // carrying the graphing numpad's function set.
    // The graph options pane, following the shipping one: a window section
    // with the four bounds and a reset link, the angle units, line thickness
    // and the graph theme.
    void CalcApp::BuildGraphOptions(int contentRight, int height)
    {
        const int left = Dp(10);
        const int right = contentRight - Dp(10);
        // Below the trace/share/options row, which stays visible above it.
        const int top = Dp(kNavRowDip) + Dp(44);
        int y = top + Dp(46); // clear of the title

        const auto sectionGap = [&] { y += Dp(22); };

        Btn reset;
        reset.label = L"Reset view";
        reset.action = ACT_GRAPH_RESET;
        reset.style = Style::Bit;
        reset.textDip = 13;
        reset.accentText = true;
        reset.rc = { right - Dp(96), top + Dp(12), right - Dp(12), top + Dp(38) };
        m_buttons.push_back(reset);

        // Window: four bounds, two to a row, each under its caption.
        sectionGap();
        const int fieldWidth = (right - left - Dp(36)) / 2;
        for (int i = 0; i < 4; ++i)
        {
            const int column = i % 2;
            const int row = i / 2;
            const int fx = left + Dp(12) + column * (fieldWidth + Dp(12));
            const int fy = y + Dp(18) + row * Dp(54);
            Btn field;
            field.action = ACT_GRAPH_FIELD_BASE + i;
            field.style = Style::Bit;
            field.textDip = 14;
            field.checked = (m_graphField == i);
            field.rc = { fx, fy, fx + fieldWidth, fy + Dp(32) };
            m_buttons.push_back(field);
        }
        y += Dp(18) + Dp(54) + Dp(32) + Dp(6);

        // Units.
        sectionGap();
        const int unitWidth = (right - left - Dp(28)) / 3;
        const int unitActions[3] = { ACT_GRAPH_ANGLE_RAD, ACT_GRAPH_ANGLE_DEG, ACT_GRAPH_ANGLE_GRAD };
        static constexpr const wchar_t* kUnitLabels[3] = { L"Radians", L"Degrees", L"Gradians" };
        for (int i = 0; i < 3; ++i)
        {
            Btn unit;
            unit.label = kUnitLabels[i];
            unit.action = unitActions[i];
            unit.style = Style::Toggle;
            unit.textDip = 13;
            unit.checked = (static_cast<int>(m_graphAngle) == i);
            unit.rc = { left + Dp(12) + i * (unitWidth + Dp(2)), y, left + Dp(12) + i * (unitWidth + Dp(2)) + unitWidth,
                        y + Dp(32) };
            m_buttons.push_back(unit);
        }
        y += Dp(32);

        // Line thickness.
        sectionGap();
        Btn thickness;
        thickness.action = ACT_GRAPH_THICKNESS;
        thickness.style = Style::Bit;
        thickness.textDip = 13;
        thickness.combo = true;
        thickness.rc = { left + Dp(12), y, right - Dp(12), y + Dp(32) };
        m_buttons.push_back(thickness);
        y += Dp(32);

        // Graph theme.
        sectionGap();
        const int themeActions[2] = { ACT_GRAPH_THEME_LIGHT, ACT_GRAPH_THEME_APP };
        static constexpr const wchar_t* kThemeLabels[2] = { L"Always light", L"Match app theme" };
        for (int i = 0; i < 2; ++i)
        {
            Btn choice;
            choice.label = kThemeLabels[i];
            choice.action = themeActions[i];
            choice.style = Style::Bit;
            choice.textDip = 14;
            choice.radio = true;
            choice.checked = (i == 0) ? m_graphAlwaysLight : !m_graphAlwaysLight;
            choice.rc = { left + Dp(12), y + i * Dp(32), right - Dp(12), y + i * Dp(32) + Dp(30) };
            m_buttons.push_back(choice);
        }
        y += Dp(32) * 2 + Dp(10);

        // The card is sized to whatever the sections needed.
        m_graphOptionsRect = { left, top, right, (std::min)(y, height - Dp(10)) };
    }

    // Graphing, laid out as GraphingCalculator.xaml has it: the title bar
    // carries a two-tab toggle, and the body shows either the plot or the
    // equation editor with its keypad -- not both stacked.
    void CalcApp::BuildGraphingLayout(int contentRight, int height, int y)
    {
        EnsureGraphing();
        const int pad = Dp(kPadDip);
        const int gap = Dp(kGapDip);
        const int left = pad;
        const int right = contentRight - pad;

        if (!m_graphEquationsView)
        {
            // ---- plot view -------------------------------------------------
            m_graphRect = { left, y, right, height - pad };
            m_graphEquationRect = RECT{ 0, 0, 0, 0 };

            // Trace, share and graph options along the top trailing corner.
            const int size = Dp(32);
            int bx = m_graphRect.right - Dp(8) - size;
            for (const auto& entry : { std::pair{ ACT_GRAPH_OPTIONS, VIcon_GraphSettings },
                                       std::pair{ ACT_GRAPH_SHARE, VIcon_Share },
                                       std::pair{ ACT_GRAPH_TRACE, VIcon_Trace } })
            {
                Btn button;
                button.action = entry.first;
                button.style = Style::Flat;
                button.vicon = entry.second;
                button.textDip = 15;
                button.rc = { bx, m_graphRect.top + Dp(8), bx + size, m_graphRect.top + Dp(8) + size };
                m_buttons.push_back(button);
                bx -= size + Dp(4);
            }

            if (m_graphOptionsOpen)
            {
                BuildGraphOptions(contentRight, height);
            }

            // Zoom and recentre stacked in the bottom trailing corner.
            int by = m_graphRect.bottom - Dp(8) - size;
            for (const auto& entry : { std::pair{ ACT_GRAPH_RESET, VIcon_Recenter },
                                       std::pair{ ACT_GRAPH_ZOOM_OUT, VIcon_Minus },
                                       std::pair{ ACT_GRAPH_ZOOM_IN, VIcon_Plus } })
            {
                Btn button;
                button.action = entry.first;
                button.style = Style::Flat;
                button.vicon = entry.second;
                button.textDip = 15;
                button.rc = { m_graphRect.right - Dp(8) - size, by, m_graphRect.right - Dp(8), by + size };
                m_buttons.push_back(button);
                by -= size + Dp(4);
            }
            return;
        }

        // ---- equations view ------------------------------------------------
        m_graphRect = RECT{ 0, 0, 0, 0 };

        // The expression field, with the f badge the shipping app puts beside it.
        const int fieldHeight = Dp(56);
        m_graphEquationRect = { left + Dp(8), y + Dp(6), right - Dp(8), y + Dp(6) + fieldHeight };

        Btn field;
        field.action = ACT_GRAPH_SELECT_BASE + m_activeEquation;
        field.style = Style::Bit;
        field.textDip = 14;
        field.rc = m_graphEquationRect;
        m_buttons.push_back(field);

        y = m_graphEquationRect.bottom + Dp(10);

        // Keypad: the three dropdowns, then the scientific grid with x, y, =
        // and the enter key in place of exp, mod, n! and equals.
        RECT keypad{ pad, height - Dp(300), contentRight - pad, height - pad };
        if (keypad.top < y)
        {
            keypad.top = y;
        }

        const int controlHeight = Dp(30);
        const int third = (keypad.right - keypad.left) / 3;
        static constexpr const wchar_t* kControlLabels[3] = { L"Trigonometry ", L"Inequalities ",
                                                              L"Function " };
        const int kControlActions[3] = { ACT_TRIG_MENU, ACT_INEQ_MENU, ACT_FUNC_MENU };
        for (int i = 0; i < 3; ++i)
        {
            Btn control;
            control.label = kControlLabels[i];
            control.action = kControlActions[i];
            control.style = Style::Flat;
            control.textDip = 11;
            control.rc = { keypad.left + i * third + gap, keypad.top, keypad.left + (i + 1) * third - gap,
                           keypad.top + controlHeight };
            m_buttons.push_back(control);
        }
        keypad.top += controlHeight + Dp(2);

        struct GraphKey
        {
            const wchar_t* label;
            int action;
            Style style;
        };
        static constexpr GraphKey keys[7][5] = {
            { { L"2ⁿᵈ", ACT_SECOND, Style::Operator }, { L"π", ACT_GRAPH_TEXT_BASE + 22, Style::Operator },
              { L"e", ACT_GRAPH_TEXT_BASE + 13, Style::Operator }, { L"C", ACT_GRAPH_CLEAR, Style::Operator },
              { L"", ACT_GRAPH_BACKSPACE, Style::Operator } },
            { { L"x²", ACT_GRAPH_TEXT_BASE + 23, Style::Operator }, { L"¹/ₓ", ACT_GRAPH_TEXT_BASE + 24, Style::Operator },
              { L"|x|", ACT_GRAPH_TEXT_BASE + 25, Style::Operator }, { L"x", ACT_GRAPH_TEXT_BASE + 0, Style::Operator },
              { L"y", ACT_GRAPH_TEXT_BASE + 26, Style::Operator } },
            { { L"²√x", ACT_GRAPH_TEXT_BASE + 27, Style::Operator }, { L"(", ACT_GRAPH_TEXT_BASE + 1, Style::Operator },
              { L")", ACT_GRAPH_TEXT_BASE + 2, Style::Operator }, { L"=", ACT_GRAPH_TEXT_BASE + 28, Style::Operator },
              { L"÷", ACT_GRAPH_TEXT_BASE + 7, Style::Operator } },
            { { L"xʸ", ACT_GRAPH_TEXT_BASE + 3, Style::Operator }, { L"7", ACT_GRAPH_TEXT_BASE + 4, Style::Number },
              { L"8", ACT_GRAPH_TEXT_BASE + 5, Style::Number }, { L"9", ACT_GRAPH_TEXT_BASE + 6, Style::Number },
              { L"×", ACT_GRAPH_TEXT_BASE + 12, Style::Operator } },
            { { L"10ˣ", ACT_GRAPH_TEXT_BASE + 29, Style::Operator }, { L"4", ACT_GRAPH_TEXT_BASE + 9, Style::Number },
              { L"5", ACT_GRAPH_TEXT_BASE + 10, Style::Number }, { L"6", ACT_GRAPH_TEXT_BASE + 11, Style::Number },
              { L"−", ACT_GRAPH_TEXT_BASE + 17, Style::Operator } },
            { { L"log", ACT_GRAPH_TEXT_BASE + 30, Style::Operator }, { L"1", ACT_GRAPH_TEXT_BASE + 14, Style::Number },
              { L"2", ACT_GRAPH_TEXT_BASE + 15, Style::Number }, { L"3", ACT_GRAPH_TEXT_BASE + 16, Style::Number },
              { L"+", ACT_GRAPH_TEXT_BASE + 21, Style::Operator } },
            { { L"ln", ACT_GRAPH_TEXT_BASE + 31, Style::Operator }, { L"(−)", ACT_GRAPH_NEGATE, Style::Number },
              { L"0", ACT_GRAPH_TEXT_BASE + 19, Style::Number }, { L".", ACT_GRAPH_TEXT_BASE + 20, Style::Number },
              { L"↵", ACT_GRAPH_ADD, Style::Accent } },
        };

        const int cellWidth = (keypad.right - keypad.left) / 5;
        const int cellHeight = (keypad.bottom - keypad.top) / 7;
        for (int row = 0; row < 7; ++row)
        {
            for (int column = 0; column < 5; ++column)
            {
                const GraphKey& key = keys[row][column];
                Btn button;
                button.label = key.label;
                button.action = key.action;
                button.style = key.style;
                button.textDip = (key.style == Style::Number) ? 17 : 14;
                if (key.action == ACT_GRAPH_ADD)
                {
                    button.vicon = VIcon_Enter;
                }
                button.rc = { keypad.left + column * cellWidth + gap, keypad.top + row * cellHeight + gap,
                              keypad.left + (column + 1) * cellWidth - gap, keypad.top + (row + 1) * cellHeight - gap };
                m_buttons.push_back(button);
            }
        }
    }

    void CalcApp::BuildDateLayout(int contentRight, int height, int y)
    {
        const int pad = Dp(kPadDip);
        const bool isDifference = m_dateModel.CurrentMode() == DateCalcModel::Mode::Difference;

        // Mode picker.
        Btn modeButton;
        modeButton.label = isDifference ? L"Difference between dates" : L"Add or subtract days";
        modeButton.combo = true;
        modeButton.action = isDifference ? ACT_DATE_MODE_OFFSET : ACT_DATE_MODE_DIFF;
        modeButton.style = Style::Flat;
        modeButton.textDip = 13;
        modeButton.rc = { pad + Dp(10), y + Dp(4), contentRight - Dp(10), y + Dp(38) };
        m_buttons.push_back(modeButton);
        y += Dp(48);

        // A single field per date, reading "September 18, 2026" with a calendar
        // glyph at its trailing edge, as the shipping app shows it.
        const auto addDateRow = [&](CivilDate& date, int dateIndex) {
            m_dateFieldText[dateIndex] = DateMath::MonthName(date.month);
            m_dateFieldText[dateIndex] += L' ';
            m_dateFieldText[dateIndex] += CalcEngine::NumericString::FromInteger(date.day);
            m_dateFieldText[dateIndex] += L", ";
            m_dateFieldText[dateIndex] += CalcEngine::NumericString::FromInteger(date.year);

            Btn field;
            field.label = m_dateFieldText[dateIndex];
            field.action = ACT_DATE_FIELD_BASE + dateIndex;
            field.style = Style::Bit; // hover-only fill, like a text field
            field.textDip = 14;
            field.dateField = true;
            field.rc = { pad + Dp(10), y, contentRight - Dp(10), y + Dp(34) };
            m_buttons.push_back(field);
            y += Dp(44);
        };

        if (isDifference)
        {
            y += Dp(18); // room for the "From" caption
            addDateRow(m_dateModel.From(), 0);
            y += Dp(18); // "To"
            addDateRow(m_dateModel.To(), 1);
            m_dateResultRect = { pad + Dp(10), y + Dp(22), contentRight - Dp(10), y + Dp(58) };
            m_dateSecondaryRect = { pad + Dp(10), y + Dp(60), contentRight - Dp(10), y + Dp(86) };
        }
        else
        {
            y += Dp(18); // "From"
            addDateRow(m_dateModel.Start(), 2);

            Btn addSubtract;
            addSubtract.label = m_dateModel.IsAdding() ? L"Add " : L"Subtract ";
            addSubtract.action = ACT_DATE_ADD_SUBTRACT;
            addSubtract.style = Style::Flat;
            addSubtract.checked = true;
            addSubtract.textDip = 13;
            addSubtract.rc = { pad + Dp(10), y + Dp(4), contentRight - Dp(10), y + Dp(38) };
            m_buttons.push_back(addSubtract);
            y += Dp(48);

            // Years / months / days steppers.
            const wchar_t* captions[3] = { L"Years", L"Months", L"Days" };
            const int values[3] = { m_dateModel.OffsetYears(), m_dateModel.OffsetMonths(), m_dateModel.OffsetDays() };
            const int usable = contentRight - pad * 2 - Dp(16);
            int x = pad + Dp(10);
            for (int i = 0; i < 3; ++i)
            {
                m_dateFieldText[10 + i] = CalcEngine::NumericString::FromInteger(values[i]);
                Btn stepper;
                stepper.label = m_dateFieldText[10 + i];
                stepper.action = ACT_DATE_FIELD_BASE + 10 + i;
                stepper.style = Style::Flat;
                stepper.checked = true;
                stepper.textDip = 13;
                stepper.rc = { x + Dp(2), y + Dp(18), x + usable / 3 - Dp(2), y + Dp(52) };
                m_buttons.push_back(stepper);
                m_dateCaptions[i] = captions[i];
                m_dateCaptionRects[i] = { x + Dp(4), y, x + usable / 3, y + Dp(18) };
                x += usable / 3;
            }
            y += Dp(62);

            m_dateResultRect = { pad + Dp(10), y + Dp(22), contentRight - Dp(10), y + Dp(58) };
            m_dateSecondaryRect = { 0, 0, 0, 0 };
        }
    }
}

namespace
{
    // ------------------------------------------------------ navigation pane

    // The nav pane lists the same entries, in the same two groups and the same
    // order, as the shipping app's NavCategory manifest.
    struct NavEntry
    {
        std::wstring_view label;
        std::wstring_view glyph;
        int action;
        bool isHeader;
    };

    constexpr NavEntry kCalculatorNav[] = {
        { L"Calculator", L"", ACT_NONE, true },
        { L"Standard", L"", ACT_MODE_STANDARD, false },
        { L"Scientific", L"", ACT_MODE_SCIENTIFIC, false },
        { L"Graphing", L"", ACT_MODE_GRAPHING, false },
        { L"Programmer", L"", ACT_MODE_PROGRAMMER, false },
        { L"Date calculation", L"", ACT_MODE_DATE, false },
    };

    void CalcApp::BuildNavLayout(int width, int height)
    {
        const int pad = Dp(kPadDip);
        const int rowHeight = Dp(38);
        // A NavigationView overlay pane is SplitViewOpenPaneLength wide, not the
        // whole window, so the content stays visible beside it under the scrim.
        const int paneWidth = (std::min)(Dp(kNavPaneDip), width);
        m_navPaneWidth = paneWidth;
        m_navSlide = paneWidth - static_cast<int>(static_cast<float>(paneWidth) * m_navAnim.Value());

        Btn close;
        close.label = L"";
        close.action = ACT_NAV_MENU;
        close.style = Style::Flat;
        close.icon = true;
        close.textDip = 14;
        close.rc = { pad + Dp(2), pad + Dp(2), pad + Dp(42), pad + Dp(38) };
        PushNav(close);

        const int listTop = Dp(kNavRowDip);
        const int listBottom = height - Dp(52);
        int y = listTop - m_navScroll;

        const auto addRow = [&](std::wstring_view label, std::wstring_view glyph, int action, bool header, bool current) {
            // Only rows that fit entirely between the header and the Settings
            // entry are built, so nothing can spill over either.
            if (y >= listTop && y + rowHeight <= listBottom)
            {
                Btn row;
                row.label = label;
                row.action = header ? ACT_NONE : action;
                row.style = header ? Style::Bit : Style::Flat;
                row.enabled = !header;
                row.checked = current;
                row.textDip = header ? 12 : 14;
                row.leftAlign = true;
                row.rc = { pad + Dp(8), y, paneWidth - Dp(8), y + rowHeight - Dp(2) };
                RECT glyphRect = row.rc;
                glyphRect.left -= m_navSlide;
                glyphRect.right -= m_navSlide;
                m_navGlyphs.push_back({ glyph, glyphRect, header });
                PushNav(row);
            }
            else
            {
                m_navGlyphs.push_back({ glyph, RECT{ 0, 0, 0, 0 }, header });
            }
            y += header ? rowHeight - Dp(6) : rowHeight;
        };

        m_navGlyphs.clear();

        for (const NavEntry& entry : kCalculatorNav)
        {
            const bool current = !entry.isHeader && m_mode != Mode::Converter
                && ((entry.action == ACT_MODE_STANDARD && m_mode == Mode::Standard)
                    || (entry.action == ACT_MODE_SCIENTIFIC && m_mode == Mode::Scientific)
                    || (entry.action == ACT_MODE_PROGRAMMER && m_mode == Mode::Programmer)
                    || (entry.action == ACT_MODE_DATE && m_mode == Mode::Date));
            addRow(entry.label, entry.glyph, entry.action, entry.isHeader, current);
        }

        addRow(L"Converter", L"", ACT_NONE, true, false);
        for (const auto& category : kConverterCategories)
        {
            const bool current = (m_mode == Mode::Converter) && (m_converterCategory == category.id);
            addRow(category.name, category.glyph, ACT_CONV_CATEGORY_BASE + category.id, false, current);
        }

        m_navContentHeight = (y + m_navScroll) - listTop;

        Btn settings;
        settings.label = L"Settings";
        settings.action = ACT_SETTINGS;
        settings.style = Style::Flat;
        settings.textDip = 14;
        settings.leftAlign = true;
        settings.rc = { pad + Dp(8), height - Dp(44), paneWidth - Dp(8), height - Dp(8) };
        RECT settingsGlyph = settings.rc;
        settingsGlyph.left -= m_navSlide;
        settingsGlyph.right -= m_navSlide;
        m_navGlyphs.push_back({ L"", settingsGlyph, false });
        PushNav(settings);
    }

    void CalcApp::BuildSettingsLayout(int width, int height)
    {
        const int pad = Dp(kPadDip);
        m_navPaneWidth = width; // the settings page covers the whole window
        m_navSlide = width - static_cast<int>(static_cast<float>(width) * m_settingsAnim.Value());

        Btn back;
        back.action = ACT_SETTINGS;
        back.style = Style::Flat;
        back.vicon = VIcon_Back;
        back.textDip = 14;
        back.rc = { Dp(2), Dp(2), Dp(46), Dp(46) };
        PushNav(back);

        // Settings.xaml pads its content by 24 and spaces the cards by 4.
        const int left = Dp(24);
        const int right = width - Dp(24);
        const int spacing = Dp(4);

        // Row 0 is the back button, row 1 the 48px "Settings" header, then the
        // scrolling content: each group title has Margin 0,12,0,4 above its card.
        const int contentTop = Dp(48) + Dp(48);
        m_settingsAppearanceRect = { left, contentTop + Dp(12), right, contentTop + Dp(32) };
        int y = contentTop + Dp(36);

        // --- App theme expander -------------------------------------------
        Btn theme;
        theme.label = L"App theme";
        theme.action = ACT_APP_THEME;
        theme.style = Style::Card;
        theme.textDip = 14;
        theme.vicon = VIcon_Palette;
        theme.rc = { left, y, right, y + Dp(64) };
        PushNav(theme);
        y += Dp(64);

        if (m_themeExpanded)
        {
            // The expanded panel is a second card butted against the first,
            // holding the three choices as radio buttons.
            const int rowHeight = Dp(40);
            m_settingsThemePanel = { left, y, right, y + rowHeight * 3 + Dp(8) };
            int rowY = y + Dp(4);
            const int actions[3] = { ACT_THEME_LIGHT, ACT_THEME_DARK, ACT_THEME_SYSTEM };
            static const wchar_t* const names[3] = { L"Light", L"Dark", L"Use system setting" };
            const int selected = (m_themeChoice == 1) ? 0 : (m_themeChoice == 2) ? 1 : 2;
            for (int i = 0; i < 3; ++i)
            {
                Btn choice;
                choice.label = names[i];
                choice.action = actions[i];
                choice.style = Style::Bit; // hover fill only, like a list row
                choice.textDip = 14;
                choice.checked = (i == selected);
                choice.radio = true;
                choice.rc = { left + Dp(8), rowY, right - Dp(8), rowY + rowHeight - Dp(2) };
                PushNav(choice);
                rowY += rowHeight;
            }
            y = m_settingsThemePanel.bottom;
        }
        else
        {
            m_settingsThemePanel = RECT{ 0, 0, 0, 0 };
        }

        // --- About --------------------------------------------------------
        y += Dp(26);
        m_settingsAboutRect = { left, y, right, y + Dp(20) };
        y += Dp(24);

        Btn about;
        about.label = L"Calculator";
        about.action = ACT_ABOUT_EXPAND;
        about.style = Style::Card;
        about.textDip = 14;
        about.vicon = VIcon_Calculator;
        about.rc = { left, y, right, y + Dp(76) };
        PushNav(about);
        y += Dp(76);

        if (m_aboutExpanded)
        {
            const int rowHeight = Dp(30);
            m_settingsAboutPanel = { left, y, right, y + rowHeight * 3 + Dp(10) };
            int rowY = y + Dp(6);
            static const wchar_t* const links[3] = {
                L"Microsoft Software License Terms", L"Microsoft Services Agreement", L"Microsoft Privacy Statement"
            };
            for (int i = 0; i < 3; ++i)
            {
                Btn link;
                link.label = links[i];
                link.action = ACT_NONE;
                link.style = Style::Bit;
                link.enabled = false;
                link.textDip = 14;
                link.accentText = true;
                link.rc = { left + Dp(8), rowY, right - Dp(8), rowY + rowHeight - Dp(2) };
                PushNav(link);
                rowY += rowHeight;
            }
            y = m_settingsAboutPanel.bottom;
        }
        else
        {
            m_settingsAboutPanel = RECT{ 0, 0, 0, 0 };
        }

        y += spacing + Dp(8);
        m_settingsFeedbackRect = { left, y, right, y + Dp(22) };
        y += Dp(22) + Dp(9);
        m_settingsContributeRect = { left, y, right, y + Dp(44) };

        m_navContentHeight = height;
    }

}
