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
		game_load.setup_info.resource_type = 1;         // standard melee resources
		game_load.setup_info.starting_minerals = 50;   // authentic melee start
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
// (visual) drag-selection; we intercept before it via handle_game_input.
//
// Classic-style, context-sensitive command card (Terran):
//   any unit   : M move · S stop · A attack · H hold · P patrol  (A/M/P then click)
//   SCV        : + G gather · R repair · B build (submenu)
//   producers  : train units (CC: S=SCV; Barracks: M/F/C/G; Factory: V/T/G)
//   abilities  : Marine T=stim · Tank E=siege / D=tank mode
//   right-click: smart default order · cancels a pending target/placement/menu
// The visible card is drawn by the JS host from the openbw_card() export.
struct play_ui : ui_functions {
	int my_player;
	race_t my_race;
	build_kit kit;

	const unit_type_t* pending_build = nullptr;   // building awaiting a placement click
	bool targeting = false;                       // an order awaiting a target click
	int mouse_x = -1, mouse_y = -1;   // off-screen until the first move, so edge-scroll stays idle at startup
	int sound_rotation = 0;                       // rotates through a unit's ack sound range
	unit_id ack_unit;                             // unit last (re)selected, for annoyed escalation
	int ack_count = 0;                            // consecutive same-unit selections
	int menu = 0;                                 // command card: 0 = root, 1 = build submenu
	int place_ok_color = -1, place_bad_color = -1;
	a_vector<uint8_t> ghost_ok, ghost_bad;   // src palette index -> tinted+dimmed index
	a_vector<uint8_t> ghost_buf;             // scratch for the rendered ghost frame

	int icon_w = 0, icon_h = 0;              // size of the last render_icon() result
	a_vector<uint8_t> icon_index, icon_rgba; // scratch: 8-bit frame, then RGBA

	enum targ_t { T_ATTACK, T_MOVE, T_PATROL, T_GATHER, T_REPAIR, T_RALLY };
	targ_t pending_targ = T_ATTACK;

	enum cmd_act { C_MOVE, C_STOP, C_ATTACK, C_GATHER, C_HOLD, C_PATROL,
	               C_BUILDMENU, C_BUILD, C_TRAIN, C_MORPH, C_STIM, C_SIEGE, C_UNSIEGE, C_REPAIR,
	               C_SELECT,     // select the player's units of cmd.ut (SCVs / Probes / Larvae)
	               C_RALLY, C_RESEARCH };
	struct cmd_t { char key; const char* label; cmd_act act; UnitTypes ut; bool enabled = false; TechTypes tech = TechTypes::None; };
	a_vector<cmd_t> card;
	a_string card_text;                           // "title\nKEY\tLabel\tEN\n…" for the JS overlay
	a_string status_text;                         // producer queue + progress, rebuilt per frame
	a_string resources_text;                      // minerals/gas/supply HUD, rebuilt per frame

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
	bool key_ctrl() { return wnd.get_key_state(224) || wnd.get_key_state(228); }

	// Control groups (classic hotkeys): Ctrl+digit assigns, digit recalls, and
	// pressing the same group again centers the camera on it.
	std::array<a_vector<unit_id>, 10> groups;
	int last_recalled_group = -1;

