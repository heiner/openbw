/* web/bot: no-op dlopen family. OpenBW/bwapi's AIModuleLoader path is compiled
 * but never used on the wasm build — we register the bot statically via
 * GameImpl::specifiedModule (see bot_main.cpp). These stubs satisfy the link. */
void* dlopen(const char* f, int flag) { (void)f; (void)flag; return 0; }
void* dlsym(void* h, const char* s) { (void)h; (void)s; return 0; }
int dlclose(void* h) { (void)h; return 0; }
const char* dlerror(void) { return "dlopen disabled in wasm"; }
