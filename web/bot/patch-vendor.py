#!/usr/bin/env python3
"""web/bot: apply the localized ports to vendor/ so the BWAPI server builds for
wasm32-wasi with -fno-exceptions. Idempotent (safe to re-run). Called by
vendor.sh after cloning. Two ports:

  1. BWData.cpp: treat __wasi__ like _WIN32 for the asio LAN/TCP networking
     (unused in-browser; pulls in <netdb.h>/sockets wasi-sdk lacks) and force
     server_n=0 (sync_server_noop) so single-player never touches sockets.
  2. Rewrite the ~16 `throw std::runtime_error(...)` in the server to
     bwapi_fatal(...) (declared in shim/fatal.h), since the build is
     -fno-exceptions.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
V = os.path.join(HERE, "vendor")
BWAPI = os.path.join(V, "bwapi", "bwapi")


def patch(path, subs, required=True):
    if not os.path.exists(path):
        if required:
            sys.exit(f"missing {path}; run vendor.sh clone first")
        return
    s = open(path).read()
    for old, new in subs:
        if new in s:
            continue  # already applied (works when `new` contains `old`, i.e. insertions)
        if old not in s:
            sys.exit(f"{path}: expected snippet not found:\n{old}")
        s = s.replace(old, new, 1)
    open(path, "w").write(s)
    print("patched", os.path.relpath(path, HERE))


# --- Port 1: BWData.cpp networking guards ------------------------------------
bwdata = os.path.join(BWAPI, "OpenBWData", "BW", "BWData.cpp")
bwh = os.path.join(BWAPI, "OpenBWData", "BW", "BWData.h")
patch(bwdata, [
    # asio tcp include is unconditional upstream; wasi has no sockets.
    ('#include "sync_server_asio_tcp.h"\n#ifndef _WIN32\n'
     '#include "sync_server_asio_local.h"',
     '#ifndef __wasi__\n#include "sync_server_asio_tcp.h"\n#endif\n'
     '#if !defined(_WIN32) && !defined(__wasi__)\n'
     '#include "sync_server_asio_local.h"'),
    # server members: tcp/local/file all noop on wasi.
    ('  bwgame::sync_server_noop noop_server;\n'
     '  bwgame::sync_server_asio_tcp tcp_server;\n'
     '#ifndef _WIN32\n  bwgame::sync_server_asio_local local_server;\n'
     '#else\n  bwgame::sync_server_noop local_server;\n#endif\n'
     '#ifndef _WIN32\n  bwgame::sync_server_asio_posix_stream file_server;\n'
     '#else\n  bwgame::sync_server_noop file_server;\n#endif',
     '  bwgame::sync_server_noop noop_server;\n'
     '#ifndef __wasi__\n  bwgame::sync_server_asio_tcp tcp_server;\n'
     '#else\n  bwgame::sync_server_noop tcp_server;\n#endif\n'
     '#if !defined(_WIN32) && !defined(__wasi__)\n'
     '  bwgame::sync_server_asio_local local_server;\n'
     '#else\n  bwgame::sync_server_noop local_server;\n#endif\n'
     '#if !defined(_WIN32) && !defined(__wasi__)\n'
     '  bwgame::sync_server_asio_posix_stream file_server;\n'
     '#else\n  bwgame::sync_server_noop file_server;\n#endif'),
    # default LAN mode: NONE on wasi -> server_n stays 0 (noop).
    ('    std::string default_lan_mode = "LOCAL_AUTO";\n'
     '#ifdef _WIN32\n    default_lan_mode = "TCP";\n#endif',
     '    std::string default_lan_mode = "LOCAL_AUTO";\n'
     '#ifdef _WIN32\n    default_lan_mode = "TCP";\n#endif\n'
     '#ifdef __wasi__\n    default_lan_mode = "NONE";'
     '  // web/bot: no sockets in browser; force server_n=0 (noop)\n#endif'),
    # tcp bind/connect: guarded (sync_server_noop has no bind/connect).
    ('      if (server_n == 1) {\n'
     '        tcp_server.bind(listen_hostname.c_str(), listen_port);\n'
     '        tcp_server.connect(connect_hostname.c_str(), connect_port);\n'
     '      } else if (server_n == 2) {\n#ifndef _WIN32',
     '      if (server_n == 1) {\n#ifndef __wasi__\n'
     '        tcp_server.bind(listen_hostname.c_str(), listen_port);\n'
     '        tcp_server.connect(connect_hostname.c_str(), connect_port);\n'
     '#endif\n      } else if (server_n == 2) {\n'
     '#if !defined(_WIN32) && !defined(__wasi__)'),
    # FD/FILE server branch: posix-only.
    ('      } else if (server_n == 3) {\n#ifndef _WIN32\n        int fd_read = -1;',
     '      } else if (server_n == 3) {\n'
     '#if !defined(_WIN32) && !defined(__wasi__)\n        int fd_read = -1;'),
])

# --- Port 2: throws -> bwapi_fatal (server is built -fno-exceptions) ----------
throw_re = re.compile(r'throw std::runtime_error\(')
for rel in ("BWAPI/Source", "OpenBWData"):
    for root, _, files in os.walk(os.path.join(BWAPI, rel)):
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            p = os.path.join(root, fn)
            s = open(p).read()
            # skip commented-out throws; only rewrite active statements.
            new = throw_re.sub("bwapi_fatal(", s)
            if new != s:
                open(p, "w").write(new)
                print("throws->fatal", os.path.relpath(p, HERE))

# --- Port 3: OpenBWData left two Game:: methods throwing that BWAPI's Server.cpp
#     reads EVERY frame (getRandomSeed, countdownTimer) to fill the shared
#     GameData. Give them sane deterministic values. (Runs after Port 2, so the
#     bodies are already bwapi_fatal(...).) The other replay-only stubs
#     (ReplayVision etc.) stay fatal — they're never reached in melee play. ---
patch(bwdata, [
    ('u32 Game::ReplayHead_gameSeed_randSeed() const\n{\n'
     '  bwapi_fatal("ReplayHead_gameSeed_randSeed");\n}',
     'u32 Game::ReplayHead_gameSeed_randSeed() const\n{\n'
     '  return 42;  // web/bot: OpenBW seeds its LCG to a fixed 42 (read per-frame)\n}'),
    ('int Game::countdownTimer() const\n{\n'
     '  bwapi_fatal("countdownTimer?");\n}',
     'int Game::countdownTimer() const\n{\n'
     '  return 0;  // web/bot: no countdown in melee (read per-frame by Server)\n}'),
])

# --- Port 4: external-state game (the web sandbox integration). Let a BW::Game
#     wrap an already-set-up bwgame::state instead of BWData owning/loading one, so
#     the bot reads the sandbox's authoritative sim and its orders are captured
#     (not fed to the sync server). See web/bot/bot_view.cpp. ---
patch(bwdata, [
    # openbwapi_impl: bot's slot + an order-capture sink.
    ('  int screen_x = 0;\n  int screen_y = 0;\n',
     '  int screen_x = 0;\n  int screen_y = 0;\n'
     '  int external_local_player = -1;  // web/bot: bot slot when wrapping external state\n'
     '  std::function<void(const uint8_t*, size_t)> command_sink;  // web/bot: capture orders\n'),
    # g_LocalHumanID: return the bot's slot in external mode (no sync local_client).
    ('int Game::g_LocalHumanID() const {\n'
     '  return impl->sync_funcs.sync_st.local_client->player_slot;\n}',
     'int Game::g_LocalHumanID() const {\n'
     '  if (impl->external_local_player >= 0) return impl->external_local_player;  // web/bot\n'
     '  return impl->sync_funcs.sync_st.local_client->player_slot;\n}'),
    # QueueCommand: route the bot's BW command bytes to the sink if one is set.
    ('void Game::QueueCommand(const void* buf, size_t size)\n{\n'
     '  if (!impl->vars.is_replay) impl->game_setup_helper.input_action((const uint8_t*)buf, size);\n}',
     'void Game::QueueCommand(const void* buf, size_t size)\n{\n'
     '  if (impl->command_sink) { impl->command_sink((const uint8_t*)buf, size); return; }  // web/bot\n'
     '  if (!impl->vars.is_replay) impl->game_setup_helper.input_action((const uint8_t*)buf, size);\n}'),
    # Factory + sink setter, appended after GameOwner::getGame().
    ('Game GameOwner::getGame()\n{\n  return {&impl->impl};\n}',
     'Game GameOwner::getGame()\n{\n  return {&impl->impl};\n}\n\n'
     '// web/bot: wrap an already-set-up bwgame::state (the web sandbox\'s) so a bot can\n'
     '// read it and issue orders WITHOUT BWData owning/advancing the game. nextFrame() is\n'
     '// never called here (the host advances the sim), so action/replay/sync stay unused;\n'
     '// g_LocalHumanID() returns localPlayerSlot and QueueCommand routes to a sink.\n'
     'struct ExternalGameOwner {\n'
     '  bwgame::action_state action_st;\n'
     '  bwgame::replay_state replay_st;\n'
     '  bwgame::sync_state sync_st;\n'
     '  game_vars vars;\n'
     '  openbwapi_impl impl;\n'
     '  ExternalGameOwner(bwgame::state& st, int localPlayerSlot)\n'
     '    : impl(vars, st, action_st, replay_st, sync_st) {\n'
     '    impl.external_local_player = localPlayerSlot;\n'
     '    vars.local_player_id = localPlayerSlot;\n'
     '    vars.game_type = 2;\n'
     '    vars.game_type_melee = true;\n'
     '    vars.is_replay = false;\n'
     '  }\n'
     '};\n\n'
     'Game makeExternalGame(void* bwgame_state_ptr, int localPlayerSlot) {\n'
     '  auto* owner = new ExternalGameOwner(*(bwgame::state*)bwgame_state_ptr, localPlayerSlot);\n'
     '  return { &owner->impl };\n'
     '}\n\n'
     'void Game::setCommandSink(std::function<void(const uint8_t*, size_t)> f) {\n'
     '  impl->command_sink = std::move(f);\n'
     '}'),
])

# BWData.h: declare the two new API pieces.
patch(bwh, [
    ('  void QueueCommand(const void* buf, size_t size);',
     '  void QueueCommand(const void* buf, size_t size);\n'
     '  void setCommandSink(std::function<void(const uint8_t*, size_t)> f);  // web/bot'),
    ('  Game getGame();\n'
     '  void setPrintTextCallback(std::function<void(const char*)> func);\n};',
     '  Game getGame();\n'
     '  void setPrintTextCallback(std::function<void(const char*)> func);\n};\n\n'
     '// web/bot: build a Game over an externally-owned bwgame::state* (see BWData.cpp).\n'
     'Game makeExternalGame(void* bwgame_state_ptr, int localPlayerSlot);'),
])

# --- Port 5: BWData.cpp defines bwgame::ui::log_str/fatal_error_str, but so does
#     the web sandbox (wasm_main.cpp). Make BWData's WEAK so the app's win in the
#     combined build; standalone (bot_main.cpp) still gets these as the definition. ---
patch(bwdata, [
    ('void log_str(a_string str) {\n  printf("%s", str.c_str());',
     '__attribute__((weak)) void log_str(a_string str) {\n  printf("%s", str.c_str());'),
    ('void fatal_error_str(a_string str){\n  bwgame::error("%s", str);',
     '__attribute__((weak)) void fatal_error_str(a_string str){\n  bwgame::error("%s", str);'),
])

print("patch-vendor: done")
