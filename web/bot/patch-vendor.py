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
        if new in s and old not in s:
            continue  # already applied
        if old not in s:
            sys.exit(f"{path}: expected snippet not found:\n{old}")
        s = s.replace(old, new, 1)
    open(path, "w").write(s)
    print("patched", os.path.relpath(path, HERE))


# --- Port 1: BWData.cpp networking guards ------------------------------------
bwdata = os.path.join(BWAPI, "OpenBWData", "BW", "BWData.cpp")
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

print("patch-vendor: done")
