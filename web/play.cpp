// web/play.cpp — OpenBW single-player sandbox, native (SDL) frontend.
//
// Single player, local, no AI and no networking: a human is dropped onto a
// melee map with the normal starting units and can pan/select, issue orders,
// and train/build (see web/sandbox.h for the shared gameplay + input layer).
// This native build is the fast-iteration harness; the browser build
// (web/wasm_main.cpp) reuses the same sandbox.h behind a <canvas> backend.
//
// Usage: openbw_play <data_dir> <map_file> [race] [player_slot]
//
// Env:
//   OPENBW_HEADLESS=<n>       run n frames headless, print diagnostics
//   OPENBW_SELFTEST=1         scripted train/build verification
//   OPENBW_SCREENSHOT=<ppm>   render one frame to a PPM (no display)
//   OPENBW_DEMO=1             with SCREENSHOT: script a worker + building

#include "sandbox.h"

#include <chrono>
#include <thread>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace bwgame;

// --- logging sinks required by ui/common.h -------------------------------
namespace bwgame {
namespace ui {
void log_str(a_string str) {
	fwrite(str.data(), str.size(), 1, stdout);
	fflush(stdout);
}
void fatal_error_str(a_string str) {
	fprintf(stderr, "fatal error: %s\n", str.c_str());
	std::terminate();
}
}
}

namespace {

// Headless verification: no window, no SDL, no rendering — just build the
// game and step the simulation, reporting that units spawned and the sim
// advances without crashing.
int run_headless(const char* data_dir, const char* map_file, int my_player,
                 race_t my_race, int frames) {
	auto load_data_file = data_loading::data_files_directory(data_dir);
	game_player player(load_data_file);
	data_loading::mpq_file<> map_loader(map_file);
	setup_melee(player.st(), map_loader, my_player, my_race);

	state& st = player.st();
	ui::log("headless: map '%s' %dx%d, slot %d, units at frame 0 = %d, minerals = %d\n",
		st.game->scenario_name, (int)st.game->map_width, (int)st.game->map_height,
		my_player, count_units(st, my_player), (int)st.current_minerals[my_player]);

	for (int i = 0; i != frames; ++i) player.next_frame();

	ui::log("headless: after %d frames: current_frame=%d, units=%d, minerals=%d\n",
		frames, (int)st.current_frame, count_units(st, my_player),
		(int)st.current_minerals[my_player]);
	ui::log("headless: OK\n");
	return 0;
}

int run_windowed(const char* data_dir, const char* map_file, int my_player, race_t my_race) {
	const size_t screen_width = 1280;
	const size_t screen_height = 800;
	auto clock = std::chrono::high_resolution_clock();
	auto load_start = clock.now();

	auto load_data_file = data_loading::data_files_directory(data_dir);
	game_player player(load_data_file);
	play_ui ui(std::move(player), my_player, my_race);
	ui.exit_on_close = false;                 // we own the shutdown path
	ui.load_data_file = [&](a_vector<uint8_t>& data, a_string filename) {
		load_data_file(data, std::move(filename));
	};
	ui.init();                                // sounds + all image/tileset data

	data_loading::mpq_file<> map_loader(map_file);
	setup_melee(ui.player.st(), map_loader, my_player, my_race);

	ui.wnd.create("OpenBW", 0, 0, (int)screen_width, (int)screen_height);
	ui.resize((int)screen_width, (int)screen_height);
	ui.set_image_data();
	xy start = ui.game_st.start_locations[my_player];
	ui.screen_pos = start - xy((int)screen_width / 2, (int)screen_height / 2);

	ui::log("loaded in %dms — map '%s', %dx%d, playing slot %d\n",
		(int)std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - load_start).count(),
		ui.game_st.scenario_name, (int)ui.game_st.map_width, (int)ui.game_st.map_height, my_player);

