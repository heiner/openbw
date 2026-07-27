#!/usr/bin/env bash
# Fetch the third-party pieces for the bot opponent into vendor/ (gitignored) and
# apply the localized ports (patch-vendor.py). Same posture as the MPQs: fetched,
# not committed.
#
# The engine is NOT vendored — build-wasm-bot.sh points OPENBW_DIR at the repo
# root, so the bot runs the exact bwgame.h sim we ship in-browser (and it carries
# the OPENBW_NO_EXCEPTIONS guards the -fno-exceptions build needs).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V="$HERE/vendor"
mkdir -p "$V"

clone() { # url dir
  [ -d "$V/$2/.git" ] || git clone --depth 1 "$1" "$V/$2"
}

# OpenBW's BWAPI layer (mainline, backed by bwgame) + the first bot.
# TODO: pin exact commits for reproducibility.
clone https://github.com/OpenBW/bwapi.git      bwapi
clone https://github.com/chriscoxe/ZZZKBot.git ZZZKBot

# Apply the ports (wasi networking guards, throws->abort, per-frame stubs).
python3 "$HERE/patch-vendor.py"

echo "vendored into $V — now run ./build-wasm-bot.sh"
