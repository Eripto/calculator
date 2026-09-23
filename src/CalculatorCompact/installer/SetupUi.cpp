// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// The setup wizard: a single fixed-size window that draws its own pages in the
// Windows 11 style, so it follows the light or dark theme and accent colour
// the way the calculator itself does. The work happens on a background thread
// and reports back step by step, which is what the progress page shows.

#include "SetupActions.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <gdiplus.h>

namespace
{
    using Setup::Outcome;

    // ---------------------------------------------------------- constants

    constexpr wchar_t kWindowClass[] = L"CompactCalculatorSetup";
    constexpr wchar_t kWindowTitle[] = L"Calculator Setup";

    constexpr int kClientWidthDip = 600;
    constexpr int kClientHeightDip = 540;
    constexpr int kMarginDip = 32;
    constexpr int kFooterDip = 80;
    constexpr int kButtonWidthDip = 120;
    constexpr int kButtonHeightDip = 32;
    constexpr int kTransitionMs = 220;
    constexpr int kMinimumStepMs = 350; // long enough to read each step as it ticks off

    constexpr UINT WM_APP_STEP_BEGIN = WM_APP + 1;
    constexpr UINT WM_APP_STEP_END = WM_APP + 2;
    constexpr UINT WM_APP_RUN_DONE = WM_APP + 3;
    constexpr UINT_PTR kAnimationTimer = 1;
    constexpr UINT_PTR kFinishTimer = 2;

    enum Id
    {
        IdNone,
        IdNext,
        IdBack,
        IdCancel,
        IdInstall,
        IdRemove,
        IdFinish,
        IdOptRedirect,
        IdOptKey,
        IdOptStart,
        IdOptStore,
        IdModeChange,
        IdModeRemove,
        IdLaunch,
    };

    enum class Page
    {
        Welcome,
        Maintenance,
        Options,
        Confirm,
        Progress,
        Finished,
        NothingToRemove,
    };

    enum class ControlType
    {
        Button,
        AccentButton,
        CheckCard,
        RadioCard,
        CheckInline,
    };

    enum class FontRole
    {
        Body,
        BodyStrong,
        Caption,
        Subtitle,
        Title,
        Count,
    };

    enum class GlyphKind
    {
        AppIcon,
        FeatureCheck,
        Success,
        Caution,
        Critical,
        Step, // drawn from the step's live state
    };

    enum class StepState
    {
        Pending,
        Active,
        Done,
        Skipped,
        Failed,
    };

    struct Control
    {
        int id = IdNone;
        ControlType type = ControlType::Button;
        RECT rc{};
        std::wstring text;
        std::wstring detail;
        bool checked = false;
        bool enabled = true;
    };

    struct Label
    {
        RECT rc{};
        std::wstring text;
        FontRole font = FontRole::Body;
        COLORREF color = 0;
        UINT flags = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX;
    };

    struct Glyph
    {
        RECT rc{};
        GlyphKind kind = GlyphKind::FeatureCheck;
        int step = -1;
    };

    // ------------------------------------------------------------ palette

    struct Palette
    {
        bool dark = false;
        COLORREF page, footer, divider;
        COLORREF card, cardHover, cardPress, cardBorder;
        COLORREF text, secondary, disabled;
        COLORREF button, buttonHover, buttonPress, buttonBorder;
        COLORREF accent, accentHover, accentPress, accentText, accentDisabled, accentDisabledText;
        COLORREF boxFill, boxBorder;
        COLORREF success, caution, critical, onSignal;
    };

    COLORREF Mix(COLORREF a, COLORREF b, double t)
    {
        auto lerp = [t](int x, int y) { return static_cast<int>(x + (y - x) * t + 0.5); };
        return RGB(lerp(GetRValue(a), GetRValue(b)), lerp(GetGValue(a), GetGValue(b)), lerp(GetBValue(a), GetBValue(b)));
    }

