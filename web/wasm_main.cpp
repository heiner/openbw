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

// Build the game. Call after all four archives' bytes are available in JS.
OPENBW_EXPORT(openbw_init) void openbw_init(int width, int height, int race, int slot) {
	g_loader = new loader_t(data_loading::data_files_directory<loader_t>(""));

	game_player player(*g_loader);
	g_ui = new play_ui(std::move(player), slot, (race_t)race);
	g_ui->exit_on_close = false;
	g_ui->load_data_file = [](a_vector<uint8_t>& data, a_string filename) {
		(*g_loader)(data, std::move(filename));
	};
	g_ui->init();

	data_loading::mpq_file<data_loading::js_file_reader<>> map_loader("openbw.map");
	setup_melee(g_ui->player.st(), map_loader, slot, (race_t)race);

	g_ui->wnd.create("OpenBW", 0, 0, width, height);
	g_ui->resize(width, height);
	g_ui->set_image_data();
	xy start = g_ui->game_st.start_locations[slot];
	g_ui->screen_pos = start - xy(width / 2, height / 2);

	ui::log("openbw_init: map '%s' %dx%d, slot %d, units=%d, fog_player=%d\n",
		g_ui->game_st.scenario_name, (int)g_ui->game_st.map_width,
		(int)g_ui->game_st.map_height, slot, count_units(g_ui->player.st(), slot),
		g_ui->fog_player);
}

// Resize the render target (e.g. when the browser window changes). Recreates
// the framebuffer at the new size; ui.resize() rebuilds the surfaces.
OPENBW_EXPORT(openbw_resize) void openbw_resize(int width, int height) {
	if (!g_ui) return;
	g_ui->wnd.destroy();
	g_ui->wnd.create("OpenBW", 0, 0, width, height);
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
// this from requestAnimationFrame.
OPENBW_EXPORT(openbw_render) void openbw_render() {
	if (g_ui) g_ui->update();
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

OPENBW_EXPORT(openbw_frame) int openbw_frame() { return g_ui ? (int)g_ui->st.current_frame : 0; }

// Desync probe (shared implementation in sandbox.h, so native and wasm can't disagree).
// Peers compare it periodically; any divergence means the sims have drifted.
OPENBW_EXPORT(openbw_checksum) unsigned openbw_checksum() {
	return g_ui ? sim_checksum(g_ui->st) : 0;
}
