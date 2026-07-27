// Module entry for the ported bot, replacing its Windows Dll.cpp.
//
// gameInit / newAIModule are the symbols OpenBW's launcher/loader looks up (and
// that a static integration calls directly). This file is also where the
// sandbox integration will live — see the TODO at the bottom.
#include <BWAPI.h>
#include "ZZZKBotAIModule.h"   // swap per bot

extern "C" void gameInit(BWAPI::Game* game) { BWAPI::BroodwarPtr = game; }
extern "C" BWAPI::AIModule* newAIModule() { return new ZZZKBotAIModule(); }

// ---------------------------------------------------------------------------
// TODO: sandbox integration — "one game_state, two views".
//
// The headless proof used OpenBW's BWAPILauncher harness (GameOwner +
// BroodwarImpl; loop = h->update() -> bwgame.nextFrame()). For the in-page
// opponent we instead share the sandbox's game_state:
//
//   * sandbox.h keeps driving player 1 (human) exactly as today.
//   * A BroodwarImpl wraps the SAME game_state; each frame, before the sim
//     steps, run the bot's onFrame (player 2) so its commands enter bwgame.
//
// Exports to add for wasm_bot_main (mirroring wasm_main.cpp):
//   openbw_bot_init(slot, race)   set player 2 = bot
//   openbw_bot_frame()            run BroodwarImpl.update() (fires onFrame)
//
// Everything this depends on already compiles for wasm32; this glue is the only
// remaining new code.
// ---------------------------------------------------------------------------
