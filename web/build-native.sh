#!/usr/bin/env bash
# Build the native OpenBW sandbox (web/play.cpp + ui/sdl2.cpp).
# Fast local iteration loop; the browser build will swap the SDL backend for
# a WASI + <canvas> one but reuse the same play.cpp game logic.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/web/openbw_play"

# SDL2 include/lib flags (fall back to explicit homebrew paths if pkg-config
# misbehaves in a sandbox).
SDL_CFLAGS="$(pkg-config --cflags sdl2 2>/dev/null || echo '-I/opt/homebrew/include -I/opt/homebrew/include/SDL2')"
SDL_LIBS="$(pkg-config --libs sdl2 2>/dev/null || echo '-L/opt/homebrew/lib -lSDL2')"

DEFS="-DOPENBW_NO_SDL_IMAGE -DOPENBW_NO_SDL_MIXER -DOPENBW_ENABLE_UI"
INCS="-I${ROOT} -I${ROOT}/ui ${SDL_CFLAGS}"
FLAGS="-std=c++14 -O2 -g -Wno-deprecated-declarations"

echo "==> compiling..."
clang++ ${FLAGS} ${DEFS} ${INCS} \
	"${ROOT}/web/play.cpp" \
	"${ROOT}/ui/sdl2.cpp" \
	${SDL_LIBS} \
	-o "${OUT}"

echo "==> built ${OUT}"
