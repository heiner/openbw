// web/wasm_backend.cpp — software implementation of the native_window /
// native_window_drawing / native_sound interfaces for the browser build.
//
// Replaces ui/sdl2.cpp. OpenBW renders into an 8-bit indexed buffer, expands it
// through the palette into an RGBA framebuffer, and this backend keeps that
// framebuffer in wasm linear memory. JS reads it each frame and blits it to a
// <canvas>. Input is injected by JS through the openbw_* exports below.
//
// Pixel format: 32-bit RGBA, little-endian byte order R,G,B,A — matching what
// canvas ImageData expects, so JS can use the buffer directly.

#include "native_window.h"
#include "native_window_drawing.h"
#include "native_sound.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <deque>
#include <array>
#include <memory>

// ---------------------------------------------------------------------------
// native_window
// ---------------------------------------------------------------------------
namespace native_window {

struct window_impl {
	int width = 0, height = 0;
	std::vector<uint8_t> framebuffer;                 // width*height*4 RGBA
	std::deque<event_t> events;
	std::array<bool, 512> keys{};
	int mouse_x = 0, mouse_y = 0;
	int button_state = 0;                             // bit (1<<(button-1))
};

// The single live window; the JS input exports target it.
static window_impl* g_window = nullptr;

window::window() {}
window::~window() {}
window::window(window&&) = default;

void window::destroy() {
	if (impl && g_window == impl.get()) g_window = nullptr;
	impl.reset();
}

bool window::create(const char* title, int x, int y, int width, int height) {
	(void)title; (void)x; (void)y;
	impl = std::make_unique<window_impl>();
	impl->width = width;
	impl->height = height;
	impl->framebuffer.assign((size_t)width * height * 4, 0);
	g_window = impl.get();
	return true;
}

void window::get_cursor_pos(int* x, int* y) {
	if (x) *x = impl ? impl->mouse_x : 0;
	if (y) *y = impl ? impl->mouse_y : 0;
}

bool window::peek_message(event_t& e) {
	if (!impl || impl->events.empty()) return false;
	e = impl->events.front();
	impl->events.pop_front();
	return true;
}

bool window::show_cursor(bool) { return true; }

bool window::get_key_state(int scancode) {
	if (!impl || scancode < 0 || scancode >= (int)impl->keys.size()) return false;
	return impl->keys[scancode];
}

bool window::get_mouse_button_state(int button) {
	if (!impl) return false;
	return (impl->button_state & (1 << (button - 1))) != 0;
}

void window::update_surface() {}   // JS reads the framebuffer directly

window::operator bool() const { return (bool)impl; }

}  // namespace native_window

// ---------------------------------------------------------------------------
// JS input + framebuffer exports (called from the JS host)
// ---------------------------------------------------------------------------
using native_window::g_window;
using ev_t = native_window::event_t;

#define OPENBW_EXPORT(name) __attribute__((export_name(#name))) extern "C"

OPENBW_EXPORT(openbw_framebuffer) uint8_t* openbw_framebuffer() {
	return g_window ? g_window->framebuffer.data() : nullptr;
}
OPENBW_EXPORT(openbw_framebuffer_width) int openbw_framebuffer_width() {
	return g_window ? g_window->width : 0;
}
OPENBW_EXPORT(openbw_framebuffer_height) int openbw_framebuffer_height() {
	return g_window ? g_window->height : 0;
}

OPENBW_EXPORT(openbw_mouse_move) void openbw_mouse_move(int x, int y) {
	if (!g_window) return;
	g_window->mouse_x = x;
	g_window->mouse_y = y;
	ev_t e;
	e.type = ev_t::type_mouse_motion;
	e.mouse_x = x; e.mouse_y = y;
	e.button_state = g_window->button_state;
	g_window->events.push_back(e);
}

// button: SDL-style 1=left, 2=middle, 3=right
OPENBW_EXPORT(openbw_mouse_button) void openbw_mouse_button(int down, int button, int x, int y, int clicks) {
	if (!g_window) return;
	g_window->mouse_x = x;
	g_window->mouse_y = y;
	int bit = 1 << (button - 1);
	if (down) g_window->button_state |= bit;
	else g_window->button_state &= ~bit;
	ev_t e;
	e.type = down ? ev_t::type_mouse_button_down : ev_t::type_mouse_button_up;
	e.button = button;
	e.mouse_x = x; e.mouse_y = y;
	e.button_state = g_window->button_state;
	e.clicks = clicks;
	g_window->events.push_back(e);
}

