# web/bot — computer opponent (BWAPI bots on OpenBW), on the side

This directory adds an optional **AI opponent** to the sandbox by compiling real
open-source BWAPI bots to WebAssembly against OpenBW's own BWAPI layer. It is
deliberately **decoupled** from the clean web build:

- `web/build-wasm.sh`, `web/wasm_main.cpp`, `web/sandbox.h` are **untouched**.
- The only shared code is the read-only engine headers (`bwgame.h`, …).
- Third-party code is **fetched, not committed** (`vendor/` is gitignored), the
  same posture as the MPQs.

The web app loads `openbw.wasm` normally and swaps to `openbw-bot.wasm` only when
the player picks "vs Computer".

## Why this works (all verified in scratch, natively + wasm32)

The stack is `bot → BWAPILIB → BWAPI/Source (server) → OpenBWData → bwgame`. It
already exists — [OpenBW/bwapi](https://github.com/OpenBW/bwapi) is a BWAPI fork
backed by OpenBW, and bots like Stardust already build against it.

Proven:

| Step | Result |
|---|---|
| BWAPILIB (31 TUs) under wasi-sdk `-fno-exceptions` | compiles clean |
| Full stack (75 TUs: BWAPI server + OpenBWData + engine) under wasi-sdk | **74/75 compile**; the 1 holdout is unused LAN/TCP networking |
| ZZZKBot vs OpenBW (native) | **plays a real game** — mines, morphs a spawning pool, pumps zerglings |

`onFrame` runs every frame and its commands drive `bwgame` (confirmed by the game
state evolving: supply drop when a drone morphs the pool, unit growth as lings
hatch).

## First bot: ZZZKBot

[chriscoxe/ZZZKBot](https://github.com/chriscoxe/ZZZKBot) — AIIDE 2015 winner, a
Zerg rush. ~6.3k LOC, **no BWTA/BWEM/boost/torch**, single-threaded. Chosen as
the pipeline-prover; more bots (Stardust, tsc-bwai) follow the same recipe.

## The port recipe (what the build applies)

1. **Windows/CRT shims** (`shim/`): a stub `<Windows.h>`, and `errno_t` +
   `localtime_s` (POSIX `localtime_r`). Replaces ZZZKBot's `Dll.cpp` with a clean
   `gameInit`/`newAIModule` entry (`bot_entry.cpp`).
2. **Strip the ~4 file-I/O sites** (opponent modeling, logging, config, debug) —
   the bot plays with defaults; no WASI FS needed.
3. **Guard `OpenBWData/BWData.cpp`** networking by treating `__wasi__` like
   `_WIN32` at ~6 sites: the three `sync_server_asio_*` includes, the unconditional
   `tcp_server` member (→ `sync_server_noop`), and the `#ifndef _WIN32` LAN-usage
   blocks. Single-player uses `sync_server_noop`; sockets are unused in-browser,
   so `<netdb.h>` never needs to exist.
4. **No 32-bit patch needed**: ZZZKBot's `(int)void*` `ClientInfo` casts are a
   64-bit *native* problem only — wasm32 has 32-bit pointers, so they're fine.
5. Compile with **wasi-sdk clang, `-fexceptions`** (bot-only artifact; the BWAPI
   server throws in a few error paths). The clean `openbw.wasm` keeps
   `-fno-exceptions`.

## Layout

    vendor.sh          fetch pinned OpenBW/bwapi + OpenBW/openbw + ZZZKBot → vendor/ (gitignored)
    shim/              Windows.h stub, compat.h (errno_t/localtime_s)
    bot_main.cpp       wasm reactor harness (init/step); static bot registration
    bot_entry.cpp      dlopen-style entry (gameInit/newAIModule) — unused on the wasm path
    build-wasm-bot.sh  wasi-sdk build → web/openbw-bot.wasm
    README.md          this file

## Status / remaining work

Verified this session (native + wasm32, in scratch):
- Full stack compiles for wasm32 (74/75 as-is; BWData needs the guard in recipe #3).
- ZZZKBot's *original* source compiles for wasm32 — the `(int)void*` casts are
  fine on 32-bit pointers, so no cast patch is needed on the wasm path.
- **Static bot registration** works via `GameImpl::specifiedModule` — no dlopen.
  `bot_main.cpp` uses it: `GameOwner` + `BroodwarImpl_handle`, set
  `specifiedModule = new <Bot>`, `startGame`; step = `update()` + `nextFrame()`.

Left to do, in order:
1. Finalize the BWData `__wasi__` guard (recipe #3), link the objects with
   `-mexec-model=reactor` → `web/openbw-bot.wasm`, and confirm it instantiates
   (this closes the wasm *link*, after the compile). Config (map/race) set on
   `autoMenuManager` directly rather than a bwapi.ini file.
2. **Human-vs-bot variant** — the last new code. Instead of `GameOwner` owning
   the game, construct the BWAPI game over the *sandbox's* `bwgame::state`
   (`BWData` holds `bwgame::state&`, so it wraps an external one); `sandbox.h`
   drives player 1 exactly as today, the bot drives player 2. Loop stays
   human-input → `update()` → `nextFrame()` → render.
