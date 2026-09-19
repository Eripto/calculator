# Calculator (compact build)

A native Win32 front end for the Windows Calculator engine in this repository,
built as a single self-contained `Calculator.exe` of **under 350 KB**.

The arithmetic and the unit conversions are not reimplementations:
`src/CalcManager` — the same engine the shipping app uses, including the Ratpack
arbitrary-precision core and the `UnitConverter` — is compiled in as-is, and the
converter is fed the unit tables generated from the shipping app's own data
loader. Results, precision, operator precedence, radix conversion, conversion
factors, history and memory therefore behave as they do in the packaged app.
What this build replaces is the presentation layer: XAML, WinUI, the
.NET/C++ ViewModels and the MSIX packaging are traded for direct GDI/GDI+
drawing, which is where the size saving comes from.

## What's included

| Area | Included |
| --- | --- |
| Standard | Full keypad, `%`, `1/x`, `x²`, `²√x`, `+/−` |
| Scientific | Full keypad, `2ⁿᵈ` inverse toggle, DEG/RAD/GRAD, `F-E`, trig (including the hyperbolic functions) and function flyouts, parentheses with open-paren counter |
| Graphing | Plots y = f(x): multiple equations in the shipping colour palette, pan, zoom and reset, a graphing keypad, and the trig and function flyouts writing into the equation |
| Programmer | HEX/DEC/OCT/BIN readouts, radix switching, QWORD/DWORD/WORD/BYTE, bitwise and bit-shift flyouts, 64-bit bit-flip board, A–F keys with radix-aware enabling |
| Date Calculation | Difference between dates, and add/subtract years-months-days, with a calendar flyout on each date field |
| Converter | All 12 unit categories — Volume, Length, Weight and mass, Temperature, Energy, Area, Speed, Time, Power, Data, Pressure, Angle — 158 units, the whimsical units, and the "About equal to" suggestions |
| Memory | `MC MR M+ M- MS` strip plus the multi-slot memory list with per-slot commands |
| History | Panel with newest-first entries; clicking one restores it by replaying its stored commands |
| Chrome | Navigation pane with both category groups, Always on top, Settings/About, light/dark theme following the system setting, system accent colour, per-monitor DPI v2, dark title bar, keyboard shortcuts |
| Motion | Sliding navigation pane and settings page, fading flyouts, page-transition on mode change, history/memory panel that slides and cross-fades both ways, a 450ms theme cross-fade, and Fluent hover and pointer-down states on every key |

### Not included, and why

- **Graphing's solver.** `src/GraphingImpl` contains only `MockGraphingImpl`,
  a stub whose `MathSolver` returns an empty graph; the real equation solver and
  renderer are a closed-source component Microsoft ships separately. So there
  was nothing to port, and the parsing, evaluation and plotting here are written
  rather than ported — see **Graphing** below. What that costs is the analysis
  the real solver backs: the key graph features panel (zeroes, extrema,
  asymptotes, period), implicit relations such as `x² + y² = 9`, and
  inequalities. What is here plots explicit functions of x.
- **Currency.** Also not functional in this repository:
  `Calculator.ViewModels/DataLoaders/CurrencyHttpClient.cs` states that the
  upstream rate endpoints are dead and substitutes placeholder data (fictional
  planet currencies). Live rates need Microsoft's own service, so the category
  is omitted rather than shipped with joke data.

## Motion

Timings and curves follow the WinUI motion guidance the shipping app is built
on: 167ms for a small state change, 250ms for a surface moving onto the screen,
and a decelerating curve (approximating `cubic-bezier(0, 0, 0, 1)`) for anything
entering.

| Where | What happens |
| --- | --- |
| Navigation pane, Settings | Slides in from the left over a scrim, and back out |
| Mode change | New content fades in while rising into place |
| Dropdown flyouts | Fade in and lift |
| History / memory panel | Slides in from the right when docked, upward when it covers the keypad, and back out the same way when dismissed |
| Every key | Hover cross-fades between keys; pointer-down shrinks the key slightly and fades the pressed fill in |

Frames are paced against the compositor with `DwmFlush` from the message loop,
not by `WM_TIMER`. A timer's 15.6ms granularity, low priority and coalescing are
what make timer-driven motion look like it stutters; the loop goes back to
blocking on `GetMessage` as soon as everything settles, so an idle window costs
nothing.

`DwmFlush` is not enough on its own. It is meant to block until the next
composition pass, but it returns immediately when the window is occluded, when
composition is off, and on some drivers under load, and the loop then repaints
as fast as the machine allows. A 16ms floor sits in front of it, which is all
the compositor can show anyway; without it, switching modes quickly pegged a
core.

