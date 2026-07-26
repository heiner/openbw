#!/usr/bin/env bash
# Build the OpenBW multiplayer relay (web/bridge.cpp) — a standalone WebSocket
# relay with no OpenBW/SDL dependency, using the vendored header-only asio.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
echo "==> compiling relay..."
clang++ -std=c++14 -O2 -I"${ROOT}" "${ROOT}/web/bridge.cpp" -o "${ROOT}/web/openbw_bridge" -lpthread
echo "==> built ${ROOT}/web/openbw_bridge"
