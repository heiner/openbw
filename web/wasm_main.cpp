// web/wasm_main.cpp — browser (wasm) entry point for the single-player sandbox.
//
// Reactor-model module: JS calls _initialize() once, then openbw_init() after
// the MPQ bytes are available, then openbw_step() (timer) + openbw_render()
// (animation frame). File
// bytes come from JS through the js_read_data / js_file_size imports (the same
// pattern the old Emscripten build used); rendering goes to the framebuffer in
// web/wasm_backend.cpp, which JS blits to a <canvas>.

#include "sandbox.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>

using namespace bwgame;

// --- imports provided by the JS host ------------------------------------
extern "C" {
__attribute__((import_module("env"), import_name("js_read_data")))
void js_read_data(int index, void* dst, unsigned offset, unsigned n);
__attribute__((import_module("env"), import_name("js_file_size")))
unsigned js_file_size(int index);
}

// --- logging sinks required by ui/common.h ------------------------------
namespace bwgame {
namespace ui {
void log_str(a_string str) { fwrite(str.data(), str.size(), 1, stdout); fflush(stdout); }
void fatal_error_str(a_string str) { fprintf(stderr, "openbw fatal: %s\n", str.c_str()); abort(); }
}
}

// --- file reader bridged to JS ------------------------------------------
// Archive index mapping (JS must provide matching bytes):
//   0 StarDat.mpq   1 BrooDat.mpq   2 Patch_rt.mpq   3 the map (.scx/.scm)
namespace bwgame {
namespace data_loading {

template<bool default_little_endian = true>
struct js_file_reader {
	size_t index = 3;
	size_t file_pointer = 0;
	js_file_reader() = default;
	explicit js_file_reader(a_string filename) { open(std::move(filename)); }
	void open(a_string filename) {
		a_string low;
		for (char c : filename) low += (char)std::tolower((unsigned char)c);
		auto has = [&](const char* s) { return low.find(s) != a_string::npos; };
		if (has("stardat")) index = 0;
		else if (has("brood")) index = 1;
		else if (has("patch")) index = 2;
		else index = 3;   // the map archive
	}
	void get_bytes(uint8_t* dst, size_t n) {
		js_read_data((int)index, dst, (unsigned)file_pointer, (unsigned)n);
		file_pointer += n;
	}
	void seek(size_t offset) { file_pointer = offset; }
	size_t tell() const { return file_pointer; }
	size_t size() { return js_file_size((int)index); }
};

}
}

// --- game globals -------------------------------------------------------
namespace {
using loader_t = data_loading::data_files_loader<data_loading::mpq_file<data_loading::js_file_reader<>>>;
loader_t* g_loader = nullptr;
play_ui* g_ui = nullptr;
}

#define OPENBW_EXPORT(name) __attribute__((export_name(#name))) extern "C"