    bool SystemUsesDarkTheme()
    {
        DWORD value = 1;
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                         RRF_RT_REG_DWORD, nullptr, &value, &size)
            != ERROR_SUCCESS)
        {
            return false;
        }
        return value == 0;
    }

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

    // Colours are the WinUI 3 theme resources, pre-blended onto the page they
    // sit on, since GDI has no alpha of its own.
    Palette LoadPalette()
    {
        Palette p{};
        const COLORREF accent = SystemAccent();
        p.dark = SystemUsesDarkTheme();
        if (p.dark)
        {
            p.page = RGB(32, 32, 32);
            p.footer = RGB(28, 28, 28);
            p.divider = RGB(20, 20, 20);
            p.card = RGB(43, 43, 43);
            p.cardHover = RGB(50, 50, 50);
            p.cardPress = RGB(39, 39, 39);
            p.cardBorder = RGB(29, 29, 29);
            p.text = RGB(255, 255, 255);
            p.secondary = RGB(197, 197, 197);
            p.disabled = RGB(120, 120, 120);
            p.button = RGB(45, 45, 45);
            p.buttonHover = RGB(50, 50, 50);
            p.buttonPress = RGB(39, 39, 39);
            p.buttonBorder = RGB(58, 58, 58);
            p.accent = Mix(accent, RGB(255, 255, 255), 0.45);
            p.accentHover = Mix(accent, RGB(255, 255, 255), 0.38);
            p.accentPress = Mix(accent, RGB(255, 255, 255), 0.30);
            p.accentText = RGB(0, 0, 0);
            p.accentDisabled = RGB(67, 67, 67);
            p.accentDisabledText = RGB(135, 135, 135);
            p.boxFill = RGB(36, 36, 36);
            p.boxBorder = RGB(154, 154, 154);
            p.success = RGB(108, 203, 95);
            p.caution = RGB(252, 225, 0);
            p.critical = RGB(255, 153, 164);
            p.onSignal = RGB(0, 0, 0);
        }
        else
        {
            p.page = RGB(243, 243, 243);
            p.footer = RGB(236, 236, 236);
            p.divider = RGB(222, 222, 222);
            p.card = RGB(251, 251, 251);
            p.cardHover = RGB(246, 246, 246);
            p.cardPress = RGB(243, 243, 243);
            p.cardBorder = RGB(229, 229, 229);
            p.text = RGB(27, 27, 27);
            p.secondary = RGB(96, 96, 96);
            p.disabled = RGB(160, 160, 160);
            p.button = RGB(251, 251, 251);
            p.buttonHover = RGB(246, 246, 246);
            p.buttonPress = RGB(245, 245, 245);
            p.buttonBorder = RGB(214, 214, 214);
            p.accent = Mix(accent, RGB(0, 0, 0), 0.20);
            p.accentHover = Mix(accent, RGB(0, 0, 0), 0.10);
            p.accentPress = Mix(accent, RGB(0, 0, 0), 0.30);
            p.accentText = RGB(255, 255, 255);
            p.accentDisabled = RGB(191, 191, 191);
            p.accentDisabledText = RGB(255, 255, 255);
            p.boxFill = RGB(245, 245, 245);
            p.boxBorder = RGB(135, 135, 135);
            p.success = RGB(15, 123, 15);
            p.caution = RGB(157, 93, 0);
            p.critical = RGB(196, 43, 28);
            p.onSignal = RGB(255, 255, 255);
        }
        return p;
    }

    // -------------------------------------------------------------- fonts

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

    Gdiplus::Color ToGp(COLORREF c, BYTE alpha = 255)
    {
        return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
    }

    // ------------------------------------------------------------- wizard

    class Wizard
    {
    public:
        void Create(HINSTANCE instance, bool uninstall);
        LRESULT Handle(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    private:
        // layout
        int D(int dip) const { return MulDiv(dip, m_dpi, 96); }
        void LoadResources();
        void Relayout();
        void SetPage(Page page);
        int ClientWidth() const { return D(kClientWidthDip); }
        int ClientHeight() const { return D(kClientHeightDip); }
        int TextHeight(const std::wstring& text, FontRole font, int width) const;
        int AddLabel(int x, int y, int width, const std::wstring& text, FontRole font, COLORREF color, UINT extraFlags = 0);
        void AddFooter(std::initializer_list<Control> buttons);
        int AddCard(int y, int id, ControlType type, const std::wstring& title, const std::wstring& detail, bool checked);
        int AddBullets(int y, const std::vector<std::wstring>& lines);
        void LayoutWelcome();
        void LayoutMaintenance();
        void LayoutOptions();
        void LayoutConfirm();
        void LayoutProgress();
        void LayoutFinished();
        void LayoutNothingToRemove();

        // painting
        void Paint(HDC hdc);
        void PaintScene(HDC hdc);
        void PaintControl(HDC hdc, Gdiplus::Graphics& g, const Control& control, bool hot, bool pressed);
        void PaintGlyph(HDC hdc, Gdiplus::Graphics& g, const Glyph& glyph);
        void PaintProgress(Gdiplus::Graphics& g);
        void PaintText(HDC hdc, const RECT& rc, const std::wstring& text, FontRole font, COLORREF color, UINT flags) const;

        // input
        int HitTest(POINT pt) const;
        void Activate(int id);
        void MoveFocus(int direction);
        int DefaultButton() const;
        void StartAnimation();
        bool Animating() const;

        // the run
        void StartRun(std::unique_ptr<Setup::Plan> plan);
        static DWORD WINAPI Worker(void* param);
        void OnRunDone();
        bool RunSucceeded() const;

        HWND m_hwnd = nullptr;
        HINSTANCE m_instance = nullptr;
        int m_dpi = 96;
        Palette m_palette{};
        HFONT m_fonts[static_cast<int>(FontRole::Count)]{};
        HICON m_icon = nullptr;

        Page m_page = Page::Welcome;
        Page m_optionsBack = Page::Welcome;
        bool m_confirmFromMaintenance = false;
        Setup::Installed m_installed{};
        DWORD m_options = Setup::OptionDefaults;
        bool m_modeRemove = false;
        bool m_launchAfter = true;

        std::vector<Control> m_controls;
        std::vector<Label> m_labels;
        std::vector<Glyph> m_glyphs;
        RECT m_contentRect{};
        RECT m_progressRect{};
        int m_hot = IdNone;
        int m_pressed = IdNone;
        int m_focus = -1; // index into m_controls
        bool m_showFocus = false;

        std::unique_ptr<Setup::Plan> m_plan;
        std::vector<StepState> m_stepStates;
        std::vector<std::wstring> m_stepNotes;
        bool m_running = false;
        int m_completed = 0;
        float m_progressShown = 0.0f;

        ULONGLONG m_transitionStart = 0;
        bool m_timerRunning = false;
        HBITMAP m_back = nullptr;
        HBITMAP m_layer = nullptr;
        SIZE m_backSize{};
    };

    Wizard g_wizard;

    LRESULT CALLBACK WizardProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        return g_wizard.Handle(hwnd, message, wParam, lParam);
    }

    // ------------------------------------------------------------ helpers

    void FillRounded(Gdiplus::Graphics& g, const RECT& rc, float radius, COLORREF color, BYTE alpha = 255)
    {
        const float w = static_cast<float>(rc.right - rc.left);
        const float h = static_cast<float>(rc.bottom - rc.top);
        if (w <= 0 || h <= 0)
        {
            return;
        }
        const float r = (std::min)(radius, (std::min)(w, h) / 2.0f);
        const float d = r * 2.0f;
        const float x = static_cast<float>(rc.left);
        const float y = static_cast<float>(rc.top);
        Gdiplus::GraphicsPath path;
        path.AddArc(x, y, d, d, 180.0f, 90.0f);
        path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
        path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
        path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
        Gdiplus::SolidBrush brush(ToGp(color, alpha));
        g.FillPath(&brush, &path);
    }

    // A 1px border is a filled shape with the fill inset over it, which keeps
    // the corners as smooth as the fill's.
    void FillBordered(Gdiplus::Graphics& g, const RECT& rc, float radius, COLORREF fill, COLORREF border, int borderWidth)
    {
        FillRounded(g, rc, radius, border);
        RECT inner = rc;
        InflateRect(&inner, -borderWidth, -borderWidth);
        FillRounded(g, inner, (std::max)(0.0f, radius - static_cast<float>(borderWidth)), fill);
    }

    void StrokeRounded(Gdiplus::Graphics& g, const RECT& rc, float radius, COLORREF color, float width)
    {
        const float w = static_cast<float>(rc.right - rc.left);
        const float h = static_cast<float>(rc.bottom - rc.top);
        const float r = (std::min)(radius, (std::min)(w, h) / 2.0f);
        const float d = r * 2.0f;
        const float x = static_cast<float>(rc.left);
        const float y = static_cast<float>(rc.top);
        Gdiplus::GraphicsPath path;
        path.AddArc(x, y, d, d, 180.0f, 90.0f);
        path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
        path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
        path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
        Gdiplus::Pen pen(ToGp(color), width);
        g.DrawPath(&pen, &path);
    }

    void FillCircle(Gdiplus::Graphics& g, const RECT& rc, COLORREF color)
    {
        Gdiplus::SolidBrush brush(ToGp(color));
        g.FillEllipse(&brush, static_cast<float>(rc.left), static_cast<float>(rc.top),
                      static_cast<float>(rc.right - rc.left), static_cast<float>(rc.bottom - rc.top));
    }

    Gdiplus::PointF At(const RECT& rc, float fx, float fy)
    {
        return Gdiplus::PointF(rc.left + (rc.right - rc.left) * fx, rc.top + (rc.bottom - rc.top) * fy);
    }

    void StrokeCheck(Gdiplus::Graphics& g, const RECT& rc, COLORREF color, float width)
    {
        Gdiplus::Pen pen(ToGp(color), width);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        const Gdiplus::PointF points[] = { At(rc, 0.27f, 0.52f), At(rc, 0.43f, 0.68f), At(rc, 0.74f, 0.35f) };
        g.DrawLines(&pen, points, 3);
    }

    void StrokeExclamation(Gdiplus::Graphics& g, const RECT& rc, COLORREF color, float width)
    {
        Gdiplus::Pen pen(ToGp(color), width);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&pen, At(rc, 0.5f, 0.27f), At(rc, 0.5f, 0.56f));
        const float dot = width * 1.1f;
        const Gdiplus::PointF centre = At(rc, 0.5f, 0.72f);
        Gdiplus::SolidBrush brush(ToGp(color));
        g.FillEllipse(&brush, centre.X - dot / 2, centre.Y - dot / 2, dot, dot);
    }

    void StrokeCross(Gdiplus::Graphics& g, const RECT& rc, COLORREF color, float width)
    {
        Gdiplus::Pen pen(ToGp(color), width);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&pen, At(rc, 0.34f, 0.34f), At(rc, 0.66f, 0.66f));
        g.DrawLine(&pen, At(rc, 0.66f, 0.34f), At(rc, 0.34f, 0.66f));
    }

    std::wstring KiloBytes(unsigned long long bytes)
    {
        // By hand: std::to_wstring goes through swprintf, and with it a full
        // printf implementation, floating point included, for one integer.
        unsigned long long kb = (bytes + 1023) / 1024;
        std::wstring digits;
        do
        {
            digits.insert(digits.begin(), static_cast<wchar_t>(L'0' + kb % 10));
            kb /= 10;
        } while (kb != 0);
        return digits + L" KB";
    }

    // ------------------------------------------------------------- create

    void Wizard::Create(HINSTANCE instance, bool uninstall)
    {
        m_instance = instance;
        m_installed = Setup::QueryInstalled();
        if (m_installed.present)
        {
            m_options = m_installed.options;
        }
        if (uninstall)
        {
            m_page = m_installed.present ? Page::Confirm : Page::NothingToRemove;
        }
        else
        {
            m_page = m_installed.present ? Page::Maintenance : Page::Welcome;
        }

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WizardProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);

        // Sized for the system DPI here; WM_DPICHANGED corrects it if the
        // window lands on a monitor with a different one.
        const int dpi = static_cast<int>(GetDpiForSystem());
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT frame{ 0, 0, MulDiv(kClientWidthDip, dpi, 96), MulDiv(kClientHeightDip, dpi, 96) };
        AdjustWindowRectExForDpi(&frame, style, FALSE, 0, static_cast<UINT>(dpi));
        const int width = frame.right - frame.left;
        const int height = frame.bottom - frame.top;

        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const RECT& work = monitor.rcWork;
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;

        CreateWindowExW(0, kWindowClass, kWindowTitle, style, x, y, width, height, nullptr, nullptr, instance, nullptr);
        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);
    }

    void Wizard::LoadResources()
    {
        m_palette = LoadPalette();
        const BOOL dark = m_palette.dark ? TRUE : FALSE;
        DwmSetWindowAttribute(m_hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(m_hwnd, 19, &dark, sizeof(dark)); // its pre-20H1 spelling

        static const wchar_t* textFace = FontInstalled(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        static const wchar_t* displayFace = FontInstalled(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
        struct Spec
        {
            int size;
            int weight;
            const wchar_t* face;
        };
        // The WinUI type ramp: Body 14, Body Strong 14 semibold, Caption 12,
        // Subtitle 20 semibold, Title 28 semibold.
        const Spec specs[] = {
            { 14, FW_NORMAL, textFace },
            { 14, FW_SEMIBOLD, textFace },
            { 12, FW_NORMAL, textFace },
            { 20, FW_SEMIBOLD, displayFace },
            { 28, FW_SEMIBOLD, displayFace },
        };
        for (int i = 0; i < static_cast<int>(FontRole::Count); ++i)
        {
            if (m_fonts[i] != nullptr)
            {
                DeleteObject(m_fonts[i]);
            }
            m_fonts[i] = CreateFontW(-D(specs[i].size), 0, 0, 0, specs[i].weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, specs[i].face);
        }
        if (m_icon != nullptr)
        {
            DestroyIcon(m_icon);
        }
        m_icon = static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(101), IMAGE_ICON, D(48), D(48), LR_DEFAULTCOLOR));
    }

    // ------------------------------------------------------------- layout

    int Wizard::TextHeight(const std::wstring& text, FontRole font, int width) const
    {
        HDC hdc = GetDC(m_hwnd);
        HGDIOBJ previous = SelectObject(hdc, m_fonts[static_cast<int>(font)]);
        RECT rc{ 0, 0, width, 0 };
        DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(hdc, previous);
        ReleaseDC(m_hwnd, hdc);
        return rc.bottom - rc.top;
    }

    int Wizard::AddLabel(int x, int y, int width, const std::wstring& text, FontRole font, COLORREF color, UINT extraFlags)
    {
        const bool singleLine = (extraFlags & DT_SINGLELINE) != 0;
        const int height = singleLine ? TextHeight(L"Ag", font, width) : TextHeight(text, font, width);
        Label label;
        label.rc = RECT{ x, y, x + width, y + height };
        label.text = text;
        label.font = font;
        label.color = color;
        label.flags = singleLine ? (DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | extraFlags) : (DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | extraFlags);
        m_labels.push_back(label);
        return y + height;
    }

    // Buttons are listed left to right and packed against the trailing edge;
    // the accent one goes last, where Windows puts the primary action.
    void Wizard::AddFooter(std::initializer_list<Control> buttons)
    {
        const int height = D(kButtonHeightDip);
        const int width = D(kButtonWidthDip);
        const int gap = D(8);
        const int top = ClientHeight() - D(kFooterDip) + (D(kFooterDip) - height) / 2;
        int right = ClientWidth() - D(24);
        std::vector<Control> ordered(buttons);
        for (auto it = ordered.rbegin(); it != ordered.rend(); ++it)
        {
            Control control = *it;
            control.rc = RECT{ right - width, top, right, top + height };
            right -= width + gap;
            m_controls.push_back(control);
        }
        // Tab order should run left to right, as the buttons read.
        std::reverse(m_controls.end() - static_cast<std::ptrdiff_t>(ordered.size()), m_controls.end());
    }

    int Wizard::AddCard(int y, int id, ControlType type, const std::wstring& title, const std::wstring& detail, bool checked)
    {
        const int left = D(kMarginDip);
        const int right = ClientWidth() - D(kMarginDip);
        const int textLeft = left + D(52);
        const int textWidth = right - D(16) - textLeft;
        const int titleHeight = TextHeight(title, FontRole::Body, textWidth);
        const int detailHeight = detail.empty() ? 0 : TextHeight(detail, FontRole::Caption, textWidth);
        const int height = (std::max)(D(60), D(12) + titleHeight + (detail.empty() ? 0 : D(2) + detailHeight) + D(13));

        Control control;
        control.id = id;
        control.type = type;
        control.rc = RECT{ left, y, right, y + height };
        control.text = title;
        control.detail = detail;
        control.checked = checked;
        m_controls.push_back(control);
        return y + height;
    }

    int Wizard::AddBullets(int y, const std::vector<std::wstring>& lines)
    {
        const int left = D(kMarginDip);
        const int width = ClientWidth() - left - D(kMarginDip) - D(28);
        for (const std::wstring& line : lines)
        {
            Glyph glyph;
            glyph.kind = GlyphKind::FeatureCheck;
            glyph.rc = RECT{ left, y + D(1), left + D(16), y + D(17) };
            m_glyphs.push_back(glyph);
            y = (std::max)(AddLabel(left + D(28), y, width, line, FontRole::Body, m_palette.text), y + D(20)) + D(10);
        }
        return y;
    }

    void Wizard::Relayout()
    {
        int focusedId = (m_focus >= 0 && m_focus < static_cast<int>(m_controls.size())) ? m_controls[static_cast<size_t>(m_focus)].id : IdNone;
        m_controls.clear();
        m_labels.clear();
        m_glyphs.clear();
        m_progressRect = RECT{};
        m_contentRect = RECT{ 0, 0, ClientWidth(), ClientHeight() - D(kFooterDip) };

        switch (m_page)
        {
        case Page::Welcome: LayoutWelcome(); break;
        case Page::Maintenance: LayoutMaintenance(); break;
        case Page::Options: LayoutOptions(); break;
        case Page::Confirm: LayoutConfirm(); break;
        case Page::Progress: LayoutProgress(); break;
        case Page::Finished: LayoutFinished(); break;
        case Page::NothingToRemove: LayoutNothingToRemove(); break;
        }

        m_focus = -1;
        for (size_t i = 0; i < m_controls.size(); ++i)
        {
            if (m_controls[i].id == focusedId)
            {
                m_focus = static_cast<int>(i);
            }
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void Wizard::LayoutWelcome()
    {
        const int left = D(kMarginDip);
        const int width = ClientWidth() - 2 * left;
        int y = D(kMarginDip);

        m_glyphs.push_back(Glyph{ RECT{ left, y, left + D(48), y + D(48) }, GlyphKind::AppIcon });
        y += D(48) + D(20);
        y = AddLabel(left, y, width, L"Set up Calculator", FontRole::Title, m_palette.text) + D(8);
        y = AddLabel(left, y, width,
                     L"A compact rebuild of Windows Calculator: the same calculation engine, in a single "
                         + KiloBytes(Setup::PayloadBytes()) + L" program.",
                     FontRole::Body, m_palette.secondary)
            + D(24);
        AddBullets(y, {
                          L"Standard, Scientific, Graphing, Programmer and Date calculation",
                          L"Twelve unit converters",
                          L"Memory and history, as in the built-in app",
                          L"Follows your light or dark theme and accent colour",
                      });

        AddFooter({ Control{ IdCancel, ControlType::Button, {}, L"Cancel" }, Control{ IdNext, ControlType::AccentButton, {}, L"Next" } });
    }

    void Wizard::LayoutMaintenance()
    {
        const int left = D(kMarginDip);
        const int width = ClientWidth() - 2 * left;
        int y = D(kMarginDip);

        m_glyphs.push_back(Glyph{ RECT{ left, y, left + D(48), y + D(48) }, GlyphKind::AppIcon });
        y += D(48) + D(20);
        y = AddLabel(left, y, width, L"Calculator is already installed", FontRole::Subtitle, m_palette.text) + D(4);
        const std::wstring where = m_installed.byScript ? L"Installed with Install-Calculator.ps1, in " + m_installed.dir
                                                        : L"Installed in " + m_installed.dir;
        y = AddLabel(left, y, width, where, FontRole::Caption, m_palette.secondary, DT_SINGLELINE | DT_PATH_ELLIPSIS) + D(24);

        y = AddCard(y, IdModeChange, ControlType::RadioCard, L"Change options",
                    L"Update Calculator and choose again how it fits into Windows.", !m_modeRemove)
            + D(4);
        AddCard(y, IdModeRemove, ControlType::RadioCard, L"Remove Calculator",
                L"Put back everything Setup changed, then delete Calculator.", m_modeRemove);

        AddFooter({ Control{ IdCancel, ControlType::Button, {}, L"Cancel" }, Control{ IdNext, ControlType::AccentButton, {}, L"Next" } });
    }

    void Wizard::LayoutOptions()
    {
        const int left = D(kMarginDip);
        const int width = ClientWidth() - 2 * left;
        int y = D(kMarginDip);

        y = AddLabel(left, y, width, L"Choose how Calculator fits in", FontRole::Subtitle, m_palette.text) + D(4);
        y = AddLabel(left, y, width, L"You can change these later, or undo all of them from Settings > Apps.", FontRole::Caption,
                     m_palette.secondary)
            + D(20);

        using namespace Setup;
        y = AddCard(y, IdOptRedirect, ControlType::CheckCard, L"Open it wherever Windows opens a calculator",
                    L"Typing calc in Run or a terminal, and apps that start calc.exe. Windows will ask for administrator permission.",
                    (m_options & OptionRedirectCalc) != 0)
            + D(4);
        y = AddCard(y, IdOptKey, ControlType::CheckCard, L"Use it for the Calculator key",
                    L"On keyboards that have one. It may take a sign-out to pick up.", (m_options & OptionCalculatorKey) != 0)
            + D(4);
        y = AddCard(y, IdOptStart, ControlType::CheckCard, L"Add it to the Start menu", L"So it comes up when you search for Calculator.",
                    (m_options & OptionStartMenu) != 0)
            + D(4);
        y = AddCard(y, IdOptStore, ControlType::CheckCard, L"Replace the Microsoft Store Calculator",
                    L"Removes the built-in app for your account, so its Start tile and calculator: links open this one. Removing Calculator brings it back.",
                    (m_options & OptionReplaceStore) != 0)
            + D(16);

        const std::wstring dir = m_installed.present ? m_installed.dir : Setup::DefaultInstallDir();
        AddLabel(left, y, width, L"Installs to " + dir, FontRole::Caption, m_palette.secondary, DT_SINGLELINE | DT_PATH_ELLIPSIS);

        AddFooter({ Control{ IdBack, ControlType::Button, {}, L"Back" },
                    Control{ IdInstall, ControlType::AccentButton, {}, m_installed.present ? L"Update" : L"Install" } });
    }

    void Wizard::LayoutConfirm()
    {
        const int left = D(kMarginDip);
        const int width = ClientWidth() - 2 * left;
        int y = D(kMarginDip);

        y = AddLabel(left, y, width, L"Remove Calculator?", FontRole::Subtitle, m_palette.text) + D(4);
        y = AddLabel(left, y, width, L"Setup will put back everything it changed:", FontRole::Body, m_palette.secondary) + D(20);

        using namespace Setup;
        std::vector<std::wstring> lines;
        const DWORD applied = m_installed.options;
        if (applied & OptionRedirectCalc)
        {
            lines.push_back(L"calc.exe opens the Windows calculator again");
        }
        if (applied & OptionCalculatorKey)
        {
            lines.push_back(L"The Calculator key goes back to how it was");
        }
        if (applied & OptionStartMenu)
        {
            lines.push_back(L"The Start menu entry is removed");
        }
        if (applied & OptionReplaceStore)
        {
            lines.push_back(L"The Microsoft Store Calculator comes back for your account");
        }
        lines.push_back(L"Calculator is removed from Settings > Apps and its files are deleted");
        y = AddBullets(y, lines) + D(8);

        if (applied & OptionRedirectCalc)
        {
            AddLabel(left, y, width, L"Windows will ask for administrator permission to give calc.exe back.", FontRole::Caption,
                     m_palette.secondary);
        }

        if (m_confirmFromMaintenance)
        {
            AddFooter({ Control{ IdBack, ControlType::Button, {}, L"Back" }, Control{ IdRemove, ControlType::AccentButton, {}, L"Remove" } });
        }
        else
        {
            AddFooter({ Control{ IdCancel, ControlType::Button, {}, L"Cancel" }, Control{ IdRemove, ControlType::AccentButton, {}, L"Remove" } });
        }
    }

    void Wizard::LayoutProgress()
    {
        const int left = D(kMarginDip);
        const int width = ClientWidth() - 2 * left;
        int y = D(kMarginDip);

        const bool removing = m_plan && m_plan->removing;
        y = AddLabel(left, y, width, removing ? L"Removing Calculator" : L"Installing Calculator", FontRole::Subtitle, m_palette.text) + D(4);

        std::wstring status = L"Getting ready…";
        for (size_t i = 0; m_plan && i < m_stepStates.size(); ++i)
        {
            if (m_stepStates[i] == StepState::Active)
            {
                status = m_plan->steps[i].title + L"…";
            }
        }
        y = AddLabel(left, y, width, status, FontRole::Caption, m_palette.secondary, DT_SINGLELINE | DT_END_ELLIPSIS) + D(16);

        m_progressRect = RECT{ left, y, left + width, y + D(4) };
        y += D(4) + D(24);

        for (size_t i = 0; m_plan && i < m_plan->steps.size(); ++i)
        {
            Glyph glyph;
            glyph.kind = GlyphKind::Step;
            glyph.step = static_cast<int>(i);
            glyph.rc = RECT{ left, y + D(1), left + D(16), y + D(17) };
            m_glyphs.push_back(glyph);

            const StepState state = m_stepStates[i];
            const COLORREF color = (state == StepState::Pending) ? m_palette.secondary : m_palette.text;
            int bottom = AddLabel(left + D(28), y, width - D(28), m_plan->steps[i].title, FontRole::Body, color);
            if (!m_stepNotes[i].empty())
            {
                bottom = AddLabel(left + D(28), bottom + D(2), width - D(28), m_stepNotes[i], FontRole::Caption, m_palette.secondary);
            }
            y = (std::max)(bottom, y + D(20)) + D(12);
        }

        Control cancel{ IdCancel, ControlType::Button, {}, L"Cancel" };
        cancel.enabled = false; // nothing here can be stopped half way safely
        AddFooter({ cancel });
    }

    bool Wizard::RunSucceeded() const
    {
        return std::none_of(m_stepStates.begin(), m_stepStates.end(), [](StepState s) { return s == StepState::Failed || s == StepState::Skipped; });
    }

    void Wizard::LayoutFinished()
    {
        const int left = D(kMarginDip);
        const int width = ClientWidth() - 2 * left;
        int y = D(kMarginDip);
        const bool removing = m_plan && m_plan->removing;
        const bool clean = RunSucceeded();
        const bool copied = !m_stepStates.empty() && m_stepStates.front() == StepState::Done;

        std::wstring title;
        std::wstring body;
        GlyphKind badge = GlyphKind::Success;
        if (removing)
        {
            if (m_plan->complete && clean)
            {
                title = L"Calculator has been removed";
                body = L"Everything Setup changed is back the way it was.";
            }
            else if (m_plan->complete)
            {
                title = L"Calculator has been removed";
                body = L"One thing needs your attention:";
                badge = GlyphKind::Caution;
            }
            else
            {
                title = L"Calculator wasn't removed";
                body = L"Run Setup again to finish. Here's what stopped it:";
                badge = GlyphKind::Critical;
            }
        }
        else if (!m_plan || !copied)
        {
            title = L"Calculator couldn't be installed";
            body = L"Nothing was changed. Here's what went wrong:";
            badge = GlyphKind::Critical;
        }
        else if (clean)
        {
            title = L"Calculator is ready";
            if (m_plan->applied & Setup::OptionRedirectCalc)
            {
                body = L"Type calc anywhere to open it, or find it in the Start menu.";
            }
            else if (m_plan->applied & Setup::OptionStartMenu)
            {
                body = L"Find it in the Start menu.";
            }
            else
            {
                body = L"It's installed in " + m_plan->installDir + L".";
            }
        }
        else
        {
            title = L"Calculator is installed";
            body = L"A few things didn't go as planned:";
            badge = GlyphKind::Caution;
        }

        m_glyphs.push_back(Glyph{ RECT{ left, y, left + D(48), y + D(48) }, badge });
        y += D(48) + D(20);
        y = AddLabel(left, y, width, title, FontRole::Subtitle, m_palette.text) + D(4);
        y = AddLabel(left, y, width, body, FontRole::Body, m_palette.secondary) + D(16);

        for (size_t i = 0; m_plan && i < m_stepStates.size(); ++i)
        {
            const StepState state = m_stepStates[i];
            if (state != StepState::Skipped && state != StepState::Failed)
            {
                continue;
            }
            m_glyphs.push_back(Glyph{ RECT{ left, y + D(1), left + D(16), y + D(17) },
                                      state == StepState::Failed ? GlyphKind::Critical : GlyphKind::Caution });
            int bottom = AddLabel(left + D(28), y, width - D(28), m_plan->steps[i].title, FontRole::BodyStrong, m_palette.text);
            if (!m_stepNotes[i].empty())
            {
                bottom = AddLabel(left + D(28), bottom + D(2), width - D(28), m_stepNotes[i], FontRole::Caption, m_palette.secondary);
            }
            y = (std::max)(bottom, y + D(20)) + D(12);
        }

        if (!removing && copied)
        {
            const int bottom = ClientHeight() - D(kFooterDip) - D(24);
            Control launch{ IdLaunch, ControlType::CheckInline, RECT{ left, bottom - D(24), left + width, bottom }, L"Open Calculator now" };
            launch.checked = m_launchAfter;
            m_controls.push_back(launch);
        }

        AddFooter({ Control{ IdFinish, ControlType::AccentButton, {}, L"Finish" } });
    }

    void Wizard::LayoutNothingToRemove()
    {
        const int left = D(kMarginDip);
        const int width = ClientWidth() - 2 * left;
        int y = D(kMarginDip);
        m_glyphs.push_back(Glyph{ RECT{ left, y, left + D(48), y + D(48) }, GlyphKind::AppIcon });
        y += D(48) + D(20);
        y = AddLabel(left, y, width, L"Calculator isn't installed", FontRole::Subtitle, m_palette.text) + D(4);
        AddLabel(left, y, width, L"There's nothing to remove.", FontRole::Body, m_palette.secondary);
        AddFooter({ Control{ IdFinish, ControlType::AccentButton, {}, L"Close" } });
    }

    void Wizard::SetPage(Page page)
    {
        m_page = page;
        m_hot = IdNone;
        m_pressed = IdNone;
        m_focus = -1;
        Relayout();
        m_transitionStart = GetTickCount64();
        StartAnimation();
    }

    // ------------------------------------------------------------ running

    struct RunContext
    {
        HWND hwnd;
        Setup::Plan* plan;
    };

    DWORD WINAPI Wizard::Worker(void* param)
    {
        std::unique_ptr<RunContext> context(static_cast<RunContext*>(param));
        // Shell links and the UAC prompt both want COM on the calling thread.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const auto& steps = context->plan->steps;
        for (size_t i = 0; i < steps.size(); ++i)
        {
            PostMessageW(context->hwnd, WM_APP_STEP_BEGIN, i, 0);
            const ULONGLONG started = GetTickCount64();
            auto* result = new Setup::StepResult(steps[i].run());
            const ULONGLONG elapsed = GetTickCount64() - started;
            if (elapsed < kMinimumStepMs)
            {
                Sleep(static_cast<DWORD>(kMinimumStepMs - elapsed));
            }
            PostMessageW(context->hwnd, WM_APP_STEP_END, i, reinterpret_cast<LPARAM>(result));

            // An install that could not even copy its files stops there; the
            // rest would only point Windows at something missing.
            if (!context->plan->removing && i == 0 && result->outcome == Outcome::Failed)
            {
                break;
            }
        }
        CoUninitialize();
        PostMessageW(context->hwnd, WM_APP_RUN_DONE, 0, 0);
        return 0;
    }

    void Wizard::StartRun(std::unique_ptr<Setup::Plan> plan)
    {
        m_plan = std::move(plan);
        m_stepStates.assign(m_plan->steps.size(), StepState::Pending);
        m_stepNotes.assign(m_plan->steps.size(), std::wstring());
        m_completed = 0;
        m_progressShown = 0.0f;
        m_running = true;
        SetPage(Page::Progress);

        auto* context = new RunContext{ m_hwnd, m_plan.get() };
        HANDLE thread = CreateThread(nullptr, 0, Worker, context, 0, nullptr);
        if (thread == nullptr)
        {
            delete context;
            m_running = false;
            SetPage(Page::Finished);
            return;
        }
        CloseHandle(thread);
    }

    void Wizard::OnRunDone()
    {
        m_running = false;
        m_installed = Setup::QueryInstalled();
        if (m_installed.present)
        {
            m_options = m_installed.options;
        }
        SetPage(Page::Finished);
    }

    // ------------------------------------------------------------- input

    int Wizard::HitTest(POINT pt) const
    {
        for (const Control& control : m_controls)
        {
            if (control.enabled && PtInRect(&control.rc, pt))
            {
                return control.id;
            }
        }
        return IdNone;
    }

    int Wizard::DefaultButton() const
    {
        for (const Control& control : m_controls)
        {
            if (control.type == ControlType::AccentButton && control.enabled)
            {
                return control.id;
            }
        }
        return IdNone;
    }

    void Wizard::MoveFocus(int direction)
    {
        const int count = static_cast<int>(m_controls.size());
        if (count == 0)
        {
            return;
        }
        int index = m_focus;
        for (int tries = 0; tries < count; ++tries)
        {
            index = (index < 0) ? (direction > 0 ? 0 : count - 1) : (index + direction + count) % count;
            if (m_controls[static_cast<size_t>(index)].enabled)
            {
                m_focus = index;
                break;
            }
        }
        m_showFocus = true;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void Wizard::Activate(int id)
    {
        using namespace Setup;
        switch (id)
        {
        case IdCancel:
            if (!m_running)
            {
                DestroyWindow(m_hwnd);
            }
            break;
        case IdNext:
            if (m_page == Page::Welcome)
            {
                m_optionsBack = Page::Welcome;
                SetPage(Page::Options);
            }
            else if (m_page == Page::Maintenance && m_modeRemove)
            {
                m_confirmFromMaintenance = true;
                SetPage(Page::Confirm);
            }
            else if (m_page == Page::Maintenance)
            {
                m_optionsBack = Page::Maintenance;
                SetPage(Page::Options);
            }
            break;
        case IdBack:
            SetPage(m_page == Page::Options ? m_optionsBack : Page::Maintenance);
            break;
        case IdInstall:
            StartRun(PlanInstall(m_options, m_hwnd));
            break;
        case IdRemove:
            StartRun(PlanUninstall(m_hwnd));
            break;
        case IdFinish:
            if (m_page == Page::Finished && m_launchAfter && m_plan && !m_plan->removing
                && !m_stepStates.empty() && m_stepStates.front() == StepState::Done)
            {
                Launch(m_plan->targetExe);
            }
            DestroyWindow(m_hwnd);
            break;
        case IdOptRedirect: m_options ^= OptionRedirectCalc; Relayout(); break;
        case IdOptKey: m_options ^= OptionCalculatorKey; Relayout(); break;
        case IdOptStart: m_options ^= OptionStartMenu; Relayout(); break;
        case IdOptStore: m_options ^= OptionReplaceStore; Relayout(); break;
        case IdModeChange: m_modeRemove = false; Relayout(); break;
        case IdModeRemove: m_modeRemove = true; Relayout(); break;
        case IdLaunch: m_launchAfter = !m_launchAfter; Relayout(); break;
        }
    }

    // --------------------------------------------------------- animation

    bool Wizard::Animating() const
    {
        const bool transitioning = GetTickCount64() - m_transitionStart < static_cast<ULONGLONG>(kTransitionMs);
        const float target = m_plan && !m_plan->steps.empty() ? static_cast<float>(m_completed) / m_plan->steps.size() : 0.0f;
        const bool progressMoving = m_page == Page::Progress && (target - m_progressShown) > 0.001f;
        return transitioning || m_running || progressMoving;
    }

    void Wizard::StartAnimation()
    {
        if (!m_timerRunning)
        {
            SetTimer(m_hwnd, kAnimationTimer, 15, nullptr);
            m_timerRunning = true;
        }
    }

    // ------------------------------------------------------------ painting

    void Wizard::PaintText(HDC hdc, const RECT& rc, const std::wstring& text, FontRole font, COLORREF color, UINT flags) const
    {
        SelectObject(hdc, m_fonts[static_cast<int>(font)]);
        SetTextColor(hdc, color);
        RECT r = rc;
        DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r, flags);
    }

    void Wizard::PaintControl(HDC hdc, Gdiplus::Graphics& g, const Control& c, bool hot, bool pressed)
    {
        const Palette& p = m_palette;
        const float radius = static_cast<float>(D(4));

        switch (c.type)
        {
        case ControlType::Button:
        case ControlType::AccentButton:
        {
            const bool accent = c.type == ControlType::AccentButton;
            COLORREF fill;
            COLORREF text;
            if (!c.enabled)
            {
                fill = accent ? p.accentDisabled : p.button;
                text = accent ? p.accentDisabledText : p.disabled;
            }
            else if (accent)
            {
                fill = pressed ? p.accentPress : hot ? p.accentHover : p.accent;
                text = p.accentText;
            }
            else
            {
                fill = pressed ? p.buttonPress : hot ? p.buttonHover : p.button;
                text = pressed ? p.secondary : p.text;
            }
            if (accent)
            {
                FillRounded(g, c.rc, radius, fill);
            }
            else
            {
                FillBordered(g, c.rc, radius, fill, p.buttonBorder, (std::max)(1, D(1)));
            }
            PaintText(hdc, c.rc, c.text, FontRole::Body, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            break;
        }
        case ControlType::CheckCard:
        case ControlType::RadioCard:
        {
            const COLORREF fill = pressed ? p.cardPress : hot ? p.cardHover : p.card;
            FillBordered(g, c.rc, radius, fill, p.cardBorder, (std::max)(1, D(1)));

            const RECT box{ c.rc.left + D(16), c.rc.top + D(13), c.rc.left + D(36), c.rc.top + D(33) };
            if (c.type == ControlType::CheckCard)
            {
                if (c.checked)
                {
                    FillRounded(g, box, radius, pressed ? p.accentPress : hot ? p.accentHover : p.accent);
                    StrokeCheck(g, box, p.accentText, D(15) / 10.0f);
                }
                else
                {
                    FillBordered(g, box, radius, p.boxFill, p.boxBorder, (std::max)(1, D(1)));
                }
            }
            else if (c.checked)
            {
                FillCircle(g, box, pressed ? p.accentPress : hot ? p.accentHover : p.accent);
                RECT dot = box;
                const int inset = hot ? D(4) : D(5);
                InflateRect(&dot, -inset, -inset);
                FillCircle(g, dot, p.accentText);
            }
            else
            {
                FillCircle(g, box, p.boxBorder);
                RECT inner = box;
                InflateRect(&inner, -(std::max)(1, D(1)), -(std::max)(1, D(1)));
                FillCircle(g, inner, p.boxFill);
            }

            const int textLeft = c.rc.left + D(52);
            const int textRight = c.rc.right - D(16);
            const int titleHeight = TextHeight(c.text, FontRole::Body, textRight - textLeft);
            RECT title{ textLeft, c.rc.top + D(12), textRight, c.rc.top + D(12) + titleHeight };
            PaintText(hdc, title, c.text, FontRole::Body, p.text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            if (!c.detail.empty())
            {
                RECT detail{ textLeft, title.bottom + D(2), textRight, c.rc.bottom };
                PaintText(hdc, detail, c.detail, FontRole::Caption, p.secondary, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            }
            break;
        }
        case ControlType::CheckInline:
        {
            const int top = c.rc.top + ((c.rc.bottom - c.rc.top) - D(20)) / 2;
            const RECT box{ c.rc.left, top, c.rc.left + D(20), top + D(20) };
            if (c.checked)
            {
                FillRounded(g, box, radius, pressed ? p.accentPress : hot ? p.accentHover : p.accent);
                StrokeCheck(g, box, p.accentText, D(15) / 10.0f);
            }
            else
            {
                FillBordered(g, box, radius, hot ? p.cardHover : p.boxFill, p.boxBorder, (std::max)(1, D(1)));
            }
            RECT text{ box.right + D(12), c.rc.top, c.rc.right, c.rc.bottom };
            PaintText(hdc, text, c.text, FontRole::Body, p.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            break;
        }
        }
    }

    void Wizard::PaintGlyph(HDC hdc, Gdiplus::Graphics& g, const Glyph& glyph)
    {
        const Palette& p = m_palette;
        const RECT& rc = glyph.rc;
        const float size = static_cast<float>(rc.right - rc.left);
        const float stroke = (std::max)(1.5f, size / 11.0f);

        auto signal = [&](COLORREF fill, int mark) {
            FillCircle(g, rc, fill);
            if (mark == 0)
            {
                StrokeCheck(g, rc, p.onSignal, stroke);
            }
            else if (mark == 1)
            {
                StrokeExclamation(g, rc, p.onSignal, stroke);
            }
            else
            {
                StrokeCross(g, rc, p.onSignal, stroke);
            }
        };

        switch (glyph.kind)
        {
        case GlyphKind::AppIcon:
            if (m_icon != nullptr)
            {
                DrawIconEx(hdc, rc.left, rc.top, m_icon, rc.right - rc.left, rc.bottom - rc.top, 0, nullptr, DI_NORMAL);
            }
            break;
        case GlyphKind::FeatureCheck:
            StrokeCheck(g, rc, p.accent, (std::max)(1.5f, D(18) / 10.0f));
            break;
        case GlyphKind::Success: signal(p.success, 0); break;
        case GlyphKind::Caution: signal(p.caution, 1); break;
        case GlyphKind::Critical: signal(p.critical, 2); break;
        case GlyphKind::Step:
        {
            const StepState state = m_stepStates[static_cast<size_t>(glyph.step)];
            if (state == StepState::Done)
            {
                signal(p.success, 0);
            }
            else if (state == StepState::Skipped)
            {
                signal(p.caution, 1);
            }
            else if (state == StepState::Failed)
            {
                signal(p.critical, 2);
            }
            else if (state == StepState::Active)
            {
                // The indeterminate ring: an arc going round once a second.
                const float angle = static_cast<float>(GetTickCount64() % 1000) * 0.36f;
                Gdiplus::Pen pen(ToGp(p.accent), stroke);
                pen.SetStartCap(Gdiplus::LineCapRound);
                pen.SetEndCap(Gdiplus::LineCapRound);
                const float inset = stroke / 2.0f + 0.5f;
                g.DrawArc(&pen, rc.left + inset, rc.top + inset, size - 2 * inset, size - 2 * inset, angle, 110.0f);
            }
            else
            {
                Gdiplus::Pen pen(ToGp(p.secondary), (std::max)(1.0f, D(12) / 10.0f));
                const float inset = 1.5f;
                g.DrawEllipse(&pen, rc.left + inset, rc.top + inset, size - 2 * inset, size - 2 * inset);
            }
            break;
        }
        }
    }

    void Wizard::PaintProgress(Gdiplus::Graphics& g)
    {
        if (m_progressRect.right <= m_progressRect.left)
        {
            return;
        }
        const Palette& p = m_palette;
        // WinUI's bar: a hairline track with a thicker rounded fill over it.
        const int mid = (m_progressRect.top + m_progressRect.bottom) / 2;
        RECT track{ m_progressRect.left, mid, m_progressRect.right, mid + (std::max)(1, D(1)) };
        FillRounded(g, track, 0.5f, p.secondary, 110);
        const int width = static_cast<int>((m_progressRect.right - m_progressRect.left) * (std::min)(1.0f, m_progressShown));
        if (width > 0)
        {
            RECT bar{ m_progressRect.left, mid - D(3) / 2, m_progressRect.left + (std::max)(width, D(3)), mid - D(3) / 2 + D(3) };
            FillRounded(g, bar, static_cast<float>(D(3)) / 2.0f, p.accent);
        }
    }

    void Wizard::PaintScene(HDC hdc)
    {
        const Palette& p = m_palette;
        RECT client{ 0, 0, ClientWidth(), ClientHeight() };
        HBRUSH page = CreateSolidBrush(p.page);
        FillRect(hdc, &client, page);
        DeleteObject(page);

        RECT footer{ 0, ClientHeight() - D(kFooterDip), ClientWidth(), ClientHeight() };
        HBRUSH footerBrush = CreateSolidBrush(p.footer);
        FillRect(hdc, &footer, footerBrush);
        DeleteObject(footerBrush);
        RECT rule{ 0, footer.top, ClientWidth(), footer.top + (std::max)(1, D(1)) };
        HBRUSH ruleBrush = CreateSolidBrush(p.divider);
        FillRect(hdc, &rule, ruleBrush);
        DeleteObject(ruleBrush);

        SetBkMode(hdc, TRANSPARENT);
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        for (const Label& label : m_labels)
        {
            PaintText(hdc, label.rc, label.text, label.font, label.color, label.flags);
        }
        for (const Glyph& glyph : m_glyphs)
        {
            PaintGlyph(hdc, g, glyph);
        }
        PaintProgress(g);
        for (size_t i = 0; i < m_controls.size(); ++i)
        {
            const Control& c = m_controls[i];
            PaintControl(hdc, g, c, c.enabled && m_hot == c.id && (m_pressed == IdNone || m_pressed == c.id),
                         c.enabled && m_pressed == c.id && m_hot == c.id);
            if (m_showFocus && static_cast<int>(i) == m_focus)
            {
                RECT ring = c.rc;
                InflateRect(&ring, D(3), D(3));
                StrokeRounded(g, ring, static_cast<float>(D(7)), p.text, static_cast<float>((std::max)(2, D(2))));
            }
        }
    }

    void Wizard::Paint(HDC target)
    {
        const int width = ClientWidth();
        const int height = ClientHeight();
        if (m_back == nullptr || m_backSize.cx != width || m_backSize.cy != height)
        {
            if (m_back) DeleteObject(m_back);
            if (m_layer) DeleteObject(m_layer);
            m_back = CreateCompatibleBitmap(target, width, height);
            m_layer = CreateCompatibleBitmap(target, width, height);
            m_backSize = SIZE{ width, height };
        }
        HDC back = CreateCompatibleDC(target);
        HGDIOBJ previousBack = SelectObject(back, m_back);

        const ULONGLONG elapsed = GetTickCount64() - m_transitionStart;
        if (elapsed >= static_cast<ULONGLONG>(kTransitionMs) || m_layer == nullptr)
        {
            PaintScene(back);
        }
        else
        {
            // A new page rises a few pixels as it fades in over the empty
            // page; the footer stays put, since it is the same chrome.
            const float t = static_cast<float>(elapsed) / kTransitionMs;
            const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
            HDC layer = CreateCompatibleDC(target);
            HGDIOBJ previousLayer = SelectObject(layer, m_layer);
            PaintScene(layer);

            HBRUSH page = CreateSolidBrush(m_palette.page);
            RECT content = m_contentRect;
            FillRect(back, &content, page);
            DeleteObject(page);
            const int footerTop = height - D(kFooterDip);
            BitBlt(back, 0, footerTop, width, height - footerTop, layer, 0, footerTop, SRCCOPY);

            const int rise = static_cast<int>(D(8) * (1.0f - eased));
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, static_cast<BYTE>(255.0f * eased), 0 };
            AlphaBlend(back, 0, rise, width, footerTop - rise, layer, 0, 0, width, footerTop - rise, blend);
            SelectObject(layer, previousLayer);
            DeleteDC(layer);
        }

        BitBlt(target, 0, 0, width, height, back, 0, 0, SRCCOPY);
        SelectObject(back, previousBack);
        DeleteDC(back);
    }

    // ------------------------------------------------------------ messages

    LRESULT Wizard::Handle(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            m_hwnd = hwnd;
            m_dpi = static_cast<int>(GetDpiForWindow(hwnd));
            LoadResources();
            Relayout();
            m_transitionStart = GetTickCount64();
            StartAnimation();
            return 0;

        case WM_DPICHANGED:
        {
            m_dpi = HIWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            LoadResources();
            Relayout();
            return 0;
        }

        case WM_SETTINGCHANGE:
            if (lParam != 0 && lstrcmpiW(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0)
            {
                LoadResources();
                Relayout();
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            Paint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kFinishTimer)
            {
                KillTimer(hwnd, kFinishTimer);
                OnRunDone();
                return 0;
            }
            if (wParam == kAnimationTimer)
            {
                if (m_plan && !m_plan->steps.empty())
                {
                    const float target = static_cast<float>(m_completed) / m_plan->steps.size();
                    m_progressShown += (target - m_progressShown) * 0.18f;
                    if (target - m_progressShown < 0.001f)
                    {
                        m_progressShown = target;
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                if (!Animating())
                {
                    KillTimer(hwnd, kAnimationTimer);
                    m_timerRunning = false;
                }
            }
            return 0;

        case WM_MOUSEMOVE:
        {
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&track);
            const int hot = HitTest(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            if (hot != m_hot)
            {
                m_hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (m_hot != IdNone)
            {
                m_hot = IdNone;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            m_pressed = HitTest(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            m_hot = m_pressed;
            m_showFocus = false;
            if (m_pressed != IdNone)
            {
                SetCapture(hwnd);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONUP:
        {
            const int pressed = m_pressed;
            m_pressed = IdNone;
            if (GetCapture() == hwnd)
            {
                ReleaseCapture();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            if (pressed != IdNone && pressed == HitTest(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }))
            {
                Activate(pressed);
            }
            return 0;
        }

        case WM_KEYDOWN:
            switch (wParam)
            {
            case VK_TAB:
                MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
                return 0;
            case VK_SPACE:
                if (m_focus >= 0 && m_focus < static_cast<int>(m_controls.size()))
                {
                    Activate(m_controls[static_cast<size_t>(m_focus)].id);
                }
                return 0;
            case VK_RETURN:
            {
                // Enter presses a focused button; anywhere else it is the
                // primary action, as it would be in any Windows dialog.
                const Control* focused = (m_focus >= 0 && m_focus < static_cast<int>(m_controls.size())) ? &m_controls[static_cast<size_t>(m_focus)] : nullptr;
                const bool focusedButton = focused && m_showFocus
                    && (focused->type == ControlType::Button || focused->type == ControlType::AccentButton);
                Activate(focusedButton ? focused->id : DefaultButton());
                return 0;
            }
            case VK_ESCAPE:
                if (!m_running)
                {
                    DestroyWindow(hwnd);
                }
                return 0;
            case VK_UP:
            case VK_DOWN:
                if (m_page == Page::Maintenance)
                {
                    m_modeRemove = (wParam == VK_DOWN);
                    Relayout();
                }
                return 0;
            }
            break;

        case WM_APP_STEP_BEGIN:
            if (wParam < m_stepStates.size())
            {
                m_stepStates[wParam] = StepState::Active;
                Relayout();
            }
            return 0;

        case WM_APP_STEP_END:
        {
            std::unique_ptr<Setup::StepResult> result(reinterpret_cast<Setup::StepResult*>(lParam));
            if (wParam < m_stepStates.size())
            {
                m_stepStates[wParam] = result->outcome == Outcome::Done ? StepState::Done
                    : result->outcome == Outcome::Skipped                ? StepState::Skipped
                                                                         : StepState::Failed;
                m_stepNotes[wParam] = result->note;
                ++m_completed;
                Relayout();
                StartAnimation();
            }
            return 0;
        }

        case WM_APP_RUN_DONE:
            // Hold on the finished checklist for a moment before moving on.
            SetTimer(hwnd, kFinishTimer, 500, nullptr);
            return 0;

        case WM_CLOSE:
            if (m_running)
            {
                MessageBeep(MB_ICONWARNING);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (m_plan)
            {
                Setup::ScheduleRemoval(*m_plan);
            }
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // The elevated half: no window, just the one registry change and an exit
    // code for the half that asked.
    if (argv != nullptr && argc >= 4 && lstrcmpiW(argv[1], L"/ifeo") == 0)
    {
        const int code = Setup::RunElevatedIfeo(argv[2], argv[3]);
        LocalFree(argv);
        return code;
    }
    const bool uninstall = argv != nullptr && argc >= 2 && lstrcmpiW(argv[1], L"/uninstall") == 0;
    if (argv != nullptr)
    {
        LocalFree(argv);
    }

    // One wizard at a time: a second launch brings the first one forward.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\CompactCalculatorSetup");
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND existing = FindWindowW(kWindowClass, nullptr))
        {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    g_wizard.Create(instance, uninstall);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    if (mutex != nullptr)
    {
        CloseHandle(mutex);
    }
    return 0;
}
