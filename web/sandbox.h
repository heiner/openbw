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
#include "replay_saver.h"

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
struct mp_slot { int slot; race_t race; };

// Melee setup for an arbitrary set of occupied slots. Single-player is just n == 1;
// multiplayer occupies every participating slot so all peers build a byte-identical
// initial state (which is what lockstep depends on).
template<typename load_data_file_F>
void setup_melee_slots(state& st, load_data_file_F&& load_data_file, const mp_slot* slots, size_t n,
                       uint32_t* seed_at_setup = nullptr) {
	game_load_functions game_load(st);
	game_load.load_map(std::forward<load_data_file_F>(load_data_file), [&]() {
		// The RNG seed as of this exact point is what a replay must record: the replay
		// reader restores it here too, before melee units are placed (which advances it).
		if (seed_at_setup) *seed_at_setup = st.lcg_rand_state;
		// Melee, NOT "use map settings". The engine derives use_map_settings from these
		// three being zero (bwgame.h), so leaving victory_condition at 0 silently ran the
		// map's own trigger set — which on a melee map shares vision between players and
		// made fog non-per-player. victory_condition 1 loads the built-in melee triggers.
		game_load.setup_info.victory_condition = 1;
		game_load.setup_info.tournament_mode = 0;
		game_load.setup_info.starting_units = 0;
		game_load.setup_info.resource_type = 1;         // standard melee resources
		game_load.setup_info.starting_minerals = 50;   // authentic melee start
		for (int i = 0; i != 12; ++i) {
			st.players[i].controller = player_t::controller_inactive;
			game_load.setup_info.create_melee_units_for_player[i] = false;
		}
		for (size_t k = 0; k != n; ++k) {
			int i = slots[k].slot;
			if (i < 0 || i >= 12) continue;
			st.players[i].controller = player_t::controller_occupied;
			st.players[i].race = slots[k].race;
			game_load.setup_info.create_melee_units_for_player[i] = true;
		}
	});
}

template<typename load_data_file_F>
void setup_melee(state& st, load_data_file_F&& load_data_file, int my_player, race_t my_race) {
	mp_slot s{my_player, my_race};
	setup_melee_slots(st, std::forward<load_data_file_F>(load_data_file), &s, 1);
}