// Build the game with `n` occupied slots. Every peer must pass an identical slot list (and
// the same map) or their initial states differ and lockstep is broken from frame 0.
static void init_game(int width, int height, int my_slot, const mp_slot* slots, size_t n) {
	g_loader = new loader_t(data_loading::data_files_directory<loader_t>(""));

	race_t my_race = race_t::terran;
	for (size_t k = 0; k != n; ++k) if (slots[k].slot == my_slot) my_race = slots[k].race;

	game_player player(*g_loader);
	g_ui = new play_ui(std::move(player), my_slot, my_race);
	g_ui->exit_on_close = false;
	g_ui->competitive = n > 1;   // victory/defeat only matters with an opponent
	g_ui->load_data_file = [](a_vector<uint8_t>& data, a_string filename) {
		(*g_loader)(data, std::move(filename));
	};
	g_ui->init();

	uint32_t rep_seed0 = 0;   // filled in at the map's setup point (see setup_melee_slots)
	data_loading::mpq_file<data_loading::js_file_reader<>> map_loader("openbw.map");
	// Tee the map's CHK out of the loader: a .rep embeds it verbatim, and it's the only
	// part of the replay we can't reconstruct from the sim afterwards.
	setup_melee_slots(g_ui->player.st(), [&](a_vector<uint8_t>& out, a_string fn) {
		map_loader(out, fn);
		a_string low;
		for (char c : fn) low += (char)std::tolower((unsigned char)c);
		if (low.find("scenario.chk") != a_string::npos) g_ui->rep_map = out;
	}, slots, n, &rep_seed0);

	g_ui->wnd.create("OpenBW Web", 0, 0, width, height);
	g_ui->resize(width, height);
	g_ui->set_image_data();
	xy start = g_ui->game_st.start_locations[my_slot];
	g_ui->screen_pos = start - xy(width / 2, height / 2);

	// Seed the replay header from the loaded game. Everything here is fixed at start, so
	// the recording only has to accumulate actions from now on.
	{
		state& st = g_ui->player.st();
		replay_saver_state& r = g_ui->rep;
		r.map_data = g_ui->rep_map.data();
		r.map_data_size = g_ui->rep_map.size();
		r.map_tile_width = g_ui->game_st.map_tile_width;
		r.map_tile_height = g_ui->game_st.map_tile_height;
		r.tileset = (int)g_ui->game_st.tileset_index;
		r.active_player_count = (int)n;
		r.slot_count = (int)n;
		r.game_speed = 6;                 // "fastest", matching our default step rate
		r.game_type = 2;                  // melee
		r.random_seed = rep_seed0;
		r.game_name = "OpenBW Web";
		r.map_name = g_ui->game_st.scenario_name;
		r.player_name = "Player";
		// Must mirror setup_melee_slots, or a replayer rebuilds a different game.
		r.setup_info.victory_condition = 1;
		r.setup_info.tournament_mode = 0;
		r.setup_info.starting_units = 0;
		r.setup_info.resource_type = 1;
		r.setup_info.starting_minerals = 50;
		for (size_t i = 0; i != 12; ++i) {
			r.players[i] = st.players[i];
			r.setup_info.create_melee_units_for_player[i] = st.players[i].controller == player_t::controller_occupied;
		}
		for (size_t k = 0; k != n; ++k)
			r.player_names[slots[k].slot] = (int)k == my_slot ? "You" : "Opponent";
		g_ui->rep_on = g_ui->rep_map.size() != 0;
	}

	ui::log("openbw_init: map '%s' %dx%d, slot %d of %d, units=%d, fog_player=%d\n",
		g_ui->game_st.scenario_name, (int)g_ui->game_st.map_width,
		(int)g_ui->game_st.map_height, my_slot, (int)n,
		count_units(g_ui->player.st(), my_slot), g_ui->fog_player);
}

// Single-player. Call after all four archives' bytes are available in JS.
OPENBW_EXPORT(openbw_init) void openbw_init(int width, int height, int race, int slot) {
	mp_slot s{slot, (race_t)race};
	init_game(width, height, slot, &s, 1);
}

// Resize the render target (e.g. when the browser window changes). Recreates
// the framebuffer at the new size; ui.resize() rebuilds the surfaces.
OPENBW_EXPORT(openbw_resize) void openbw_resize(int width, int height) {
	if (!g_ui) return;
	g_ui->wnd.destroy();
	g_ui->wnd.create("OpenBW Web", 0, 0, width, height);
	g_ui->resize(width, height);
}

// Nudge the camera by (dx, dy) map pixels (e.g. from a trackpad/wheel scroll).
// ui.update() clamps screen_pos to the map each frame.
OPENBW_EXPORT(openbw_pan) void openbw_pan(int dx, int dy) {
	if (g_ui) g_ui->screen_pos += xy(dx, dy);
}

// One simulation frame. JS drives this from a fixed-interval timer, so it keeps
// ticking (throttled) while the tab is backgrounded and never fast-forwards a
// backlog on return — unlike advancing by elapsed wall-clock time.
OPENBW_EXPORT(openbw_step) void openbw_step() {
	if (g_ui) g_ui->state_functions::next_frame();
}

// Render the current state into the framebuffer and process input. JS drives
// this from requestAnimationFrame, passing performance.now() so time-based UI
// effects (the target-ring blink, the click marker) are refresh-rate-independent
// — a frame counter would run twice as fast on a 120 Hz display.
OPENBW_EXPORT(openbw_render) void openbw_render(double now_ms) {
	if (g_ui) { g_ui->ui_now = now_ms; g_ui->update(); }
}

// The current command card as "title\nKEY\tLabel\tEN\n…" — the JS host renders it.
OPENBW_EXPORT(openbw_card) const char* openbw_card() {
	return g_ui ? g_ui->card_text.c_str() : "";
}

// Producer status "<progress%>\t<name>…" (empty if not training) — rebuilt per call.
OPENBW_EXPORT(openbw_status) const char* openbw_status() {
	return g_ui ? g_ui->build_status() : "";
}

// "minerals\tgas\tsupply_used\tsupply_max" for the resource HUD.
OPENBW_EXPORT(openbw_resources) const char* openbw_resources() {
	return g_ui ? g_ui->resources() : "";
}

// Pointer mode so JS can pick a cursor: 0 normal, 1 targeting, 2 placing.
OPENBW_EXPORT(openbw_cursor) int openbw_cursor() {
	return g_ui ? g_ui->cursor() : 0;
}

