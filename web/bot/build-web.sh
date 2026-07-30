#!/usr/bin/env bash
# Build a "vs Computer" module: the clean web sandbox (web/wasm_main.cpp +
# web/wasm_backend.cpp, built with -DOPENBW_WITH_BOT) linked with OpenBW's BWAPI
# server + a bot, all running as a read-view over the sandbox's own bwgame::state
# (web/bot/bot_view.cpp). One simulation, two players: the human drives slot 0
# through the UI, the bot drives slot 1.
#
#   ./build-web.sh             -> web/openbw-bot.wasm     (ZZZKBot, Zerg)
#   BOT=mcrave ./build-web.sh  -> web/openbw-mcrave.wasm  (McRave, multi-race)
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

BOT="${BOT:-zzzkbot}"
case "$BOT" in
  zzzkbot) OUT="$ROOT/web/openbw-bot.wasm" ;;
  mcrave)  OUT="$ROOT/web/openbw-mcrave.wasm" ;;
  *) echo "unknown BOT='$BOT' (want: zzzkbot | mcrave)"; exit 1 ;;
esac
OBJ="$BUILD/obj/$BOT"; mkdir -p "$OBJ"

[ -d "$V/bwapi" ] || { echo "run ./vendor.sh first (clone + patch vendor/)"; exit 1; }
python3 "$HERE/patch-vendor.py"

# 1. BWAPI server libs for wasm32 (one consistent CMake config; engine = repo).
#    Shared across bots — the .obj outputs live under $BUILD and are linked below.
echo "==> configuring + building bwapi libs..."
cmake -S "$BWAPI" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$WSDK/share/cmake/wasi-sdk-p1.cmake" \
  -DOPENBW_DIR="$ROOT" -DOPENBW_ENABLE_UI=0 -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_CXX_FLAGS="-fno-exceptions -DOPENBW_NO_EXCEPTIONS -include $HERE/shim/fatal.h" \
  -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --target BWAPICore BWAPILIB BWAPIObj OpenBWData -j8 >/dev/null

# 2. Compile the sandbox (+bot hook) — shared by all bots. Everything is C++17 so
#    the engine's bwgame.h is instantiated identically across the sandbox and BWAPI
#    TUs — the shared bwgame::state must have one layout.
STD="-std=c++17 -fno-exceptions -O2"
SB_DEFS="-DOPENBW_NO_EXCEPTIONS -DOPENBW_ENABLE_UI -DOPENBW_NO_SDL_IMAGE -DOPENBW_NO_SDL_MIXER -DOPENBW_WITH_BOT"
echo "==> compiling sandbox..."
"$CXX" $STD $SB_DEFS -I"$ROOT" -I"$ROOT/ui" -c "$ROOT/web/wasm_main.cpp"    -o "$OBJ/wasm_main.o"
"$CXX" $STD $SB_DEFS -I"$ROOT" -I"$ROOT/ui" -c "$ROOT/web/wasm_backend.cpp" -o "$OBJ/wasm_backend.o"
"$CC" -O2 -c "$HERE/shim/dlstub.c" -o "$OBJ/dlstub.o"

BWAPI_INCS="-I$BWAPI/include -I$BWAPI/BWAPI/Source -I$BWAPI/BWAPICore -I$BWAPI/OpenBWData \
  -I$BWAPI/BWAPI/openbw -I$BWAPI/Util/Source"

# 3. Compile the read-view glue + the bot itself (bot-specific).
if [ "$BOT" = zzzkbot ]; then
  echo "==> compiling bot view + ZZZKBot..."
  BOT_INCS="$BWAPI_INCS -I$V/ZZZKBot/ZZZKBot/Source -I$HERE/shim"
  "$CXX" $STD $BOT_INCS -c "$HERE/bot_view.cpp" -o "$OBJ/bot_view.o"
  "$CXX" $STD $BOT_INCS -include "$HERE/shim/compat.h" \
    -c "$V/ZZZKBot/ZZZKBot/Source/ZZZKBotAIModule.cpp" -o "$OBJ/ZZZKBotAIModule.o"
  BOT_OBJS="$OBJ/bot_view.o $OBJ/ZZZKBotAIModule.o"
