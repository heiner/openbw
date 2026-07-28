#!/usr/bin/env bash
# Build web/openbw-bot.wasm: the "vs Computer" module. It is the clean web sandbox
# (web/wasm_main.cpp + web/wasm_backend.cpp, built with -DOPENBW_WITH_BOT) linked
# with OpenBW's BWAPI server + ZZZKBot, which run as a read-view over the sandbox's
# own bwgame::state (web/bot/bot_view.cpp). One simulation, two players: the human
# drives slot 0 through the UI, the bot drives slot 1.
#
# The clean single-player build (web/build-wasm.sh -> openbw.wasm) is untouched;
# the only shared source is wasm_main.cpp's compile-guarded openbw_bot_attach hook.
#
# Prereq: ./vendor.sh (clone + patch vendor/). Engine = the repo (OPENBW_DIR=root),
# so the bot reads the exact sim we ship.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
V="$HERE/vendor"
WSDK="${WASI_SDK:-$HOME/src/wasi-sdk-33.0-arm64-macos}"
CXX="$WSDK/bin/clang++"
CC="$WSDK/bin/clang"
BWAPI="$V/bwapi/bwapi"
BUILD="$HERE/build/web"
OUT="$ROOT/web/openbw-bot.wasm"

[ -d "$V/bwapi" ] || { echo "run ./vendor.sh first (clone + patch vendor/)"; exit 1; }
python3 "$HERE/patch-vendor.py"

# 1. BWAPI server libs for wasm32 (one consistent CMake config; engine = repo).
echo "==> configuring + building bwapi libs..."
cmake -S "$BWAPI" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$WSDK/share/cmake/wasi-sdk-p1.cmake" \
  -DOPENBW_DIR="$ROOT" -DOPENBW_ENABLE_UI=0 -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_CXX_FLAGS="-fno-exceptions -DOPENBW_NO_EXCEPTIONS -include $HERE/shim/fatal.h" \
  -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --target BWAPICore BWAPILIB BWAPIObj OpenBWData -j8 >/dev/null

# 2. Compile the sandbox (+bot hook), the read-view glue, and the bot. Everything
#    is C++17 so the engine's bwgame.h is instantiated identically across the
#    sandbox and BWAPI TUs — the shared bwgame::state must have one layout.
STD="-std=c++17 -fno-exceptions -O2"
SB_DEFS="-DOPENBW_NO_EXCEPTIONS -DOPENBW_ENABLE_UI -DOPENBW_NO_SDL_IMAGE -DOPENBW_NO_SDL_MIXER -DOPENBW_WITH_BOT"
BOT_INCS="-I$BWAPI/include -I$BWAPI/BWAPI/Source -I$BWAPI/BWAPICore -I$BWAPI/OpenBWData \
  -I$BWAPI/BWAPI/openbw -I$BWAPI/Util/Source -I$V/ZZZKBot/ZZZKBot/Source -I$HERE/shim"
OBJ="$BUILD/obj"; mkdir -p "$OBJ"
echo "==> compiling sandbox + bot view + ZZZKBot..."
"$CXX" $STD $SB_DEFS -I"$ROOT" -I"$ROOT/ui" -c "$ROOT/web/wasm_main.cpp"    -o "$OBJ/wasm_main.o"
"$CXX" $STD $SB_DEFS -I"$ROOT" -I"$ROOT/ui" -c "$ROOT/web/wasm_backend.cpp" -o "$OBJ/wasm_backend.o"
"$CXX" $STD $BOT_INCS -c "$HERE/bot_view.cpp" -o "$OBJ/bot_view.o"
"$CXX" $STD $BOT_INCS -include "$HERE/shim/compat.h" \
  -c "$V/ZZZKBot/ZZZKBot/Source/ZZZKBotAIModule.cpp" -o "$OBJ/ZZZKBotAIModule.o"
"$CC" -O2 -c "$HERE/shim/dlstub.c" -o "$OBJ/dlstub.o"

# 3. Link the reactor module (exports via export_name attributes).
echo "==> linking $OUT ..."
LIBOBJS=$(find "$BUILD" -name '*.obj')
"$CXX" -mexec-model=reactor -fno-exceptions -Wl,-z,stack-size=8388608 \
  "$OBJ/wasm_main.o" "$OBJ/wasm_backend.o" "$OBJ/bot_view.o" \
  "$OBJ/ZZZKBotAIModule.o" "$OBJ/dlstub.o" $LIBOBJS \
  -o "$OUT"
echo "==> built $OUT ($(ls -la "$OUT" | awk '{print $5}') bytes)"
"$WSDK/bin/wasm-objdump" -x "$OUT" 2>/dev/null | grep -iE "openbw_bot_(attach|tick|out|supply)" | head