// Desync probe: folds the whole visible sim state into one value. Peers compare it
// periodically; any divergence means the sims have drifted and lockstep is broken.
// Must stay coarse enough to be cheap but fine enough to catch real drift — per-unit
// position/hp/order are what actually diverge, so economy counters alone are not enough.
// Single definition shared by the native test and the wasm export so they can't disagree.
inline unsigned sim_checksum(state& st) {
	unsigned h = 2166136261u;
	auto mix = [&h](unsigned v) { h = (h ^ v) * 16777619u; };
	mix((unsigned)st.current_frame);
	mix(st.lcg_rand_state);
	for (int i = 0; i != 12; ++i) {
		if (st.players[i].controller != player_t::controller_occupied) continue;
		mix((unsigned)st.current_minerals[i]);
		mix((unsigned)st.current_gas[i]);
		for (unit_t* u : ptr(st.player_units[i])) {
			mix((unsigned)u->index);
			mix((unsigned)u->sprite->position.x);
			mix((unsigned)u->sprite->position.y);
			mix((unsigned)u->hp.raw_value);
			mix((unsigned)(u->order_type ? (int)u->order_type->id : 0xffff));
		}
	}
	return h;
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
// BW command-byte writer. Emits framed records into `out`: [u16 len][u8 opcode][payload],
// little-endian, with payload layouts mirroring the matching read_action_* in actions.h.
// A null unit target serialises as id 0 (real ids are index+1) and "no unit type" as
// UnitTypes::None; both read back as nullptr.
//
// NOTE: this is a wire format shared between peers — bump PROTOCOL in web/net.js on any
// change here, or mismatched builds will desync instead of failing cleanly.
struct bw_cmd {
	a_vector<uint8_t>& out;
	a_vector<uint8_t> pend;
	explicit bw_cmd(a_vector<uint8_t>& out) : out(out) {}

	void begin(int op) { pend.clear(); pend.push_back((uint8_t)op); }
	void u8(int x) { pend.push_back((uint8_t)x); }
	void u16(int x) { pend.push_back((uint8_t)(x & 0xff)); pend.push_back((uint8_t)((x >> 8) & 0xff)); }
	void end() {
		size_t n = pend.size();
		out.push_back((uint8_t)(n & 0xff));
		out.push_back((uint8_t)((n >> 8) & 0xff));
		out.insert(out.end(), pend.begin(), pend.end());
	}

	void select(const uint16_t* ids, size_t n) { begin(9); u8((int)n); for (size_t i = 0; i != n; ++i) u16(ids[i]); end(); }
	void build(Orders o, UnitTypes t, int tx, int ty) { begin(12); u8((int)o); u16(tx); u16(ty); u16((int)t); end(); }
	void default_order(xy pos, uint16_t target, bool q) {
		begin(20); u16(pos.x); u16(pos.y); u16(target); u16((int)UnitTypes::None); u8(q ? 1 : 0); end();
	}
	void order(Orders o, xy pos, uint16_t target, bool q) {
		begin(21); u16(pos.x); u16(pos.y); u16(target); u16((int)UnitTypes::None); u8((int)o); u8(q ? 1 : 0); end();
	}
	void bare(int op) { begin(op); end(); }                         // 49 cancel research, 51 cancel upgrade, 54 stim
	void queued(int op, bool q) { begin(op); u8(q ? 1 : 0); end(); } // 26 stop, 43 hold, 38 siege, 37 unsiege
	void type(int op, UnitTypes t) { begin(op); u16((int)t); end(); }// 31 train, 35 morph, 53 morph building
	void id8(int op, int v) { begin(op); u8(v); end(); }             // 48 research, 50 upgrade
	void cancel_slot(int slot) { begin(32); u16(slot); end(); }      // 32 cancel build queue
	void liftoff(xy pos) { begin(47); u16(pos.x); u16(pos.y); end(); }   // 47 building lift off
	void unload_all(bool q) { begin(40); u8(q ? 1 : 0); end(); }     // 40 unload all cargo
	void unload(uint16_t target) { begin(41); u16(target); end(); } // 41 unload one unit
	void return_cargo(bool q) { begin(30); u8(q ? 1 : 0); end(); }   // 30 return cargo
	void cloak() { begin(33); u8(0); end(); }                        // 33 cloak
	void decloak() { begin(34); u8(0); end(); }                      // 34 decloak
	void train_fighter() { begin(39); end(); }                       // 39 build interceptor/scarab
	void burrow(bool q) { begin(44); u8(q ? 1 : 0); end(); }         // 44 burrow
	void unburrow() { begin(45); u8(0); end(); }                     // 45 unburrow
	void morph_archon() { begin(42); end(); }                        // 42 merge archon
	void morph_dark_archon() { begin(90); end(); }                   // 90 merge dark archon
	void player_leave(int reason) { begin(87); u8(reason); end(); }  // 87 leave (resign)
	void cancel_build() { begin(24); end(); }                        // 24 cancel building under construction
	void cancel_morph() { begin(25); end(); }                        // 25 cancel morph
	void cancel_nuke() { begin(46); end(); }                         // 46 cancel nuke
	void cancel_addon() { begin(52); end(); }                        // 52 cancel addon
};

// Apply one player's framed batch, in order, through the engine's action reader. `rec`
// sees each decoded record first — used to tee the stream into a replay. The bytes are
// already exactly BW's action format, so a recorded stream is a valid .rep body.
template<typename action_functions_T, typename record_F>
void apply_bw_commands(action_functions_T& af, int owner, const uint8_t* data, size_t size, record_F&& rec) {
	size_t i = 0;
	while (i + 2 <= size) {
		size_t n = (size_t)data[i] | ((size_t)data[i + 1] << 8);
		i += 2;
		if (n == 0 || i + n > size) break;
		rec(owner, data + i, n);
		af.read_action(owner, data + i, n);
		i += n;
	}
}

// Convenience overload with no recorder.
template<typename action_functions_T>
void apply_bw_commands(action_functions_T& af, int owner, const uint8_t* data, size_t size) {
	apply_bw_commands(af, owner, data, size, [](int, const uint8_t*, size_t) {});
}

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
	bool spectator = false;   // watching a bot-vs-bot: nobody's player (no acks; HUD follows selection)
	build_kit kit;

	const unit_type_t* pending_build = nullptr;   // building awaiting a placement click
	bool pending_land = false;                    // that placement is a flying building landing
	const unit_type_t* pending_addon = nullptr;   // placing an addon: pending_build is the parent
	bool targeting = false;                       // an order awaiting a target click
	bool paused = false;                          // frozen: ignore game input (camera still ok)
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

	enum targ_t { T_ATTACK, T_MOVE, T_PATROL, T_GATHER, T_REPAIR, T_RALLY, T_SPELL };
	targ_t pending_targ = T_ATTACK;
	Orders pending_spell_order{};   // the spell to cast once its target is clicked
	bool pending_spell_unit = false;   // does that spell target a unit (vs the ground)?

	enum cmd_act { C_MOVE, C_STOP, C_ATTACK, C_GATHER, C_HOLD, C_PATROL,
	               C_BUILDMENU, C_ADVMENU, C_BUILD, C_TRAIN, C_MORPH, C_MORPHBLDG, C_STIM, C_SIEGE, C_UNSIEGE, C_REPAIR,
	               C_SELECT,     // select the player's units of cmd.ut (SCVs / Probes / Larvae)
	               C_RALLY, C_RESEARCH, C_UPGRADE,
	               C_LIFT, C_LAND,      // Terran flying buildings
	               C_UNLOAD, C_UNLOADALL,     // eject cargo from a bunker / transport
	               C_RETURN,                  // worker return cargo
	               C_BURROW, C_UNBURROW,      // Zerg burrow
	               C_CLOAK, C_DECLOAK,        // Wraith / Ghost cloak
	               C_FIGHTER,                 // Carrier interceptor / Reaver scarab
	               C_ARCHON, C_DARCHON,       // High / Dark Templar merges
	               C_SPELL,                   // a targeted spellcaster ability
	               C_CANCEL };                // cancel a morph / addon / nuke (opcode in cmd.unit)
	struct cmd_t { char key; const char* label; cmd_act act; UnitTypes ut; bool enabled = false;
	               TechTypes tech = TechTypes::None; UpgradeTypes upg = UpgradeTypes::None;
	               uint16_t unit = 0;          // target unit id (C_UNLOAD)
	               UnitTypes req = UnitTypes::None; };   // grayed: the missing prerequisite to show
	a_vector<cmd_t> card;
	a_string card_text;                           // "title\nKEY\tLabel\tEN\n…" for the JS overlay
	a_string status_text;                         // producer queue + progress, rebuilt per frame
	a_string resources_text;                      // minerals/gas/supply HUD, rebuilt per frame
	a_string error_text, error_status_text;       // last blocked-command reason, for the JS toast
	int error_seq = 0;                            // bumped whenever error_text is (re)set
	bool show_order_lines = false;                   // draw selected-unit order/rally lines (off by default)
	int line_move_color = -1, line_atk_color = -1;   // order/rally line palette indices (lazy)
	int ring_neutral_color = -1;                     // yellow ring for neutral targets (lazy)
	double ui_now = 0;                               // wall-clock ms from JS (performance.now), for time-based UI
	unit_id flash_unit;                              // target whose ring is flashing (0 = none)
	double flash_start = -1;                         // ms when the flash began (-1 = idle)
	// Event feedback: unit-ready voices on completion, "under attack" voice + minimap flash.
	int under_attack_sound = -2;                  // advisor sfx id (-2 = unresolved, -1 = not found)
	int alert_color = -1;                         // minimap flash palette index (lazy)
	int alert_cooldown = 0;                       // update-ticks until the voice may replay
	int event_tick = 0;                           // local tick for the flash blink phase
	static const int ALERT_TTL = 90;              // 3 sweeps of the 30-tick ping cycle
	struct alert_t { xy pos; int ttl; };
	a_vector<alert_t> alerts;                     // active minimap flash markers
	xy last_event_pos; bool have_last_event = false;   // Space recenters here
	a_unordered_map<uint16_t, int> last_life;     // per own unit: last hp+shields, to spot damage
	a_unordered_set<uint16_t> announced;          // own units whose ready sound has fired
	bool events_seeded = false;                   // first poll seeds silently (no startup spam)
	// System message log (eliminations, later chat), shown as fading lines top-left.
	struct log_msg { a_string text; int age; };
	a_vector<log_msg> msg_log;
	a_string msg_out;                             // rebuilt for openbw_messages()
	static const int MSG_TTL = 480;               // ticks a message stays before fading out
	a_vector<uint8_t> player_had_units = a_vector<uint8_t>(8);   // slot ever had a unit
	a_vector<uint8_t> player_eliminated = a_vector<uint8_t>(8);  // already announced dead
	int outcome = 0;                              // 0 undecided, 1 victory, 2 defeat
	bool competitive = false;                     // true only with an opponent (multiplayer)

	// Replay recording. Our command stream already *is* BW's action format, so recording
	// is just teeing it into replay_saver_state along with the map's CHK bytes.
	replay_saver_state rep;
	a_vector<uint8_t> rep_map;                    // CHK bytes backing rep.map_data
	a_vector<uint8_t> rep_out;                    // the serialised .rep, built on demand
	bool rep_on = false;
	void rep_record(int owner, const uint8_t* data, size_t n) {
		if (rep_on) replay_saver_functions(rep).add_action(st.current_frame, owner, data, n);
	}

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
	double last_recall_ms = -1e9;   // ui_now at the last recall, for double-tap timing
	static constexpr double GROUP_DBLTAP_MS = 300;   // recenter only within this window

	void assign_group(int n) {
		auto& g = groups[n];
		g.clear();
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (u && u->owner == my_player) g.push_back(uid);
		}
		last_recalled_group = -1;   // next tap of n selects, not centers
	}

	// Put the camera's centre on a map position (clamped to the map by ui.update()).
	void center_on(xy pos) { screen_pos = xy(pos.x - view_width / 2, pos.y - view_height / 2); }

	void center_on_selection() {
		long sx = 0, sy = 0; int n = 0;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (u) { sx += u->sprite->position.x; sy += u->sprite->position.y; ++n; }
		}
		if (n) center_on(xy((int)(sx / n), (int)(sy / n)));
	}

	void recall_group(int n) {
		// Center only on a genuine double-tap — the same group again within the window. A slow
		// second press just reselects, as in the original (the camera doesn't jump).
		bool refocus = (n == last_recalled_group && ui_now - last_recall_ms < GROUP_DBLTAP_MS);
		last_recall_ms = ui_now;
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

	// ---- deterministic command stream --------------------------------------------
	// Every action leaves as BW command bytes (see bw_cmd) and is applied through
	// read_action() — the same path replays use — so all peers apply an identical stream
	// and stay in lockstep. Single-player is just a one-player session with zero input
	// delay, so this path is always exercised.
	a_vector<uint8_t> outgoing;   // framed records drained by the host each frame
	bw_cmd cmds{outgoing};

	void cmd_build(Orders o, const unit_type_t* t, int tx, int ty) { cmds.build(o, t->id, tx, ty); }
	void cmd_default_order(xy pos, unit_t* target, bool q) {
		cmds.default_order(pos, get_unit_id(target).raw_value, q);
	}
	void cmd_order(Orders o, xy pos, unit_t* target, bool q) {
		cmds.order(o, pos, get_unit_id(target).raw_value, q);
	}
	void cmd_liftoff(xy pos) { cmds.liftoff(pos); }
	void cmd_bare(int opcode) { cmds.bare(opcode); }
	void cmd_queued(int opcode, bool q) { cmds.queued(opcode, q); }
	void cmd_type(int opcode, UnitTypes t) { cmds.type(opcode, t); }
	void cmd_id8(int opcode, int v) { cmds.id8(opcode, v); }
	void cmd_cancel_slot(int slot) { cmds.cancel_slot(slot); }
	void apply_commands(int owner, const uint8_t* data, size_t size) {
		// Every applied action is also recorded, so the replay matches the game exactly —
		// including in multiplayer, where the peer's actions come through here too.
		apply_bw_commands(*this, owner, data, size,
			[this](int o, const uint8_t* d, size_t n) { rep_record(o, d, n); });
	}

	// Push the UI's visual selection into the sim's per-player selection, capped to BW's
	// 12-unit limit, own units only. Emitted as a command so every peer applies the same
	// selection immediately before whatever order follows it.
	void sync_selection() {
		uint16_t ids[12]; size_t n = 0;
		for (auto uid : current_selection) {
			if (n == 12) break;
			unit_t* u = get_unit(uid);
			if (u && u->owner == my_player) ids[n++] = get_unit_id(u).raw_value;
		}
		cmds.select(ids, n);
	}

	// Sync only the primary unit to the sim. Single-unit actions (action_build via
	// get_single_selected_unit) silently no-op when the whole group is synced — with several
	// SCVs selected, placing a building did nothing. BW sends one worker from the group too;
	// the on-screen selection is untouched.
	void sync_primary() {
		unit_t* u = primary_selected();
		if (!u) return;
		uint16_t id = get_unit_id(u).raw_value;
		cmds.select(&id, 1);
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
		if (spectator) return;   // a spectator is nobody's player — no unit voices, for anyone
		if (!u || first <= 0 || last < first) return;
		int id = first + (sound_rotation++ % (last - first + 1));
		play_sound(id, u->sprite->position, u, false);   // 4-arg form (override hides the others)
	}

	// Selection ack: the unit's "what" line, escalating to its "pissed"
	// (annoyed) lines when the same single unit is clicked repeatedly.
	void on_selection(bool) override {
		if (current_selection.size() > 12) current_selection.resize(12);   // BW's 12-unit cap
		// Can't select what you can't see: drop anything hidden by fog.
		if (fog_player >= 0) {
			a_vector<unit_t*> vis;
			for (auto uid : current_selection) { unit_t* u = get_unit(uid); if (u && !unit_hidden_by_fog(u)) vis.push_back(u); }
			if (vis.size() != current_selection.size()) {
				current_selection_clear();
				for (unit_t* u : vis) current_selection_add(u);
			}
		}
		// BW's other selection rule: a selection is either your own units, or exactly one
		// unit that isn't yours. You can't box up an enemy army to inspect it, and you
		// can't mix theirs in with yours — dragging over both keeps only yours.
		{
			a_vector<unit_t*> own;
			for (auto uid : current_selection) {
				unit_t* u = get_unit(uid);
				if (u && u->owner == my_player) own.push_back(u);
			}
			if (!own.empty()) {
				// A selection can't mix units with buildings or hold more than one
				// building: keep the non-buildings, or a single building if that's all.
				a_vector<unit_t*> keep;
				for (unit_t* u : own) if (!ut_building(u)) keep.push_back(u);
				if (keep.empty()) keep.push_back(own.front());
				if (keep.size() != current_selection.size()) {
					current_selection_clear();
					for (unit_t* u : keep) current_selection_add(u);
				}
			} else if (current_selection.size() > 1) {
				unit_t* first = nullptr;
				for (auto uid : current_selection) { first = get_unit(uid); if (first) break; }
				current_selection_clear();
				if (first) current_selection_add(first);
			}
		}
		menu = 0;
		last_recalled_group = -1;   // a fresh selection resets group double-tap tracking
		unit_t* u = primary_selected();
		if (!u) { ack_count = 0; refresh_card(); return; }
		// Like the original: selecting a building with a rally waypoint pops the ground
		// marker there, so you can see where its units will gather. (Same real-rally test
		// as draw_order_lines: a set point that isn't just the building itself.)
		if (u_grounded_building(u)) {
			xy r = u->building.rally.pos;
			if (r != xy() && !close_xy(r, u->sprite->position)) show_marker(r);
		}
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

	// The engine's melee triggers decide the winner and report it here (victory_state >= 3).
	// Note they only ever mark the *winner*: a wiped-out player's victory_state stays 0, so
	// a defeat has to be inferred from someone else having won. Both peers run the same
	// deterministic sim, so a 1v1 reaches the same verdict on the same frame for free.
	void on_victory_state(int owner, int state) override {
		// Single-player is a sandbox — a lone melee player "wins" instantly, so ignore it.
		if (!competitive || outcome || state == 0) return;
		if (owner == my_player) outcome = state >= 3 ? 1 : 2;
		else if (state >= 3) outcome = 2;          // someone else won, so we lost
	}
	void check_last_standing() {
		if (!competitive || outcome) return;
		if (st.players[my_player].victory_state >= 3) { outcome = 1; return; }
		for (int i = 0; i != 8; ++i)
			if (i != my_player && st.players[i].victory_state >= 3) { outcome = 2; return; }
		// Direct defeat signal, in case the triggers haven't resolved yet: nothing left.
		if (events_seeded && st.players[my_player].initially_active &&
		    count_units(st, my_player) == 0) outcome = 2;
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

	// Names from rez/stat_txt.tbl (loaded lazily), race prefix stripped — so labels read
	// "Overlord" / "Supply Depot" / "Leg Enhancements" instead of "Unit"/"Building".
	a_vector<a_string> stat_txt_names;
	const char* stat_name(int i) {
		if (stat_txt_names.empty()) {
			a_vector<uint8_t> d;
			load_data_file(d, "rez/stat_txt.tbl");
			if (d.size() >= 2) {
				int count = d[0] | (d[1] << 8);
				stat_txt_names.resize(count);
				for (int j = 0; j != count; ++j) {
					int off = d[2 + j * 2] | (d[3 + j * 2] << 8);
					const char* s = (const char*)&d[off];
					for (const char* pre : {"Terran ", "Protoss ", "Zerg "}) {   // strip race prefix
						const char* a = s; const char* b = pre;
						while (*b && *a == *b) { ++a; ++b; }
						if (!*b) { s = a; break; }
					}
					stat_txt_names[j] = s;
				}
			}
		}
		return (i >= 0 && i < (int)stat_txt_names.size()) ? stat_txt_names[i].c_str() : "?";
	}
	const char* unit_name(UnitTypes id) { return stat_name((int)id); }
	// upgrade/tech .label is a 1-based stat_txt index (0 = none), unlike the unit id.
	const char* upgrade_name(const upgrade_type_t* up) { return stat_name(up->label - 1); }
	const char* tech_name(const tech_type_t* te) { return stat_name(te->label - 1); }

	// Pick a hotkey for `label`: first unused letter in the word, else any unused letter.
	char pick_key(const char* label) {
		auto used = [&](char c) { for (auto& cc : card) if (cc.key == c) return true; return false; };
		for (const char* s = label; *s; ++s) {
			char c = (*s >= 'A' && *s <= 'Z') ? (char)(*s + 32) : *s;
			if (c >= 'a' && c <= 'z' && !used(c)) return c;
		}
		for (char c = 'a'; c <= 'z'; ++c) if (!used(c)) return c;
		return '?';
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

	// Add everything `u` can currently produce — the whole tech tree, gated by the
	// engine's own can-build/upgrade/research checks so a button appears as soon as its
	// prerequisites are met. buildings_only splits the worker's build submenu (which
	// lists buildings) from a producer's card (units/morphs + upgrades + research).
	// Canonical original-StarCraft train/morph hotkey per unit (0 = none, fall back to
	// auto-assign). Keys are collision-free within each production building, matching the
	// game; sources: Liquipedia. Buildings keep auto-assignment for now — matching those
	// 1:1 would need splitting the worker Build card into basic/advanced sub-menus.
	static char bw_train_key(UnitTypes id) {
		using U = UnitTypes;
		switch (id) {
		case U::Terran_SCV: return 's'; case U::Terran_Marine: return 'm';
		case U::Terran_Firebat: return 'f'; case U::Terran_Medic: return 'c';
		case U::Terran_Ghost: return 'g'; case U::Terran_Vulture: return 'v';
		case U::Terran_Siege_Tank_Tank_Mode: return 't'; case U::Terran_Goliath: return 'g';
		case U::Terran_Wraith: return 'w'; case U::Terran_Dropship: return 'd';
		case U::Terran_Science_Vessel: return 'v'; case U::Terran_Battlecruiser: return 'b';
		case U::Terran_Valkyrie: return 'y';
		case U::Protoss_Probe: return 'p'; case U::Protoss_Zealot: return 'z';
		case U::Protoss_Dragoon: return 'd'; case U::Protoss_High_Templar: return 't';
		case U::Protoss_Dark_Templar: return 'k'; case U::Protoss_Shuttle: return 's';
		case U::Protoss_Reaver: return 'v'; case U::Protoss_Observer: return 'o';
		case U::Protoss_Scout: return 's'; case U::Protoss_Carrier: return 'c';
		case U::Protoss_Arbiter: return 'a'; case U::Protoss_Corsair: return 'o';
		case U::Zerg_Drone: return 'd'; case U::Zerg_Zergling: return 'z';
		case U::Zerg_Hydralisk: return 'h'; case U::Zerg_Mutalisk: return 'm';
		case U::Zerg_Overlord: return 'o'; case U::Zerg_Queen: return 'q';
		case U::Zerg_Scourge: return 's'; case U::Zerg_Defiler: return 'f';
		case U::Zerg_Ultralisk: return 'u'; case U::Zerg_Lurker: return 'l';
		case U::Zerg_Guardian: return 'g'; case U::Zerg_Devourer: return 'd';
		default: return 0;
		}
	}

	// Targeted spellcaster abilities. Each is action_order (opcode 21) with the spell's
	// Cast* order, gated by unit_can_use_tech (tech researched + right unit) and grayed when
	// energy is short. targ_unit picks a unit target; otherwise it targets the ground.
	struct spell_t { UnitTypes caster; TechTypes tech; Orders order; bool targ_unit; const char* label; char key; };
	static const spell_t* spells(size_t& n) {
		using U = UnitTypes; using T = TechTypes; using O = Orders;
		static const spell_t s[] = {
			{U::Terran_Ghost, T::Lockdown, O::CastLockdown, true, "Lockdown", 'l'},
			{U::Terran_Science_Vessel, T::Defensive_Matrix, O::CastDefensiveMatrix, true, "Defensive Matrix", 'd'},
			{U::Terran_Science_Vessel, T::Irradiate, O::CastIrradiate, true, "Irradiate", 'i'},
			{U::Terran_Science_Vessel, T::EMP_Shockwave, O::CastEMPShockwave, false, "EMP Shockwave", 'e'},
			{U::Terran_Battlecruiser, T::Yamato_Gun, O::FireYamatoGun, true, "Yamato Gun", 'y'},
			{U::Terran_Medic, T::Restoration, O::CastRestoration, true, "Restoration", 'r'},
			{U::Terran_Medic, T::Optical_Flare, O::CastOpticalFlare, true, "Optical Flare", 'f'},
			{U::Protoss_High_Templar, T::Psionic_Storm, O::CastPsionicStorm, false, "Psionic Storm", 't'},
			{U::Protoss_High_Templar, T::Hallucination, O::CastHallucination, true, "Hallucination", 'l'},
			{U::Protoss_Dark_Archon, T::Feedback, O::CastFeedback, true, "Feedback", 'f'},
			{U::Protoss_Dark_Archon, T::Maelstrom, O::CastMaelstrom, false, "Maelstrom", 'e'},
			{U::Protoss_Dark_Archon, T::Mind_Control, O::CastMindControl, true, "Mind Control", 'c'},
			{U::Protoss_Arbiter, T::Recall, O::CastRecall, false, "Recall", 'r'},
			{U::Protoss_Arbiter, T::Stasis_Field, O::CastStasisField, false, "Stasis Field", 't'},
			{U::Protoss_Corsair, T::Disruption_Web, O::CastDisruptionWeb, false, "Disruption Web", 'd'},
			{U::Zerg_Queen, T::Ensnare, O::CastEnsnare, false, "Ensnare", 'e'},
			{U::Zerg_Queen, T::Parasite, O::CastParasite, true, "Parasite", 'r'},
			{U::Zerg_Queen, T::Spawn_Broodlings, O::CastSpawnBroodlings, true, "Spawn Broodlings", 'b'},
			{U::Zerg_Defiler, T::Dark_Swarm, O::CastDarkSwarm, false, "Dark Swarm", 'w'},
			{U::Zerg_Defiler, T::Plague, O::CastPlague, false, "Plague", 'g'},
			{U::Zerg_Defiler, T::Consume, O::CastConsume, true, "Consume", 'c'},
			// Building/free abilities. Scanner Sweep and Infest are gated by unit_can_use_tech
			// like the rest; Nuclear Strike is special-cased below (no energy, needs a missile).
			{U::Terran_Comsat_Station, T::Scanner_Sweep, O::CastScannerSweep, false, "Scanner Sweep", 's'},
			{U::Zerg_Queen, T::Infestation, O::CastInfestation, true, "Infest CC", 'i'},
			{U::Terran_Ghost, T::None, O::CastNuclearStrike, false, "Nuclear Strike", 'n'},   // None = the nuke special-case
		};
		n = sizeof(s) / sizeof(s[0]);
		return s;
	}
	const spell_t* find_spell(TechTypes t) {
		size_t n; const spell_t* s = spells(n);
		for (size_t i = 0; i != n; ++i) if (s[i].tech == t) return &s[i];
		return nullptr;
	}

	// Canonical BW hotkey for building placement and building morphs (Lair, Sunken…).
	static char bw_build_key(UnitTypes id) {
		using U = UnitTypes;
		switch (id) {
		// Terran basic
		case U::Terran_Command_Center: return 'c'; case U::Terran_Supply_Depot: return 's';
		case U::Terran_Refinery: return 'r'; case U::Terran_Barracks: return 'b';
		case U::Terran_Engineering_Bay: return 'e'; case U::Terran_Missile_Turret: return 't';
		case U::Terran_Academy: return 'a'; case U::Terran_Bunker: return 'u';
		// Terran advanced
		case U::Terran_Factory: return 'f'; case U::Terran_Starport: return 's';
		case U::Terran_Science_Facility: return 'i'; case U::Terran_Armory: return 'a';
		// Protoss basic
		case U::Protoss_Nexus: return 'n'; case U::Protoss_Pylon: return 'p';
		case U::Protoss_Assimilator: return 'a'; case U::Protoss_Gateway: return 'g';
		case U::Protoss_Forge: return 'f'; case U::Protoss_Photon_Cannon: return 'c';
		case U::Protoss_Cybernetics_Core: return 'y'; case U::Protoss_Shield_Battery: return 'b';
		// Protoss advanced
		case U::Protoss_Robotics_Facility: return 'r'; case U::Protoss_Stargate: return 's';
		case U::Protoss_Citadel_of_Adun: return 'c'; case U::Protoss_Robotics_Support_Bay: return 'b';
		case U::Protoss_Fleet_Beacon: return 'f'; case U::Protoss_Templar_Archives: return 't';
		case U::Protoss_Observatory: return 'o'; case U::Protoss_Arbiter_Tribunal: return 'a';
		// Zerg basic
		case U::Zerg_Hatchery: return 'h'; case U::Zerg_Creep_Colony: return 'c';
		case U::Zerg_Extractor: return 'e'; case U::Zerg_Spawning_Pool: return 's';
		case U::Zerg_Evolution_Chamber: return 'v'; case U::Zerg_Hydralisk_Den: return 'd';
		// Zerg advanced
		case U::Zerg_Spire: return 's'; case U::Zerg_Queens_Nest: return 'q';
		case U::Zerg_Nydus_Canal: return 'n'; case U::Zerg_Ultralisk_Cavern: return 'u';
		case U::Zerg_Defiler_Mound: return 'd';
		// Zerg building morphs
		case U::Zerg_Lair: return 'l'; case U::Zerg_Hive: return 'h';
		case U::Zerg_Greater_Spire: return 'g'; case U::Zerg_Sunken_Colony: return 'u';
		case U::Zerg_Spore_Colony: return 's';
		// Terran add-ons
		case U::Terran_Comsat_Station: return 'c'; case U::Terran_Nuclear_Silo: return 'n';
		case U::Terran_Machine_Shop: return 'c'; case U::Terran_Control_Tower: return 'c';
		case U::Terran_Covert_Ops: return 'c'; case U::Terran_Physics_Lab: return 'p';
		default: return 0;
		}
	}

	// Canonical BW research/upgrade hotkeys (Liquipedia "Shortcuts"). Upgrades and
	// researched techs get their own tables; the card falls back to auto-assign only
	// for anything not listed here.
	static char bw_upgrade_key(UpgradeTypes id) {
		using U = UpgradeTypes;
		switch (id) {
		case U::Terran_Infantry_Weapons: case U::Terran_Vehicle_Weapons: return 'w';
		case U::Terran_Infantry_Armor: return 'a';
		case U::Terran_Vehicle_Plating: return 'p';
		case U::Terran_Ship_Weapons: return 's'; case U::Terran_Ship_Plating: return 'h';
		case U::U_238_Shells: return 'u'; case U::Caduceus_Reactor: return 'd';
		case U::Ion_Thrusters: return 'i'; case U::Charon_Boosters: return 'c';
		case U::Apollo_Reactor: return 'a'; case U::Titan_Reactor: return 't';
		case U::Ocular_Implants: return 'o'; case U::Moebius_Reactor: return 'm';
		case U::Colossus_Reactor: return 'c';
		case U::Protoss_Ground_Weapons: case U::Protoss_Air_Weapons: return 'w';
		case U::Protoss_Ground_Armor: case U::Protoss_Air_Armor: return 'a';
		case U::Protoss_Plasma_Shields: case U::Singularity_Charge: case U::Scarab_Damage:
		case U::Sensor_Array: return 's';
		case U::Leg_Enhancements: return 'l';
		case U::Reaver_Capacity: case U::Carrier_Capacity: return 'c';
		case U::Gravitic_Drive: case U::Gravitic_Thrusters: case U::Gravitic_Boosters: return 'g';
		case U::Khaydarin_Amulet: case U::Khaydarin_Core: return 'k';
		case U::Argus_Talisman: return 't'; case U::Apial_Sensors: return 'a'; case U::Argus_Jewel: return 'j';
		case U::Ventral_Sacs: return 'v'; case U::Antennae: return 'a'; case U::Pneumatized_Carapace: return 'p';
		case U::Metabolic_Boost: case U::Muscular_Augments: case U::Metasynaptic_Node: return 'm';
		case U::Adrenal_Glands: case U::Anabolic_Synthesis: return 'a';
		case U::Grooved_Spines: case U::Gamete_Meiosis: return 'g';
		case U::Zerg_Flyer_Attacks: case U::Zerg_Missile_Attacks: return 'a';
		case U::Zerg_Flyer_Carapace: case U::Chitinous_Plating: case U::Zerg_Carapace: return 'c';
		case U::Zerg_Melee_Attacks: return 'm';
		default: return 0;
		}
	}
	static char bw_research_key(TechTypes id) {
		using T = TechTypes;
		switch (id) {
		case T::Stim_Packs: return 't'; case T::Restoration: return 'r'; case T::Optical_Flare: return 'f';
		case T::Spider_Mines: return 'm'; case T::Tank_Siege_Mode: return 's'; case T::Cloaking_Field: return 'c';
		case T::EMP_Shockwave: return 'e'; case T::Irradiate: return 'i'; case T::Lockdown: return 'l';
		case T::Personnel_Cloaking: return 'c'; case T::Yamato_Gun: return 'y';
		case T::Psionic_Storm: return 'p'; case T::Hallucination: return 'h'; case T::Mind_Control: return 'm';
		case T::Maelstrom: return 'e'; case T::Disruption_Web: return 'd'; case T::Recall: return 'r';
		case T::Stasis_Field: return 's';
		case T::Burrowing: return 'b'; case T::Lurker_Aspect: return 'l'; case T::Spawn_Broodlings: return 'b';
		case T::Ensnare: return 'e'; case T::Plague: return 'g'; case T::Consume: return 'c';
		default: return 0;
		}
	}
	// Whether a structure lives in the worker's Advanced (V) build menu rather than Basic (B).
	static bool bw_build_advanced(UnitTypes id) {
		using U = UnitTypes;
		switch (id) {
		case U::Terran_Factory: case U::Terran_Starport: case U::Terran_Science_Facility:
		case U::Terran_Armory:
		case U::Protoss_Robotics_Facility: case U::Protoss_Stargate: case U::Protoss_Citadel_of_Adun:
		case U::Protoss_Robotics_Support_Bay: case U::Protoss_Fleet_Beacon:
		case U::Protoss_Templar_Archives: case U::Protoss_Observatory: case U::Protoss_Arbiter_Tribunal:
		case U::Zerg_Spire: case U::Zerg_Queens_Nest: case U::Zerg_Nydus_Canal:
		case U::Zerg_Ultralisk_Cavern: case U::Zerg_Defiler_Mound: return true;
		default: return false;
		}
	}

	// The prerequisite building(s) for a unit/building/addon, from BWAPI's requiredUnits
	// (bwapi/BWAPILIB/Source/UnitType.cpp). out[0]/out[1] are UnitTypes::None when absent.
	static void bw_requires(UnitTypes id, UnitTypes out[2]) {
		using U = UnitTypes;
		out[0] = U::None; out[1] = U::None;
		switch (id) {
		case U::Terran_Barracks:          out[0] = U::Terran_Command_Center; break;
		case U::Terran_Academy:           out[0] = U::Terran_Barracks; break;
		case U::Terran_Factory:           out[0] = U::Terran_Barracks; break;
		case U::Terran_Starport:          out[0] = U::Terran_Factory; break;
		case U::Terran_Science_Facility:  out[0] = U::Terran_Starport; break;
		case U::Terran_Engineering_Bay:   out[0] = U::Terran_Command_Center; break;
		case U::Terran_Armory:            out[0] = U::Terran_Factory; break;
		case U::Terran_Missile_Turret:    out[0] = U::Terran_Engineering_Bay; break;
		case U::Terran_Bunker:            out[0] = U::Terran_Barracks; break;
		case U::Terran_Comsat_Station:    out[0] = U::Terran_Academy; break;
		case U::Terran_Nuclear_Silo:      out[0] = U::Terran_Covert_Ops; break;
		case U::Zerg_Spawning_Pool:       out[0] = U::Zerg_Hatchery; break;
		case U::Zerg_Evolution_Chamber:   out[0] = U::Zerg_Hatchery; break;
		case U::Zerg_Hydralisk_Den:       out[0] = U::Zerg_Spawning_Pool; break;
		case U::Zerg_Spire:               out[0] = U::Zerg_Lair; break;
		case U::Zerg_Queens_Nest:         out[0] = U::Zerg_Lair; break;
		case U::Zerg_Nydus_Canal:         out[0] = U::Zerg_Hive; break;
		case U::Zerg_Ultralisk_Cavern:    out[0] = U::Zerg_Hive; break;
		case U::Zerg_Defiler_Mound:       out[0] = U::Zerg_Hive; break;
		case U::Protoss_Gateway:          out[0] = U::Protoss_Nexus; break;
		case U::Protoss_Forge:            out[0] = U::Protoss_Nexus; break;
		case U::Protoss_Cybernetics_Core: out[0] = U::Protoss_Gateway; break;
		case U::Protoss_Photon_Cannon:    out[0] = U::Protoss_Forge; break;
		case U::Protoss_Shield_Battery:   out[0] = U::Protoss_Gateway; break;
		case U::Protoss_Citadel_of_Adun:  out[0] = U::Protoss_Cybernetics_Core; break;
		case U::Protoss_Templar_Archives: out[0] = U::Protoss_Citadel_of_Adun; break;
		case U::Protoss_Robotics_Facility:out[0] = U::Protoss_Cybernetics_Core; break;
		case U::Protoss_Observatory:      out[0] = U::Protoss_Robotics_Facility; break;
		case U::Protoss_Robotics_Support_Bay: out[0] = U::Protoss_Robotics_Facility; break;
		case U::Protoss_Stargate:         out[0] = U::Protoss_Cybernetics_Core; break;
		case U::Protoss_Fleet_Beacon:     out[0] = U::Protoss_Stargate; break;
		case U::Protoss_Arbiter_Tribunal: out[0] = U::Protoss_Templar_Archives; out[1] = U::Protoss_Stargate; break;
		// Trainable units gated on a tech building or addon (producer is bw_what_builds).
		case U::Terran_Firebat: case U::Terran_Medic: out[0] = U::Terran_Academy; break;
		case U::Terran_Ghost:          out[0] = U::Terran_Academy; out[1] = U::Terran_Covert_Ops; break;
		case U::Terran_Goliath:        out[0] = U::Terran_Armory; break;
		case U::Terran_Siege_Tank_Tank_Mode: out[0] = U::Terran_Machine_Shop; break;
		case U::Terran_Dropship:       out[0] = U::Terran_Control_Tower; break;
		case U::Terran_Science_Vessel: out[0] = U::Terran_Control_Tower; out[1] = U::Terran_Science_Facility; break;
		case U::Terran_Battlecruiser:  out[0] = U::Terran_Control_Tower; out[1] = U::Terran_Physics_Lab; break;
		case U::Terran_Valkyrie:       out[0] = U::Terran_Control_Tower; out[1] = U::Terran_Armory; break;
		case U::Protoss_Dragoon:       out[0] = U::Protoss_Cybernetics_Core; break;
		case U::Protoss_High_Templar: case U::Protoss_Dark_Templar: out[0] = U::Protoss_Templar_Archives; break;
		case U::Protoss_Reaver:        out[0] = U::Protoss_Robotics_Support_Bay; break;
		case U::Protoss_Observer:      out[0] = U::Protoss_Observatory; break;
		case U::Protoss_Arbiter:       out[0] = U::Protoss_Arbiter_Tribunal; break;
		case U::Protoss_Carrier:       out[0] = U::Protoss_Fleet_Beacon; break;
		default: break;
		}
	}

	// Zerg buildings that morph from another building (not worker-built), so they don't
	// belong in the worker's build menu.
	static bool bw_is_building_morph(UnitTypes id) {
		using U = UnitTypes;
		return id == U::Zerg_Lair || id == U::Zerg_Hive || id == U::Zerg_Greater_Spire ||
		       id == U::Zerg_Sunken_Colony || id == U::Zerg_Spore_Colony;
	}

	// The building an addon attaches to (BWAPI whatBuilds); None if not an addon.
	static UnitTypes bw_addon_parent(UnitTypes id) {
		using U = UnitTypes;
		switch (id) {
		case U::Terran_Comsat_Station: case U::Terran_Nuclear_Silo: return U::Terran_Command_Center;
		case U::Terran_Machine_Shop:   return U::Terran_Factory;
		case U::Terran_Control_Tower:  return U::Terran_Starport;
		case U::Terran_Covert_Ops: case U::Terran_Physics_Lab: return U::Terran_Science_Facility;
		default: return U::None;
		}
	}

	// The building that trains a unit (BWAPI whatBuilds), independent of tech — the producer
	// identity that unit_can_build() checks first. Lets a producer's card show its whole
	// roster, graying the units still gated on tech, exactly as BW does. Zerg units morph
	// from Larva (not a building) and keep their own card, so they're omitted here.
	static UnitTypes bw_what_builds(UnitTypes id) {
		using U = UnitTypes;
		switch (id) {
		case U::Terran_SCV:                  return U::Terran_Command_Center;
		case U::Terran_Marine: case U::Terran_Firebat:
		case U::Terran_Medic:  case U::Terran_Ghost:            return U::Terran_Barracks;
		case U::Terran_Vulture: case U::Terran_Goliath:
		case U::Terran_Siege_Tank_Tank_Mode:                   return U::Terran_Factory;
		case U::Terran_Wraith: case U::Terran_Dropship: case U::Terran_Science_Vessel:
		case U::Terran_Battlecruiser: case U::Terran_Valkyrie:  return U::Terran_Starport;
		case U::Protoss_Probe:                                 return U::Protoss_Nexus;
		case U::Protoss_Zealot: case U::Protoss_Dragoon:
		case U::Protoss_High_Templar: case U::Protoss_Dark_Templar: return U::Protoss_Gateway;
		case U::Protoss_Shuttle: case U::Protoss_Reaver:
		case U::Protoss_Observer:                              return U::Protoss_Robotics_Facility;
		case U::Protoss_Scout: case U::Protoss_Corsair:
		case U::Protoss_Arbiter: case U::Protoss_Carrier:      return U::Protoss_Stargate;
		default: return U::None;
		}
	}

	// One-line effect blurbs for the command-card hover, in BW's own terms.
	static const char* bw_upgrade_desc(UpgradeTypes id) {
		using U = UpgradeTypes;
		switch (id) {
		case U::Terran_Infantry_Weapons: return "+1 infantry damage";
		case U::Terran_Vehicle_Weapons:  return "+1 vehicle damage";
		case U::Terran_Ship_Weapons:     return "+1 ship damage";
		case U::Terran_Infantry_Armor:   return "+1 infantry armor";
		case U::Terran_Vehicle_Plating:  return "+1 vehicle armor";
		case U::Terran_Ship_Plating:     return "+1 ship armor";
		case U::U_238_Shells:            return "+1 Marine range";
		case U::Ion_Thrusters:           return "Faster Vultures";
		case U::Caduceus_Reactor:        return "+50 Medic energy";
		case U::Charon_Boosters:         return "+3 Goliath air range";
		case U::Titan_Reactor:           return "+50 Science Vessel energy";
		case U::Ocular_Implants:         return "+2 Ghost sight";
		case U::Moebius_Reactor:         return "+50 Ghost energy";
		case U::Apollo_Reactor:          return "+50 Wraith energy";
		case U::Colossus_Reactor:        return "+50 Battlecruiser energy";
		case U::Zerg_Melee_Attacks:      return "+1 melee damage";
		case U::Zerg_Missile_Attacks:    return "+1 ranged damage";
		case U::Zerg_Flyer_Attacks:      return "+1 air damage";
		case U::Zerg_Carapace:           return "+1 ground armor";
		case U::Zerg_Flyer_Carapace:     return "+1 air armor";
		case U::Ventral_Sacs:            return "Overlords carry units";
		case U::Antennae:                return "+2 Overlord sight";
		case U::Pneumatized_Carapace:    return "Faster Overlords";
		case U::Metabolic_Boost:         return "Faster Zerglings";
		case U::Adrenal_Glands:          return "Faster Zergling attack";
		case U::Muscular_Augments:       return "Faster Hydralisks";
		case U::Grooved_Spines:          return "+1 Hydralisk range";
		case U::Gamete_Meiosis:          return "+50 Queen energy";
		case U::Metasynaptic_Node:       return "+50 Defiler energy";
		case U::Chitinous_Plating:       return "+2 Ultralisk armor";
		case U::Anabolic_Synthesis:      return "Faster Ultralisks";
		case U::Protoss_Ground_Weapons:  return "+1 ground damage";
		case U::Protoss_Air_Weapons:     return "+1 air damage";
		case U::Protoss_Ground_Armor:    return "+1 ground armor";
		case U::Protoss_Air_Armor:       return "+1 air armor";
		case U::Protoss_Plasma_Shields:  return "+1 shields";
		case U::Singularity_Charge:      return "+2 Dragoon range";
		case U::Leg_Enhancements:        return "Faster Zealots";
		case U::Scarab_Damage:           return "+ Scarab damage";
		case U::Reaver_Capacity:         return "Reaver holds 10 Scarabs";
		case U::Gravitic_Drive:          return "Faster Shuttles";
		case U::Sensor_Array:            return "+2 Observer sight";
		case U::Gravitic_Boosters:       return "Faster Observers";
		case U::Khaydarin_Amulet:        return "+50 High Templar energy";
		case U::Apial_Sensors:           return "+2 Scout sight";
		case U::Gravitic_Thrusters:      return "Faster Scouts";
		case U::Carrier_Capacity:        return "Carrier holds 8 Interceptors";
		case U::Khaydarin_Core:          return "+50 Arbiter energy";
		case U::Argus_Jewel:             return "+50 Corsair energy";
		case U::Argus_Talisman:          return "+50 Dark Archon energy";
		default: return "";
		}
	}
	static const char* bw_tech_desc(TechTypes id) {
		using T = TechTypes;
		switch (id) {
		case T::Stim_Packs:         return "+speed & attack, costs HP";
		case T::Lockdown:           return "Freezes a mechanical unit";
		case T::EMP_Shockwave:      return "Drains energy & shields";
		case T::Spider_Mines:       return "Vultures lay mines";
		case T::Scanner_Sweep:      return "Reveals an area";
		case T::Tank_Siege_Mode:    return "Siege mode: long range";
		case T::Defensive_Matrix:   return "Absorbs 250 damage";
		case T::Irradiate:          return "Damages biological units";
		case T::Yamato_Gun:         return "Heavy single-target blast";
		case T::Cloaking_Field:     return "Wraith cloak";
		case T::Personnel_Cloaking: return "Ghost cloak";
		case T::Restoration:        return "Removes status effects";
		case T::Optical_Flare:      return "Blinds a unit";
		case T::Burrowing:          return "Ground units can burrow";
		case T::Lurker_Aspect:      return "Morph Hydralisk to Lurker";
		case T::Spawn_Broodlings:   return "Kills a unit, spawns broodlings";
		case T::Plague:             return "Area damage over time";
		case T::Consume:            return "Eat a unit for energy";
		case T::Ensnare:            return "Slows units in an area";
		case T::Parasite:           return "See through a unit";
		case T::Dark_Swarm:         return "Blocks ranged attacks";
		case T::Infestation:        return "Infest a damaged Terran CC";
		case T::Psionic_Storm:      return "Area damage";
		case T::Hallucination:      return "Illusory copies of a unit";
		case T::Recall:             return "Teleport units to the Arbiter";
		case T::Stasis_Field:       return "Freezes units in an area";
		case T::Disruption_Web:     return "Blocks ground attacks";
		case T::Mind_Control:       return "Take control of a unit";
		case T::Feedback:           return "Burns energy as damage";
		case T::Maelstrom:          return "Stuns biological units";
		default: return "";
		}
	}

	// First prerequisite the owner hasn't completed yet (for the "Requires X" hint).
	UnitTypes first_missing_req(UnitTypes id, int owner) {
		UnitTypes r[2]; bw_requires(id, r);
		for (UnitTypes x : r)
			if (x != UnitTypes::None && !player_has_completed_unit(owner, x)) return x;
		return UnitTypes::None;
	}

	// cat: 0 = no menu filter (a producer's own list); 1 = worker Basic build; 2 = Advanced.
	void add_producible(unit_t* u, bool buildings_only, int cat = 0) {
		bool worker = ut_worker(u), building = ut_building(u);
		race_t race = unit_race(u);
		for (size_t i = 0; i != game_st.unit_types.vec.size(); ++i) {
			UnitTypes id = (UnitTypes)i;
			const unit_type_t* t = get_unit_type(id);
			bool tb = ut_building(t), addon = ut_addon(t);
			// Producer cards list units plus their addons (addons are buildings, but the
			// producer builds them, not a worker); the worker submenu lists real buildings.
			if (buildings_only ? (!tb || addon) : (tb && !addon)) continue;
			if (cat && tb && bw_build_advanced(id) != (cat == 2)) continue;
			bool can = unit_can_build(u, t);
			UnitTypes req = UnitTypes::None;
			if (!can) {
				// Not buildable yet: keep it, grayed with its missing prereq, for the lists BW
				// always shows — the worker's building menu, a producer's addons, and a producer's
				// own unit roster (Barracks shows Firebat/Medic grayed without an Academy, etc.).
				// bw_build_key is non-zero only for real build-menu buildings.
				bool always = (buildings_only && bw_build_key(id) && unit_race(t) == race && !bw_is_building_morph(id))
				           || (!buildings_only && addon && bw_addon_parent(id) == u->unit_type->id)
				           || (!buildings_only && !tb && bw_what_builds(id) == u->unit_type->id);
				if (!always) continue;
				req = first_missing_req(id, u->owner);
			}
			cmd_act act = worker ? C_BUILD : building ? (tb && !addon ? C_MORPHBLDG : C_TRAIN) : C_MORPH;
			const char* nm = unit_name(id);
			// Canonical BW key when it's free on this card; otherwise auto-assign.
			char k = tb ? bw_build_key(id) : bw_train_key(id);
			bool used = false;
			if (k) for (auto& c : card) if (c.key == k) { used = true; break; }
			card.push_back({(k && !used) ? k : pick_key(nm), nm, act, id, can,
			                TechTypes::None, UpgradeTypes::None, 0, req});
		}
		if (buildings_only) return;
		for (size_t i = 0; i != game_st.upgrade_types.vec.size(); ++i) {
			const upgrade_type_t* up = get_upgrade_type((UpgradeTypes)i);
			if (!unit_can_upgrade(u, up)) continue;
			const char* nm = upgrade_name(up);
			char k = bw_upgrade_key(up->id);
			if (k) for (auto& c : card) if (c.key == k) { k = 0; break; }
			card.push_back({k ? k : pick_key(nm), nm, C_UPGRADE, UnitTypes::None, true, TechTypes::None, up->id});
		}
		for (size_t i = 0; i != game_st.tech_types.vec.size(); ++i) {
			const tech_type_t* te = get_tech_type((TechTypes)i);
			if (!unit_can_research(u, te)) continue;
			const char* nm = tech_name(te);
			char k = bw_research_key(te->id);
			if (k) for (auto& c : card) if (c.key == k) { k = 0; break; }
			card.push_back({k ? k : pick_key(nm), nm, C_RESEARCH, UnitTypes::None, true, te->id});
		}
	}

	// The unit-specific part of the card (not the build submenu); returns the title.
	const char* card_for_unit(unit_t* u, UnitTypes id) {
		using U = UnitTypes;
		// A Terran SCV mid-construction shows only Cancel (Esc), like the original — all the
		// normal worker actions are hidden. Cancel halts the build via Stop: the worker walks
		// free and the building is left as an unfinished shell (resume by right-clicking it
		// with an SCV). Only the SCV building it (order ConstructingBuilding) is in this state.
		if (unit_is(u, U::Terran_SCV) && u->order_type->id == Orders::ConstructingBuilding) {
			card.push_back({'\x1b', "Halt Construction", C_STOP, U::None, true});
			return unit_name(id);
		}
		bool building = ut_building(u), worker = ut_worker(u);
		// Movement orders for commandable mobile units (not buildings, larvae, eggs).
		if (!building && !unit_is(u, U::Zerg_Larva) && !unit_is_egg(u)) add_move_orders();
		if (worker) {
			// Carrying minerals/gas swaps Gather for Return Cargo in the same slot, as in
			// the original.
			if (u->carrying_flags & 3) card.push_back({'c', "Return Cargo", C_RETURN, U::None});
			else card.push_back({'g', "Gather", C_GATHER, U::None});
			if (id == U::Terran_SCV) card.push_back({'r', "Repair", C_REPAIR, U::None});
			card.push_back({'b', "Build", C_BUILDMENU, U::None});
			card.push_back({'v', "Advanced", C_ADVMENU, U::None});
		} else {
			add_producible(u, false);   // train/morph units, upgrades, research
		}
		// Terran buildings that fly: lift off when landed, land when airborne. A lifted
		// building can't produce or rally, so it only ever offers Land. Lift Off is grayed
		// while the building is busy (training, researching, upgrading, building an addon)
		// — mirrors action_liftoff, which would refuse it anyway. Not offered at all until
		// the building has finished being built.
		if (ut_flying_building(u) && u_completed(u)) {
			if (u_grounded_building(u)) {
				bool busy = unit_is_constructing(u) || unit_is_researching(u) || unit_is_upgrading(u)
				         || !u->build_queue.empty()
				         || (u->secondary_order_type && u->secondary_order_type->id == Orders::BuildAddon
				             && u->current_build_unit && !u_completed(u->current_build_unit));
				card.push_back({pick_key("Lift"), "Lift Off", C_LIFT, U::None, !busy});
			} else card.push_back({pick_key("Land"), "Land", C_LAND, U::None});
		}
		if (building && u_grounded_building(u)) {
			bool produces = false;
			for (auto& c : card) if (c.act == C_TRAIN || c.act == C_MORPHBLDG) { produces = true; break; }
			if (produces) card.push_back({pick_key("Rally"), "Set Rally Point", C_RALLY, U::None});
			if (id == U::Zerg_Hatchery || id == U::Zerg_Lair || id == U::Zerg_Hive)
				// Key off a prefix of the label, so the letter the host highlights is the
				// hotkey. Passing "Larva" picked 'l' and lit the l in "Select" instead.
				card.push_back({pick_key("Select"), "Select Larva", C_SELECT, U::Zerg_Larva});
		}
		// Unit abilities (not part of the build/upgrade/research enumeration).
		if (id == U::Terran_Marine || id == U::Terran_Firebat)
			card.push_back({'t', "Stim Pack", C_STIM, U::None});
		else if (id == U::Terran_Siege_Tank_Tank_Mode || id == U::Terran_Siege_Tank_Tank_Mode_Turret)
			card.push_back({'o', "Siege Mode", C_SIEGE, U::None});
		else if (id == U::Terran_Siege_Tank_Siege_Mode || id == U::Terran_Siege_Tank_Siege_Mode_Turret)
			card.push_back({'o', "Tank Mode", C_UNSIEGE, U::None});
		// Burrow: Zerg ground units once Burrowing is researched (Lurkers innately).
		if (ut_can_burrow(u) && (unit_is(u, U::Zerg_Lurker) || player_has_researched(my_player, TechTypes::Burrowing))) {
			if (u_burrowed(u)) card.push_back({'u', "Unburrow", C_UNBURROW, U::None});
			else card.push_back({'u', "Burrow", C_BURROW, U::None});
		}
		// Cloak: Wraith / Ghost with the matching tech researched.
		bool cloakable = (id == U::Terran_Wraith && player_has_researched(my_player, TechTypes::Cloaking_Field)) ||
		                 (id == U::Terran_Ghost && player_has_researched(my_player, TechTypes::Personnel_Cloaking));
		if (cloakable) {
			if (u_cloaked(u)) card.push_back({'c', "Decloak", C_DECLOAK, U::None});
			else card.push_back({'c', "Cloak", C_CLOAK, U::None});
		}
		// Carrier interceptors / Reaver scarabs are built by the unit itself.
		if (unit_is_carrier(u)) card.push_back({pick_key("Interceptor"), "Build Interceptor", C_FIGHTER, U::Protoss_Interceptor});
		else if (unit_is_reaver(u)) card.push_back({'r', "Build Scarab", C_FIGHTER, U::Protoss_Scarab});
		// Two templar merge into an archon. Both merges are 'r' in BW (on separate unit cards).
		if (id == U::Protoss_High_Templar) card.push_back({'r', "Merge Archon", C_ARCHON, U::Protoss_Archon});
		else if (id == U::Protoss_Dark_Templar) card.push_back({'r', "Dark Archon", C_DARCHON, U::Protoss_Dark_Archon});
		// Targeted spellcaster abilities: shown once the tech is available, grayed when
		// energy is short. The canonical key falls back to auto-assign on a collision.
		size_t nsp; const spell_t* sp = spells(nsp);
		for (size_t i = 0; i != nsp; ++i) {
			if (sp[i].caster != id) continue;
			bool have_energy;
			if (sp[i].tech == TechTypes::None) {
				// Nuclear Strike isn't a real tech: any Ghost can call in a built missile.
				if (!player_has_completed_unit(my_player, U::Terran_Nuclear_Missile)) continue;
				have_energy = true;
			} else {
				const tech_type_t* te = get_tech_type(sp[i].tech);
				if (!unit_can_use_tech(u, te, my_player)) continue;
				have_energy = u->energy.integer_part() >= te->energy_cost;
			}
			char k = sp[i].key;
			for (auto& c : card) if (c.key == k) { k = 0; break; }
			card.push_back({k ? k : pick_key(sp[i].label), sp[i].label, C_SPELL,
			                U::None, have_energy, sp[i].tech});
		}
		// Cancel whatever this unit is mid-way through: a nuke it's painting, an addon it's
		// building, or a Zerg morph. (A production queue is cancelled via its status chips.)
		int cancel_op = 0;
		if (unit_is_ghost(u) && u->connected_unit && unit_is(u->connected_unit, U::Terran_Nuclear_Missile))
			cancel_op = 46;
		else if (u_grounded_building(u) && u->secondary_order_type->id == Orders::BuildAddon &&
		         u->current_build_unit && !u_completed(u->current_build_unit))
			cancel_op = 52;
		else if (unit_race(u) == race_t::zerg && !u->build_queue.empty())
			cancel_op = 25;
		else if (u_grounded_building(u) && !u_completed(u))
			cancel_op = 24;   // a building still going up (Terran/Protoss); refunds and frees the worker
		if (cancel_op)
			// Cancel's hotkey is Escape in the original, not an auto-assigned letter. '\x1b'
			// is matched by run_command from both the Escape key (see the key handler) and a
			// click on the button (the host sends it as the sym).
			card.push_back({'\x1b', "Cancel", C_CANCEL, U::None, true,
			                TechTypes::None, UpgradeTypes::None, (uint16_t)cancel_op});
		// Cargo: a bunker/transport shows each carried unit (click its icon to eject just
		// that one) plus an Unload button that ejects everything.
		if (unit_provides_space(u)) {
			bool any = false;
			for (unit_t* c : loaded_units(u)) {
				card.push_back({pick_key(unit_name(c->unit_type->id)), unit_name(c->unit_type->id),
				                C_UNLOAD, c->unit_type->id, true, TechTypes::None, UpgradeTypes::None,
				                get_unit_id(c).raw_value});
				any = true;
			}
			if (any) card.push_back({pick_key("Unload"), "Unload", C_UNLOADALL, U::None});
		}
		return unit_name(id);
	}

	// The command-card button that lights up for a unit's current order (its "mode"),
	// like the original — or -1 if the order maps to no button.
	static int order_active_act(Orders id) {
		using O = Orders;
		switch (id) {
		case O::Stop: case O::Guard: case O::PlayerGuard:                      return C_STOP;
		case O::Move: case O::Follow:                                          return C_MOVE;
		case O::AttackDefault: case O::MoveToAttack: case O::AttackUnit:
		case O::AttackFixedRange: case O::AttackTile: case O::AttackMove:      return C_ATTACK;
		case O::HoldPosition: case O::QueenHoldPosition:                       return C_HOLD;
		case O::Patrol:                                                        return C_PATROL;
		case O::Harvest1: case O::Harvest2: case O::MoveToGas: case O::WaitForGas:
		case O::HarvestGas: case O::MoveToMinerals: case O::WaitForMinerals:
		case O::MiningMinerals:                                                return C_GATHER;
		case O::ReturnGas: case O::ReturnMinerals:                             return C_RETURN;
		case O::Repair: case O::MoveToRepair:                                  return C_REPAIR;
		default:                                                               return -1;
		}
	}

	// Rebuild the context command card for the current selection (all three races).
	void refresh_card() {
		card.clear();
		card_text.clear();
		unit_t* u = primary_selected();          // own unit — drives the action buttons
		unit_t* sel = u;                          // the unit we describe (own preferred, else first)
		if (!sel) for (auto uid : current_selection) { sel = get_unit(uid); if (sel) break; }
		if (!sel) { menu = 0; return; }
		int active_act = u ? order_active_act(u->order_type->id) : -1;   // highlight the current mode

		const char* title;
		if (u && menu == 1) { title = "Build"; add_producible(u, true, 1); }
		else if (u && menu == 2) { title = "Advanced Build"; add_producible(u, true, 2); }
		else if (u) title = card_for_unit(u, u->unit_type->id);
		else { title = unit_name(sel->unit_type->id); menu = 0; }   // neutral/enemy: name only, no actions

		// Producibles and spells set their own enabled (build/upgrade/research availability,
		// or a spell's energy); the plain orders default to enabled; a couple of abilities
		// need a tech check.
		for (auto& c : card) {
			switch (c.act) {
			case C_STIM:  c.enabled = player_has_researched(my_player, TechTypes::Stim_Packs); break;
			case C_SIEGE: c.enabled = player_has_researched(my_player, TechTypes::Tank_Siege_Mode); break;
			case C_BUILD: case C_TRAIN: case C_MORPH: case C_MORPHBLDG:
			case C_UPGRADE: case C_RESEARCH: case C_SPELL: break;   // enabled already set
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
		// Energy for casters and energy buildings (Comsat, Nuclear Silo…); shown alongside
		// shields for the Protoss casters that have both.
		if (ut_has_energy(sel) && !u_hallucination(sel)) {
			if (!stat2.empty()) stat2 += " \xc2\xb7 ";   // " · "
			stat2 += format("Energy %d/%d", sel->energy.integer_part(), unit_max_energy(sel).integer_part()).c_str();
		}
		card_text += title;
		card_text += '\t'; card_text += format("HP %d/%d", hp, maxhp).c_str();
		card_text += '\t'; card_text += stat2.c_str();
		// Status effects (field 4). Blind and parasite have no in-world overlay sprite, so
		// this panel line is the only place they're visible; the rest also draw on the unit
		// but listing them keeps the readout complete, as retail's status icons do.
		a_string fx;
		bool fx_first = true;
		auto fx_add = [&](bool on, const char* name) {
			if (!on) return;
			if (!fx_first) fx += " \xc2\xb7 ";   // " · "
			fx += name; fx_first = false;
		};
		fx_add(sel->stim_timer,                   "Stim");
		fx_add(sel->defensive_matrix_hp != 0_fp8, "Matrix");
		fx_add(sel->ensnare_timer,                "Ensnared");
		fx_add(sel->plague_timer,                 "Plague");
		fx_add(sel->irradiate_timer,              "Irradiated");
		fx_add(sel->lockdown_timer,               "Locked Down");
		fx_add(sel->stasis_timer,                 "Stasis");
		fx_add(sel->maelstrom_timer,              "Maelstrom");
		fx_add(sel->blinded_by,                   "Blind");
		fx_add(sel->parasite_flags,               "Parasited");
		card_text += '\t'; card_text += fx.c_str();
		for (auto& c : card) {
			// Icon frame for build/train buttons is the unit id; -1 for plain orders.
			int icon = (c.ut != UnitTypes::None) ? (int)c.ut : -1;
			int minc = 0, gasc = 0;   // resource cost of the thing this button makes
			if (c.act == C_UPGRADE) {
				const upgrade_type_t* up = get_upgrade_type(c.upg);
				minc = upgrade_mineral_cost(my_player, up); gasc = upgrade_gas_cost(my_player, up);
			} else if (c.act == C_RESEARCH) {
				const tech_type_t* te = get_tech_type(c.tech);
				minc = te->mineral_cost; gasc = te->gas_cost;
			} else if (c.ut != UnitTypes::None &&
			           (c.act == C_BUILD || c.act == C_TRAIN || c.act == C_MORPH || c.act == C_MORPHBLDG)) {
				const unit_type_t* ut = get_unit_type(c.ut);
				minc = ut->mineral_cost; gasc = ut->gas_cost;
			}
			bool afford = (int)st.current_minerals[my_player] >= minc && (int)st.current_gas[my_player] >= gasc;
			const char* desc = c.act == C_UPGRADE ? bw_upgrade_desc(c.upg)
			                 : (c.act == C_RESEARCH || c.act == C_SPELL) ? bw_tech_desc(c.tech) : "";
			// KEY \t Label \t enabled \t icon \t minerals \t gas \t affordable \t requires \t desc \t active
			card_text += '\n'; card_text += c.key; card_text += '\t'; card_text += c.label;
			card_text += '\t'; card_text += (c.enabled ? '1' : '0');
			card_text += '\t'; card_text += format("%d", icon).c_str();
			card_text += '\t'; card_text += format("%d", minc).c_str();
			card_text += '\t'; card_text += format("%d", gasc).c_str();
			card_text += '\t'; card_text += (afford ? '1' : '0');
			card_text += '\t'; card_text += (c.req != UnitTypes::None) ? unit_name(c.req) : "";
			card_text += '\t'; card_text += desc;
			card_text += '\t'; card_text += (active_act >= 0 && (int)c.act == active_act) ? '1' : '0';
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

	// Producer status for the JS overlay: "<progress%>\t<name>\t<name>…" (the first name
	// is what's in progress), or empty if idle. Covers unit training and — since those
	// live on a different field — tech research and upgrades.
	const char* build_status() {
		status_text.clear();
		unit_t* u = primary_selected();
		if (!u) return status_text.c_str();
		auto pct = [](int done, int total) { return total > 0 ? 100 * done / total : 0; };
		if (!u->build_queue.empty()) {
			int prog = 0;
			if (u->current_build_unit) {
				int bt = u->current_build_unit->unit_type->build_time;
				prog = pct(bt - u->current_build_unit->remaining_build_time, bt);
			} else {
				// A Zerg egg / morphing unit counts down its own remaining_build_time
				// toward the queued unit's build time (no separate current_build_unit).
				// Right after queueing, a producer sits here for a frame with
				// remaining_build_time still 0 — show 0%, not a full bar, until it starts.
				int bt = u->build_queue.front()->build_time;
				prog = u->remaining_build_time > 0 ? pct(bt - u->remaining_build_time, bt) : 0;
			}
			status_text += format("%d", prog);
			for (auto* ut : u->build_queue) { status_text += '\t'; status_text += unit_name(ut->id); }
		} else if (unit_is_researching(u)) {
			const tech_type_t* te = u->building.researching_type;
			status_text += format("%d", pct(te->research_time - u->building.upgrade_research_time, te->research_time));
			status_text += '\t'; status_text += tech_name(te);
		} else if (unit_is_upgrading(u)) {
			const upgrade_type_t* up = u->building.upgrading_type;
			int tot = upgrade_time_cost(my_player, up);
			status_text += format("%d", pct(tot - u->building.upgrade_research_time, tot));
			status_text += '\t'; status_text += upgrade_name(up);
		}
		return status_text.c_str();
	}

	// "minerals\tgas\tsupply_used\tsupply_max" for the resource HUD.
	const char* resources() {
		int p = my_player, ri = (int)my_race;   // race index into the supply arrays
		// Spectating: show the economy of whichever player's unit is selected.
		if (spectator) if (unit_t* u = primary_selected(); u && u->owner < 8) { p = u->owner; ri = (int)unit_race(u); }
		int used = st.supply_used[p][ri].raw_value / 2;
		int max = st.supply_available[p][ri].raw_value / 2;
		if (max > 200) max = 200;
		resources_text = format("%d\t%d\t%d\t%d", (int)st.current_minerals[p], (int)st.current_gas[p], used, max);
		return resources_text.c_str();
	}

	// End-game score for the game-over screen. First line "outcome\tframe"; then one line
	// per player that started the game: "isMe\trace\tcolor\tunits\tbuildings\tminerals\tgas
	// \tscore". Stats come straight from the sim's cumulative counters.
	a_string stats_out;
	const char* stats_text() {
		stats_out = format("%d\t%d", outcome, (int)st.current_frame);
		for (int i = 0; i != 8; ++i) {
			if (!st.players[i].initially_active) continue;
			stats_out += format("\n%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d",
				i == my_player ? 1 : 0, (int)st.players[i].race, st.players[i].color,
				st.total_non_buildings_ever_completed[i], st.total_buildings_ever_completed[i],
				st.total_minerals_gathered[i], st.total_gas_gathered[i],
				st.unit_score[i] + st.building_score[i]);
		}
		return stats_out.c_str();
	}

	// ---- blocked-command feedback ------------------------------------------------
	// A blocked command's reason, tied to its advisor error voice (*AdErr00/01/02).
	enum err_kind { E_NONE = 0, E_MINERALS, E_GAS, E_SUPPLY, E_ENERGY };
	int err_sound_cache[5] = { -1, -2, -2, -2, -1 };   // per kind, lazy (-2 = unresolved)

	const char* supply_error_msg() const {
		int r = (int)my_race;   // 1 = Terran, 2 = Protoss, else Zerg
		return r == 1 ? "You must construct additional supply depots"
		     : r == 2 ? "You must construct additional pylons"
		     :          "Not enough food — build more overlords";
	}
	const char* err_message(err_kind k) const {
		return k == E_MINERALS ? "Not enough minerals"
		     : k == E_GAS      ? "Not enough vespene gas"
		     : k == E_ENERGY   ? "Not enough energy"
		     : k == E_SUPPLY   ? supply_error_msg() : "";
	}
	// The race's advisor error line by filename (Terran's are lowercase tAdErr, so the
	// lookup is case-insensitive): 00 = minerals, 01 = gas, 02 = supply.
	int err_sound(err_kind k) {
		if (err_sound_cache[k] == -2) {
			int r = (int)my_race;
			char buf[10] = { r == 1 ? 'T' : r == 2 ? 'P' : 'Z', 'A', 'd', 'E', 'r', 'r', '0',
			                 char('0' + (k == E_GAS ? 1 : k == E_SUPPLY ? 2 : 0)), 0, 0 };
			err_sound_cache[k] = find_sound(buf);
		}
		return err_sound_cache[k];
	}
	void raise_error(err_kind k) {
		error_text = err_message(k); ++error_seq;
		int s = err_sound(k);   // on-screen centre → full volume
		// Advisor errors are global UI feedback in BW: always audible, not gated by fog or
		// where the camera happens to point. Playing at xy() makes sound_audible pass and the
		// shared player picks full volume.
		if (s >= 0) play_sound(s, xy(), nullptr, false);
	}
	// "seq\tmessage": the host shows a toast whenever seq changes.
	const char* error_status() {
		error_status_text = format("%d\t%s", error_seq, error_text.c_str());
		return error_status_text.c_str();
	}
	// Why a visible command can't run right now (E_NONE = it can). Tech is already gated
	// by which buttons appear, so the only live blockers are resources / supply.
	err_kind block_reason(const cmd_t& c) {
		int minc = 0, gasc = 0; const unit_type_t* ut = nullptr;
		if (c.act == C_UPGRADE) {
			const upgrade_type_t* up = get_upgrade_type(c.upg);
			minc = upgrade_mineral_cost(my_player, up); gasc = upgrade_gas_cost(my_player, up);
		} else if (c.act == C_RESEARCH) {
			const tech_type_t* te = get_tech_type(c.tech);
			minc = te->mineral_cost; gasc = te->gas_cost;
		} else if (c.ut != UnitTypes::None) {
			ut = get_unit_type(c.ut);
			minc = ut->mineral_cost; gasc = ut->gas_cost;
		}
		if ((int)st.current_minerals[my_player] < minc) return E_MINERALS;
		if ((int)st.current_gas[my_player] < gasc) return E_GAS;
		if (ut && (c.act == C_TRAIN || c.act == C_MORPH) &&
		    !has_available_supply_for(my_player, ut, false)) return E_SUPPLY;
		return E_NONE;
	}

	// Resign. In a 1v1 this leaves through the command stream (opcode 87) so both peers
	// apply it deterministically — we're marked defeated and, with only the opponent left,
	// the melee triggers hand them the win. Solo (sandbox, no verdict system), just show
	// defeat locally.
	void resign() { if (competitive) cmds.player_leave(1); else outcome = 2; }

	// Cancel the status chip the host clicked (0 = the one in progress), refunding it.
	// Training queues cancel by slot; a research/upgrade in progress has its own action.
	void cancel_queue_slot(int slot) {
		if (slot < 0) return;
		sync_selection();
		unit_t* u = primary_selected();
		if (u && u->build_queue.empty() && unit_is_researching(u)) { cmd_bare(49); return; }
		if (u && u->build_queue.empty() && unit_is_upgrading(u)) { cmd_bare(51); return; }
		cmd_cancel_slot(slot);
	}

	// ---- event feedback (unit-ready / under-attack) ------------------------------
	// Case-insensitive search of the sfx table (Terran advisor files are lowercase,
	// Zerg/Protoss upper) — used a handful of times at most, then cached by the callers.
	int find_sound(const char* substr) const {
		auto lc = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
		a_string want;
		for (const char* p = substr; *p; ++p) want += lc(*p);
		for (size_t i = 0; i != sound_filenames.size(); ++i) {
			a_string low;
			for (char c : sound_filenames[i]) low += lc(c);
			if (low.find(want) != a_string::npos) return (int)i;
		}
		return -1;
	}
	// The race's advisor "your forces are under attack" line (*AdUpd00), by filename so
	// it survives sfx-table index shifts.
	void resolve_alert_sound() {
		int r = (int)my_race;   // 1 Terran, 2 Protoss, else Zerg
		under_attack_sound = find_sound(r == 1 ? "TAdUpd00" : r == 2 ? "PAdUpd00" : "ZAdUpd00");
	}
	// Auto-spawned / transient units the game never announces (larva trickle, morph
	// eggs, carrier interceptors, reaver scarabs) — skip their ready voice.
	static bool announces_ready(const unit_t* u) {
		using U = UnitTypes;
		switch (u->unit_type->id) {
		case U::Zerg_Larva: case U::Zerg_Egg: case U::Zerg_Cocoon: case U::Zerg_Lurker_Egg:
		case U::Protoss_Interceptor: case U::Protoss_Scarab: return false;
		default: return u->unit_type->ready_sound > 0;
		}
	}
	void add_alert(xy pos) {
		have_last_event = true; last_event_pos = pos;   // Space recenters on the latest hit
		// Dedup by location, not globally: an existing nearby ping is left to expire rather
		// than refreshed (so a sustained fight doesn't ping forever), but a hit in a new area
		// always pings — a single global cooldown used to swallow those and drop alerts.
		bool near = false;
		for (auto& a : alerts) {
			int dx = a.pos.x - pos.x, dy = a.pos.y - pos.y;
			if (dx * dx + dy * dy < 256 * 256) { near = true; break; }   // ~8 tiles
		}
		if (!near && alerts.size() < 8) alerts.push_back({pos, ALERT_TTL});
		// Voice: quiet while the fight is already on screen (as the original is), and a
		// long gap between announcements.
		bool on_screen = pos.x >= screen_pos.x && pos.y >= screen_pos.y &&
		                 pos.x < screen_pos.x + (int)screen_width &&
		                 pos.y < screen_pos.y + (int)screen_height;
		if (alert_cooldown == 0 && under_attack_sound >= 0 && !on_screen) {
			// On-screen centre → full volume, so the advisor is audible wherever the hit is.
			play_sound(under_attack_sound, xy(), nullptr, false);   // global advisor line, as in BW
			alert_cooldown = 1800;   // ~30 s at 60 fps
		}
	}

	static const char* color_name(int c) {
		static const char* n[] = {"Red", "Blue", "Teal", "Purple", "Orange", "Brown", "White", "Yellow"};
		return (c >= 0 && c < 8) ? n[c] : "Player";
	}

	void post_message(a_string text) {
		if (msg_log.size() > 6) msg_log.erase(msg_log.begin());
		msg_log.push_back({std::move(text), 0});
	}

	// Newline-joined active messages for the host overlay (fades them itself).
	const char* game_messages() {
		msg_out.clear();
		for (auto& m : msg_log) { if (!msg_out.empty()) msg_out += '\n'; msg_out += m.text; }
		return msg_out.c_str();
	}

	// Debug JS API (window.__bw.x.openbw_debug_*): the browser-console/automation story for
	// driving the game without pixel-hunting the canvas — clicking units that move between a
	// screenshot and the click is hopelessly flaky. List the units, then select by id.
	//
	// debug_units(): one unit per line, "id \t type \t owner \t x \t y \t order".
	const char* debug_units() {
		debug_text.clear();
		for (size_t i = 0; i != 8; ++i)
			for (unit_t* u : ptr(st.player_units[i]))
				debug_text += format("%u\t%s\t%d\t%d\t%d\t%d\n", (unsigned)get_unit_id(u).raw_value,
					unit_name(u->unit_type->id), u->owner,
					u->sprite->position.x, u->sprite->position.y, (int)u->order_type->id);
		return debug_text.c_str();
	}
	// debug_select(id, add): select a unit by the id debug_units() reported; add=1 extends
	// the selection (shift-click), add=0 replaces it. Runs the normal selection path.
	void debug_select(int raw_id, int add) {
		unit_t* u = get_unit(unit_id((uint16_t)raw_id));
		if (!u) return;
		if (!add) current_selection_clear();
		current_selection_add(u);
		on_selection(false);
	}
	// debug_set_life(id, hp, shields): set a unit's HP/shields (-1 leaves a value alone).
	// For exercising damage-dependent UI (wireframe states, shield rings) without a fight.
	void debug_set_life(int raw_id, int hp, int shields) {
		unit_t* u = get_unit(unit_id((uint16_t)raw_id));
		if (!u) return;
		if (hp >= 0) set_unit_hp(u, fp8::integer(hp));
		if (shields >= 0) {
			fp8 s = fp8::integer(shields);
			fp8 mx = fp8::integer(u->unit_type->shield_points);
			u->shield_points = s < mx ? s : mx;
		}
	}

	// Read-only state dump of the primary selected unit, for debugging from the JS console
	// (window.__bw.x.openbw_debug_dump). One `key=value` per line.
	a_string debug_text;
	const char* debug_dump() {
		debug_text.clear();
		debug_text += format("selection=%d\n", (int)current_selection.size());
		{   // the sim-side selection (what single-unit actions like build key off)
			int n = 0; const char* first = "none";
			for (unit_t* su : selected_units(my_player)) { if (!n) first = unit_name(su->unit_type->id); ++n; }
			debug_text += format("sim_selection=%d first=%s\n", n, first);
		}
		unit_t* u = primary_selected();
		if (!u) { debug_text += "primary=none\n"; return debug_text.c_str(); }
		debug_text += format("unit=%s owner=%d\n", unit_name(u->unit_type->id), u->owner);
		debug_text += format("order=%d order2=%d\n", (int)u->order_type->id, (int)u->secondary_order_type->id);
		debug_text += format("attack_move=%d attack_unit=%d\n",
			(int)u->unit_type->attack_move->id, (int)u->unit_type->attack_unit->id);
		debug_text += format("ground_weapon=%d\n", (int)u->unit_type->ground_weapon->id);
		debug_text += format("pos=%d,%d hp=%d\n", u->sprite->position.x, u->sprite->position.y, u->hp.integer_part());
		debug_text += format("move_target=%d,%d order_target=%d,%d\n",
			u->move_target.pos.x, u->move_target.pos.y, u->order_target.pos.x, u->order_target.pos.y);
		return debug_text.c_str();
	}

	// Announce a participant the moment it loses its last unit/building.
	void poll_eliminations() {
		for (int i = 0; i != 8; ++i) {
			if (!st.players[i].initially_active) continue;
			bool alive = false;
			for (unit_t* u : ptr(st.player_units[i])) { (void)u; alive = true; break; }
			if (alive) player_had_units[i] = 1;
			else if (player_had_units[i] && !player_eliminated[i]) {
				player_eliminated[i] = 1;
				post_message(format("%s has been eliminated.", color_name(st.players[i].color)));
			}
		}
	}

	// Once per frame: fire unit-ready voices on completion and raise under-attack alerts
	// when an own unit loses life. The first pass seeds silently so startup units are quiet.
	void poll_events() {
		++event_tick;
		if (alert_cooldown > 0) --alert_cooldown;
		for (size_t i = 0; i != alerts.size();) {
			if (--alerts[i].ttl <= 0) { alerts[i] = alerts.back(); alerts.pop_back(); }
			else ++i;
		}
		for (auto& m : msg_log) ++m.age;
		while (!msg_log.empty() && msg_log.front().age > MSG_TTL) msg_log.erase(msg_log.begin());
		if (under_attack_sound == -2) resolve_alert_sound();
		bool seeding = !events_seeded;
		for (unit_t* u : ptr(st.player_units[my_player])) {
			uint16_t id = get_unit_id(u).raw_value;
			if (u_completed(u) && announced.insert(id).second && !seeding && announces_ready(u))
				play_sound(u->unit_type->ready_sound, u->sprite->position, u, false);
			int life = u->hp.ceil().integer_part() + u->shield_points.integer_part();
			auto it = last_life.find(id);
			if (it != last_life.end() && life < it->second && !seeding) add_alert(u->sprite->position);
			last_life[id] = life;
		}
		events_seeded = true;
		check_last_standing();
		poll_eliminations();
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

	void start_target(targ_t t) { clear_pending(); targeting = true; pending_targ = t; }

	void clear_pending() { pending_build = nullptr; pending_land = false; pending_addon = nullptr; }

	// Helpers for the flying-building commands, which act on the selected building itself.
	xy u_pos_of_selected() { unit_t* u = primary_selected(); return u ? u->sprite->position : xy(); }
	UnitTypes landing_type() { unit_t* u = primary_selected(); return u ? u->unit_type->id : UnitTypes::None; }

	// Execute the command bound to `key` in the current card. Returns false if
	// the key isn't a command (so base key handling can run).
	bool run_command(char key) {
		for (auto& c : card) {
			if (c.key != key) continue;
			if (!c.enabled) {
				// Grayed spells are only ever blocked on energy — say so, don't fail silent.
				if (c.act == C_SPELL) raise_error(E_ENERGY);
				return true;
			}
			// Only the commands that actually spend are gated here. Build is excluded
			// because its cost is taken on the placement click, and Land carries a unit
			// type (the building's own) but costs nothing.
			switch (c.act) {
			case C_TRAIN: case C_MORPH: case C_MORPHBLDG: case C_RESEARCH: case C_UPGRADE:
				if (err_kind r = block_reason(c)) { raise_error(r); return true; }
				break;
			default: break;
			}
			switch (c.act) {
			case C_MOVE:      start_target(T_MOVE); break;
			case C_ATTACK:    start_target(T_ATTACK); break;
			case C_PATROL:    start_target(T_PATROL); break;
			case C_GATHER:    start_target(T_GATHER); break;
			case C_REPAIR:    start_target(T_REPAIR); break;
			case C_STOP:      sync_selection(); cmd_queued(26, key_shift()); break;
			case C_HOLD:      sync_selection(); cmd_queued(43, key_shift()); break;
			case C_BUILDMENU: menu = 1; refresh_card(); break;
			case C_ADVMENU:   menu = 2; refresh_card(); break;
			case C_BUILD:     pending_build = get_unit_type(c.ut); menu = 0; refresh_card(); break;
			case C_TRAIN:     if (const unit_type_t* at = get_unit_type(c.ut); at && ut_addon(at)) {
			                      // Addons ask for a placement (like a building): drop it in place
			                      // for no lift, or elsewhere to lift off and move there.
			                      if (unit_t* b = primary_selected()) {
			                          pending_build = b->unit_type; pending_addon = at;
			                          menu = 0; refresh_card();
			                      }
			                  } else { sync_selection(); cmd_type(31, c.ut); }
			                  break;
			case C_MORPH:     sync_selection(); cmd_type(35, c.ut); break;
			case C_MORPHBLDG: sync_selection(); cmd_type(53, c.ut); break;
			case C_SELECT:    select_units_of_type(c.ut); break;
			case C_RALLY:     start_target(T_RALLY); break;
			case C_RESEARCH:  sync_selection(); cmd_id8(48, (int)c.tech); break;
			case C_UPGRADE:   sync_selection(); cmd_id8(50, (int)c.upg); break;
			case C_STIM:      sync_selection(); cmd_bare(54); break;
			case C_SIEGE:     sync_selection(); cmd_queued(38, key_shift()); break;
			case C_UNSIEGE:   sync_selection(); cmd_queued(37, key_shift()); break;
			case C_LIFT:      sync_selection(); cmd_liftoff(u_pos_of_selected()); break;
			case C_LAND:      pending_build = get_unit_type(c.ut != UnitTypes::None ? c.ut : landing_type());
			                  pending_land = true; menu = 0; refresh_card(); break;
			case C_UNLOAD:    sync_selection(); cmds.unload(c.unit); break;
			case C_UNLOADALL: sync_selection(); cmds.unload_all(key_shift()); break;
			case C_RETURN:    sync_selection(); cmds.return_cargo(key_shift()); break;
			case C_BURROW:    sync_selection(); cmds.burrow(key_shift()); break;
			case C_UNBURROW:  sync_selection(); cmds.unburrow(); break;
			case C_CLOAK:     sync_selection(); cmds.cloak(); break;
			case C_DECLOAK:   sync_selection(); cmds.decloak(); break;
			case C_FIGHTER:   sync_selection(); cmds.train_fighter(); break;
			case C_ARCHON:    sync_selection(); cmds.morph_archon(); break;
			case C_DARCHON:   sync_selection(); cmds.morph_dark_archon(); break;
			case C_SPELL:     if (const spell_t* s = find_spell(c.tech)) {
			                      pending_spell_order = s->order; pending_spell_unit = s->targ_unit;
			                      start_target(T_SPELL);
			                  } break;
			case C_CANCEL:    sync_selection();
			                  if (c.unit == 46) cmds.cancel_nuke();
			                  else if (c.unit == 52) cmds.cancel_addon();
			                  else if (c.unit == 24) cmds.cancel_build();
			                  else cmds.cancel_morph();
			                  break;
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
		if (target && unit_hidden_by_fog(target)) target = nullptr;   // can't target through fog
		if (target && pending_targ != T_PATROL) flash_target(target);
		show_marker(pos);
		sync_selection();
		bool q = key_shift();
		switch (pending_targ) {
		// A unit target force-attacks it (AttackDefault → the unit's attack_unit); a-move to
		// ground uses AttackMove directly. AttackDefault + no target resolves to the unit's
		// attack_move, which is Guard for workers (they'd stand still) — AttackMove moves them.
		case T_ATTACK: cmd_order(target ? Orders::AttackDefault : Orders::AttackMove, pos, target, q); break;
		case T_MOVE:   cmd_order(Orders::Move, pos, target, q); break;
		case T_PATROL: cmd_order(Orders::Patrol, pos, nullptr, q); break;
		case T_GATHER: cmd_default_order(pos, target, q); break;
		case T_REPAIR: cmd_order(Orders::Repair, pos, target, q); break;
		case T_RALLY:  cmd_order(target ? Orders::RallyPointUnit : Orders::RallyPointTile, pos, target, false); break;
		case T_SPELL:  cmd_order(pending_spell_order, pos, pending_spell_unit ? target : nullptr, false); break;
		}
		if (unit_t* u = primary_selected())
			play_unit_ack(u, u->unit_type->first_yes_sound, u->unit_type->last_yes_sound);
	}

	// The tile a building would be placed at for cursor map position `map_pos`.
	// Refineries (Assimilator / Refinery / Extractor) must sit exactly on a geyser,
	// so snap to the geyser under the cursor rather than centering on the cursor.
	void placement_tile(xy map_pos, int& tx, int& ty) {
		unit_t* g;
		if (unit_is_refinery(pending_build) && (g = select_get_unit_at(map_pos)) &&
		    unit_is(g, UnitTypes::Resource_Vespene_Geyser)) {
			tx = (g->sprite->position.x - g->unit_type->placement_size.x / 2) / 32;
			ty = (g->sprite->position.y - g->unit_type->placement_size.y / 2) / 32;
		} else {
			tx = (map_pos.x - pending_build->placement_size.x / 2 + 16) / 32;   // +16 = round to nearest tile
			ty = (map_pos.y - pending_build->placement_size.y / 2 + 16) / 32;
		}
		if (tx < 0) tx = 0;
		if (ty < 0) ty = 0;
	}

	void place_pending(int mx, int my) {
		int tx, ty;
		placement_tile(screen_to_map(mx, my), tx, ty);   // tile of pending_build (the parent, for an addon)
		if (pending_addon) {
			if ((int)st.current_minerals[my_player] < pending_addon->mineral_cost) { raise_error(E_MINERALS); return; }
			if ((int)st.current_gas[my_player] < pending_addon->gas_cost) { raise_error(E_GAS); return; }
			sync_primary();   // build is a single-unit action (see sync_primary)
			// The command carries the addon's tile; the sim derives the parent's destination
			// from it and lifts off only when that differs from where the parent stands.
			cmd_build(Orders::PlaceAddon, pending_addon,
			          tx + pending_addon->addon_position.x / 32, ty + pending_addon->addon_position.y / 32);
			clear_pending();
			return;
		}
		// Landing an existing building is free; only a real build spends, and the cost is
		// taken now, at placement — report a shortfall and keep the ghost up.
		if (!pending_land) {
			if ((int)st.current_minerals[my_player] < pending_build->mineral_cost) { raise_error(E_MINERALS); return; }
			if ((int)st.current_gas[my_player] < pending_build->gas_cost) { raise_error(E_GAS); return; }
		}
		sync_primary();   // build is a single-unit action (see sync_primary)
		cmd_build(pending_land ? Orders::BuildingLand : kit.build_order, pending_build, tx, ty);
		clear_pending();   // one-shot; re-open the menu to place another
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

	// --- Group wireframes (unit\wirefram\grpwire.grp): the multi-selection row, as in BW's
	// console. Frame index = unit type id. A frame's pixels use only a few fixed palette
	// indices — 192/193 (outline/detail pair) plus 208-211 or 216-219 (one per damage-tracked
	// part) — which the original recolors by damage state; the RGB never comes from a palette,
	// so we map them straight to our own colors and don't need the UI palette we lack. Parts
	// turn yellow→orange→red as HP drops, in an order seeded by the unit's
	// wireframe_randomizer (an approximation of BW's exact rule). Shields tint the outline blue.
	grp_t wire_grp;       // grpwire.grp: 32x32 group tiles (131 frames, multi-selectable ids)
	grp_t wire_big_grp;   // wirefram.grp: 64x64 single-unit wireframes (all 228 unit types)
	bool wire_grp_loaded = false;
	a_string wires_text;
	a_vector<uint8_t> wire_index, wire_rgba;

	bool load_wire_grp() {
		if (!wire_grp_loaded) {
			wire_grp_loaded = true;
			a_vector<uint8_t> data;
			load_data_file(data, "unit\\wirefram\\grpwire.grp");
			wire_grp = read_grp(data_loading::data_reader_le(data.data(), data.data() + data.size()));
			load_data_file(data, "unit\\wirefram\\wirefram.grp");
			wire_big_grp = read_grp(data_loading::data_reader_le(data.data(), data.data() + data.size()));
		}
		return !wire_grp.frames.empty();
	}

	// Single selections use the big 64x64 console wireframe, groups the 32x32 tiles.
	int wire_box() { return current_selection.size() == 1 ? 64 : 32; }

	unit_t* selected_nth(size_t i) {
		size_t n = 0;
		for (auto uid : current_selection)
			if (unit_t* u = get_unit(uid)) if (n++ == i) return u;
		return nullptr;
	}

	// Damage summary for a unit, OG-style: only green, yellow, and red. Each of the 4 parts
	// has two steps (green→yellow→red); `steps` (0..8) is dealt round-robin across the parts
	// in wireframe_randomizer order, so light damage shows a couple of yellow parts and reds
	// accumulate as HP drops. The outline pair follows overall HP thirds (`oc`), so a
	// near-dead unit reads fully red. Also the JS change signature — redraw only when moved.
	void wire_state(unit_t* u, int& steps, bool& shield) {
		// Exact fp8 compare: ANY damage must show — a wireframe is all-green only at 100%.
		bool damaged = u->hp < u->unit_type->hitpoints;
		double r = u->unit_type->hitpoints.raw_value
			? (double)u->hp.raw_value / u->unit_type->hitpoints.raw_value : 1.0;
		steps = damaged ? (int)((1.0 - r) * 8) + 1 : 0;   // ceil-like: at least one yellow part
		if (steps > 8) steps = 8;
		shield = u->unit_type->has_shield && u->shield_points.integer_part() > 0;
	}

	// One line per selected unit: "id \t name \t hp \t maxhp \t state". `state` changes iff
	// the tile must be redrawn. Shown for single selections too, as in the original.
	const char* wires() {
		wires_text.clear();
		size_t n = 0;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (!u) continue;
			if (n++ == 12) break;
			int steps; bool shield;
			wire_state(u, steps, shield);
			wires_text += format("%u\t%s\t%d\t%d\t%d\n", (unsigned)get_unit_id(u).raw_value,
				unit_name(u->unit_type->id), u->hp.ceil().integer_part(),
				u->unit_type->hitpoints.ceil().integer_part(), (steps << 1) | (int)shield);
		}
		return wires_text.c_str();
	}

	// Render the i-th selected unit's wireframe as RGBA in a wire_box()-sized square:
	// 64x64 (wirefram.grp, all 228 types) for a single selection, 32x32 (grpwire.grp)
	// tiles for a group. Active Protoss shields draw as a blue ring hugging the
	// silhouette — the wireframe itself keeps its damage colors, as in the original.
	const uint8_t* render_wireframe(size_t i) {
		if (!load_wire_grp()) return nullptr;
		unit_t* u = selected_nth(i);
		if (!u) return nullptr;
		size_t box = (size_t)wire_box();
		const grp_t& grp = box == 64 ? wire_big_grp : wire_grp;
		size_t fi = (size_t)u->unit_type->id;
		if (fi >= grp.frames.size()) return nullptr;
		const auto& f = grp.frames[fi];
		wire_index.assign(box * box, 0);
		// draw_frame's offset args are a source-side crop; the GRP frame offset is the
		// destination position that centers the drawing in the box, so bake it into the
		// dst pointer (mis-passing it as the crop pushed every wireframe off-center).
		draw_frame(f, false, wire_index.data() + (size_t)f.offset.y * box + f.offset.x, box,
		           0, 0, f.size.x, f.size.y, [](uint8_t c, uint8_t) { return c; });
		int steps; bool shield;
		wire_state(u, steps, shield);
		int rnd = u->wireframe_randomizer & 3;
		// Overall damage class for whole-frame tinting: green / yellow / red by HP thirds.
		int cls = steps == 0 ? 0 : steps <= 5 ? 1 : 2;   // healthy / hurt / critical (~thirds)
		// OG tri-color part states (208-211 frames): 0 green, 1 yellow, 2 red.
		static const uint8_t PART[3][3] = { {44,228,52}, {232,208,16}, {216,24,24} };
		// Zerg frames (216-219) are a 4-shade concentric gradient of ONE organism (216 =
		// core, 219 = body/outline). In the original a HEALTHY drone is mostly red with a
		// green core, and a dying one is dark blue with a magenta core — damage slides the
		// 4-index window along a green→red→purple→blue cycle.
		static const uint8_t ZGRAD[8][3] = {
			{64,240,64},    // bright green core
			{150,220,40},   // green-yellow
			{232,140,20},   // orange
			{228,50,24},    // bright red
			{200,70,170},   // magenta
			{110,60,190},   // purple
			{50,50,160},    // blue
			{28,28,100},    // dark blue
		};
		wire_rgba.assign(box * box * 4, 0);
		for (size_t p = 0; p != box * box; ++p) {
			uint8_t c = wire_index[p];
			if (!c) continue;
			uint8_t* o = &wire_rgba[p * 4];
			if (c == 192 || c == 193) {
				// The 192/193 band IS the shield-ring layer baked into the GRP: the original
				// draws it blue while shields hold and hides it entirely otherwise. (Evidence:
				// the Zerg-only frames — a race with no shields — don't contain these indices.)
				if (!shield) continue;
				if (c == 192) { o[0] = 72; o[1] = 136; o[2] = 255; }
				else          { o[0] = 48; o[1] = 92;  o[2] = 190; }
			} else if (c >= 216 && c <= 219) {
				int gi = (c - 216) + 2 * cls;   // damage slides the window along the cycle
				if (gi > 7) gi = 7;
				o[0] = ZGRAD[gi][0]; o[1] = ZGRAD[gi][1]; o[2] = ZGRAD[gi][2];
			} else {
				// Mechanical line art: damage-tracked parts (208-211), low 2 bits pick the
				// part. Its rank in the degradation order is randomizer-seeded; `steps` deals
				// one step per part round-robin, so each part is green, yellow, or red.
				int rank = ((int)(c & 3) + rnd) % 4;
				int st = steps / 4 + (rank < steps % 4 ? 1 : 0);
				if (st > 2) st = 2;
				o[0] = PART[st][0]; o[1] = PART[st][1]; o[2] = PART[st][2];
			}
			o[3] = 255;
		}
		return wire_rgba.data();
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
		// Match the command-card unit icons: the carried delivery sprite rotates
		// with its carrier, so use frame 12 (south-east) when it's directional.
		const image_type_t& it = global_st.image_types.vec[(size_t)id];
		return rasterize_icon(global_st.image_grp[(size_t)id],
			it.has_directional_frames ? 12 : 0, nullptr);
	}

	a_vector<uint8_t> pbar_rgba;   // scratch for the rendered progress bar
	int pbar_w = 0, pbar_h = 0;
	// The producer/research progress bar, drawn exactly like the in-world health bar: the
	// same thpbar.pcx palette entries and the same green bevel gradient, quantised to 3px
	// segments. `img.hp_bar_colors[ci]` is a palette index; img.wpe expands it to RGB.
	const uint8_t* render_progress_bar(int percent, int width) {
		if (percent < 0) percent = 0; else if (percent > 100) percent = 100;
		int w = width < 19 ? 19 : width;
		w -= (w - 1) % 3;                        // matches draw_health_bars' width rule
		int dw = percent * w / 100;              // fill quantised to whole 3px segments
		if (percent > 0 && dw < 3) dw = 3;
		else if (dw % 3) dw += (dw % 3 > 1) ? 3 - dw % 3 : -(dw % 3);
		if (percent == 0) dw = 0;
		if (dw > w) dw = w;
		const int h = 5;
		// Always the "full health" green gradient (production progress doesn't change
		// colour), notched into little segments by a dark divider every few pixels. The
		// not-yet-filled part is left fully transparent, so the black HUD shows through.
		const int fill[] = {18, 0, 1, 2, 18};    // green bevel + dark top/bottom border
		const int SEG = 3;                       // 2px green tick + 1px gap → many small bars
		pbar_w = w; pbar_h = h;
		pbar_rgba.assign((size_t)w * h * 4, 0);   // transparent everywhere by default
		for (int y = 0; y != h; ++y)
			for (int x = 0; x < dw; ++x) {
				bool notch = (x % SEG) == SEG - 1;
				int pi = img.hp_bar_colors.at(notch ? 18 : fill[y]);   // 18 = dark divider
				uint8_t* px = &pbar_rgba[((size_t)y * w + x) * 4];
				px[0] = tileset_img.wpe[4 * pi]; px[1] = tileset_img.wpe[4 * pi + 1];
				px[2] = tileset_img.wpe[4 * pi + 2]; px[3] = 255;
			}
		return pbar_rgba.data();
	}

	// Flash a target's ring when an order is aimed at it (attack / gather / follow), like
	// the original's target-acquisition blink.
	// Time-based (ms), so the blink is refresh-rate-independent: 1s show, 1s hide, 1s show.
	static constexpr double FLASH_PHASE_MS = 1000, FLASH_TOTAL_MS = 3000;
	void flash_target(unit_t* t) { if (t) { flash_unit = get_unit_id(t); flash_start = ui_now; } }
	bool flash_showing() {
		if (flash_start < 0) return false;
		double e = ui_now - flash_start;
		return e < FLASH_TOTAL_MS && ((int)(e / FLASH_PHASE_MS)) % 2 == 0;   // show phases 0 and 2
	}
	uint8_t flash_color() {
		if (line_move_color < 0) {
			line_move_color = nearest_palette_color(40, 240, 40);
			line_atk_color = nearest_palette_color(240, 40, 40);
		}
		if (ring_neutral_color < 0) ring_neutral_color = nearest_palette_color(240, 220, 40);
		unit_t* t = get_unit(flash_unit);
		if (!t) return (uint8_t)line_move_color;
		return (uint8_t)(t->owner == my_player ? line_move_color
		              : t->owner < 8 ? line_atk_color : ring_neutral_color);
	}
	// Draw the flashing target ring here, inside the sprite pass, so the unit's body occludes
	// it exactly like the normal selection circle (which is drawn the same way).
	void draw_sprite(const sprite_t* sprite, uint8_t* data, size_t data_pitch) override {
		if (flash_showing()) {
			unit_t* t = get_unit(flash_unit);
			if (t && t->sprite == sprite) draw_selection_ring(t, flash_color(), data, data_pitch);
		}
		ui_functions::draw_sprite(sprite, data, data_pitch);
	}

	// The animated click marker BW drops where you order — the real Cursor_Marker sprite,
	// but drawn client-side (never a sim create_thingy, which would advance sprite/RNG state
	// and desync multiplayer). Plays through the GRP's frames once over its short life.
	xy marker_pos;
	double marker_start = -1;
	static constexpr double MARKER_MS = 400;
	void show_marker(xy pos) { marker_pos = pos; marker_start = ui_now; }
	void draw_cursor_marker(uint8_t* data, size_t pitch) {
		if (marker_start < 0) return;
		double e = ui_now - marker_start;
		if (e >= MARKER_MS) { marker_start = -1; return; }
		auto* it = get_image_type(ImageTypes::IMAGEID_Cursor_Marker);
		auto* grp = global_st.image_grp[(size_t)it->id];
		if (grp->frames.empty()) return;
		int nf = (int)grp->frames.size();
		int idx = (int)(e / MARKER_MS * nf);
		if (idx >= nf) idx = nf - 1;
		auto& frame = grp->frames.at(idx);
		xy mp = marker_pos;
		mp.x += (int)frame.offset.x - (int)grp->width / 2;
		mp.y += (int)frame.offset.y - (int)grp->height / 2;
		int sx = mp.x - screen_pos.x, sy = mp.y - screen_pos.y;
		if (sx >= (int)screen_width || sy >= (int)screen_height) return;
		size_t w = frame.size.x, h = frame.size.y;
		if (sx + (int)w <= 0 || sy + (int)h <= 0) return;
		size_t ox = sx < 0 ? (size_t)-sx : 0, oy = sy < 0 ? (size_t)-sy : 0;
		uint8_t* dst = data + sy * pitch + sx;
		w = std::min(w, screen_width - sx);
		h = std::min(h, screen_height - sy);
		draw_frame(frame, false, dst, pitch, ox, oy, w, h, [](uint8_t v, uint8_t) { return v; });
	}

	// A clipped ellipse outline (midpoint) in screen space — the target ring. Rendering
	// only, so ordinary float math is fine (not part of the deterministic sim).
	// Draw a unit's actual selection circle sprite in a specific colour. Mirrors
	// ui_functions::draw_selection_circle exactly (same GRP, position, clipping) — it just
	// remaps the ring's colour band to `color` instead of the owner's player colour.
	void draw_selection_ring(const unit_t* u, uint8_t color, uint8_t* data, size_t data_pitch) {
		const sprite_t* sprite = u->sprite;
		auto* image_type = get_image_type((ImageTypes)((int)ImageTypes::IMAGEID_Selection_Circle_22pixels + sprite->sprite_type->selection_circle));
		xy map_pos = sprite->position + xy(0, sprite->sprite_type->selection_circle_vpos);
		auto* grp = global_st.image_grp[(size_t)image_type->id];
		auto& frame = grp->frames.at(0);
		map_pos.x += int(frame.offset.x - grp->width / 2);
		map_pos.y += int(frame.offset.y - grp->height / 2);
		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;
		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;
		size_t width = frame.size.x, height = frame.size.y;
		if (screen_x + (int)width <= 0 || screen_y + (int)height <= 0) return;
		size_t offset_x = 0, offset_y = 0;
		if (screen_x < 0) offset_x = -screen_x;
		if (screen_y < 0) offset_y = -screen_y;
		uint8_t* dst = data + screen_y * data_pitch + screen_x;
		width = std::min(width, screen_width - screen_x);
		height = std::min(height, screen_height - screen_y);
		auto remap = [color](uint8_t new_value, uint8_t) {
			return new_value < 8 ? color : new_value;
		};
		draw_frame(frame, false, dst, data_pitch, offset_x, offset_y, width, height, remap);
	}

	// The blinking target ring: the unit's own selection circle, coloured by allegiance —
	// green for your own, yellow neutral, red enemy. ~1s per phase (show / hide / show).
	// A clipped Bresenham line in screen space (map coords minus the camera).
	void draw_map_line(uint8_t* data, size_t data_pitch, xy a, xy b, uint8_t color) {
		int x0 = a.x - screen_pos.x, y0 = a.y - screen_pos.y;
		int x1 = b.x - screen_pos.x, y1 = b.y - screen_pos.y;
		int adx = x1 > x0 ? x1 - x0 : x0 - x1, ady = y1 > y0 ? y1 - y0 : y0 - y1;
		int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = adx - ady;
		for (;;) {
			if ((unsigned)x0 < screen_width && (unsigned)y0 < screen_height)
				data[(size_t)y0 * data_pitch + x0] = color;
			if (x0 == x1 && y0 == y1) break;
			int e2 = 2 * err;
			if (e2 > -ady) { err -= ady; x0 += sx; }
			if (e2 <  adx) { err += adx; y0 += sy; }
		}
	}
	// A small filled diamond marking an order's destination.
	void draw_marker(uint8_t* data, size_t data_pitch, xy p, uint8_t color) {
		int cx = p.x - screen_pos.x, cy = p.y - screen_pos.y;
		for (int dy = -3; dy <= 3; ++dy)
			for (int dx = -3; dx <= 3; ++dx) {
				if ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) > 3) continue;
				int px = cx + dx, py = cy + dy;
				if ((unsigned)px < screen_width && (unsigned)py < screen_height)
					data[(size_t)py * data_pitch + px] = color;
			}
	}
	static bool close_xy(xy a, xy b) { int dx = a.x - b.x, dy = a.y - b.y; return dx * dx + dy * dy < 16 * 16; }

	// Selected units' orders as lines (green move/rally, red attack) with a diamond at
	// each destination, so where things are headed — and a producer's rally — is visible.
	void draw_order_lines(uint8_t* data, size_t data_pitch) {
		if (!show_order_lines || current_selection.empty()) return;
		if (line_move_color < 0) {
			line_move_color = nearest_palette_color(40, 240, 40);
			line_atk_color  = nearest_palette_color(240, 40, 40);
		}
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (!u || u->owner != my_player) continue;
			if (u_grounded_building(u)) {
				xy r = u->building.rally.pos;   // rally defaults to the building itself
				if (r != xy() && !close_xy(r, u->sprite->position)) {
					draw_map_line(data, data_pitch, u->sprite->position, r, line_move_color);
					draw_marker(data, data_pitch, r, line_move_color);
				}
				continue;
			}
			// Mobile unit: its active destination, then any shift-queued waypoints.
			xy prev = u->sprite->position; bool any = false;
			auto seg = [&](xy to, unit_t* tu) {
				if (to == xy() || close_xy(to, prev)) return;
				uint8_t col = (tu && tu->owner != my_player && tu->owner < 8) ? line_atk_color : line_move_color;
				draw_map_line(data, data_pitch, prev, to, col);
				prev = to; any = true;
			};
			if (u->move_target.pos != xy()) seg(u->move_target.pos, u->order_target.unit);
			for (order_t* o : ptr(u->order_queue)) {
				xy t = o->target.position;
				if (t == xy() && o->target.unit) t = o->target.unit->sprite->position;
				seg(t, o->target.unit);
			}
			if (any) draw_marker(data, data_pitch, prev, line_move_color);
		}
	}

	// Minimap with blinking red blips at recent under-attack locations (drawn on top of
	// the base minimap, which the engine renders after draw_callback).
	// Minimap ping, like the original: four bracket lines sweeping inward onto the spot.
	// A converging animation catches the eye far better than a static blip, and repeating
	// the sweep keeps drawing attention for as long as the alert lives.
	void draw_minimap(uint8_t* data, size_t data_pitch) override {
		ui_functions::draw_minimap(data, data_pitch);
		if (alerts.empty()) return;
		rect area = get_minimap_area();
		if (area.from == area.to || (size_t)(area.to.x - area.from.x) != game_st.map_tile_width) return;
		if (alert_color < 0) alert_color = nearest_palette_color(255, 40, 40);
		auto plot = [&](int px, int py) {
			if (px >= area.from.x && px < area.to.x && py >= area.from.y && py < area.to.y)
				data[(size_t)py * data_pitch + px] = (uint8_t)alert_color;
		};
		const int CYCLE = 30, R0 = 14, R1 = 2;
		for (auto& a : alerts) {
			int mx = area.from.x + a.pos.x / 32, my = area.from.y + a.pos.y / 32;
			// A full box outline closing in on the spot, repeating every CYCLE ticks.
			int r = R0 - (((ALERT_TTL - a.ttl) % CYCLE) * (R0 - R1)) / CYCLE;
			for (int i = -r; i <= r; ++i) {
				plot(mx + i, my - r); plot(mx + i, my + r);   // top / bottom
				plot(mx - r, my + i); plot(mx + r, my + i);   // left / right
			}
			plot(mx, my);
		}
	}

	// Placement preview: a faded, tinted silhouette of the actual building, plus a
	// footprint outline so the exact tiles it will occupy are unambiguous.
	void draw_callback(uint8_t* data, size_t data_pitch) override {
		ui_functions::draw_callback(data, data_pitch);
		draw_order_lines(data, data_pitch);
		draw_cursor_marker(data, data_pitch);   // the ring is drawn in draw_sprite (occluded by the unit)

		if (!pending_build) return;
		if (place_ok_color < 0) {
			place_ok_color = nearest_palette_color(40, 240, 40);
			place_bad_color = nearest_palette_color(240, 40, 40);
		}
		if (ghost_ok.empty()) build_ghost_luts();

		int tx, ty;
		placement_tile(screen_to_map(mouse_x, mouse_y), tx, ty);   // snaps refineries to the geyser
		unit_t* builder = primary_selected();
		auto ok_at = [&](const unit_type_t* t, int px, int py) {
			xy c(32 * px + t->placement_size.x / 2, 32 * py + t->placement_size.y / 2);
			return builder && can_place_building(builder, my_player, t, c, false, false);
		};

		if (pending_addon) {
			int ax = tx + pending_addon->addon_position.x / 32, ay = ty + pending_addon->addon_position.y / 32;
			// One placement: both the parent's new footprint and the addon's must be clear,
			// so a bad spot for either shows the whole thing red.
			bool ok = ok_at(pending_build, tx, ty) && ok_at(pending_addon, ax, ay);
			draw_placement_ghost(pending_build, tx, ty, ok, data, data_pitch);
			draw_placement_ghost(pending_addon, ax, ay, ok, data, data_pitch);
		} else {
			draw_placement_ghost(pending_build, tx, ty, ok_at(pending_build, tx, ty), data, data_pitch);
		}
	}

	// One placement ghost: a faded, tinted silhouette of the building's first frame plus a
	// footprint outline so the exact tiles it will occupy are unambiguous.
	void draw_placement_ghost(const unit_type_t* type, int tx, int ty, bool ok, uint8_t* data, size_t data_pitch) {
		int w = type->placement_size.x, h = type->placement_size.y;
		xy center(32 * tx + w / 2, 32 * ty + h / 2);

		// Render the frame into a scratch buffer with the tint table applied, then
		// dithered-blit it (checkerboard = translucency).
		const grp_t* grp = global_st.image_grp[(size_t)type->flingy->sprite->image->id];
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
		poll_events();
		refresh_card();
	}

	bool handle_game_input(const native_window::event_t& e) override {
		using ev = native_window::event_t;
		if (e.type == ev::type_mouse_motion) { mouse_x = e.mouse_x; mouse_y = e.mouse_y; return false; }

		if (paused) {
			// No selection, orders, or hotkeys (nor their sounds/markers) while paused;
			// let the base still pan (minimap click, arrow keys, edge scroll).
			if (e.type == ev::type_mouse_button_down && e.button == 1) {
				xy p; return minimap_point(e.mouse_x, e.mouse_y, p) ? false : true;
			}
			if (e.type == ev::type_mouse_button_down) return true;
			return false;
		}

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
				clear_pending(); targeting = false; menu = 0; refresh_card();
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
				if (target && unit_hidden_by_fog(target)) target = nullptr;   // right-click through fog = move
			}
			// On a unit, only the target flash blinks; the ground marker is for moves.
			if (target) flash_target(target);
			else show_marker(map_pos);
			sync_selection();
			cmd_default_order(map_pos, target, key_shift());
			if (unit_t* u = primary_selected())
				play_unit_ack(u, u->unit_type->first_yes_sound, u->unit_type->last_yes_sound);
			return true;
		}
		if (e.type == ev::type_key_down && e.scancode == 41) {   // Escape
			// First back out of any pending placement / target / submenu, as in the original.
			if (pending_build || targeting || menu != 0) {
				clear_pending(); targeting = false; menu = 0; refresh_card();
				return true;
			}
			// Nothing pending: Escape is the Cancel hotkey (construction, morph, addon, nuke).
			run_command('\x1b');
			return true;
		}
		if (e.type == ev::type_key_down && e.scancode == 44) {   // Space: jump to the most recent alert
			if (have_last_event) center_on(last_event_pos);
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