// Render a command-button icon (frame == unit id) to RGBA; returns a pointer into
// wasm memory valid until the next call, with the size in openbw_icon_w/h.
OPENBW_EXPORT(openbw_icon) const uint8_t* openbw_icon(int frame) {
	return g_ui ? g_ui->render_icon(frame) : nullptr;
}
OPENBW_EXPORT(openbw_icon_w) int openbw_icon_w() { return g_ui ? g_ui->icon_w : 0; }
OPENBW_EXPORT(openbw_icon_h) int openbw_icon_h() { return g_ui ? g_ui->icon_h : 0; }

// Resource-HUD icon (0 = minerals, 1 = gas) to RGBA; same size convention as openbw_icon.
OPENBW_EXPORT(openbw_res_icon) const uint8_t* openbw_res_icon(int which) {
	return g_ui ? g_ui->render_res_icon(which) : nullptr;
}

// Production progress bar (percent, target width) rendered like the in-world health bar,
// as RGBA; actual size in openbw_progress_bar_w/h.
OPENBW_EXPORT(openbw_progress_bar) const uint8_t* openbw_progress_bar(int percent, int width) {
	return g_ui ? g_ui->render_progress_bar(percent, width) : nullptr;
}
OPENBW_EXPORT(openbw_progress_bar_w) int openbw_progress_bar_w() { return g_ui ? g_ui->pbar_w : 0; }
OPENBW_EXPORT(openbw_progress_bar_h) int openbw_progress_bar_h() { return g_ui ? g_ui->pbar_h : 0; }

// Edge-scroll direction for the cursor: 0 none, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SW, 7 W, 8 NW.
OPENBW_EXPORT(openbw_edge) int openbw_edge() { return g_ui ? g_ui->edge_dir : 0; }

// Last blocked-command reason as "seq\tmessage" — the host shows a toast when seq changes.
OPENBW_EXPORT(openbw_error) const char* openbw_error() { return g_ui ? g_ui->error_status() : ""; }

// Cancel the build-queue slot a status chip was clicked on (0 = the one in progress).
OPENBW_EXPORT(openbw_cancel) void openbw_cancel(int slot) { if (g_ui) g_ui->cancel_queue_slot(slot); }

// Toggle the selected-unit order/rally line overlay (off by default).
OPENBW_EXPORT(openbw_set_order_lines) void openbw_set_order_lines(int on) {
	if (g_ui) g_ui->show_order_lines = on != 0;
}

// --- deterministic command stream (lockstep multiplayer) --------------------
// Local input is serialised to BW command bytes rather than applied directly. The host
// drains them each frame, schedules them for a later frame, ships them to peers, and every
// peer applies the identical stream via openbw_apply — keeping all sims bit-identical.
// The buffer holds framed records: [u16 len][opcode][payload].

OPENBW_EXPORT(openbw_out_ptr) const uint8_t* openbw_out_ptr() {
	return g_ui && !g_ui->outgoing.empty() ? g_ui->outgoing.data() : nullptr;
}
OPENBW_EXPORT(openbw_out_len) int openbw_out_len() { return g_ui ? (int)g_ui->outgoing.size() : 0; }
OPENBW_EXPORT(openbw_out_clear) void openbw_out_clear() { if (g_ui) g_ui->outgoing.clear(); }

// Scratch buffer the host writes a peer's batch into before calling openbw_apply.
namespace { a_vector<uint8_t> g_inbuf; }
OPENBW_EXPORT(openbw_in_ptr) uint8_t* openbw_in_ptr(int len) {
	if (len < 0) len = 0;
	g_inbuf.resize((size_t)len);
	return g_inbuf.empty() ? nullptr : g_inbuf.data();
}
// Apply `len` bytes of framed commands as `owner`. Must be called on every peer for every
// player, in the same order, on the same frame.
OPENBW_EXPORT(openbw_apply) void openbw_apply(int owner, int len) {
	if (g_ui && len > 0 && (size_t)len <= g_inbuf.size())
		g_ui->apply_commands(owner, g_inbuf.data(), (size_t)len);
}

// Multiplayer init: the host writes the slot list into the openbw_in_ptr scratch buffer as
// int32 pairs [slot, race, slot, race, …] first, then calls this. Every peer must pass an
// identical list and load the same map, or the initial states differ and lockstep breaks
// at frame 0. (int is 32-bit on wasm32, so the pairs read back directly.)
OPENBW_EXPORT(openbw_init_mp) void openbw_init_mp(int width, int height, int my_slot, int n) {
	if (n <= 0 || g_inbuf.size() < (size_t)n * 2 * sizeof(int)) return;
	a_vector<mp_slot> slots;
	const int* p = (const int*)g_inbuf.data();
	for (int i = 0; i != n; ++i) slots.push_back(mp_slot{p[2 * i], (race_t)p[2 * i + 1]});
	init_game(width, height, my_slot, slots.data(), slots.size());
}

