#!/usr/bin/env bash
# Build web/openbw-bot.wasm: OpenBW's engine + OpenBW's BWAPI server + a ported
# bot (ZZZKBot), as a wasm32-wasi reactor. The AI-opponent counterpart to the
# clean web/build-wasm.sh (which is left untouched).
#
# Method: drive the vendored OpenBW/bwapi CMake with the wasi-sdk toolchain so
# every server TU gets ONE consistent config (the macro-generated BW::Game /
# Unit / Bullet methods must be defined identically across TUs, which hand-rolled
# per-TU compiles from compile_commands.json failed to guarantee). Then compile
# the bot + reactor harness and link everything.
#
# Prereqs: ./vendor.sh (clones OpenBW/bwapi + ZZZKBot into vendor/, runs
# patch-vendor.py). Engine comes from the repo itself (OPENBW_DIR=repo root) so
# the bot runs the exact sim we ship in-browser.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
V="$HERE/vendor"
WSDK="${WASI_SDK:-$HOME/src/wasi-sdk-33.0-arm64-macos}"
CXX="$WSDK/bin/clang++"
CC="$WSDK/bin/clang"
BWAPI="$V/bwapi/bwapi"
BUILD="$HERE/build/wasm"
OUT="$ROOT/web/openbw-bot.wasm"

[ -d "$V/bwapi" ] || { echo "run ./vendor.sh first (clones + patches vendor/)"; exit 1; }
python3 "$HERE/patch-vendor.py"

# 1. Configure + build the BWAPI server libs for wasm32 (one consistent config).
echo "==> configuring bwapi (wasi-sdk toolchain)..."
cmake -S "$BWAPI" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$WSDK/share/cmake/wasi-sdk-p1.cmake" \
  -DOPENBW_DIR="$ROOT" \
  -DOPENBW_ENABLE_UI=0 \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_CXX_FLAGS="-fno-exceptions -DOPENBW_NO_EXCEPTIONS -include $HERE/shim/fatal.h" \
  -DCMAKE_BUILD_TYPE=Release >/dev/null
echo "==> building server libs..."
cmake --build "$BUILD" --target BWAPICore BWAPILIB BWAPIObj OpenBWData -j8 >/dev/null

# 2. Compile the bot + reactor harness (same flags/std as the server).
STD="-std=c++17 -fno-exceptions -O2"
INCS="-I$BWAPI/include -I$BWAPI/BWAPI/Source -I$BWAPI/BWAPICore -I$BWAPI/OpenBWData \
  -I$BWAPI/BWAPI/openbw -I$BWAPI/Util/Source \
  -I$V/ZZZKBot/ZZZKBot/Source -I$HERE/shim -I$ROOT"
OBJ="$BUILD/bot"; mkdir -p "$OBJ"
echo "==> compiling bot + harness..."
# ZZZKBot: only the AI module (Dll.cpp is the Windows entry — replaced by static
# registration in bot_main.cpp). compat.h supplies errno_t/localtime_s; its
# file-I/O just fails-open at runtime (no WASI filesystem needed).
"$CXX" $STD $INCS -include "$HERE/shim/compat.h" \
  -c "$V/ZZZKBot/ZZZKBot/Source/ZZZKBotAIModule.cpp" -o "$OBJ/ZZZKBotAIModule.o"
"$CXX" $STD $INCS -include "$HERE/shim/fatal.h" \
  -c "$HERE/bot_main.cpp" -o "$OBJ/bot_main.o"
"$CC" -O2 -c "$HERE/shim/dlstub.c" -o "$OBJ/dlstub.o"

# 3. Link everything into a reactor wasm (exports via export_name attributes).
echo "==> linking $OUT ..."
# CMake emits the server libs as *.obj (per target dir); our bot objects above
# are *.o, so this grabs exactly the 69 server/engine objects.
LIBOBJS=$(find "$BUILD" -name '*.obj')
"$CXX" -mexec-model=reactor -fno-exceptions -Wl,-z,stack-size=8388608 \
  $LIBOBJS "$OBJ/ZZZKBotAIModule.o" "$OBJ/bot_main.o" "$OBJ/dlstub.o" \
  -o "$OUT"

echo "==> built $OUT ($(ls -la "$OUT" | awk '{print $5}') bytes)"
"$WSDK/bin/wasm-objdump" -x "$OUT" 2>/dev/null | grep -iE "openbw_bot" | head
