#!/usr/bin/env bash
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
#
# Cross-compiles Calculator.exe with mingw-w64. The calculation engine is taken
# straight from ../CalcManager, so this produces the shipping engine behind a
# native Win32 front end.
#
#   sudo apt-get install mingw-w64
#   ./build.sh [--arch x86_64|i686] [--out DIR]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE="$ROOT/../CalcManager"
ARCH="x86_64"
OUT="$ROOT/out"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2 ;;
    --out)  OUT="$2";  shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

CXX="${ARCH}-w64-mingw32-g++"
WINDRES="${ARCH}-w64-mingw32-windres"
STRIP="${ARCH}-w64-mingw32-strip"

command -v "$CXX" >/dev/null || { echo "error: $CXX not found (apt-get install mingw-w64)" >&2; exit 1; }

OBJ="$OUT/obj"
mkdir -p "$OBJ"

# -Os plus section GC and --strip-all is what keeps the binary small; the engine
# needs exceptions (CalcErr) but uses no RTTI.
CXXFLAGS=(
  -std=c++20 -Os -DNDEBUG -DUNICODE -D_UNICODE
  -municode -fno-rtti
  -ffunction-sections -fdata-sections
  -fno-ident -fmerge-all-constants
  -Wall -Wno-unknown-pragmas
  -I"$ENGINE" -I"$ENGINE/Header Files" -I"$ROOT/app"
)

LDFLAGS=(
  -municode -mwindows -static
  -Wl,--gc-sections -Wl,--strip-all -Wl,--no-insert-timestamp
)

LIBS=(-lgdiplus -lmsimg32 -lgdi32 -luser32 -ldwmapi -ladvapi32 -lshell32 -lole32 -luuid -lmsvcrt)

echo "==> building engine ($ARCH)"
SOURCES=()
while IFS= read -r file; do
  # pch.cpp is MSVC-only; everything else in CalcManager is built, including the
  # unit converter (CALC_SUPPORTS_CURRENCY_ASYNC is left undefined, which drops
  # only the PPL-based live-currency fetch).
  case "$file" in
    */pch.cpp) continue ;;
  esac
  SOURCES+=("$file")
done < <(find "$ENGINE" -name '*.cpp' | sort)

OBJECTS=()
for file in "${SOURCES[@]}"; do
  name="$(basename "$file" .cpp)_$(printf '%s' "$file" | cksum | cut -d' ' -f1).o"
  "$CXX" "${CXXFLAGS[@]}" -c "$file" -o "$OBJ/$name"
  OBJECTS+=("$OBJ/$name")
done

echo "==> building front end"
for file in "$ROOT"/app/*.cpp; do
  name="$(basename "$file" .cpp).o"
  "$CXX" "${CXXFLAGS[@]}" -c "$file" -o "$OBJ/$name"
  OBJECTS+=("$OBJ/$name")
done

echo "==> compiling resources"
"$WINDRES" -I"$ROOT/res" "$ROOT/res/app.rc" -O coff -o "$OBJ/app.res.o"
OBJECTS+=("$OBJ/app.res.o")

echo "==> linking"
"$CXX" "${OBJECTS[@]}" "${LDFLAGS[@]}" "${LIBS[@]}" -o "$OUT/Calculator.exe"
"$STRIP" --strip-all "$OUT/Calculator.exe" 2>/dev/null || true

SIZE=$(stat -c %s "$OUT/Calculator.exe")
printf '==> %s\n    %s bytes (%s KB)\n' "$OUT/Calculator.exe" "$SIZE" "$((SIZE / 1024))"
BUDGET_KB="${CALC_SIZE_BUDGET_KB:-425}"
if (( SIZE > BUDGET_KB * 1024 )); then
  echo "    ERROR: over the ${BUDGET_KB} KB budget" >&2
  exit 1
fi