Two further implementation notes. Animated values are computed from the clock on demand
rather than stepped, so a dropped frame never leaves an animation stranded
part-way. And because GDI text has no alpha of its own, the transitions that
need a true fade render to an offscreen layer and `AlphaBlend` it, rather than
faking it with colour interpolation. Three do: the mode change and flyouts
render the moving surface into the layer, while the panel and the theme change
take a copy of the frame *before* the change and blend that back over the
finished frame -- the panel is drawn by three separate passes and the theme
touches every pixel in the window, so in both cases the old frame is the
cheaper thing to hold. The frame timer only runs while something is moving.

## Visual fidelity

The layout was checked side by side against screenshots of the shipping app and
corrected where it diverged:

| Where | What changed |
| --- | --- |
| Keypad | 6px between keys and the same margin around the grid, instead of running the keys to the window edge; 4px corner radius |
| Standard, Scientific | Mathematical variables set in italic — `x²`, `¹/ₓ`, `²√x`, `xʸ`, `10ˣ`, `n!`, `|x|` — while operators and named functions (`exp`, `mod`, `log`, `ln`) stay upright |
| Scientific | DEG and F-E moved above the memory strip; the Trigonometry and Function dropdowns below it. The separate `hyp` toggle is gone, its functions folded into the 12-item Trigonometry flyout, as the shipping app does it |
| Programmer | HEX/DEC/OCT/BIN left-aligned with an accent bar marking the active radix, rather than a filled toggle; `«` / `»` for the shifts; "Bit shift" in sentence case |
| Date Calculation | One field per date reading "September 18, 2026" with a calendar flyout, replacing three month/day/year spinners |
| Settings | Rebuilt as cards under "Appearance" and "About" section headers, replacing a flat list of rows |
| Combo boxes | Unit pickers and the date-mode picker anchor their text to the leading edge with the chevron at the trailing one, instead of centring both |
| Glyphs | `M−` uses a real minus sign (U+2212), not a hyphen |
| Title bar | Hamburger, mode name in `SubtitleTextBlockStyle` (20px semibold), then the keep-on-top button immediately after the title, as `MainPage.xaml` lays it out. History is the only trailing item |
| Navigation pane | `SplitViewOpenPaneLength` (256px) rather than the full window, so the keypad stays visible beside it; WinUI selection indicator (a 3x16 accent bar on the leading edge) and a scrollbar |
| Settings | The app theme is a `SettingsExpander` holding Light / Dark / Use system setting radio buttons, so clicking the card expands it instead of cycling the theme. About expands to the licence links, and the page closes with the "Send feedback" link and the contribute paragraph |
| Chrome icons | Drawn as paths rather than font glyphs (see below) |

### Why the icons are vector paths

The hamburger, back arrow, history, keep-on-top, backspace, chevrons, calendar,
gear, radio buttons and the two settings header icons are drawn with GDI+ paths
on a 16x16 grid, the same grid the icon fonts are designed on.

Font glyphs were the obvious choice and the wrong one. `Segoe Fluent Icons` and
`Segoe MDL2 Assets` do not carry every codepoint on every machine, and the
text-presentation characters the code fell back on can be substituted by a
colour emoji font -- which puts blue and orange into a title bar that is
supposed to be monochrome. Paths render identically everywhere and cannot be
recoloured by font substitution. The navigation pane's category icons went the same way for the
same reason: several of their codepoints -- Programmer, Volume, Weight and
mass, Temperature, Speed, Data among them -- came up as empty boxes on a real
machine even though the font was present.


## Building

Needs `mingw-w64` on Linux, or run it under MSYS2/WSL on Windows:

```sh
sudo apt-get install mingw-w64
./build.sh                      # -> out/Calculator.exe (x86-64)
./build.sh --arch i686          # 32-bit (note: larger, see below)
```

`build.sh` fails if the binary exceeds the size budget (350 KB by default,
override with `CALC_SIZE_BUDGET_KB`).

## Tests

`CalcManager` and the front-end models are portable C++, so the tests build and
run with the host compiler — no Windows and no cross-compiler required:

```sh
./tests/run_tests.sh
```

175 checks covering arithmetic, scientific functions, programmer radices,
memory, history, unit conversions across every category, date arithmetic,
expression parsing for the graphing mode, and error handling. Two of them are differential tests against a reference
implementation rather than fixed expectations:

