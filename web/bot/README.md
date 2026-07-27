# web/bot — computer opponent (BWAPI bots on OpenBW), on the side

This directory adds an optional **AI opponent** to the sandbox by compiling real
open-source BWAPI bots to WebAssembly against OpenBW's own BWAPI layer. It is
deliberately **decoupled** from the clean web build:

- `web/build-wasm.sh`, `web/wasm_main.cpp`, `web/sandbox.h` are **untouched**.
- The only shared code is the read-only engine headers (`bwgame.h`, …) — in fact
  the bot builds against the **repo's own engine** (`OPENBW_DIR` = repo root), so
  it runs the exact sim we ship in-browser.
- Third-party code is **fetched, not committed** (`vendor/` is gitignored), the
  same posture as the MPQs. `openbw-bot.wasm` is a build output (also gitignored),
  like `openbw.wasm`.

The web app loads `openbw.wasm` normally and swaps to `openbw-bot.wasm` only when
the player picks "vs Computer".

## Status: it builds and plays in wasm32 ✅

`./vendor.sh && ./build-wasm-bot.sh` produces `web/openbw-bot.wasm` (~5.8 MB), a
wasm32-wasi **reactor**. Verified:

| Check | Result |
|---|---|
| Full stack builds for wasm32 (engine + BWAPI server + OpenBWData + ZZZKBot) | **compiles clean**, one consistent CMake config |
| `WebAssembly.compile` | **valid**; imports only `wasi_snapshot_preview1` (same as `openbw.wasm`) |
| Exports | `openbw_bot_init`, `openbw_bot_step`, `openbw_bot_frame`, `_initialize` |
| Runtime (node:wasi, real MPQs + a map) | `init` loads MPQs+map, then **ZZZKBot plays 1500+ deterministic frames, no crash** (past its ~frame-1000 pool morph) |

So the whole pipeline works at runtime in the browser target, not just natively.

## The stack

