// web/bridge.cpp — OpenBW multiplayer relay (M1 of the retail-interop bridge).
//
// A tiny standalone WebSocket relay. Two browser OpenBW clients connect to
// ws://<this-host>:<port>/ and it forwards the lockstep stream between them — no
// copy-paste handshake, no external service. It is transport-only: it never parses
// the game protocol, only pipes opaque WebSocket frames from each client to the
// other. This is the shell the UDP/Storm side (M2/M3) will bolt onto: one side
// stays a browser over WebSocket, the other becomes retail over UDP.
//
// The copy-paste WebRTC path in the browser is kept as the zero-dependency
// fallback; this relay is the "share an address" convenience.
//
// Build:  clang++ -std=c++14 -O2 -I. web/bridge.cpp -o openbw_bridge -lpthread
// Run:    ./openbw_bridge [--port 8100]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <thread>
#include <mutex>
#include <memory>
#include <istream>

#define ASIO_STANDALONE
#include "deps/asio/asio.hpp"

using asio::ip::tcp;

// --- SHA-1 (only needed for the WebSocket Sec-WebSocket-Accept key) ---------
struct sha1 {
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	std::vector<uint8_t> buf;
	static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }
	void update(const std::string& s) { buf.insert(buf.end(), s.begin(), s.end()); }
	std::array<uint8_t, 20> finish() {
		uint64_t ml = (uint64_t)buf.size() * 8;
		buf.push_back(0x80);
		while (buf.size() % 64 != 56) buf.push_back(0);
		for (int i = 7; i >= 0; --i) buf.push_back((uint8_t)((ml >> (i * 8)) & 0xff));
		for (size_t off = 0; off < buf.size(); off += 64) {
			uint32_t w[80];
			for (int i = 0; i < 16; i++)
				w[i] = (buf[off + i*4] << 24) | (buf[off + i*4+1] << 16) | (buf[off + i*4+2] << 8) | buf[off + i*4+3];
			for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
			uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
			for (int i = 0; i < 80; i++) {
				uint32_t f, k;
				if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999; }
				else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
				else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
				else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
				uint32_t t = rol(a, 5) + f + e + k + w[i];
				e = d; d = c; c = rol(b, 30); b = a; a = t;
			}
			h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
		}
		std::array<uint8_t, 20> out;
		for (int i = 0; i < 5; i++)
			for (int j = 0; j < 4; j++) out[i*4 + j] = (uint8_t)((h[i] >> (24 - j*8)) & 0xff);
		return out;
	}
};

static std::string base64(const uint8_t* p, size_t n) {
	static const char* t =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string o;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)p[i] << 16;
		if (i + 1 < n) v |= (uint32_t)p[i+1] << 8;
		if (i + 2 < n) v |= p[i+2];
		o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
		o += i + 1 < n ? t[(v >> 6) & 63] : '=';
		o += i + 2 < n ? t[v & 63] : '=';
	}
	return o;
}

// --- minimal WebSocket server framing ---------------------------------------
static bool ws_handshake(tcp::socket& s) {
	asio::error_code ec;
	asio::streambuf reqbuf;
	// Browsers send the upgrade request then wait for the 101 before any frame,
	// so nothing beyond the request lands in the buffer here.
	asio::read_until(s, reqbuf, "\r\n\r\n", ec);
	if (ec) return false;
	std::istream is(&reqbuf);
	std::string line, key;
	while (std::getline(is, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) break;
		auto c = line.find(':');
		if (c == std::string::npos) continue;
		std::string name = line.substr(0, c), val = line.substr(c + 1);
		while (!val.empty() && val.front() == ' ') val.erase(val.begin());
		// header names are case-insensitive
		for (auto& ch : name) ch = (char)tolower((unsigned char)ch);
		if (name == "sec-websocket-key") key = val;
	}
	if (key.empty()) return false;
	sha1 h; h.update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
	auto d = h.finish();
	std::string resp =
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Accept: " + base64(d.data(), 20) + "\r\n\r\n";
	asio::write(s, asio::buffer(resp), ec);
	return !ec;
}