	// Advance the simulation in real time at "fastest" (~42ms per logical
	// frame). state_functions::next_frame() is the core sim step — NOT
	// replay_functions::next_frame, which would replay an (empty) action buffer.
	const auto frame_time = std::chrono::milliseconds(42);
	auto last_frame = clock.now();
	while (!ui.window_closed) {
		auto now = clock.now();
		int steps = 0;
		while (now - last_frame >= frame_time && steps < 8) {
			last_frame += frame_time;
			++steps;
			ui.state_functions::next_frame();
		}
		ui.update();                          // render + camera/selection input
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	return 0;
}

// Scripted economy actions for a visual demo (OPENBW_DEMO): train a worker and
// start a production building near the base, using the same action_* calls the
// interactive keys use.
void script_econ_demo(play_ui& ui) {
	state& st = ui.player.st();
	int p = ui.my_player;
	if (ui.my_race == race_t::zerg) return;
	st.current_minerals[p] = 1000;
	UnitTypes townhall = ui.my_race == race_t::protoss ? UnitTypes::Protoss_Nexus
	                                                   : UnitTypes::Terran_Command_Center;
	if (unit_t* th = find_unit_of_type(st, p, townhall)) {
		ui.action_select(p, th);
		ui.action_train(p, ui.get_unit_type(ui.kit.worker));
	}
	if (unit_t* w = find_unit_of_type(st, p, ui.kit.worker))
		try_build_near(ui, p, ui.kit, w, st.game->start_locations[p]);
}

// Headless render: build the game, warm it up a few frames, render one frame
// into an in-memory RGBA surface (no window/display needed) and dump it as a
// binary PPM. Proves the whole data->map->sprite->pixel pipeline off-screen.
int run_screenshot(const char* data_dir, const char* map_file, int my_player,
                   race_t my_race, const char* out_path, int warmup_frames) {
	const int W = 1280, H = 800;
	auto load_data_file = data_loading::data_files_directory(data_dir);
	game_player player(load_data_file);
	play_ui ui(std::move(player), my_player, my_race);
	ui.create_window = false;                 // render to memory only
	ui.draw_ui_elements = getenv("OPENBW_UI") != nullptr;   // minimap/UI if requested
	ui.load_data_file = [&](a_vector<uint8_t>& data, a_string filename) {
		load_data_file(data, std::move(filename));
	};
	ui.init();

	data_loading::mpq_file<> map_loader(map_file);
	setup_melee(ui.player.st(), map_loader, my_player, my_race);

	ui.resize(W, H);
	ui.set_image_data();
	xy start = ui.game_st.start_locations[my_player];
	ui.screen_pos = start - xy(W / 2, H / 2);

	if (getenv("OPENBW_DEMO")) script_econ_demo(ui);

	for (int i = 0; i != warmup_frames; ++i) ui.state_functions::next_frame();
	ui.update();                              // draw into rgba_surface

	int pitch = 0, height = 0; uint32_t* px = nullptr;
	std::tie(pitch, height, px) = ui.get_rgba_buffer();

	FILE* f = fopen(out_path, "wb");
	if (!f) { fprintf(stderr, "screenshot: cannot open %s\n", out_path); return 1; }
	fprintf(f, "P6\n%d %d\n255\n", W, height);
	for (int y = 0; y != height; ++y) {
		for (int x = 0; x != W; ++x) {
			uint32_t p = px[y * pitch + x];   // little-endian: R,G,B,A
			uint8_t rgb[3] = { (uint8_t)(p & 0xff), (uint8_t)((p >> 8) & 0xff), (uint8_t)((p >> 16) & 0xff) };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	ui::log("screenshot: wrote %s (%dx%d, %d warmup frames)\n", out_path, W, height, warmup_frames);
	return 0;
}

// Scripted headless verification: train a worker and place a building through
// the action_* API, confirming the sim carries them out.
int run_selftest(const char* data_dir, const char* map_file, int my_player, race_t my_race) {
	auto load_data_file = data_loading::data_files_directory(data_dir);
	game_player player(load_data_file);
	data_loading::mpq_file<> map_loader(map_file);
	setup_melee(player.st(), map_loader, my_player, my_race);

	state& st = player.st();
	action_state action_st;
	action_functions af(st, action_st);
	build_kit kit = kit_for(my_race);

	st.current_minerals[my_player] = 1000;   // enough to exercise both actions

	// --- train / morph a worker ---
	int workers0 = count_units_of_type(st, my_player, kit.worker);
	int min0 = (int)st.current_minerals[my_player];
	if (my_race == race_t::zerg) {
		if (unit_t* larva = find_unit_of_type(st, my_player, UnitTypes::Zerg_Larva)) {
			af.action_select(my_player, larva);
			af.action_morph(my_player, af.get_unit_type(kit.worker));
		}
	} else {
		UnitTypes townhall = my_race == race_t::protoss ? UnitTypes::Protoss_Nexus
		                                                : UnitTypes::Terran_Command_Center;
		if (unit_t* th = find_unit_of_type(st, my_player, townhall)) {
			af.action_select(my_player, th);
			af.action_train(my_player, af.get_unit_type(kit.worker));
		}
	}
	ui::log("selftest: train worker (unit %d): minerals %d -> %d\n",
		(int)kit.worker, min0, (int)st.current_minerals[my_player]);

	// --- build a production building with a worker (Terran/Protoss) ---
	int building_before = count_units_of_type(st, my_player, kit.production);
	bool placed = false;
	if (my_race != race_t::zerg) {
		if (unit_t* worker = find_unit_of_type(st, my_player, kit.worker))
			placed = try_build_near(af, my_player, kit, worker, st.game->start_locations[my_player]);
	}
	ui::log("selftest: build placement: %s\n", placed ? "found buildable tile" : "NO buildable tile near base");

	for (int i = 0; i != 600; ++i) player.next_frame();

	ui::log("selftest: after 600 frames: workers %d -> %d, building(unit %d) %d -> %d, total units %d\n",
		workers0, count_units_of_type(st, my_player, kit.worker), (int)kit.production,
		building_before, count_units_of_type(st, my_player, kit.production), count_units(st, my_player));
	ui::log("selftest: done\n");
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	const char* data_dir = argc > 1 ? argv[1] : "data";
	const char* map_file = argc > 2 ? argv[2] : nullptr;
	race_t my_race = parse_race(argc > 3 ? argv[3] : "terran");
	int my_player = argc > 4 ? atoi(argv[4]) : 0;

	if (!map_file) {
		fprintf(stderr, "usage: %s <data_dir> <map_file> [zerg|terran|protoss] [slot]\n", argv[0]);
		return 1;
	}

	if (getenv("OPENBW_SELFTEST")) return run_selftest(data_dir, map_file, my_player, my_race);

	const char* shot = getenv("OPENBW_SCREENSHOT");
	if (shot) {
		const char* wf = getenv("OPENBW_HEADLESS");
		return run_screenshot(data_dir, map_file, my_player, my_race, shot, wf ? atoi(wf) : 48);
	}
	const char* headless = getenv("OPENBW_HEADLESS");
	if (headless) return run_headless(data_dir, map_file, my_player, my_race, atoi(headless));
	return run_windowed(data_dir, map_file, my_player, my_race);
}