`bot → BWAPILIB → BWAPI/Source (server) → OpenBWData → bwgame`. It already exists —
[OpenBW/bwapi](https://github.com/OpenBW/bwapi) is a BWAPI fork backed by OpenBW.
The one wasm-specific move is **static** bot registration (no dlopen):
`bot_main.cpp` sets `GameImpl::specifiedModule` before `startGame`, so the server
uses our bot directly and it shares the global `BWAPI::BroodwarPtr`.

## First bot: ZZZKBot

[chriscoxe/ZZZKBot](https://github.com/chriscoxe/ZZZKBot) — AIIDE 2015 winner, a
Zerg rush. ~6.3k LOC, **no BWTA/BWEM/boost/torch**, single-threaded, just two
source files (we compile `ZZZKBotAIModule.cpp`; `Dll.cpp` is the Windows entry,
replaced by static registration). More bots follow the same recipe.

## The port recipe (what the build applies)

The build **drives the vendored OpenBW/bwapi CMake with the wasi-sdk toolchain
file** so every server TU gets ONE identical config. (Hand-rolling per-TU compiles
from `compile_commands.json` dropped config CMake applies uniformly — the
macro-generated `BW::Game`/`Unit`/`Bullet` methods came out inconsistent across
TUs and failed to link. The toolchain-file approach fixes that.)

On top of that (`patch-vendor.py`, idempotent):

1. **`-fno-exceptions` + throws→abort.** The clean build is `-fno-exceptions`; the
   BWAPI server writes ~16 error paths as `throw std::runtime_error(...)`. Rewrite
   them to `bwapi_fatal(...)` (declared in `shim/fatal.h`, force-included) which
   aborts. (`-fwasm-exceptions` links but hits a legacy-vs-new wasm-EH instruction
   mix on instantiate — the same wall the pre-bot build hit.)
2. **`__wasi__` networking guards in `OpenBWData/BW/BWData.cpp`.** Treat `__wasi__`
   like `_WIN32` (no asio LAN/TCP — it pulls in `<netdb.h>`/sockets wasi-sdk
   lacks): guard the `sync_server_asio_*` includes, make tcp/local/file servers
   `sync_server_noop`, guard `bind`/`connect`, and force `default_lan_mode=NONE`
   so single-player stays `server_n==0` (pure noop).
3. **Two per-frame stubs.** OpenBWData left `getRandomSeed()`
   (`ReplayHead_gameSeed_randSeed`) and `countdownTimer()` *throwing*, but BWAPI's
   `Server.cpp` reads both **every frame** to fill the shared `GameData`. Return
   sane deterministic values (42 — OpenBW's fixed LCG seed — and 0). The other
   replay-only stubs stay fatal (never reached in melee play).
4. **Windows/CRT shims** (`shim/`): a stub `<Windows.h>`, and `errno_t` +
   `localtime_s` (POSIX `localtime_r`), force-included into the bot. ZZZKBot's
   file-I/O (opponent modeling/logging) just **fails-open at runtime** — no WASI
   filesystem needed, the bot plays with defaults.
5. **dlopen no-op stub** (`shim/dlstub.c`): the AIModuleLoader path is compiled
   but unused (we use `specifiedModule`); the stub satisfies the link.

No 32-bit cast patch is needed: ZZZKBot's `(int)void*` `ClientInfo` casts are a
64-bit-*native* problem only — wasm32 pointers are 32-bit.

## Layout

    vendor.sh          fetch mainline OpenBW/bwapi + ZZZKBot -> vendor/ (gitignored), run patch-vendor.py
    patch-vendor.py    the localized ports (idempotent): exceptions, wasi net guards, per-frame stubs
    shim/              Windows.h stub, compat.h (errno_t/localtime_s), fatal.h (bwapi_fatal), dlstub.c
    bot_main.cpp       wasm reactor harness (init/step/frame); static bot registration
    bot_entry.cpp      dlopen-style entry (gameInit/newAIModule) — unused on the wasm path
    build-wasm-bot.sh  CMake+wasi-sdk build -> web/openbw-bot.wasm
    README.md          this file

To reproduce:

    cd web/bot && ./vendor.sh && ./build-wasm-bot.sh
    # standalone play check (needs Patch_rt.mpq/BrooDat.mpq/StarDat.mpq + a .scx
    # named as in a bwapi.ini [auto_menu] block, in a preopened dir):
    #   OPENBW_MPQ_PATH=. BWAPI_CONFIG_INI=bwapi.ini  under node:wasi

## Remaining work (in-browser wiring), in order

1. **MPQ/map provisioning in-browser.** The standalone bot uses the engine's
   *stock* file loader (real `open()` of `Patch_rt.mpq` etc. from
   `OPENBW_MPQ_PATH`). The clean sandbox instead feeds MPQ bytes from JS via an
   *indexed* `js_file_reader` (0=StarDat, 1=BrooDat, 2=Patch_rt, 3=map — see
   `web/wasm_main.cpp`). To load data in-browser the bot must go through the same
   JS path (route OpenBWData's loader through `js_file_reader`, or populate a WASI
   in-memory FS with the bytes JS already fetched). This is the main new plumbing.
2. **Human-vs-bot variant** — the last new code. Instead of `GameOwner` owning the
   game, construct the BWAPI game over the *sandbox's* `bwgame::state` (`BWData`
   holds `bwgame::state&`, so it wraps an external one); `sandbox.h` drives player
   1 as today, the bot drives player 2. Loop: human input → `update()` (fires the
   bot's `onFrame`) → `nextFrame()` → render.
3. **Wire into the page** as a "vs Computer" mode (map/race selection; the
   standalone path reads these from a `bwapi.ini`, the in-page path sets them on
   `autoMenuManager`/the setup directly). Add the bot build to the Pages workflow
   once the mode is live.
