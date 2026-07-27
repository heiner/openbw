// Windows-CRT shims for bots ported to OpenBW/wasm. Force-included by the build
// (-include). Only what the tested bots reference; extend per bot as needed.
#pragma once
#include <ctime>

typedef int errno_t;

// Windows' localtime_s(tm*, time_t*) in terms of POSIX localtime_r(time_t*, tm*).
static inline errno_t localtime_s(struct tm* out, const time_t* t) {
  return localtime_r(t, out) ? 0 : 1;
}
