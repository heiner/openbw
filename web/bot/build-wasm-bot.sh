#!/usr/bin/env bash
# Build openbw-bot.wasm: the engine + OpenBW's BWAPI server + a ported bot.
#
# Method mirrors the verified proof: let CMake enumerate the exact source set and
# flags for the BWAPILauncher path, then re-issue each compile with wasi-sdk clang
# targeting wasm32, and link with the bot + bot_entry.cpp. 74/75 of those TUs were
# confirmed to compile as-is; the 1 (BWData.cpp) needs vendor.sh's netdb guard.
#
# Status: the compile pipeline is proven; the final link + reactor exports depend
# on bot_entry.cpp's sandbox integration glue (see README "Remaining work").
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V="$HERE/vendor"
WSDK="${WASI_SDK:-$HOME/src/wasi-sdk-33.0-arm64-macos}"
CXX="$WSDK/bin/clang++"
BWAPI="$V/bwapi/bwapi"
OUTOBJ="$HERE/build/obj"; mkdir -p "$OUTOBJ"

[ -d "$V/bwapi" ] || { echo "run ./vendor.sh first"; exit 1; }

# 1. Configure natively just to emit compile_commands.json (source list + flags).
cmake -S "$V/bwapi" -B "$HERE/build/cc" \
      -DOPENBW_DIR="$V/openbw" -DOPENBW_ENABLE_UI=0 \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null

# 2. Re-compile each launcher-path TU with wasi-sdk (wasm32, -fexceptions).
python3 - "$HERE/build/cc/compile_commands.json" "$CXX" "$OUTOBJ" <<'PY'
import json,sys,os,subprocess
cc=json.load(open(sys.argv[1])); CXX=sys.argv[2]; OUT=sys.argv[3]
keep=('BWAPILIB','BWAPIObj','OpenBWData','BWAPICore','BWAPILauncher')
seen=set(); rc=0
for e in cc:
    if not any(k+'.dir' in e.get('output','') for k in keep): continue
    f=e['file']
    if f in seen: continue
    seen.add(f)
    flags=[t for t in e['command'].split() if t.startswith(('-I','-D','-std'))]
    o=os.path.join(OUT, os.path.basename(f)+'.o')
    r=subprocess.run([CXX,'-fexceptions','-O2','-c',*flags,f,'-o',o])
    rc |= r.returncode
sys.exit(rc)
PY

# 3. Compile the bot + entry (shims force-included), then link → openbw-bot.wasm.
#    (Bot source globs + the reactor link are finalized once bot_entry.cpp's
#     sandbox glue lands — see README.)
echo "TODO: compile vendor/ZZZKBot + bot_entry.cpp and link -mexec-model=reactor -> web/openbw-bot.wasm"
echo "objects in $OUTOBJ"
