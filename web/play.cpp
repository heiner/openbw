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
//   OPENBW_NETTEST=<n>        command-byte vs direct-call determinism harness
//   OPENBW_FOGTEST=<n>        assert fog of war is per-player in a 2-player melee

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
			// Input is serialised to command bytes (see bw_cmd), not applied directly, so
			// drain and apply it before stepping — the native equivalent of the browser
			// host's lockstep pump. Without this, orders would silently do nothing.
			if (!ui.outgoing.empty()) {
				ui.apply_commands(my_player, ui.outgoing.data(), ui.outgoing.size());
				ui.outgoing.clear();
			}
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

// Fog probe: set up a 2-player melee and report, per player, how many tiles each has
// explored — and specifically whether each has explored the *other's* start location.
// Fog is per-player only if each sees its own start and not the opponent's.
int run_fogtest(const char* data_dir, const char* map_file, int frames) {
	auto load = data_loading::data_files_directory(data_dir);
	game_player p(load);
	mp_slot slots[2] = { {0, race_t::terran}, {1, race_t::protoss} };
	{ data_loading::mpq_file<> m(map_file); setup_melee_slots(p.st(), m, slots, 2); }
	state& st = p.st();
	for (int i = 0; i != frames; ++i) p.next_frame();

	for (int pl = 0; pl != 2; ++pl) {
		uint8_t mask = (uint8_t)(1 << pl);
		size_t vis = 0, exp = 0, total = st.game->map_tile_width * st.game->map_tile_height;
		for (size_t i = 0; i != total; ++i) {
			if ((st.tiles[i].visible & mask) == 0) ++vis;
			if ((st.tiles[i].explored & mask) == 0) ++exp;
		}
		ui::log("fogtest: player %d: visible=%d explored=%d of %d\n", pl, (int)vis, (int)exp, (int)total);
		for (int other = 0; other != 2; ++other) {
			xy s = st.game->start_locations[other];
			size_t idx = (size_t)(s.y / 32) * st.game->map_tile_width + (s.x / 32);
			ui::log("fogtest:   start loc of player %d -> visible=%d explored=%d\n", other,
			        (st.tiles[idx].visible & mask) == 0, (st.tiles[idx].explored & mask) == 0);
		}
	}
	return 0;
}

// Win/lose probe: wipe out player 1 in a 2-player melee and confirm the engine's melee
// triggers declare player 0 the winner (victory_state >= 3) and player 1 defeated.
int run_wintest(const char* data_dir, const char* map_file, int wipe, int frames) {
	auto load = data_loading::data_files_directory(data_dir);
	game_player p(load);
	mp_slot slots[2] = { {0, race_t::terran}, {1, race_t::protoss} };
	{ data_loading::mpq_file<> m(map_file); setup_melee_slots(p.st(), m, slots, 2); }
	state& st = p.st();
	action_state as; action_functions af(st, as);

	for (int i = 0; i != 30; ++i) p.next_frame();
	ui::log("wintest: start   p0 victory_state=%d p1 victory_state=%d (both expect 0)\n",
	        st.players[0].victory_state, st.players[1].victory_state);

	// Collect first — killing mutates the list we'd be iterating.
	a_vector<unit_t*> doomed;
	for (unit_t* u : ptr(st.player_units[wipe])) doomed.push_back(u);
	ui::log("wintest: wiping %d units of player %d\n", (int)doomed.size(), wipe);
	for (unit_t* u : doomed) af.kill_unit(u);

	for (int i = 0; i != frames; ++i) p.next_frame();
	ui::log("wintest: end     p0 victory_state=%d (expect >=3 win) p1 victory_state=%d (expect 1-2 loss)\n",
	        st.players[0].victory_state, st.players[1].victory_state);
	// Mirror the rule play_ui uses: the survivor is told it won, and the wiped player has
	// to infer defeat from that (the engine never sets its own victory_state).
	int other = wipe == 0 ? 1 : 0;
	int survivor_outcome = af.player_won(other) ? 1 : 0;
	int wiped_outcome = af.player_won(other) ? 2 : (count_units(st, wipe) == 0 ? 2 : 0);
	bool ok = survivor_outcome == 1 && wiped_outcome == 2;
	ui::log("wintest: survivor outcome=%d (expect 1 victory), wiped outcome=%d (expect 2 defeat)\n",
	        survivor_outcome, wiped_outcome);
	ui::log("wintest: %s\n", ok ? "OK" : "FAILED");
	return ok ? 0 : 1;
}

