// web/bot: a log sink the browser provides. McRave's Logger writes to a file
// (bwapi-data/write/logger.txt) which doesn't exist in wasm; mcrave.patch routes
// it here instead, and openbw.js prints it to the console in debug mode. Declared
// as an env import the same way as js_file_size / js_read_data (wasm_main.cpp).
#pragma once

extern "C" __attribute__((import_module("env"), import_name("js_bot_log")))
void js_bot_log(const char* ptr, int len);