// Read one frame; returns the opcode (1 text, 2 binary, 8 close, 9 ping, 10 pong)
// or -1 on error/EOF. Client frames are always masked.
static int ws_read(tcp::socket& s, std::vector<uint8_t>& payload) {
	asio::error_code ec;
	uint8_t hdr[2];
	asio::read(s, asio::buffer(hdr, 2), ec); if (ec) return -1;
	int opcode = hdr[0] & 0x0f;
	bool masked = (hdr[1] & 0x80) != 0;
	uint64_t len = hdr[1] & 0x7f;
	if (len == 126) { uint8_t e[2]; asio::read(s, asio::buffer(e, 2), ec); if (ec) return -1;
	                  len = ((uint64_t)e[0] << 8) | e[1]; }
	else if (len == 127) { uint8_t e[8]; asio::read(s, asio::buffer(e, 8), ec); if (ec) return -1;
	                       len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i]; }
	uint8_t mask[4] = {0};
	if (masked) { asio::read(s, asio::buffer(mask, 4), ec); if (ec) return -1; }
	payload.resize((size_t)len);
	if (len) { asio::read(s, asio::buffer(payload.data(), (size_t)len), ec); if (ec) return -1; }
	if (masked) for (size_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
	return opcode;
}

// --- a connected client, with a mutex to serialize writes from either thread ---
struct Client {
	tcp::socket sock;
	std::mutex wmutex;
	explicit Client(tcp::socket&& s) : sock(std::move(s)) {}
	void send(int opcode, const uint8_t* p, size_t n) {
		std::lock_guard<std::mutex> lk(wmutex);
		std::vector<uint8_t> f;
		f.push_back((uint8_t)(0x80 | opcode));
		if (n < 126) f.push_back((uint8_t)n);
		else if (n < 65536) { f.push_back(126); f.push_back((uint8_t)((n >> 8) & 0xff)); f.push_back((uint8_t)(n & 0xff)); }
		else { f.push_back(127); for (int i = 7; i >= 0; --i) f.push_back((uint8_t)((n >> (i * 8)) & 0xff)); }
		f.insert(f.end(), p, p + n);
		asio::error_code ec;
		asio::write(sock, asio::buffer(f), ec);
	}
	void text(const char* s) { send(1, (const uint8_t*)s, strlen(s)); }
};

// A single 2-seat room. Enough for 1v1; multi-room is a later concern.
static std::mutex g_room;
static std::shared_ptr<Client> g_slots[2];

static void handle(std::shared_ptr<Client> c) {
	if (!ws_handshake(c->sock)) return;

	int idx = -1;
	std::shared_ptr<Client> partner;
	{
		std::lock_guard<std::mutex> lk(g_room);
		if (!g_slots[0]) idx = 0; else if (!g_slots[1]) idx = 1;
		if (idx < 0) { c->text("{\"t\":\"_relay\",\"error\":\"full\"}"); return; }
		g_slots[idx] = c;
		partner = g_slots[1 - idx];
	}
	fprintf(stderr, "openbw_bridge: client joined seat %d%s\n", idx, partner ? " (paired)" : " (waiting)");
	if (partner) { c->text("{\"t\":\"_relay\",\"peer\":\"connected\"}");
	               partner->text("{\"t\":\"_relay\",\"peer\":\"connected\"}"); }
	else c->text("{\"t\":\"_relay\",\"waiting\":true}");

	std::vector<uint8_t> payload;
	for (;;) {
		int op = ws_read(c->sock, payload);
		if (op < 0 || op == 8) break;                 // error / EOF / close
		if (op == 9) { c->send(10, payload.data(), payload.size()); continue; }   // ping -> pong
		if (op == 1 || op == 2) {                     // text / binary -> forward to partner
			std::shared_ptr<Client> pt;
			{ std::lock_guard<std::mutex> lk(g_room); pt = g_slots[1 - idx]; }
			if (pt) pt->send(op, payload.data(), payload.size());
		}
	}

	std::shared_ptr<Client> pt;
	{ std::lock_guard<std::mutex> lk(g_room); if (g_slots[idx] == c) g_slots[idx].reset(); pt = g_slots[1 - idx]; }
	if (pt) pt->text("{\"t\":\"_relay\",\"peer\":\"left\"}");
	fprintf(stderr, "openbw_bridge: seat %d left\n", idx);
}

int main(int argc, char** argv) {
	int port = 8100;
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);

	try {
		asio::io_service io;
		tcp::acceptor acc(io, tcp::endpoint(tcp::v4(), (unsigned short)port));
		fprintf(stderr, "openbw_bridge: relay listening on ws://0.0.0.0:%d/  (share your LAN IP)\n", port);
		for (;;) {
			tcp::socket s(io);
			asio::error_code ec;
			acc.accept(s, ec);
			if (ec) continue;
			std::thread(handle, std::make_shared<Client>(std::move(s))).detach();
		}
	} catch (const std::exception& e) {
		fprintf(stderr, "openbw_bridge: fatal — %s\n", e.what());
		return 1;
	}
}