#ifdef OPENBW_WITH_BOT
// Computer opponent (web/bot/bot_view.cpp): attach a BWAPI read-view over THIS
// sandbox's live state so a bot can drive player `slot`. Compiled ONLY into the
// vs-computer module (openbw-bot.wasm, built with -DOPENBW_WITH_BOT); the clean
// openbw.wasm never sees this and is byte-identical. Call after openbw_init_mp.
extern "C" void bot_view_attach(void* state_ptr, int slot);
OPENBW_EXPORT(openbw_bot_attach) void openbw_bot_attach(int slot) {
	if (g_ui) bot_view_attach(&g_ui->player.st(), slot);
}
#endif

// 0 undecided, 1 victory, 2 defeat. Both peers derive this from the shared deterministic
// sim, so they agree without exchanging anything.
OPENBW_EXPORT(openbw_outcome) int openbw_outcome() { return g_ui ? g_ui->outcome : 0; }

// Number of start locations on the loaded map. The host picks starting slots before the
// map is parsed (start locations aren't known that early), so it works from a table; this
// lets the host verify that table at runtime instead of trusting it silently.
OPENBW_EXPORT(openbw_start_locations) int openbw_start_locations() {
	if (!g_ui) return 0;
	int n = 0;
	for (size_t i = 0; i != 8; ++i) if (g_ui->game_st.start_locations[i] != xy()) ++n;
	return n;
}

// Loaded-map metadata for the lobby info line + a runtime cross-check of the host's
// table (like openbw_start_locations). Tileset index: 0 Badlands, 1 Space, 2
// Installation, 3 Ashworld, 4 Jungle, 5 Desert, 6 Ice, 7 Twilight.
OPENBW_EXPORT(openbw_map_w) int openbw_map_w() { return g_ui ? (int)g_ui->game_st.map_tile_width : 0; }
OPENBW_EXPORT(openbw_map_h) int openbw_map_h() { return g_ui ? (int)g_ui->game_st.map_tile_height : 0; }
OPENBW_EXPORT(openbw_map_tileset) int openbw_map_tileset() { return g_ui ? (int)g_ui->game_st.tileset_index : 0; }

// Serialise the recorded game as a BW .rep and hand back the bytes. Returns 0 if nothing
// was recorded. The result stays valid until the next call.
OPENBW_EXPORT(openbw_save_replay) const uint8_t* openbw_save_replay() {
	if (!g_ui || !g_ui->rep_on) return nullptr;
	g_ui->rep_out.clear();
	// vector_writer only writes into already-reserved capacity (its left() is
	// capacity - size) and throws otherwise, so size the buffer up front.
	size_t hist = 0;
	for (auto& v : g_ui->rep.history) hist += v.size();
	g_ui->rep_out.reserve(g_ui->rep.map_data_size + hist + (size_t)(1 << 20));
	auto w = data_loading::make_vector_writer(g_ui->rep_out);
	replay_saver_functions(g_ui->rep).save_replay(g_ui->player.st().current_frame, w);
	return g_ui->rep_out.data();
}
OPENBW_EXPORT(openbw_replay_len) int openbw_replay_len() { return g_ui ? (int)g_ui->rep_out.size() : 0; }

// Resign: concede the game. Routed through the command stream so a 1v1 stays in lockstep.
OPENBW_EXPORT(openbw_resign) void openbw_resign() { if (g_ui) g_ui->resign(); }

// End-game score for the game-over screen (see play_ui::stats_text for the format).
OPENBW_EXPORT(openbw_stats) const char* openbw_stats() { return g_ui ? g_ui->stats_text() : ""; }

// Reveal the whole map (fog_player < 0 turns off fog rendering). Render-only, no sim
// effect — used by the game-over "Look at map" so you can review the final board.
OPENBW_EXPORT(openbw_reveal_map) void openbw_reveal_map() { if (g_ui) g_ui->fog_player = -1; }

// Debug: force the outcome (1 victory, 2 defeat) to exercise the game-over screen without
// playing to elimination. Bypasses the competitive gate, so it works in single-player too.
OPENBW_EXPORT(openbw_debug_outcome) void openbw_debug_outcome(int v) { if (g_ui) g_ui->outcome = v; }

OPENBW_EXPORT(openbw_frame) int openbw_frame() { return g_ui ? (int)g_ui->st.current_frame : 0; }

// Desync probe (shared implementation in sandbox.h, so native and wasm can't disagree).
// Peers compare it periodically; any divergence means the sims have drifted.
OPENBW_EXPORT(openbw_checksum) unsigned openbw_checksum() {
	return g_ui ? sim_checksum(g_ui->st) : 0;
}
