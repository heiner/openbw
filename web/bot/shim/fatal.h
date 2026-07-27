// web/bot: force-included into the BWAPI server TUs so the handful of error
// paths that upstream writes as `throw std::runtime_error(...)` become an
// abort() instead. The wasm build is -fno-exceptions (matching the clean
// openbw.wasm), so these must not throw. vendor.sh rewrites those throws to
// bwapi_fatal(); this declares it.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

[[noreturn]] inline void bwapi_fatal(const std::string& msg) {
  std::fprintf(stderr, "openbw-bot fatal: %s\n", msg.c_str());
  std::abort();
}