- the decimal parser that replaced `std::wregex` is checked against that regex
  over ~75,000 generated inputs;
- the exact fixed-point formatter is checked against the C library's `%.*f`
  over 36,000 values.

## How the size budget was met

The first working build was 1292 KB, and nearly all of the excess was C++
runtime pulled in by a handful of references rather than by the calculator.

| Change | Saved |
| --- | --- |
| Dropped `std::wregex` from `scidisp.cpp` for a hand-written parser of the same grammar | ~620 KB (regex + iostream + locale) |
| Made `<iostream>` in `Ratpack/support.cpp` conditional — it was only there for the `GEN_CONST` constant-dump helpers | included above |
| Replaced `std::__throw_*` and the global `operator new`/`delete`, which pulled `std::logic_error` → narrow `std::string` → `std::random_device` | ~63 KB |
| Routed integer, double and parse formatting through `CalcEngine::NumericString` (CRT imports) instead of `std::to_wstring`, `std::stod` and stream manipulators | ~37 KB |
| Replaced libstdc++'s verbose terminate handler (drops the symbol demangler) | ~48 KB |
| Operator-name table: `unordered_map` of `std::wstring` → flat `constexpr` array | ~15 KB |
| Packed the generated and engine string tables to plain pointers, widest field first | ~9 KB |
| PNG-compressed icon sizes above 16px | ~10 KB |
| `-Os`, `-fno-rtti`, `--gc-sections`, `--strip-all` | — |

The engine edits are behaviour-preserving. The decimal parser is the only one
with real substance, and it is verified against the original expression; the
rest are dead-code guards, container and field-layout swaps, and formatting
calls with identical output.

### A second pass, after the graphing mode

Adding graphing pushed the binary to 399 KB. A linker map (`-Wl,-Map`) put it
back under 350 KB without changing a single drawn pixel:

| Change | Saved |
| --- | --- |
| Link-time optimisation, which ICEd on the first attempt but works with `-flto-partition=none` | ~52 KB |
| Force-including a prelude that sets `_GLIBCXX_EXTERN_TEMPLATE` to -1, so `std::wstring`'s members are emitted locally and `--gc-sections` can drop the unused ones instead of linking all of libstdc++'s `wstring-inst.o` | ~22 KB |
| `GetLocalTime` in place of `time` + `localtime_s`, and `RtlGenRandom` — which is what `rand_s` calls — in place of `rand_s`, dropping both CRT shims and the secure-parameter handler behind them | ~10 KB linked, most of it absorbed by PE section alignment |
| `--disable-runtime-pseudo-reloc`; nothing here relies on auto-import | — |

409,088 bytes to 327,680. Every mode was captured before and after, including
the arithmetic and divide-by-zero paths, and compared channel by channel: zero
differing pixels across all eight.

Two things were measured and then rejected rather than kept. Replacing libm's
`pow`, `cbrt` and inverse hyperbolics with identities built from `log`, `exp`
and `sqrt` saves about 13 KB, but it moves results by a few ulp; LTO alone made
the target, so precision stayed exactly as it was. And `-fno-exceptions` on the
UI translation units is not available at all — `main.cpp` catches what the
engine throws.

Three things that did *not* work, for the record: LTO ICEs in GCC 13's
mingw-w64 (`binds_to_current_def_p`); `-fno-asynchronous-unwind-tables` makes
GCC fall back to SjLj exceptions, which mingw does not provide; and a 32-bit
build is larger, because its DWARF exception tables outweigh the smaller
pointers.

## Graphing

The shipping app hands each equation to `Graphing::IMathSolver`, which this
repository does not contain. What it does contain, and what this mode follows,
is the shape of the feature: the function set in `GraphingNumPad.xaml`, the
fourteen equation colours in `App.xaml` (both theme variants), and the
`NavCategory.cs` placement, glyph and keyboard shortcut.

Expressions compile to a flat postfix program rather than a node tree — no
per-node allocation and a good deal less code — and evaluate over doubles with
a fixed stack. The grammar covers implicit multiplication (`2x`, `3sin(x)`,
`(x+1)(x-1)`), right-associative `^`, absolute-value bars, unparenthesised
function arguments, the superscript forms the keypads produce (`x²`, `sin⁻¹`),
and `π` and `e`. A leading `y=` is stripped. Anything that does not parse
leaves the curve undrawn rather than guessing at what was meant.

Undefined points come back as NaN and break the curve, so `sqrt(x)` simply
stops at the origin; a jump larger than four times the view height breaks it
too, which is what keeps the two branches of `1/x` from being joined by a line
through the asymptote. Curves are sampled once per pixel column.

