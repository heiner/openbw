// web/bot/bot_view.cpp — the "one sim, two views" glue for human-vs-bot.
//
// The web sandbox (web/sandbox.h) stays the single authoritative simulation: it
// loads the map/MPQs, sets up an N-player melee, renders, and advances the sim.
// This file bolts a bot onto player `slot` WITHOUT a second sim: it constructs a
// BWAPI server as a read-only VIEW over the sandbox's own bwgame::state (via
// BW::makeExternalGame), runs the bot's onFrame each frame, and captures the BW
// command bytes the bot issues. Those bytes are framed exactly like the sandbox's
// own (bw_cmd: [u16 len][opcode][payload]) and drained by JS, which feeds them to
// the lockstep as slot `slot`'s turn — the same path a remote peer's commands take.
//
// So the bot is just another command producer. No shared-determinism problem (one
// state), no separate data load (it reads the sandbox's loaded game), and replays
// still record every action because the bytes go through play_ui::apply_commands.

#include "BWAPI/GameImpl.h"
#include "BW/BWData.h"
#include <BWAPI.h>
#include "ZZZKBotAIModule.h"   // swap per bot

#include <vector>
#include <cstdint>

namespace {
BWAPI::BroodwarImpl_handle* g_h = nullptr;
std::vector<uint8_t> g_out;     // this frame's bot commands, framed for apply_bw_commands
int g_slot = -1;
}

extern "C" {

// Attach the read-view. `state_ptr` is the sandbox's bwgame::state* (passed opaque
// so this TU needn't include the engine); `slot` is the player the bot controls.
// Called from wasm_main.cpp's openbw_bot_attach export (which has g_ui's state).
void bot_view_attach(void* state_ptr, int slot) {
  if (g_h || !state_ptr) return;
  g_slot = slot;
  BW::Game game = BW::makeExternalGame(state_ptr, slot);
  g_h = new BWAPI::BroodwarImpl_handle(game);
  (*g_h)->specifiedModule = new ZZZKBotAIModule();
  // Capture the bot's orders instead of feeding BWData's sync server. Frame each
  // command [u16 len][bytes] so it drops straight into apply_bw_commands.
  (*g_h)->bwgame.setCommandSink([](const uint8_t* buf, size_t n) {
    g_out.push_back((uint8_t)(n & 0xff));
    g_out.push_back((uint8_t)((n >> 8) & 0xff));
    g_out.insert(g_out.end(), buf, buf + n);
  });
}

// Run the bot's onFrame for the current sim frame (fires onGameStart the first
// time). Orders land in g_out. Call once per sim frame, before the frame steps.
__attribute__((export_name("openbw_bot_tick")))
void openbw_bot_tick() { if (g_h) (*g_h)->update(); }

// Drain this frame's framed bot commands (JS applies them as slot g_slot).
__attribute__((export_name("openbw_bot_out_ptr")))
const uint8_t* openbw_bot_out_ptr() { return g_out.empty() ? nullptr : g_out.data(); }
__attribute__((export_name("openbw_bot_out_len")))
int openbw_bot_out_len() { return (int)g_out.size(); }
__attribute__((export_name("openbw_bot_out_clear")))
void openbw_bot_out_clear() { g_out.clear(); }

// Debug: the bot player's supply used — a cheap proof the bot is developing.
__attribute__((export_name("openbw_bot_supply")))
int openbw_bot_supply() {
  if (!g_h || !BWAPI::BroodwarPtr) return -1;
  BWAPI::Player self = BWAPI::Broodwar->self();
  if (!self) return -1;
  return self->supplyUsed();
}

} // extern "C"