OPENBW_EXPORT(openbw_key) void openbw_key(int down, int sym, int scancode) {
	if (!g_window) return;
	if (scancode >= 0 && scancode < (int)g_window->keys.size()) g_window->keys[scancode] = down != 0;
	ev_t e;
	e.type = down ? ev_t::type_key_down : ev_t::type_key_up;
	e.sym = sym;
	e.scancode = scancode;
	g_window->events.push_back(e);
}

// ---------------------------------------------------------------------------
// native_window_drawing — software surfaces
// ---------------------------------------------------------------------------
namespace native_window_drawing {

struct palette_impl : palette {
	color colors[256];
	palette_impl() { memset(colors, 0, sizeof(colors)); }
	virtual void set_colors(color c[256]) override { memcpy(colors, c, sizeof(colors)); }
};

static inline uint32_t pack(int r, int g, int b, int a) {
	return (uint32_t)(uint8_t)r | ((uint32_t)(uint8_t)g << 8) |
	       ((uint32_t)(uint8_t)b << 16) | ((uint32_t)(uint8_t)a << 24);
}

struct mem_surface : surface {
	int bpp;                            // 1 = indexed, 4 = rgba
	std::vector<uint8_t> owned;
	uint8_t* px;
	palette_impl* pal = nullptr;
	blend_mode blend = blend_mode::none;
	int alpha_mod = 255;

	mem_surface(int width, int height, int bpp, uint8_t* external = nullptr) : bpp(bpp) {
		w = width; h = height; pitch = width * bpp;
		if (external) {
			px = external;
		} else {
			owned.assign((size_t)pitch * height, 0);
			px = owned.data();
		}
	}

	virtual void set_palette(palette* p) override { pal = (palette_impl*)p; }
	virtual void* lock() override { return px; }
	virtual void unlock() override {}
	virtual void set_alpha(int a) override { alpha_mod = a; }
	virtual void set_blend_mode(blend_mode b) override { blend = b; }

	virtual void fill(int r, int g, int b, int a) override {
		if (bpp == 4) {
			uint32_t v = pack(r, g, b, a);
			uint32_t* p = (uint32_t*)px;
			for (size_t i = 0, n = (size_t)w * h; i != n; ++i) p[i] = v;
		} else {
			memset(px, r & 0xff, (size_t)pitch * h);
		}
	}

	// Read a source pixel as RGBA components.
	inline void read_rgba(int sx, int sy, int& r, int& g, int& b, int& a) const {
		if (bpp == 1) {
			uint8_t idx = px[(size_t)sy * pitch + sx];
			color c = pal ? pal->colors[idx] : color{0, 0, 0, 255};
			r = c.r; g = c.g; b = c.b; a = 255;
		} else {
			uint32_t v = ((uint32_t*)px)[(size_t)sy * (pitch / 4) + sx];
			r = v & 0xff; g = (v >> 8) & 0xff; b = (v >> 16) & 0xff; a = (v >> 24) & 0xff;
		}
	}

	// Compose one source pixel onto dst respecting the blend mode + alpha mod.
	inline void write_pixel(uint32_t* dp, int r, int g, int b, int a) const {
		if (blend != blend_mode::alpha) { *dp = pack(r, g, b, 255); return; }
		int ea = a * alpha_mod / 255;
		if (ea <= 0) return;
		if (ea >= 255) { *dp = pack(r, g, b, 255); return; }
		uint32_t o = *dp;
		int orr = o & 0xff, og = (o >> 8) & 0xff, ob = (o >> 16) & 0xff;
		*dp = pack((r * ea + orr * (255 - ea)) / 255,
		           (g * ea + og * (255 - ea)) / 255,
		           (b * ea + ob * (255 - ea)) / 255, 255);
	}