The mode follows `GraphingCalculator.xaml`'s shape: a two-tab toggle in the
title bar switches between the plot and the equation editor rather than
stacking both. The plot fills its view, with the axes tipped by arrowheads and
labelled with an italic `x` and `y` and a single `0` at the origin -- no
numeric ticks, as the shipping graph has none. Trace, share and graph options
sit on a card in the top trailing corner, zoom and recentre on another in the
bottom one. The editor carries the `f` badge and expression field, the
Trigonometry / Inequalities / Function dropdowns, and the same five-by-seven
keypad the shipping app uses, down to `x`, `y`, `(−)` and the return key.

Graph options is the shipping pane: the four window bounds, which can be typed
into, a reset link, the angle units, the line thickness and the graph theme.
The settings are real rather than decorative -- the units reach the evaluator,
so a sine is drawn in degrees when degrees are chosen, and the thickness
reaches the pen.

Pan by dragging the plot, zoom with the wheel or the buttons in the corner. A
pan only begins when the plot is frontmost: the plot rect covers nearly the
whole window, so without that check a click on a navigation item over the top
of it began a pan instead, and the button-up that should have switched modes
was swallowed.

Share copies the plot. The shipping app hands it to the Windows share sheet,
which is a WinRT surface a desktop process cannot reach, so the clipboard
carries the same two payloads to the same places: the plot as a bitmap for
anything that takes an image, and the equations as text for anything that does
not. A short confirmation appears over the plot and fades.

Trace is laid out but inert: it needs the analysis the closed-source solver
provides. Inequalities insert their symbol, which the parser does not accept,
so such an equation shows as not plotted.

The whole mode costs 43 KB of the binary.

## Memory

The window's off-screen buffers dominate the footprint, and they used to be
rebuilt constantly. Every `WM_PAINT` created and destroyed a full-window bitmap,
and a frame that was compositing a transition or a flyout fade created a second
and sometimes a third. At 360x620x32bpp each one is 892KB.

They are now retained in a small `Surface` wrapper, recreated only when the
client area changes size and shared between the two transitions that composite
through `AlphaBlend` (they never overlap inside a frame). The scratch layer is
released as soon as a transition ends, and both go when the window is minimised
or closed.

The back buffer deliberately stays put while the window is on screen. An
earlier version released it on a two-second idle timer and emptied the working
set along with it, which turned out to be worse than doing nothing: the trim
dropped the working set, the very next paint reallocated the buffer and faulted
every page back in, and two seconds later it repeated. Task Manager showed that
as memory jumping about, and the page faults made each paint slower. Holding
one buffer steady costs 892KB and stays flat.

Counted inside the app over an identical scripted session -- three cycles of
Standard, Scientific, Programmer, Standard:

| | Bitmap allocations | Bitmap memory requested |
| --- | --- | --- |
| Before | 779 | 663 MB |
| After | 19 | 16 MB |

Alongside that, GDI+ is started with `SuppressBackgroundThread` and its
notification hook pumped by hand, which drops a thread and its stack.

Leaving a mode hands its state back before the next one is built. The converter
engine, which holds the current category's unit list and suggestion set, is
dropped; the graphing mode's compiled programs go while the equation text
stays, so the equations come back as they were and recompile on their next
paint; and the layout vectors, which had grown to whatever the largest mode
needed, are shrunk rather than carried into a mode that needs fewer buttons.

One leak sat underneath all of this, in `Ratpack`. `ChangeConstants` reloads
the precomputed constants through `READRAWRAT`, which called `createrat`
without destroying what the pointer already held -- so every mode switch
orphaned 32 rationals and their numerators and denominators, around 15KB a
time, growing without bound for as long as the app ran. `DUPRAT` a few lines
above it does the destroy first; `READRAWRAT` now does too. Measured against
the engine directly, 2000 mode switches went from +29MB to +0KB, and
LeakSanitizer reports nothing.

None of this changes what is drawn: every mode captured before and after is
pixel-identical, zero differing subpixels.

A word on the target. Under 100KB is not reachable for a Win32 window, and the
arithmetic says so before any profiling does: one back buffer for this window is
892KB on its own, GDI+ maps several MB when it initialises, and a process that
has merely loaded user32, gdi32 and the CRT is past a megabyte before it draws
anything. What was achievable was removing the churn, and that is what the table
above measures.

## Requirements

Windows 10 or 11, x86-64. No installer, no runtime, no DLLs to ship — GDI,
GDI+ and DWM are all part of Windows.