else
  # McRave: bot_view bolts on McRaveModule (Main/Header.h). Its ~113 TUs (McRave +
  # BWEB + Horizon + bundled BWEM) build with the MSVC->clang flags the port proved:
  # parse templates at instantiation (-fdelayed-template-parsing) and force-include
  # the std headers MSVC gives transitively. Source fixes live in mcrave.patch; the
  # DLL entry (Dll.cpp) is excluded — the bot is registered statically.
  MCSRC="$V/mcrave/Source"
  BOT_INCS="$BWAPI_INCS -I$MCSRC/McRave -I$MCSRC/BWEM -I$MCSRC/BWEB -I$MCSRC/Horizon -I$HERE/shim"
  MC_FLAGS="-DOPENBW_NO_EXCEPTIONS -fdelayed-template-parsing \
    -include cfloat -include climits -include cmath \
    -include $HERE/shim/compat.h -include $HERE/shim/fatal.h"
  echo "==> compiling bot view (McRave)..."
  "$CXX" $STD $BOT_INCS $MC_FLAGS \
    -DOPENBW_BOT_MODULE_HEADER='"Main/Header.h"' -DOPENBW_BOT_MODULE_CLASS=McRaveModule \
    -c "$HERE/bot_view.cpp" -o "$OBJ/bot_view.o"
  echo "==> compiling McRave (~113 TUs; parallel)..."
  mkdir -p "$OBJ/mc"
  n=0
  while IFS= read -r f; do
    o="$OBJ/mc/$(echo "${f#$MCSRC/}" | tr '/' '_').o"
    "$CXX" $STD $BOT_INCS $MC_FLAGS -c "$f" -o "$o" &
    n=$((n + 1)); [ $((n % 8)) -eq 0 ] && wait
  done < <(find "$MCSRC/McRave" "$MCSRC/BWEB" "$MCSRC/Horizon" "$MCSRC/BWEM" -name '*.cpp' ! -name 'Dll.cpp')
  wait
  # Backgrounded compiles can't trip `set -e`; verify every TU produced an object.
  srcn=$(find "$MCSRC/McRave" "$MCSRC/BWEB" "$MCSRC/Horizon" "$MCSRC/BWEM" -name '*.cpp' ! -name 'Dll.cpp' | wc -l | tr -d ' ')
  objn=$(find "$OBJ/mc" -name '*.o' | wc -l | tr -d ' ')
  [ "$srcn" -eq "$objn" ] || { echo "McRave: only $objn/$srcn TUs compiled — see errors above"; exit 1; }
  BOT_OBJS="$OBJ/bot_view.o $(find "$OBJ/mc" -name '*.o' | tr '\n' ' ')"
fi

# 4. Link the reactor module (exports via export_name attributes).
echo "==> linking $OUT ..."
LIBOBJS=$(find "$BUILD" -name '*.obj')
"$CXX" -mexec-model=reactor -fno-exceptions -Wl,-z,stack-size=8388608 \
  "$OBJ/wasm_main.o" "$OBJ/wasm_backend.o" "$OBJ/dlstub.o" $BOT_OBJS $LIBOBJS \
  -o "$OUT"
echo "==> built $OUT ($(ls -la "$OUT" | awk '{print $5}') bytes)"
# Best-effort sanity print of the bot exports. wasm-objdump is a WABT tool the WASI
# SDK doesn't bundle, so skip it when absent and never let this diagnostic (under
# `set -e -o pipefail`) fail a build that already succeeded above.
if [ -x "$WSDK/bin/wasm-objdump" ]; then
  "$WSDK/bin/wasm-objdump" -x "$OUT" 2>/dev/null | grep -iE "openbw_bot_(attach|tick|out|supply)" | head || true
fi
