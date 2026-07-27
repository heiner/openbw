#!/usr/bin/env bash
# Fetch the third-party pieces for the bot opponent into vendor/ (gitignored) and
# apply the small, localized ports. Pin the commits before relying on this.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V="$HERE/vendor"
mkdir -p "$V"

clone() { # repo dir [ref]
  local url=$1 dir=$2 ref=${3:-}
  if [ ! -d "$V/$dir/.git" ]; then git clone --depth 1 ${ref:+--branch "$ref"} "$url" "$V/$dir"; fi
}

# OpenBW's BWAPI layer (backed by bwgame) + a matching engine + the bot.
# TODO: pin exact commits for reproducibility.
clone https://github.com/OpenBW/bwapi.git    bwapi
clone https://github.com/OpenBW/openbw.git   openbw
clone https://github.com/chriscoxe/ZZZKBot.git ZZZKBot

# --- Port 1: guard OpenBWData's asio LAN/TCP networking for wasi (unused; pulls
#     in <netdb.h> which wasi-sdk lacks). Single-player uses sync_server_noop. ---
BWDATA="$V/bwapi/bwapi/OpenBWData/BW/BWData.cpp"
if [ -f "$BWDATA" ] && ! grep -q "__wasi__ .* web/bot" "$BWDATA"; then
  # Wrap the TCP asio server include; the LAN branches (server_n==1/2) are only
  # reached at runtime under OPENBW_LAN_MODE, which we never set in-browser.
  perl -0pi -e 's{(#include "sync_server_asio_tcp.h")}{#ifndef __wasi__  // web/bot: LAN unused in browser\n$1\n#endif}' "$BWDATA"
  echo "patched (wasi netdb guard): $BWDATA"
  echo "NOTE: verify the LAN server_n==1/2 branches also compile out under __wasi__." >&2
fi

# --- Port 2: strip ZZZKBot's file I/O (opponent modeling / logging / config /
#     debug). The bot plays with defaults; no WASI filesystem needed. ---
echo "TODO: strip fstream sites in vendor/ZZZKBot/ZZZKBot/Source/ZZZKBotAIModule.cpp"
echo "      (guarded no-ops) — see README 'port recipe' step 2."

echo "vendored into $V"