	virtual void blit(surface* dst_s, int x, int y) override {
		mem_surface* dst = (mem_surface*)dst_s;
		if (dst->bpp != 4) return;                      // only rgba destinations are used
		uint32_t* d = (uint32_t*)dst->px;
		int dpitch = dst->pitch / 4;
		int x0 = x < 0 ? -x : 0;                         // clamp the x span once per surface
		int x1 = dst->w - x < w ? dst->w - x : w;
		for (int sy = 0; sy < h; ++sy) {
			int dy = y + sy;
			if (dy < 0 || dy >= dst->h) continue;
			uint32_t* drow = d + (size_t)dy * dpitch + x;
			for (int sx = x0; sx < x1; ++sx) {
				int r, g, b, a;
				read_rgba(sx, sy, r, g, b, a);
				write_pixel(&drow[sx], r, g, b, a);
			}
		}
	}

	virtual void blit_scaled(surface* dst_s, int x, int y, int dw, int dh) override {
		mem_surface* dst = (mem_surface*)dst_s;
		if (dst->bpp != 4 || dw <= 0 || dh <= 0) return;
		uint32_t* d = (uint32_t*)dst->px;
		int dpitch = dst->pitch / 4;
		for (int oy = 0; oy < dh; ++oy) {
			int dy = y + oy;
			if (dy < 0 || dy >= dst->h) continue;
			int sy = oy * h / dh;
			for (int ox = 0; ox < dw; ++ox) {
				int dx = x + ox;
				if (dx < 0 || dx >= dst->w) continue;
				int r, g, b, a;
				read_rgba(ox * w / dw, sy, r, g, b, a);
				write_pixel(&d[(size_t)dy * dpitch + dx], r, g, b, a);
			}
		}
	}
};

palette* new_palette() { return new palette_impl(); }
void delete_palette(palette* pal) { delete pal; }

std::unique_ptr<surface> create_rgba_surface(int width, int height) {
	return std::unique_ptr<surface>(new mem_surface(width, height, 4));
}

std::unique_ptr<surface> get_window_surface(native_window::window* wnd) {
	auto* impl = wnd->impl.get();
	return std::unique_ptr<surface>(new mem_surface(impl->width, impl->height, 4, impl->framebuffer.data()));
}

std::unique_ptr<surface> convert_to_8_bit_indexed(surface* s) {
	// Content is redrawn every frame; we only need a same-size 8-bit buffer.
	return std::unique_ptr<surface>(new mem_surface(s->w, s->h, 1));
}

std::unique_ptr<surface> load_image(const char*) { return nullptr; }
std::unique_ptr<surface> load_image(const void*, size_t) { return nullptr; }

}  // namespace native_window_drawing

// ---------------------------------------------------------------------------
// native_sound — bridged to the JS host's Web Audio (see openbw.js).
// The engine loads WAV bytes from the MPQs and hands them to load_wav; JS
// decodes them to AudioBuffers (by id) and plays/stops them per channel.
// ---------------------------------------------------------------------------
extern "C" {
__attribute__((import_module("env"), import_name("js_sound_load")))
int js_sound_load(const void* data, unsigned size);              // -> sound id
__attribute__((import_module("env"), import_name("js_sound_play")))
void js_sound_play(int id, int channel, int volume, int pan);
__attribute__((import_module("env"), import_name("js_sound_is_playing")))
int js_sound_is_playing(int channel);
__attribute__((import_module("env"), import_name("js_sound_stop")))
void js_sound_stop(int channel);
__attribute__((import_module("env"), import_name("js_sound_set_volume")))
void js_sound_set_volume(int channel, int volume);
}

namespace native_sound {

int frequency = 0;
int channels = 8;

struct wasm_sound : sound {
	int id;
	explicit wasm_sound(int id) : id(id) {}
};

void init() {}
void play(int channel, sound* s, int volume, int pan) {
	js_sound_play(s ? ((wasm_sound*)s)->id : -1, channel, volume, pan);
}
bool is_playing(int channel) { return js_sound_is_playing(channel) != 0; }
void stop(int channel) { js_sound_stop(channel); }
void set_volume(int channel, int volume) { js_sound_set_volume(channel, volume); }
std::unique_ptr<sound> load_wav(const void* data, size_t size) {
	return std::unique_ptr<sound>(new wasm_sound(js_sound_load(data, (unsigned)size)));
}

}  // namespace native_sound
