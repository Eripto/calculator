#!/usr/bin/env bash
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
#
# Builds and runs the compact-build engine tests with the host compiler.
# CalcManager is portable C++, so this needs neither Windows nor mingw.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE="$ROOT/../CalcManager"
OUT="${1:-$ROOT/out/tests}"
CXX="${CXX:-g++}"

mkdir -p "$OUT"

FLAGS=(-std=c++20 -O1 -g0 -DNDEBUG -I"$ENGINE" -I"$ENGINE/Header Files" -I"$ROOT/app" -I"$ROOT")

echo "==> building engine for tests"
OBJECTS=()
while IFS= read -r file; do
  case "$file" in
    # pch.cpp is MSVC-only. scidisp.cpp is compiled into the test itself so the
    # file-static decimal parser is reachable.
    */pch.cpp|*/scidisp.cpp) continue ;;
  esac
  name="$(basename "$file" .cpp)_$(printf '%s' "$file" | cksum | cut -d' ' -f1).o"
  "$CXX" "${FLAGS[@]}" -c "$file" -o "$OUT/$name"
  OBJECTS+=("$OUT/$name")
done < <(find "$ENGINE" -name '*.cpp' | sort)

echo "==> building front-end models"
for file in "$ROOT"/app/*.cpp; do
  case "$file" in
    */main.cpp) continue ;;   # Win32 UI, not host-buildable
  esac
  name="$(basename "$file" .cpp).o"
  "$CXX" "${FLAGS[@]}" -c "$file" -o "$OUT/$name"
  OBJECTS+=("$OUT/$name")
done

echo "==> building tests"
"$CXX" "${FLAGS[@]}" -c "$ROOT/tests/engine_tests.cpp" -o "$OUT/engine_tests.o"
OBJECTS+=("$OUT/engine_tests.o")

"$CXX" "${OBJECTS[@]}" -o "$OUT/engine_tests"

echo "==> running"
"$OUT/engine_tests"

# Setup carries the calculator LZMA-packed and unpacks it with its own decoder
# (installer/Unpack.cpp). Round-trip real executables through the same packer
# the build uses, and check damaged input is refused.
echo "==> installer payload round trip"
"$CXX" -std=c++20 -O2 -Wall -o "$OUT/unpack_test" "$ROOT/tests/unpack_test.cpp" "$ROOT/installer/Unpack.cpp"
PACKED=()
for input in "$OUT/engine_tests" "$ROOT/out/Calculator.exe"; do
  [ -f "$input" ] || continue
  python3 "$ROOT/tools/pack_payload.py" "$input" "$OUT/$(basename "$input").cz" >/dev/null
  PACKED+=("$OUT/$(basename "$input").cz" "$input")
done
"$OUT/unpack_test" "${PACKED[@]}"