// Lockstep foundation test. Two sims run from the same map and seed: sim A is driven
// through the BW command-byte writer (the multiplayer path), sim B through direct
// action_* calls (the path the UI used before). Identical checksums prove the
// serialisation round-trips exactly — a wrong payload layout diverges immediately —
// and that stepping both from the same stream stays deterministic.
int run_nettest(const char* data_dir, const char* map_file, int my_player, race_t my_race, int frames) {
	auto load = data_loading::data_files_directory(data_dir);
	game_player pa(load), pb(load);
	{ data_loading::mpq_file<> m(map_file); setup_melee(pa.st(), m, my_player, my_race); }
	{ data_loading::mpq_file<> m(map_file); setup_melee(pb.st(), m, my_player, my_race); }
	state& sa = pa.st(); state& sb = pb.st();
	action_state asa, asb;
	action_functions afa(sa, asa), afb(sb, asb);
	build_kit kit = kit_for(my_race);
	sa.current_minerals[my_player] = 1000;
	sb.current_minerals[my_player] = 1000;
	const int units0 = count_units(sa, my_player);
	const unsigned sum0 = sim_checksum(sa);   // baseline, for the liveness check below

	int failures = 0;
	// Equality alone would also pass if a command were a no-op on *both* sims, so log the
	// observable state too and assert liveness at the end.
	auto check = [&](const char* what) {
		unsigned ca = sim_checksum(sa), cb = sim_checksum(sb);
		ui::log("nettest: %-20s cmd=%08x direct=%08x  min=%d/%d units=%d/%d  %s\n",
		        what, ca, cb,
		        (int)sa.current_minerals[my_player], (int)sb.current_minerals[my_player],
		        count_units(sa, my_player), count_units(sb, my_player),
		        ca == cb ? "ok" : "MISMATCH");
		if (ca != cb) ++failures;
	};

	a_vector<uint8_t> buf;
	bw_cmd w(buf);
	auto flush = [&]() { apply_bw_commands(afa, my_player, buf.data(), buf.size()); buf.clear(); };

	// --- Terran flying buildings: lift off (opcode 47). Must run before the town
	// hall starts producing — the engine refuses to lift a building that's busy. ---
	if (my_race == race_t::terran) {
		unit_t* ca = find_unit_of_type(sa, my_player, UnitTypes::Terran_Command_Center);
		unit_t* cb = find_unit_of_type(sb, my_player, UnitTypes::Terran_Command_Center);
		if (ca && cb) {
			uint16_t id = afa.get_unit_id(ca).raw_value;
			xy p0 = ca->sprite->position;
			w.select(&id, 1); w.liftoff(p0); flush();
			afb.action_select(my_player, cb);
			afb.action_liftoff(my_player, cb->sprite->position);
			check("lift off");
			for (int i = 0; i != 80; ++i) { pa.next_frame(); pb.next_frame(); }
			ui::log("nettest: after lift off, CC grounded = %d (expect 0)\n",
			        (int)afa.u_grounded_building(ca));

			// Land it again (opcode 12 with the BuildingLand order and the building's own
			// type) so the rest of the test has a working town hall.
			int tx = (p0.x - ca->unit_type->placement_size.x / 2) / 32;
			int ty = (p0.y - ca->unit_type->placement_size.y / 2) / 32;
			uint16_t id2 = afa.get_unit_id(ca).raw_value;
			w.select(&id2, 1); w.build(Orders::BuildingLand, ca->unit_type->id, tx, ty); flush();
			afb.action_select(my_player, cb);
			afb.action_build(my_player, afb.get_order_type(Orders::BuildingLand), cb->unit_type,
			                 {(size_t)tx, (size_t)ty});
			check("land");
			for (int i = 0; i != 200; ++i) { pa.next_frame(); pb.next_frame(); }
			ui::log("nettest: after landing, CC grounded = %d (expect 1)\n",
			        (int)afa.u_grounded_building(ca));
		}
	}

	// --- train (or morph) a worker from the town hall: opcodes 9 + 31/35 ---
	UnitTypes townhall = my_race == race_t::zerg ? UnitTypes::Zerg_Larva
	                   : my_race == race_t::protoss ? UnitTypes::Protoss_Nexus
	                                                : UnitTypes::Terran_Command_Center;
	unit_t* ta = find_unit_of_type(sa, my_player, townhall);
	unit_t* tb = find_unit_of_type(sb, my_player, townhall);
	if (ta && tb) {
		uint16_t id = afa.get_unit_id(ta).raw_value;
		w.select(&id, 1);
		w.type(my_race == race_t::zerg ? 35 : 31, kit.worker);
		flush();
		afb.action_select(my_player, tb);
		if (my_race == race_t::zerg) afb.action_morph(my_player, afb.get_unit_type(kit.worker));
		else afb.action_train(my_player, afb.get_unit_type(kit.worker));
		check("train worker");
	}

	// --- move a worker: opcodes 9 + 20 (default order, position target) ---
	unit_t* wa = find_unit_of_type(sa, my_player, kit.worker);
	unit_t* wb = find_unit_of_type(sb, my_player, kit.worker);
	if (wa && wb) {
		uint16_t id = afa.get_unit_id(wa).raw_value;
		xy dest = wa->sprite->position + xy(96, 96);
		w.select(&id, 1); w.default_order(dest, 0, false); flush();
		afb.action_select(my_player, wb);
		afb.action_default_order(my_player, dest, nullptr, nullptr, false);
		check("worker move order");
	}

	// --- place a supply building: opcodes 9 + 12 ---
	if (wa && wb) {
		int tx = wa->sprite->position.x / 32 + 4, ty = wa->sprite->position.y / 32;
		uint16_t id = afa.get_unit_id(wa).raw_value;
		w.select(&id, 1); w.build(kit.build_order, kit.supply, tx, ty); flush();
		afb.action_select(my_player, wb);
		afb.action_build(my_player, afb.get_order_type(kit.build_order),
		                 afb.get_unit_type(kit.supply), {(size_t)tx, (size_t)ty});
		check("build supply");
	}

	// --- stop / hold: opcodes 26 / 43 ---
	w.queued(26, false); flush(); afb.action_stop(my_player, false);          check("stop");
	w.queued(43, false); flush(); afb.action_hold_position(my_player, false); check("hold position");

	// --- step both sims and confirm they never drift ---
	for (int i = 0; i != frames; ++i) {
		pa.next_frame(); pb.next_frame();
		if (sim_checksum(sa) != sim_checksum(sb)) {
			ui::log("nettest: DIVERGED at frame %d\n", (int)sa.current_frame);
			++failures;
			break;
		}
	}
	// Liveness: if the commands had all been silently dropped, both sims would still match.
	// Spending minerals and moving the sim off its initial state proves otherwise. Don't
	// assert on unit count — Zerg morphs larva->egg->unit without growing it, so that
	// signal is race- and frame-count dependent.
	if ((int)sa.current_minerals[my_player] >= 1000) {
		ui::log("nettest: LIVENESS FAILED - no minerals spent, commands did nothing\n");
		++failures;
	}
	if (sim_checksum(sa) == sum0) {
		ui::log("nettest: LIVENESS FAILED - sim state never changed\n");
		++failures;
	}
	ui::log("nettest: units %d -> %d\n", units0, count_units(sa, my_player));
	ui::log("nettest: %s (%d frames, checksum %08x)\n",
	        failures ? "FAILED" : "OK", frames, sim_checksum(sa));
	return failures ? 1 : 0;
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

	const char* wintest = getenv("OPENBW_WINTEST");
	if (wintest) return run_wintest(data_dir, map_file, my_player, atoi(wintest));

	const char* fogtest = getenv("OPENBW_FOGTEST");
	if (fogtest) return run_fogtest(data_dir, map_file, atoi(fogtest));

	const char* nettest = getenv("OPENBW_NETTEST");
	if (nettest) return run_nettest(data_dir, map_file, my_player, my_race, atoi(nettest));

	const char* shot = getenv("OPENBW_SCREENSHOT");
	if (shot) {
		const char* wf = getenv("OPENBW_HEADLESS");
		return run_screenshot(data_dir, map_file, my_player, my_race, shot, wf ? atoi(wf) : 48);
	}
	const char* headless = getenv("OPENBW_HEADLESS");
	if (headless) return run_headless(data_dir, map_file, my_player, my_race, atoi(headless));
	return run_windowed(data_dir, map_file, my_player, my_race);
}
