// web/sandbox.h — shared single-player sandbox gameplay, used by both the
// native build (web/play.cpp) and the browser/wasm build (web/wasm_main.cpp).
//
// Backend-agnostic: melee game setup + the play_ui input/command layer.
// Depends only on ui.h / bwgame.h, never on the SDL or wasm presentation
// backend, so the exact same gameplay code runs in both.

#ifndef OPENBW_WEB_SANDBOX_H
#define OPENBW_WEB_SANDBOX_H

#include "ui.h"
#include "bwgame.h"

#include <cstring>

namespace bwgame {

inline race_t parse_race(const char* s) {
	if (!strcmp(s, "zerg")) return race_t::zerg;
	if (!strcmp(s, "protoss")) return race_t::protoss;
	return race_t::terran;
}

// Configure a "you vs nothing" melee game: one occupied human slot, everyone
// else inactive. Mirrors the melee setup path that replay.h drives. Runs
// synchronously — when it returns, `st` is ready to simulate. `load_data_file`
// is the functor that pulls staredit/scenario.chk from the map archive.
template<typename load_data_file_F>
void setup_melee(state& st, load_data_file_F&& load_data_file, int my_player, race_t my_race) {
	game_load_functions game_load(st);
	game_load.load_map(std::forward<load_data_file_F>(load_data_file), [&]() {
		game_load.setup_info.victory_condition = 0;   // melee, not UMS
		game_load.setup_info.tournament_mode = 0;
		game_load.setup_info.starting_units = 0;
		game_load.setup_info.resource_type = 1;       // standard melee resources
		game_load.setup_info.starting_minerals = 50;  // classic melee start
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
}

inline int count_units(state& st, int owner) {
	int n = 0;
	for (unit_t* u : ptr(st.player_units.at(owner))) { (void)u; ++n; }
	return n;
}

inline unit_t* find_unit_of_type(state& st, int owner, UnitTypes id) {
	for (unit_t* u : ptr(st.player_units.at(owner)))
		if (u->unit_type->id == id) return u;
	return nullptr;
}

inline int count_units_of_type(state& st, int owner, UnitTypes id) {
	int n = 0;
	for (unit_t* u : ptr(st.player_units.at(owner)))
		if (u->unit_type->id == id) ++n;
	return n;
}

// Per-race unit types for the minimal build/train command card.
struct build_kit {
	UnitTypes worker, military, supply, production;
	Orders build_order;   // race's building-placement order
};

inline build_kit kit_for(race_t race) {
	switch (race) {
	case race_t::protoss:
		return {UnitTypes::Protoss_Probe, UnitTypes::Protoss_Zealot,
		        UnitTypes::Protoss_Pylon, UnitTypes::Protoss_Gateway,
		        Orders::PlaceProtossBuilding};
	case race_t::zerg:
		return {UnitTypes::Zerg_Drone, UnitTypes::Zerg_Zergling,
		        UnitTypes::Zerg_Overlord, UnitTypes::Zerg_Spawning_Pool,
		        Orders::DroneStartBuild};
	default:  // terran
		return {UnitTypes::Terran_SCV, UnitTypes::Terran_Marine,
		        UnitTypes::Terran_Supply_Depot, UnitTypes::Terran_Barracks,
		        Orders::PlaceBuilding};
	}
}

// Select `worker` and search outward from `start` for a buildable tile, placing
// the kit's production building there. Returns true if a spot was found. `af`
// is any action_functions (native play_ui or a bare action_functions).
template<typename action_functions_T>
bool try_build_near(action_functions_T& af, int owner, const build_kit& kit, unit_t* worker, xy start) {
	const unit_type_t* bt = af.get_unit_type(kit.production);
	const order_type_t* order = af.get_order_type(kit.build_order);
	af.action_select(owner, worker);
	int start_tx = start.x / 32, start_ty = start.y / 32;
	for (int r = 2; r <= 8; ++r)
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx) {
				int tx = start_tx + dx, ty = start_ty + dy;
				if (tx < 0 || ty < 0) continue;
				xy center(32 * tx + bt->placement_size.x / 2, 32 * ty + bt->placement_size.y / 2);
				if (af.can_place_building(worker, owner, bt, center, false, false)) {
					af.action_build(owner, order, bt, {(size_t)tx, (size_t)ty});
					return true;
				}
			}
	return false;
}

// Turns UI input into player commands. The base ui_functions handles camera +
// (visual) drag-selection; we intercept before it via handle_game_input to
// issue orders and open the build/train card.
//
//   right-click : smart default order (move / attack / gather by cursor target)
//   s / h       : stop / hold           shift = queue
//   w / m       : train worker / military from the selected producer
//   f / b       : build supply / production (enter placement, left-click to site)
//   c / esc     : cancel placement
struct play_ui : ui_functions {
	int my_player;
	race_t my_race;
	build_kit kit;

	const unit_type_t* pending_build = nullptr;   // building awaiting a placement click
	int mouse_x = 0, mouse_y = 0;

	play_ui(game_player player, int my_player, race_t my_race)
		: ui_functions(std::move(player)), my_player(my_player), my_race(my_race),
		  kit(kit_for(my_race)) {
		fog_player = my_player;   // render from this player's perspective
	}

	xy screen_to_map(int mx, int my) const {
		return screen_pos + xy(
			(fp16::integer(mx) / view_scale).integer_part(),
			(fp16::integer(my) / view_scale).integer_part());
	}

	// 225/229 = SDL LSHIFT/RSHIFT scancodes. Both backends speak SDL scancodes
	// (the browser host maps ShiftLeft/Right -> 225/229 in openbw.js).
	bool key_shift() { return wnd.get_key_state(225) || wnd.get_key_state(229); }

	// Push the UI's visual selection into the sim's per-player action selection,
	// capped to BW's 12-unit limit (action_select errors past 12), own units only.
	void sync_selection() {
		a_vector<unit_t*> units;
		for (auto uid : current_selection) {
			if (units.size() == 12) break;
			unit_t* u = get_unit(uid);
			if (u && u->owner == my_player) units.push_back(u);
		}
		action_select(my_player, units);
	}

	// Train (Terran/Protoss) or morph (Zerg) a unit from the current selection.
	void train(UnitTypes type) {
		sync_selection();
		if (my_race == race_t::zerg) action_morph(my_player, get_unit_type(type));
		else action_train(my_player, get_unit_type(type));
	}

	// Arm a building for placement; the next left-click sites it.
	void begin_build(UnitTypes type) { pending_build = get_unit_type(type); }

	void place_pending(int mx, int my) {
		xy map_pos = screen_to_map(mx, my);
		int tx = (map_pos.x - pending_build->placement_size.x / 2) / 32;
		int ty = (map_pos.y - pending_build->placement_size.y / 2) / 32;
		if (tx < 0) tx = 0;
		if (ty < 0) ty = 0;
		sync_selection();
		action_build(my_player, get_order_type(kit.build_order), pending_build,
		             {(size_t)tx, (size_t)ty});
		pending_build = nullptr;   // one-shot; re-press the key to place another
	}

	// Draw a footprint box at the cursor while placing (view_scale is 1 here).
	void draw_callback(uint8_t* data, size_t data_pitch) override {
		ui_functions::draw_callback(data, data_pitch);
		if (!pending_build) return;
		int w = pending_build->placement_size.x;
		int h = pending_build->placement_size.y;
		rect r{{mouse_x - w / 2, mouse_y - h / 2}, {mouse_x + w / 2, mouse_y + h / 2}};
		line_rectangle(data, data_pitch, r, 117);   // greenish palette index
	}

	bool handle_game_input(const native_window::event_t& e) override {
		using ev = native_window::event_t;
		if (e.type == ev::type_mouse_motion) { mouse_x = e.mouse_x; mouse_y = e.mouse_y; return false; }

		// Left-click sites a pending building instead of drag-selecting.
		if (e.type == ev::type_mouse_button_down && e.button == 1 && pending_build) {
			place_pending(e.mouse_x, e.mouse_y);
			return true;
		}
		// Right-click = smart default order.
		if (e.type == ev::type_mouse_button_down && e.button == 3) {
			if (pending_build) { pending_build = nullptr; return true; }   // cancel placement
			if (current_selection.empty()) return false;                    // else let base pan
			xy map_pos = screen_to_map(e.mouse_x, e.mouse_y);
			unit_t* target = select_get_unit_at(map_pos);
			sync_selection();
			action_default_order(my_player, map_pos, target, nullptr, key_shift());
			return true;
		}
		if (e.type == ev::type_key_down) {
			switch (e.sym) {
			case 's': sync_selection(); action_stop(my_player, key_shift()); return true;
			case 'h': sync_selection(); action_hold_position(my_player, key_shift()); return true;
			case 'w': train(kit.worker); return true;
			case 'm': train(kit.military); return true;
			case 'f':   // supply — Zerg overlord is a morph, not a building
				if (my_race == race_t::zerg) train(kit.supply);
				else begin_build(kit.supply);
				return true;
			case 'b': begin_build(kit.production); return true;
			case 'c': pending_build = nullptr; return true;
			default: break;
			}
		}
		return false;                                      // left-click select, camera, base keys
	}
};

}  // namespace bwgame

#endif
