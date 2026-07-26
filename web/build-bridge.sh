#!/usr/bin/env bash
# Build the OpenBW multiplayer bridge (web/bridge.cpp) — a standalone WebSocket
# endpoint with no OpenBW/SDL dependency, using the vendored header-only asio.
#
# Two modes:
#   relay  (M1)  one process, two browsers over WebSocket:
#                  ./openbw_bridge --port 8100
#                then both browsers "Host/Join on relay" ws://<host-LAN-IP>:8100
#
#   bridge (M2)  one process per machine, browser over WS <-> peer over UDP.
#                Each browser talks to its LOCAL bridge; the two bridges carry the
#                lockstep stream to each other over reliable UDP (RUdp). This is the
#                shape the retail bridge needs (M3 swaps the UDP peer for real BW).
#                  machine A:  ./openbw_bridge --udp-peer <B-IP>:6112
#                  machine B:  ./openbw_bridge --udp-peer <A-IP>:6112
#                then browser A "Host on relay" ws://localhost:8100,
#                     browser B "Join on relay" ws://localhost:8100
#                (--ws-port default 8100, --udp-listen default 6112)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
echo "==> compiling bridge..."
clang++ -std=c++14 -O2 -I"${ROOT}" "${ROOT}/web/bridge.cpp" -o "${ROOT}/web/openbw_bridge" -lpthread
echo "==> built ${ROOT}/web/openbw_bridge"
