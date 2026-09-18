# Calculator (compact build)

A native Win32 front end for the Windows Calculator engine in this repository,
built as a single self-contained `Calculator.exe` of **under 300 KB**.

The arithmetic is not a reimplementation: `src/CalcManager` — the same engine
the shipping app uses, including the Ratpack arbitrary-precision core — is
compiled in as-is. Results, precision, operator precedence, radix conversion,
history and memory semantics are therefore identical to the shipping
Calculator. What this build replaces is the presentation layer: XAML, WinUI,
the .NET/C++ ViewModels and the MSIX packaging are traded for direct GDI/GDI+
drawing, which is where the size saving comes from.

## What's included

| Area | Included |
| --- | --- |
| Standard mode | Full keypad, `%`, `1/x`, `x²`, `²√x`, `+/−` |
| Scientific mode | Full keypad, `2ⁿᵈ` inverse toggle, DEG/RAD/GRAD, `hyp`, `F-E`, trig and function flyouts, parentheses with open-paren counter |
| Programmer mode | HEX/DEC/OCT/BIN readouts, radix switching, QWORD/DWORD/WORD/BYTE, bitwise and bit-shift flyouts, 64-bit bit-flip board, A–F keys with radix-aware enabling |
| Memory | `MC MR M+ M- MS` strip plus the multi-slot memory list with per-slot commands |
| History | Panel with newest-first entries; clicking one restores it by replaying its stored commands |
| Chrome | Light/dark theme following the system setting, system accent colour, per-monitor DPI v2, dark title bar, keyboard shortcuts |

**Not included:** the unit/currency converters, graphing mode and date
calculation. Those live in the app layer (`src/Calculator`,
`src/Calculator.ViewModels`, `src/GraphingImpl`) rather than in the engine, and
the converters additionally need network access for currency rates.

## Building

Needs `mingw-w64` on Linux, or run it under MSYS2/WSL on Windows:

```sh
sudo apt-get install mingw-w64
./build.sh                      # -> out/Calculator.exe (x86-64)
./build.sh --arch i686          # 32-bit (note: larger, see below)
```

`build.sh` fails loudly if the binary exceeds the 300 KB budget.

## Tests

`CalcManager` is portable C++, so the engine tests build and run with the host
compiler — no Windows and no cross-compiler required:

```sh
./tests/run_tests.sh
```

The suite covers arithmetic, scientific functions, programmer radices, memory,
history and error handling, and differential-tests the decimal parser described
below against the `std::wregex` it replaced.

## How the size budget was met

Starting point was 1292 KB; nearly all of the excess was C++ runtime pulled in
by a handful of references rather than by the calculator itself.

| Change | Saved |
| --- | --- |
| Dropped `std::wregex` from `scidisp.cpp` for a hand-written parser of the same grammar | ~620 KB (regex + iostream + locale) |
| Made `<iostream>` in `Ratpack/support.cpp` conditional — it was only there for the `GEN_CONST` constant-dump helpers | included above |
| `wstringstream` → `swprintf` for the `rand` command | included above |
| Replaced `std::__throw_*` and the global `operator new`/`delete`, which pulled `std::logic_error` → narrow `std::string` → `std::random_device` | ~63 KB |
| Seeded `mt19937` from the OS CSPRNG (`rand_s`) instead of `std::random_device` | included above |
| Operator-name table: `unordered_map` of `std::wstring` → flat `constexpr` array of `std::wstring_view` | ~15 KB |
| Replaced libstdc++'s verbose terminate handler (drops the symbol demangler) | ~48 KB |
| PNG-compressed icon sizes above 16px | ~10 KB |
| `-Os`, `-fno-rtti`, `--gc-sections`, `--strip-all` | — |

The engine edits are behaviour-preserving. The decimal parser is the only one
with real substance, and it is verified against the original expression over
~75,000 generated inputs; the rest are dead-code guards, a container swap and a
formatting call with identical output in the C locale.

Two notes on things that did *not* work: LTO ICEs in GCC 13's mingw-w64
(`binds_to_current_def_p`), and a 32-bit build is ~48 % *larger* because its
DWARF exception tables outweigh the smaller pointers.

## Requirements

Windows 10 or 11, x86-64. No installer, no runtime, no DLLs to ship — GDI,
GDI+ and DWM are all part of Windows.
