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

clone_pin() { # url dir commit  — shallow-fetch an exact commit (reproducible)
  local d="$V/$2"
  [ -d "$d/.git" ] && return
  git init -q "$d"
  git -C "$d" remote add origin "$1"
  git -C "$d" fetch -q --depth 1 origin "$3"
  git -C "$d" checkout -q FETCH_HEAD
}

apply_patch() { # dir patchfile  — idempotent (skip if already applied)
  local d="$V/$1"
  if git -C "$d" apply --reverse --check "$2" 2>/dev/null; then
    echo "  $1: patch already applied"
  else
    git -C "$d" apply "$2" && echo "  $1: patched ($(basename "$2"))"
  fi
}

# OpenBW's BWAPI layer (mainline, backed by bwgame) + the first bot.
# TODO: pin exact commits for reproducibility (bwapi, ZZZKBot).
clone https://github.com/OpenBW/bwapi.git      bwapi
clone https://github.com/chriscoxe/ZZZKBot.git ZZZKBot

# McRave (Cmccrave) — a strong multi-race bot. Pinned: the port (mcrave.patch, the
# MSVC->clang/-fno-exceptions fixes) is line-sensitive, so the clone must be exact.
# One clone covers BWEB + BWEM + Horizon too — they live under McRave/Source/.
clone_pin https://github.com/Cmccrave/McRave.git mcrave 7d1719a22d8b896f957abae50e2ea5efff974fe2

# Apply the ports.
python3 "$HERE/patch-vendor.py"                    # BWAPI server (wasi net, throws->abort, external-game)
apply_patch mcrave "$HERE/mcrave.patch"            # McRave + BWEB/BWEM/Horizon MSVC->clang fixes

echo "vendored into $V — now run ./build-wasm-bot.sh (ZZZKBot) or BOT=mcrave ./build-web.sh"
