// Headless bot reactor for wasm: OpenBW + BWAPI server + a statically-linked bot,
// exposed as init/step exports (no main loop). Mirrors BWAPILauncher/Main.cpp.
//
// The one wasm-specific move is STATIC module registration. The launcher dlopen's
// the bot and looks up gameInit/newAIModule; we can't dlopen in wasm, but the
// BWAPI server has a hook: GameImpl::specifiedModule (BWAPI/Source/GameUpdate.cpp
// ~356 -> `this->client = this->specifiedModule`). Setting it before startGame
// makes the server use our bot directly. Same address space, so the bot shares
// the global BWAPI::BroodwarPtr — no gameInit needed.
//
// This is the bot-vs-idle / bot-vs-bot runner. The in-page human-vs-bot variant
// reuses this but constructs the BWAPI game over the SANDBOX's game_state instead
// of GameOwner's (BWData holds `bwgame::state&`, so it can wrap an external one),
// and drives player 1 through sandbox.h as today. See README "Remaining work".

#include "BWAPI/GameImpl.h"
#include "BW/BWData.h"
#include <BWAPI.h>
#include "ZZZKBotAIModule.h"   // swap per bot

namespace {
BW::GameOwner* g_owner = nullptr;
BWAPI::BroodwarImpl_handle* g_h = nullptr;
}

extern "C" {

// Create the game, register the bot statically, and start the match.
__attribute__((export_name("openbw_bot_init")))
void openbw_bot_init() {
  g_owner = new BW::GameOwner();
  g_h = new BWAPI::BroodwarImpl_handle(g_owner->getGame());   // sets BWAPI::BroodwarPtr
  // Static registration: no dlopen, no gameInit — the bot shares BroodwarPtr.
  (*g_h)->specifiedModule = new ZZZKBotAIModule();
  // Map / race / players still come from config (autoMenuManager). For wasm we
  // set those fields directly instead of a bwapi.ini file — TODO once wired to
  // the page's map picker.
  (*g_h)->autoMenuManager.startGame();
}

// Advance one frame: run the bot's onFrame (via update) then step the sim.
// Returns 0 when the game is over.
__attribute__((export_name("openbw_bot_step")))
int openbw_bot_step() {
  if (!g_h || (*g_h)->bwgame.gameOver()) return 0;
  (*g_h)->update();            // fires the bot's onFrame; applies its commands
  (*g_h)->bwgame.nextFrame();  // advance bwgame one frame
  return 1;
}

__attribute__((export_name("openbw_bot_frame")))
int openbw_bot_frame() { return g_h ? (int)(*g_h)->bwgame.getFrameCount() : 0; }

} // extern "C"