	void assign_group(int n) {
		auto& g = groups[n];
		g.clear();
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (u && u->owner == my_player) g.push_back(uid);
		}
		last_recalled_group = -1;   // next tap of n selects, not centers
	}

	void center_on_selection() {
		long sx = 0, sy = 0; int n = 0;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (u) { sx += u->sprite->position.x; sy += u->sprite->position.y; ++n; }
		}
		if (n) screen_pos = xy((int)(sx / n) - view_width / 2, (int)(sy / n) - view_height / 2);
	}

	void recall_group(int n) {
		bool refocus = (n == last_recalled_group);   // second tap of the same group
		current_selection_clear();
		for (auto uid : groups[n]) {
			unit_t* u = get_unit(uid);   // skips units that have since died
			if (u && u->owner == my_player) current_selection_add(u);
		}
		if (current_selection.empty()) { last_recalled_group = -1; on_selection(false); return; }
		if (refocus) center_on_selection();   // don't re-ack on a recenter
		else on_selection(false);
		last_recalled_group = n;
	}

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

	unit_t* primary_selected() {
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (u && u->owner == my_player) return u;
		}
		return nullptr;
	}

	// Play one of a unit's voice-ack sounds (first..last inclusive), the way the
	// original game client did on select/order — the sim never plays these.
	void play_unit_ack(unit_t* u, int first, int last) {
		if (!u || first <= 0 || last < first) return;
		int id = first + (sound_rotation++ % (last - first + 1));
		play_sound(id, u->sprite->position, u, false);   // 4-arg form (override hides the others)
	}

	// Selection ack: the unit's "what" line, escalating to its "pissed"
	// (annoyed) lines when the same single unit is clicked repeatedly.
	void on_selection(bool) override {
		if (current_selection.size() > 12) current_selection.resize(12);   // BW's 12-unit cap
		menu = 0;
		last_recalled_group = -1;   // a fresh selection resets group double-tap tracking
		unit_t* u = primary_selected();
		if (!u) { ack_count = 0; refresh_card(); return; }
		if (current_selection.size() == 1 && get_unit_id(u) == ack_unit) ++ack_count;
		else { ack_unit = get_unit_id(u); ack_count = 0; }
		const unit_type_t* ut = u->unit_type;
		int what_n = ut->first_what_sound > 0 && ut->last_what_sound >= ut->first_what_sound
			? ut->last_what_sound - ut->first_what_sound + 1 : 0;
		if (ack_count >= what_n && ut->first_pissed_sound > 0 && ut->last_pissed_sound >= ut->first_pissed_sound)
			play_unit_ack(u, ut->first_pissed_sound, ut->last_pissed_sound);
		else
			play_unit_ack(u, ut->first_what_sound, ut->last_what_sound);
		refresh_card();
	}

	int nearest_palette_color(int r, int g, int b) {
		const auto& wpe = tileset_img.wpe;
		int best = 0, best_score = 1 << 30;
		for (int i = 0; i != 256; ++i) {
			int dr = r - wpe[4 * i], dg = g - wpe[4 * i + 1], db = b - wpe[4 * i + 2];
			int s = dr * dr + dg * dg + db * db;
			if (s < best_score) { best_score = s; best = i; }
		}
		return best;
	}

	void add_move_orders() {
		card.push_back({'m', "Move", C_MOVE, UnitTypes::None});
		card.push_back({'s', "Stop", C_STOP, UnitTypes::None});
		card.push_back({'a', "Attack", C_ATTACK, UnitTypes::None});
		card.push_back({'h', "Hold", C_HOLD, UnitTypes::None});
		card.push_back({'p', "Patrol", C_PATROL, UnitTypes::None});
	}

	// Proper unit/building name from stat_txt.tbl (loaded lazily), race prefix stripped —
	// so the card title is e.g. "Overlord" / "Supply Depot" instead of "Unit"/"Building".
	a_vector<a_string> unit_names;
	const char* unit_name(UnitTypes id) {
		if (unit_names.empty()) {
			a_vector<uint8_t> d;
			load_data_file(d, "rez/stat_txt.tbl");
			if (d.size() >= 2) {
				int count = d[0] | (d[1] << 8);
				unit_names.resize(count);
				for (int i = 0; i != count; ++i) {
					int off = d[2 + i * 2] | (d[3 + i * 2] << 8);
					const char* s = (const char*)&d[off];
					for (const char* pre : {"Terran ", "Protoss ", "Zerg "}) {   // strip race prefix
						const char* a = s; const char* b = pre;
						while (*b && *a == *b) { ++a; ++b; }
						if (!*b) { s = a; break; }
					}
					unit_names[i] = s;
				}
			}
		}
		int i = (int)id;
		return (i >= 0 && i < (int)unit_names.size()) ? unit_names[i].c_str() : "Unit";
	}

	// Select up to 12 of the player's units of a type — handy for the fiddly-to-click
	// ones (workers, and especially Zerg larvae clustered at the hatchery).
	void select_units_of_type(UnitTypes type) {
		current_selection_clear();
		for (unit_t* u : ptr(st.player_units[my_player])) {
			if (unit_is(u, type) && !unit_dying(u)) {
				current_selection_add(u);
				if (current_selection.size() >= 12) break;
			}
		}
		on_selection(false);
	}

	// Push the current race's basic buildings into the build submenu (menu == 1).
	void add_build_menu() {
		using U = UnitTypes;
		if (my_race == race_t::protoss) {
			card.push_back({'p', "Pylon", C_BUILD, U::Protoss_Pylon});
			card.push_back({'g', "Gateway", C_BUILD, U::Protoss_Gateway});
			card.push_back({'a', "Assimilator", C_BUILD, U::Protoss_Assimilator});
			card.push_back({'n', "Nexus", C_BUILD, U::Protoss_Nexus});
			card.push_back({'f', "Forge", C_BUILD, U::Protoss_Forge});
			card.push_back({'y', "Cybernetics Core", C_BUILD, U::Protoss_Cybernetics_Core});
		} else if (my_race == race_t::zerg) {
			card.push_back({'h', "Hatchery", C_BUILD, U::Zerg_Hatchery});
			card.push_back({'s', "Spawning Pool", C_BUILD, U::Zerg_Spawning_Pool});
			card.push_back({'e', "Extractor", C_BUILD, U::Zerg_Extractor});
			card.push_back({'v', "Evolution Chamber", C_BUILD, U::Zerg_Evolution_Chamber});
			card.push_back({'d', "Hydralisk Den", C_BUILD, U::Zerg_Hydralisk_Den});
		} else {
			card.push_back({'s', "Supply Depot", C_BUILD, U::Terran_Supply_Depot});
			card.push_back({'b', "Barracks", C_BUILD, U::Terran_Barracks});
			card.push_back({'r', "Refinery", C_BUILD, U::Terran_Refinery});
			card.push_back({'e', "Engineering Bay", C_BUILD, U::Terran_Engineering_Bay});
			card.push_back({'f', "Factory", C_BUILD, U::Terran_Factory});
			card.push_back({'c', "Command Center", C_BUILD, U::Terran_Command_Center});
		}
	}

	// The unit-specific part of the card (not the build submenu); returns the title.
	const char* card_for_unit(unit_t* u, UnitTypes id) {
		using U = UnitTypes;
		// Workers (SCV / Probe / Drone): orders + gather + build submenu.
		if (id == kit.worker) {
			add_move_orders();
			card.push_back({'g', "Gather", C_GATHER, U::None});
			if (id == U::Terran_SCV) card.push_back({'r', "Repair", C_REPAIR, U::None});
			card.push_back({'b', "Build", C_BUILDMENU, U::None});
			return id == U::Protoss_Probe ? "Probe" : id == U::Zerg_Drone ? "Drone" : "SCV";
		}
		switch (id) {
		// --- Terran ---
		case U::Terran_Command_Center:
			card.push_back({'s', "Train SCV", C_TRAIN, U::Terran_SCV}); return "Command Center";
		case U::Terran_Barracks:
			card.push_back({'m', "Marine", C_TRAIN, U::Terran_Marine});
			card.push_back({'f', "Firebat", C_TRAIN, U::Terran_Firebat});
			card.push_back({'c', "Medic", C_TRAIN, U::Terran_Medic});
			card.push_back({'g', "Ghost", C_TRAIN, U::Terran_Ghost}); return "Barracks";
		case U::Terran_Factory:
			card.push_back({'v', "Vulture", C_TRAIN, U::Terran_Vulture});
			card.push_back({'t', "Siege Tank", C_TRAIN, U::Terran_Siege_Tank_Tank_Mode});
			card.push_back({'g', "Goliath", C_TRAIN, U::Terran_Goliath}); return "Factory";
		case U::Terran_Marine:
			add_move_orders(); card.push_back({'t', "Stim Pack", C_STIM, U::None}); return "Marine";
		case U::Terran_Siege_Tank_Tank_Mode: case U::Terran_Siege_Tank_Tank_Mode_Turret:
			add_move_orders(); card.push_back({'e', "Siege Mode", C_SIEGE, U::None}); return "Siege Tank";
		case U::Terran_Siege_Tank_Siege_Mode: case U::Terran_Siege_Tank_Siege_Mode_Turret:
			card.push_back({'d', "Tank Mode", C_UNSIEGE, U::None}); return "Siege Tank";
		// --- Protoss ---
		case U::Protoss_Nexus:
			card.push_back({'p', "Train Probe", C_TRAIN, U::Protoss_Probe}); return "Nexus";
		case U::Protoss_Gateway:
			card.push_back({'z', "Zealot", C_TRAIN, U::Protoss_Zealot});
			card.push_back({'d', "Dragoon", C_TRAIN, U::Protoss_Dragoon});
			card.push_back({'t', "High Templar", C_TRAIN, U::Protoss_High_Templar}); return "Gateway";
		// --- Zerg ---
		case U::Zerg_Larva:
			card.push_back({'d', "Drone", C_MORPH, U::Zerg_Drone});
			card.push_back({'z', "Zergling", C_MORPH, U::Zerg_Zergling});
			card.push_back({'o', "Overlord", C_MORPH, U::Zerg_Overlord});
			card.push_back({'h', "Hydralisk", C_MORPH, U::Zerg_Hydralisk});
			card.push_back({'m', "Mutalisk", C_MORPH, U::Zerg_Mutalisk}); return "Larva";
		case U::Zerg_Hatchery: case U::Zerg_Lair: case U::Zerg_Hive:
			card.push_back({'r', "Set Rally Point", C_RALLY, U::None});
			card.push_back({'s', "Select Larva", C_SELECT, U::Zerg_Larva});
			card.push_back({'b', "Evolve Burrow", C_RESEARCH, U::None, false, TechTypes::Burrowing});
			return unit_name(id);
		default: break;
		}
		if (ut_building(u)) return unit_name(id);
		add_move_orders();
		return unit_name(id);
	}

	// Rebuild the context command card for the current selection (all three races).
	void refresh_card() {
		card.clear();
		card_text.clear();
		unit_t* u = primary_selected();          // own unit — drives the action buttons
		unit_t* sel = u;                          // the unit we describe (own preferred, else first)
		if (!sel) for (auto uid : current_selection) { sel = get_unit(uid); if (sel) break; }
		if (!sel) { menu = 0; return; }

		const char* title;
		if (u && menu == 1) { title = "Build"; add_build_menu(); }
		else if (u) title = card_for_unit(u, u->unit_type->id);
		else { title = unit_name(sel->unit_type->id); menu = 0; }   // neutral/enemy: name only, no actions

		// Gray out commands whose requirements aren't met (e.g. Ghost w/o Academy,
		// Stim before the upgrade is researched, a larva morph without its building).
		for (auto& c : card) {
			switch (c.act) {
			case C_TRAIN:
			case C_MORPH: c.enabled = unit_can_build(u, get_unit_type(c.ut)); break;
			case C_BUILD: c.enabled = unit_build_order_valid(u, get_order_type(kit.build_order), get_unit_type(c.ut), my_player); break;
			case C_STIM:  c.enabled = player_has_researched(my_player, TechTypes::Stim_Packs); break;
			case C_SIEGE: c.enabled = player_has_researched(my_player, TechTypes::Tank_Siege_Mode); break;
			case C_RESEARCH: c.enabled = !player_has_researched(my_player, c.tech) && !st.tech_researching[my_player][c.tech]; break;
			default:      c.enabled = true;
			}
		}

		// Title line carries the unit's status: "name \t HP x/y \t <second stat, or empty>".
		int hp = sel->hp.ceil().integer_part(), maxhp = sel->unit_type->hitpoints.ceil().integer_part();
		a_string stat2;
		if (sel->unit_type->has_shield)
			stat2 = format("Shields %d/%d", sel->shield_points.integer_part(), sel->unit_type->shield_points);
		else if (ut_resource(sel))
			stat2 = format("%s %d", unit_is_mineral_field(sel) ? "Minerals" : "Gas", sel->building.resource.resource_count);
		card_text += title;
		card_text += '\t'; card_text += format("HP %d/%d", hp, maxhp).c_str();
		card_text += '\t'; card_text += stat2.c_str();
		for (auto& c : card) {
			// Icon frame for build/train buttons is the unit id; -1 for plain orders.
			int icon = (c.ut != UnitTypes::None) ? (int)c.ut : -1;
			int minc = 0, gasc = 0;   // resource cost of the thing this button makes
			if (c.ut != UnitTypes::None && (c.act == C_BUILD || c.act == C_TRAIN || c.act == C_MORPH)) {
				const unit_type_t* ut = get_unit_type(c.ut);
				minc = ut->mineral_cost; gasc = ut->gas_cost;
			}
			bool afford = (int)st.current_minerals[my_player] >= minc && (int)st.current_gas[my_player] >= gasc;
			// KEY \t Label \t enabled \t icon \t minerals \t gas \t affordable
			card_text += '\n'; card_text += c.key; card_text += '\t'; card_text += c.label;
			card_text += '\t'; card_text += (c.enabled ? '1' : '0');
			card_text += '\t'; card_text += format("%d", icon).c_str();
			card_text += '\t'; card_text += format("%d", minc).c_str();
			card_text += '\t'; card_text += format("%d", gasc).c_str();
			card_text += '\t'; card_text += (afford ? '1' : '0');
		}
	}

	static const char* unit_short_name(UnitTypes id) {
		using U = UnitTypes;
		switch (id) {
		case U::Terran_SCV: return "SCV";
		case U::Terran_Marine: return "Marine";
		case U::Terran_Firebat: return "Firebat";
		case U::Terran_Medic: return "Medic";
		case U::Terran_Ghost: return "Ghost";
		case U::Terran_Vulture: return "Vulture";
		case U::Terran_Siege_Tank_Tank_Mode: return "Siege Tank";
		case U::Terran_Goliath: return "Goliath";
		case U::Terran_Wraith: return "Wraith";
		case U::Terran_Dropship: return "Dropship";
		case U::Terran_Science_Vessel: return "Sci Vessel";
		case U::Terran_Battlecruiser: return "Battlecruiser";
		case U::Terran_Valkyrie: return "Valkyrie";
		case U::Terran_Nuclear_Missile: return "Nuke";
		default: return "Unit";
		}
	}

	// Producer status for the JS overlay: "<progress%>\t<name>\t<name>…" (the
	// first name is the unit being trained), or empty if nothing is training.
	const char* build_status() {
		status_text.clear();
		unit_t* u = primary_selected();
		if (u && !u->build_queue.empty()) {
			int prog = 0;
			if (u->current_build_unit) {
				int bt = u->current_build_unit->unit_type->build_time;
				if (bt > 0) prog = 100 * (bt - u->current_build_unit->remaining_build_time) / bt;
			} else {
				// A Zerg egg / morphing unit counts down its own remaining_build_time
				// toward the queued unit's build time (no separate current_build_unit).
				int bt = u->build_queue.front()->build_time;
				if (bt > 0) prog = 100 * (bt - u->remaining_build_time) / bt;
			}
			status_text += format("%d", prog);
			for (auto* ut : u->build_queue) { status_text += '\t'; status_text += unit_name(ut->id); }
		}
		return status_text.c_str();
	}

	// "minerals\tgas\tsupply_used\tsupply_max" for the resource HUD.
	const char* resources() {
		int p = my_player, ri = (int)my_race;   // race index into the supply arrays
		int used = st.supply_used[p][ri].raw_value / 2;
		int max = st.supply_available[p][ri].raw_value / 2;
		if (max > 200) max = 200;
		resources_text = format("%d\t%d\t%d\t%d", (int)st.current_minerals[p], (int)st.current_gas[p], used, max);
		return resources_text.c_str();
	}

	// Is there a unit under the current cursor position?
	bool hovering_unit() {
		return mouse_x >= 0 && mouse_y >= 0 && select_get_unit_at(screen_to_map(mouse_x, mouse_y)) != nullptr;
	}

	// Pointer mode for the host to pick a cursor: 0 normal · 1 targeting · 2 placing ·
	// 3 normal over a unit (→ pointer) · 4 targeting over a unit.
	int cursor() {
		if (pending_build) return 2;
		if (targeting) return hovering_unit() ? 4 : 1;
		return hovering_unit() ? 3 : 0;
	}

	void start_target(targ_t t) { pending_build = nullptr; targeting = true; pending_targ = t; }

	// Execute the command bound to `key` in the current card. Returns false if
	// the key isn't a command (so base key handling can run).
	bool run_command(char key) {
		for (auto& c : card) {
			if (c.key != key) continue;
			if (!c.enabled) return true;   // grayed out — consume the key, do nothing
			switch (c.act) {
			case C_MOVE:      start_target(T_MOVE); break;
			case C_ATTACK:    start_target(T_ATTACK); break;
			case C_PATROL:    start_target(T_PATROL); break;
			case C_GATHER:    start_target(T_GATHER); break;
			case C_REPAIR:    start_target(T_REPAIR); break;
			case C_STOP:      sync_selection(); action_stop(my_player, key_shift()); break;
			case C_HOLD:      sync_selection(); action_hold_position(my_player, key_shift()); break;
			case C_BUILDMENU: menu = 1; refresh_card(); break;
			case C_BUILD:     pending_build = get_unit_type(c.ut); menu = 0; refresh_card(); break;
			case C_TRAIN:     sync_selection(); action_train(my_player, get_unit_type(c.ut)); break;
			case C_MORPH:     sync_selection(); action_morph(my_player, get_unit_type(c.ut)); break;
			case C_SELECT:    select_units_of_type(c.ut); break;
			case C_RALLY:     start_target(T_RALLY); break;
			case C_RESEARCH:  sync_selection(); action_research(my_player, get_tech_type(c.tech)); break;
			case C_STIM:      sync_selection(); action_stim_pack(my_player); break;
			case C_SIEGE:     sync_selection(); action_siege(my_player, key_shift()); break;
			case C_UNSIEGE:   sync_selection(); action_unsiege(my_player, key_shift()); break;
			}
			return true;
		}
		return false;
	}

	// Issue the pending target order at a clicked position/unit. Attack uses
	// AttackDefault, which force-attacks any unit target (even own/neutral).
	// If (mx,my) is inside the minimap, set `out` to the map pixel it points at and
	// return true; else return false. Mirrors ui.h's move_minimap mapping so orders
	// and attack/move targets can be given straight from the minimap.
	bool minimap_point(int mx, int my, xy& out) {
		rect a = get_minimap_area();
		if (a.to.x <= a.from.x || mx < a.from.x || mx >= a.to.x || my < a.from.y || my >= a.to.y) return false;
		int x = (mx - a.from.x) * (int)game_st.map_tile_width / (a.to.x - a.from.x);
		int y = (my - a.from.y) * (int)game_st.map_tile_height / (a.to.y - a.from.y);
		out = xy(32 * x + 16, 32 * y + 16);
		return true;
	}

	void issue_target(xy pos) {
		unit_t* target = select_get_unit_at(pos);
		sync_selection();
		bool q = key_shift();
		switch (pending_targ) {
		case T_ATTACK: action_order(my_player, get_order_type(Orders::AttackDefault), pos, target, nullptr, q); break;
		case T_MOVE:   action_order(my_player, get_order_type(Orders::Move), pos, target, nullptr, q); break;
		case T_PATROL: action_order(my_player, get_order_type(Orders::Patrol), pos, nullptr, nullptr, q); break;
		case T_GATHER: action_default_order(my_player, pos, target, nullptr, q); break;
		case T_REPAIR: action_order(my_player, get_order_type(Orders::Repair), pos, target, nullptr, q); break;
		case T_RALLY:  action_order(my_player, get_order_type(target ? Orders::RallyPointUnit : Orders::RallyPointTile), pos, target, nullptr, false); break;
		}
		if (unit_t* u = primary_selected())
			play_unit_ack(u, u->unit_type->first_yes_sound, u->unit_type->last_yes_sound);
	}

	void place_pending(int mx, int my) {
		xy map_pos = screen_to_map(mx, my);
		int tx = (map_pos.x - pending_build->placement_size.x / 2 + 16) / 32;   // +16 = round to nearest tile
		int ty = (map_pos.y - pending_build->placement_size.y / 2 + 16) / 32;
		if (tx < 0) tx = 0;
		if (ty < 0) ty = 0;
		sync_selection();
		action_build(my_player, get_order_type(kit.build_order), pending_build, {(size_t)tx, (size_t)ty});
		pending_build = nullptr;   // one-shot; re-open the menu to place another
	}

	// Precompute the two tint tables (built once): each maps a source palette
	// index to a dimmed, colour-washed nearest palette index — green for a
	// placeable tile, red for a blocked one.
	void build_ghost_luts() {
		ghost_ok.resize(256);
		ghost_bad.resize(256);
		const auto& wpe = tileset_img.wpe;
		for (int i = 0; i != 256; ++i) {
			int r = wpe[4 * i], g = wpe[4 * i + 1], b = wpe[4 * i + 2];
			auto wash = [&](int tr, int tg, int tb) {
				int c = nearest_palette_color((r * 45 + tr * 55) / 100,
				                              (g * 45 + tg * 55) / 100,
				                              (b * 45 + tb * 55) / 100);
				return (uint8_t)(c ? c : 1);   // keep 0 reserved as "empty" in the scratch buffer
			};
			ghost_ok[i]  = wash(20, 235, 60);
			ghost_bad[i] = wash(235, 40, 40);
		}
	}

	// Rasterize GRP frame `fi` into an RGBA icon: palette-expanded and nearest-neighbour
	// downscaled to fit ICON_MAX. `pc` (or null) is the player-colour ramp for indices
	// 8..15 (as in draw_image); index 0 stays transparent. Uses the tileset palette (the
	// only one OpenBW loads — the game's cmdicons need a UI palette not in our assets).
	// Returns the pixels (valid until the next call) with icon_w/icon_h set.
	const uint8_t* rasterize_icon(const grp_t* grp, size_t fi, const uint8_t* pc) {
		static const int ICON_MAX = 36;
		if (fi >= grp->frames.size()) fi = 0;
		const auto& f = grp->frames.at(fi);
		int w = f.size.x, h = f.size.y;
		icon_index.assign((size_t)w * h, 0);
		draw_frame(f, false, icon_index.data(), w, 0, 0, w, h,
		           [pc](uint8_t c, uint8_t) -> uint8_t { return (pc && c >= 8 && c < 16) ? pc[c - 8] : c; });

		int scale = std::max(1, (std::max(w, h) + ICON_MAX - 1) / ICON_MAX);
		int ow = (w + scale - 1) / scale, oh = (h + scale - 1) / scale;
		icon_rgba.assign((size_t)ow * oh * 4, 0);
		const auto& wpe = tileset_img.wpe;
		for (int y = 0; y != oh; ++y) {
			for (int xo = 0; xo != ow; ++xo) {
				int sx = xo * scale + scale / 2, sy = y * scale + scale / 2;
				if (sx >= w) sx = w - 1;
				if (sy >= h) sy = h - 1;
				uint8_t c = icon_index[(size_t)sy * w + sx];
				if (!c) continue;   // transparent
				uint8_t* o = &icon_rgba[((size_t)y * ow + xo) * 4];
				o[0] = wpe[4 * c + 0]; o[1] = wpe[4 * c + 1]; o[2] = wpe[4 * c + 2]; o[3] = 255;
			}
		}
		icon_w = ow; icon_h = oh;
		return icon_rgba.data();
	}

	// Command-card icon: a unit's sprite, coloured with the player's ramp. Directional
	// units use frame 12 (south-east, 3/4 front); buildings aren't directional (frame 0).
	const uint8_t* render_icon(int unit_id) {
		if (unit_id < 0) { icon_w = icon_h = 0; return nullptr; }
		const unit_type_t* utp = get_unit_type((UnitTypes)unit_id);
		const image_type_t* it = utp->flingy->sprite->image;
		const grp_t* grp = global_st.image_grp[(size_t)it->id];
		const uint8_t* pc = img.player_unit_colors.at(st.players[my_player].color).data();
		return rasterize_icon(grp, it->has_directional_frames ? 12 : 0, pc);
	}

	// Resource-HUD icon: 0 = minerals, 1 = gas, 2 = supply. Minerals/gas are the neutral
	// sprites a worker carries when delivering (blue mineral chunk / green gas container,
	// per race). Supply reuses the player-coloured unit renderer for the race's supply
	// provider (Supply Depot / Pylon / Overlord), so it sits naturally beside the others.
	const uint8_t* render_res_icon(int which) {
		if (which == 2) {
			int uid = (int)my_race == 2 ? (int)UnitTypes::Protoss_Pylon
			        : (int)my_race == 0 ? (int)UnitTypes::Zerg_Overlord
			        : (int)UnitTypes::Terran_Supply_Depot;
			return render_icon(uid);
		}
		ImageTypes id = ImageTypes::IMAGEID_Mineral_Chunk_Type1;
		if (which == 1) {
			id = (int)my_race == 2 ? ImageTypes::IMAGEID_Protoss_Gas_Orb_Type1
			   : (int)my_race == 0 ? ImageTypes::IMAGEID_Zerg_Gas_Sac_Type1
			   : ImageTypes::IMAGEID_Terran_Gas_Tank_Type1;
		}
		return rasterize_icon(global_st.image_grp[(size_t)id], 0, nullptr);
	}

	// Placement preview: a faded, tinted silhouette of the actual building, plus a
	// footprint outline so the exact tiles it will occupy are unambiguous.
	void draw_callback(uint8_t* data, size_t data_pitch) override {
		ui_functions::draw_callback(data, data_pitch);

		// While targeting, bracket the unit under the cursor so it's clear there's a
		// target there (red for attack, green otherwise) — like the game's target cursor.
		if (targeting && mouse_x >= 0) {
			if (place_ok_color < 0) {
				place_ok_color = nearest_palette_color(40, 240, 40);
				place_bad_color = nearest_palette_color(240, 40, 40);
			}
			if (unit_t* t = select_get_unit_at(screen_to_map(mouse_x, mouse_y))) {
				int cx = t->sprite->position.x - screen_pos.x, cy = t->sprite->position.y - screen_pos.y;
				int hw = (int)t->sprite->width / 2 + 3, hh = (int)t->sprite->height / 2 + 3;
				uint8_t col = (uint8_t)(pending_targ == T_ATTACK ? place_bad_color : place_ok_color);
				line_rectangle(data, data_pitch, {{cx - hw, cy - hh}, {cx + hw, cy + hh}}, col);
			}
		}

		if (!pending_build) return;
		if (place_ok_color < 0) {
			place_ok_color = nearest_palette_color(40, 240, 40);
			place_bad_color = nearest_palette_color(240, 40, 40);
		}
		if (ghost_ok.empty()) build_ghost_luts();

		int w = pending_build->placement_size.x, h = pending_build->placement_size.y;
		xy map_pos = screen_to_map(mouse_x, mouse_y);
		int tx = (map_pos.x - w / 2 + 16) / 32; if (tx < 0) tx = 0;   // +16 = round to nearest tile
		int ty = (map_pos.y - h / 2 + 16) / 32; if (ty < 0) ty = 0;
		xy center(32 * tx + w / 2, 32 * ty + h / 2);   // building's map position
		unit_t* builder = primary_selected();
		bool ok = builder && can_place_building(builder, my_player, pending_build, center, false, false);

		// Render the completed building's first frame into a scratch buffer with the
		// tint table applied, then dithered-blit it (checkerboard = translucency).
		const grp_t* grp = global_st.image_grp[(size_t)pending_build->flingy->sprite->image->id];
		const auto& frame = grp->frames.at(0);
		int fw = frame.size.x, fh = frame.size.y;
		const uint8_t* lut = (ok ? ghost_ok : ghost_bad).data();
		ghost_buf.assign((size_t)fw * fh, 0);
		draw_frame(frame, false, ghost_buf.data(), fw, 0, 0, fw, fh,
		           [lut](uint8_t c, uint8_t) { return lut[c]; });

		int gx = center.x + (int)frame.offset.x - (int)grp->width / 2 - screen_pos.x;
		int gy = center.y + (int)frame.offset.y - (int)grp->height / 2 - screen_pos.y;
		for (int y = 0; y != fh; ++y) {
			int py = gy + y;
			if (py < 0 || py >= (int)screen_height) continue;
			const uint8_t* src = ghost_buf.data() + (size_t)y * fw;
			uint8_t* row = data + (size_t)py * data_pitch;
			for (int x = 0; x != fw; ++x) {
				if (!src[x] || ((x ^ y) & 1)) continue;   // 0 = empty, checkerboard = faded
				int px = gx + x;
				if (px >= 0 && px < (int)screen_width) row[px] = src[x];
			}
		}

		int ox = tx * 32 - screen_pos.x, oy = ty * 32 - screen_pos.y;
		line_rectangle(data, data_pitch, {{ox, oy}, {ox + w, oy + h}},
		               (uint8_t)(ok ? place_ok_color : place_bad_color));
	}

	// Render a frame, then rebuild the command card so it tracks live state — a
	// building finishing construction, tech completing, resources changing what's
	// affordable — without needing a re-select. The JS host only touches the DOM when
	// the card text actually changes, so refreshing every frame is cheap.
	int edge_dir = 0;   // current edge-scroll direction (0 none, 1=N clockwise to 8=NW)

	// Scroll the camera when the cursor rests against a screen edge (classic RTS
	// edge panning). ui.update() clamps screen_pos to the map afterwards. The host
	// parks the cursor off-screen (negative) on mouse-leave so this stops then.
	void edge_scroll() {
		edge_dir = 0;
		const int margin = 6, speed = 16;
		if (mouse_x < 0 || mouse_y < 0) return;
		int dx = 0, dy = 0;
		if (mouse_x < margin) dx = -speed; else if (mouse_x >= (int)screen_width - margin) dx = speed;
		if (mouse_y < margin) dy = -speed; else if (mouse_y >= (int)screen_height - margin) dy = speed;
		if (!dx && !dy) return;
		screen_pos += xy(dx, dy);
		static const int dir[3][3] = {{8, 1, 2}, {7, 0, 3}, {6, 5, 4}};   // [dy sign][dx sign]
		edge_dir = dir[dy > 0 ? 2 : dy < 0 ? 0 : 1][dx > 0 ? 2 : dx < 0 ? 0 : 1];
	}

	void update() {
		ui_functions::update();   // processes input first, so edge_scroll sees the fresh cursor
		edge_scroll();
		refresh_card();
	}

	bool handle_game_input(const native_window::event_t& e) override {
		using ev = native_window::event_t;
		if (e.type == ev::type_mouse_motion) { mouse_x = e.mouse_x; mouse_y = e.mouse_y; return false; }

		if (e.type == ev::type_mouse_button_down && e.button == 1) {
			if (pending_build) { place_pending(e.mouse_x, e.mouse_y); refresh_card(); return true; }
			if (targeting) {
				xy p;
				if (!minimap_point(e.mouse_x, e.mouse_y, p)) p = screen_to_map(e.mouse_x, e.mouse_y);
				issue_target(p); targeting = false; return true;
			}
			return false;   // base handles drag-selection / minimap camera moves
		}
		if (e.type == ev::type_mouse_button_down && e.button == 3) {
			if (pending_build || targeting || menu) {   // cancel pending mode / submenu
				pending_build = nullptr; targeting = false; menu = 0; refresh_card();
				return true;
			}
			if (current_selection.empty()) return false;   // let base pan
			xy map_pos;
			unit_t* target = nullptr;
			if (minimap_point(e.mouse_x, e.mouse_y, map_pos)) {
				// From the minimap: a position order (no unit sits under a minimap pixel).
			} else {
				map_pos = screen_to_map(e.mouse_x, e.mouse_y);
				target = select_get_unit_at(map_pos);
			}
			sync_selection();
			action_default_order(my_player, map_pos, target, nullptr, key_shift());
			if (unit_t* u = primary_selected())
				play_unit_ack(u, u->unit_type->first_yes_sound, u->unit_type->last_yes_sound);
			return true;
		}
		if (e.type == ev::type_key_down && e.scancode == 41) {   // Escape: back out of any pending mode / submenu
			pending_build = nullptr; targeting = false; menu = 0; refresh_card();
			return true;
		}
		if (e.type == ev::type_key_down && e.sym >= '0' && e.sym <= '9') {
			int n = e.sym - '0';
			if (key_ctrl()) assign_group(n); else recall_group(n);
			return true;
		}
		if (e.type == ev::type_key_down && e.sym > 0) return run_command((char)e.sym);
		return false;
	}
};

}  // namespace bwgame

#endif
