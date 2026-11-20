#!/usr/bin/env bash
# Build native/points_random.dll with LLVM clang and the Visual Studio libraries.
# SDK headers: templates/addon/native/include in https://github.com/Swofty-Developments/SwoftyLauncher
set -euo pipefail
cd "$(dirname "$0")"
SDK="${1:?path to the SDK include dir: SwoftyLauncher/templates/addon/native/include}"
clang++ --target=x86_64-pc-windows-msvc -std=c++23 -O2 -Wall -Wextra -fms-runtime-lib=static -I"$SDK" -shared \
    -o points_random.dll src/points_random.cpp
rm -f points_random.lib points_random.exp
echo built native/points_random.dll
