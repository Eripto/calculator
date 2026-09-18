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
| Scientific | Full keypad, `2ⁿᵈ` inverse toggle, DEG/RAD/GRAD, `hyp`, `F-E`, trig and function flyouts, parentheses with open-paren counter |
| Programmer | HEX/DEC/OCT/BIN readouts, radix switching, QWORD/DWORD/WORD/BYTE, bitwise and bit-shift flyouts, 64-bit bit-flip board, A–F keys with radix-aware enabling |
| Date Calculation | Difference between dates, and add/subtract years-months-days, with month/day/year pickers |
| Converter | All 12 unit categories — Volume, Length, Weight and mass, Temperature, Energy, Area, Speed, Time, Power, Data, Pressure, Angle — 158 units, the whimsical units, and the "About equal to" suggestions |
| Memory | `MC MR M+ M- MS` strip plus the multi-slot memory list with per-slot commands |
| History | Panel with newest-first entries; clicking one restores it by replaying its stored commands |
| Chrome | Navigation pane with both category groups, Always on top, Settings/About, light/dark theme following the system setting, system accent colour, per-monitor DPI v2, dark title bar, keyboard shortcuts |

### Not included, and why

- **Graphing.** It cannot be built from this repository. `src/GraphingImpl`
  contains only `MockGraphingImpl` — a stub whose `MathSolver` returns an empty
  graph. The real equation solver and renderer are a closed-source component
  Microsoft ships separately, so any build from this source has a non-functional
  graphing mode.
- **Currency.** Also not functional in this repository:
  `Calculator.ViewModels/DataLoaders/CurrencyHttpClient.cs` states that the
  upstream rate endpoints are dead and substitutes placeholder data (fictional
  planet currencies). Live rates need Microsoft's own service, so the category
  is omitted rather than shipped with joke data.

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

130 checks covering arithmetic, scientific functions, programmer radices,
memory, history, unit conversions across every category, date arithmetic, and
error handling. Two of them are differential tests against a reference
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

Three things that did *not* work, for the record: LTO ICEs in GCC 13's
mingw-w64 (`binds_to_current_def_p`); `-fno-asynchronous-unwind-tables` makes
GCC fall back to SjLj exceptions, which mingw does not provide; and a 32-bit
build is larger, because its DWARF exception tables outweigh the smaller
pointers.

## Requirements

Windows 10 or 11, x86-64. No installer, no runtime, no DLLs to ship — GDI,
GDI+ and DWM are all part of Windows.
