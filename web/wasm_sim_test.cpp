// web/wasm_sim_test.cpp — WebAssembly sim bring-up test.
//
// Proves the OpenBW *simulation* compiles and runs under WebAssembly (WASI),
// with no UI/SDL at all. Run with a WASI host (Node's node:wasi) that preopens
// the game data + map directories. This de-risks the whole browser port before
// any canvas/JS backend exists.

#include "bwgame.h"
#include <cstdio>

using namespace bwgame;

static int count_units(state& st, int owner) {
	int n = 0;
	for (unit_t* u : ptr(st.player_units.at(owner))) { (void)u; ++n; }
	return n;
}

int main(int argc, char** argv) {
	const char* data_dir = argc > 1 ? argv[1] : "/data";
	const char* map_file = argc > 2 ? argv[2] : "/maps/Benzene.scx";
	int my_player = 0;
	race_t my_race = race_t::terran;

	auto load_data_file = data_loading::data_files_directory(data_dir);
	game_player player(load_data_file);

	data_loading::mpq_file<> map_loader(map_file);
	game_load_functions game_load(player.st());
	game_load.load_map(map_loader, [&]() {
		state& st = player.st();
		game_load.setup_info.victory_condition = 0;
		game_load.setup_info.tournament_mode = 0;
		game_load.setup_info.starting_units = 0;
		game_load.setup_info.resource_type = 1;
		game_load.setup_info.starting_minerals = 50;
		for (int i = 0; i != 12; ++i) {
			if (i == my_player) {
				st.players[i].controller = player_t::controller_occupied;
				st.players[i].race = my_race;
				game_load.setup_info.create_melee_units_for_player[i] = true;
			} else {
				st.players[i].controller = player_t::controller_inactive;
				game_load.setup_info.create_melee_units_for_player[i] = false;
			}
		}
	});

	state& st = player.st();
	printf("wasm sim: map '%s' %dx%d, units=%d, minerals=%d\n",
		st.game->scenario_name.c_str(), (int)st.game->map_width, (int)st.game->map_height,
		count_units(st, my_player), (int)st.current_minerals[my_player]);

	for (int i = 0; i != 500; ++i) player.next_frame();

	printf("wasm sim: after 500 frames: frame=%d units=%d minerals=%d\n",
		(int)st.current_frame, count_units(st, my_player), (int)st.current_minerals[my_player]);
	printf("wasm sim: OK\n");
	return 0;
}
