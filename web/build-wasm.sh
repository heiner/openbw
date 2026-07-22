#!/usr/bin/env bash
# Build the browser (wasm) sandbox module: web/wasm_main.cpp + web/wasm_backend.cpp.
# Reactor model (JS calls _initialize then openbw_init/openbw_step/openbw_render). No SDL.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WSDK="${WASI_SDK:-/Users/heiner/src/wasi-sdk-33.0-arm64-macos}"
OUT="${ROOT}/web/openbw.wasm"

DEFS="-DOPENBW_NO_EXCEPTIONS -DOPENBW_ENABLE_UI -DOPENBW_NO_SDL_IMAGE -DOPENBW_NO_SDL_MIXER"
INCS="-I${ROOT} -I${ROOT}/ui"
FLAGS="-std=c++14 -O2 -fno-exceptions -mexec-model=reactor"
LDFLAGS="-Wl,-z,stack-size=8388608"   # exports come from __attribute__((export_name))

echo "==> compiling wasm..."
"${WSDK}/bin/clang++" ${FLAGS} ${DEFS} ${INCS} ${LDFLAGS} \
	"${ROOT}/web/wasm_main.cpp" \
	"${ROOT}/web/wasm_backend.cpp" \
	-o "${OUT}"

echo "==> built ${OUT} ($(ls -la "${OUT}" | awk '{print $5}') bytes)"
"${WSDK}/bin/wasm-objdump" -x "${OUT}" 2>/dev/null | grep -A20 "Export\[" | head -30 || true
