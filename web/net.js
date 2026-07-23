// web/net.js — deterministic lockstep turn queue for multiplayer.
//
// The simulation is deterministic (fixed LCG seed, integer/fixed-point math — it's why
// OpenBW replays BW bit-exactly), so peers stay identical as long as every peer applies
// the same player commands on the same frame. This module owns that scheduling:
//
//   - local input is drained from the wasm as BW command bytes (see bw_cmd in sandbox.h)
//   - it is tagged for `frame + delay` and broadcast to peers
//   - a frame is only simulated once *every* player's batch for it has arrived
//
// Nothing here knows about WebRTC: the caller supplies a `send` callback and pushes
// incoming turns in via receiveTurn(). That keeps single-player, a loopback harness, and
// a real peer connection on exactly the same code path.

// Every wire surface: the command byte format (bw_cmd in web/sandbox.h), the turn framing
// here, and the lobby/handshake messages. Bump on ANY change to those — peers can easily
// be on different deploys, and a version mismatch must fail loudly instead of desyncing.
export const PROTOCOL = 1;

const EMPTY = new Uint8Array(0);

// --- signalling -------------------------------------------------------------------
// There is no signalling server: the offer and answer are exchanged by copy-paste, like
// the original game's direct-cable mode. They're gzipped and base64url'd to stay short.
// Measured on a typical machine, a data-channel-only offer is ~850 B raw / ~720 chars
// encoded — but the size is dominated by ICE candidate lines, and candidate count scales
// with the number of network interfaces (VPNs, Docker bridges, IPv6). So callers must
// check the encoded length and fall back to a paste box rather than assume a URL fits.
export const MAX_URL_CODE = 1800;   // beyond this, some chat/mail clients mangle the link

const b64urlEncode = (bytes) => {
  let bin = '';
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
};
const b64urlDecode = (s) => {
  const b64 = s.replace(/-/g, '+').replace(/_/g, '/');
  const bin = atob(b64 + (b64.length % 4 ? '='.repeat(4 - (b64.length % 4)) : ''));
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
};

export async function packSignal(obj) {
  const stream = new Blob([JSON.stringify(obj)]).stream().pipeThrough(new CompressionStream('gzip'));
  return b64urlEncode(new Uint8Array(await new Response(stream).arrayBuffer()));
}
export async function unpackSignal(code) {
  const stream = new Blob([b64urlDecode(code.trim())]).stream()
    .pipeThrough(new DecompressionStream('gzip'));
  return JSON.parse(await new Response(stream).text());
}

// Peers can easily be on different deploys (we cache-bust per commit), so a version
// mismatch must fail loudly here rather than surface later as an unexplained desync.
function checkProtocol(msg) {
  if (msg && msg.v === PROTOCOL) return msg;
  throw new Error(
    `Protocol mismatch: you're on v${PROTOCOL}, the other side is on v${msg && msg.v != null ? msg.v : '?'}. ` +
    `Both players need the same version — reload the page (and have them do the same) to get the latest.`);
}

// --- transport --------------------------------------------------------------------
// STUN lets most home NATs connect. There's no TURN, so symmetric NAT will fail; a plain
// LAN works even with no ICE servers at all.
export const DEFAULT_ICE = [{ urls: 'stun:stun.l.google.com:19302' }];

export class PeerLink {
  constructor({ onControl = () => {}, onOpen = () => {}, onClose = () => {},
                iceServers = DEFAULT_ICE } = {}) {
    this.pc = new RTCPeerConnection({ iceServers });
    this.ch = null;
    this.onControl = onControl; this.onOpen = onOpen; this.onClose = onClose;
    // Turns can arrive before the game has finished loading and attached a consumer.
    // They are never retransmitted, so dropping them would stall lockstep permanently —
    // queue until setTurnHandler() is called, then flush.
    this._onTurn = null;
    this._queued = [];
    this.pc.onconnectionstatechange = () => {
      const s = this.pc.connectionState;
      if (s === 'failed' || s === 'disconnected' || s === 'closed') this.onClose(s);
    };
  }

  #wire(ch) {
    this.ch = ch;
    ch.binaryType = 'arraybuffer';
    ch.onopen = () => this.onOpen();
    ch.onclose = () => this.onClose('closed');
    // Strings are JSON control messages; binary is a turn ([u32 frame][command bytes]).
    ch.onmessage = (e) => {
      if (typeof e.data === 'string') { this.onControl(JSON.parse(e.data)); return; }
      const frame = new DataView(e.data).getUint32(0, true);
      const bytes = new Uint8Array(e.data, 4);
      if (this._onTurn) this._onTurn(frame, bytes);
      else if (this._queued.length < 20000) this._queued.push([frame, bytes]);
    };
  }

  /** Attach the turn consumer, replaying anything buffered while the game was loading. */
  setTurnHandler(fn) {
    this._onTurn = fn;
    const q = this._queued;
    this._queued = [];
    for (const [f, b] of q) fn(f, b);
  }

  // Non-trickle ICE: the blob must be self-contained, so we wait for candidates before
  // encoding it. Waiting for iceGatheringState === 'complete' is the obvious way and also
  // the slow way — it doesn't settle until every transport finishes or times out (mDNS
  // registration, a blocked STUN server), which routinely costs seconds. Instead, finish
  // shortly after the candidates that actually matter arrive: once we have a
  // server-reflexive (public) candidate a peer behind NAT has what it needs, so we only
  // grant stragglers a brief grace period. Anything arriving after that is dropped, which
  // is the accepted trade for a pasteable, non-trickle blob.
  async #gathered() {
    if (this.pc.iceGatheringState === 'complete') return;
    await new Promise((res) => {
      let settled = false, grace = 0;
      const finish = () => {
        if (settled) return;
        settled = true;
        clearTimeout(grace); clearTimeout(hard);
        this.pc.removeEventListener('icegatheringstatechange', onState);
        this.pc.removeEventListener('icecandidate', onCand);
        res();
      };
      const onState = () => { if (this.pc.iceGatheringState === 'complete') finish(); };
      const onCand = (e) => {
        if (!e.candidate) return finish();        // null candidate: gathering really is done
        const srflx = e.candidate.candidate.includes(' typ srflx');
        clearTimeout(grace);
        grace = setTimeout(finish, srflx ? 250 : 700);
      };
      this.pc.addEventListener('icegatheringstatechange', onState);
      this.pc.addEventListener('icecandidate', onCand);
      const hard = setTimeout(finish, 2500);      // hard cap if STUN is unreachable
    });
  }

  /** Host: build the invite code. */
  async createOffer(extra = {}) {
    this.#wire(this.pc.createDataChannel('bw', { ordered: true }));
    await this.pc.setLocalDescription(await this.pc.createOffer());
    await this.#gathered();
    return packSignal({ v: PROTOCOL, ...extra, sdp: this.pc.localDescription.sdp });
  }

  /** Joiner: consume the invite, return {info, answer} to paste back. */
  async acceptOffer(code, extra = {}) {
    const info = checkProtocol(await unpackSignal(code));
    this.pc.ondatachannel = (e) => this.#wire(e.channel);
    await this.pc.setRemoteDescription({ type: 'offer', sdp: info.sdp });
    await this.pc.setLocalDescription(await this.pc.createAnswer());
    await this.#gathered();
    return { info, answer: await packSignal({ v: PROTOCOL, ...extra, sdp: this.pc.localDescription.sdp }) };
  }

  /** Host: consume the pasted response. */
  async acceptAnswer(code) {
    const info = checkProtocol(await unpackSignal(code));
    await this.pc.setRemoteDescription({ type: 'answer', sdp: info.sdp });
    return info;
  }

  sendTurn(frame, bytes) {
    if (!this.ch || this.ch.readyState !== 'open') return;
    const buf = new Uint8Array(4 + bytes.length);
    new DataView(buf.buffer).setUint32(0, frame, true);
    buf.set(bytes, 4);
    this.ch.send(buf);
  }
  /**
   * Control messages are JSON and always carry a frame counter, so the receiver can tell
   * how far along the sender was and reject anything inconsistent. Set `frameOf` once the
   * game is running to stamp the live frame.
   */
  sendControl(obj) {
    if (this.ch && this.ch.readyState === 'open')
      this.ch.send(JSON.stringify({ f: this.frameOf ? this.frameOf() : 0, ...obj }));
  }
  close() {
    try { if (this.ch) this.ch.close(); } catch {}
    try { this.pc.close(); } catch {}
  }
}

export class Lockstep {
  /**
   * @param {object}  o
   * @param {object}  o.x          wasm exports
   * @param {WebAssembly.Memory} o.memory
   * @param {number[]} o.slots     participating player slots ([0] solo, [0,1] for 1v1)
   * @param {number}  o.localSlot  which of those is us
   * @param {number}  o.delay      input delay in frames (0 solo; a few frames for a peer)
   * @param {(msg:object)=>void} o.send
   */
  constructor({ x, memory, slots, localSlot, delay = 0, send = () => {} }) {
    this.x = x;
    this.memory = memory;
    this.slots = slots.slice();
    this.localSlot = localSlot;
    this.delay = delay;
    this.send = send;
    this.frame = 0;           // next frame to simulate
    this.nextLocalFrame = 0;  // next frame we still owe a local batch for
    this.turns = new Map();   // frame -> Map(slot -> Uint8Array)
    this.stalledSince = 0;
  }

  #batch(f) {
    let m = this.turns.get(f);
    if (!m) { m = new Map(); this.turns.set(f, m); }
    return m;
  }

  /**
   * A peer's turn arrived. Every turn carries the frame it applies to, and it is only
   * legal inside a narrow window: a turn for a frame we've already simulated can never be
   * applied (lockstep never advances past a missing turn, so it must be a duplicate or a
   * replay), and one far in the future means the peer is out of step or misbehaving.
   * Returns a verdict so the host can drop a peer that sends bad frames.
   */
  receiveTurn(slot, frame, bytes) {
    if (!Number.isInteger(frame) || frame < 0)
      return { ok: false, why: `malformed frame ${frame}` };
    if (frame < this.frame)
      return { ok: false, why: `turn for frame ${frame}, already simulated up to ${this.frame}` };
    if (frame > this.frame + this.horizon())
      return { ok: false, why: `turn for frame ${frame}, too far ahead of ${this.frame}` };
    const b = this.#batch(frame);
    if (b.has(slot))
      return { ok: false, why: `duplicate turn for frame ${frame}` };
    b.set(slot, bytes && bytes.length ? bytes : EMPTY);
    return { ok: true };
  }

  /** How far ahead of the current frame a peer's turn may legitimately be. */
  horizon() { return this.delay * 4 + 8; }

  /** Pull locally generated commands out of the wasm (copy: the buffer is reused). */
  #drainLocal() {
    const len = this.x.openbw_out_len();
    if (!len) return EMPTY;
    const out = new Uint8Array(this.memory.buffer, this.x.openbw_out_ptr(), len).slice();
    this.x.openbw_out_clear();
    return out;
  }

  /** Hand one player's batch to the sim. openbw_in_ptr can grow memory, so view after. */
  #apply(slot, bytes) {
    if (!bytes || !bytes.length) return;
    const dst = this.x.openbw_in_ptr(bytes.length);
    new Uint8Array(this.memory.buffer, dst, bytes.length).set(bytes);
    this.x.openbw_apply(slot, bytes.length);
  }

  /** Slots we're still waiting on for frame `f`. */
  missing(f) {
    const m = this.turns.get(f);
    return this.slots.filter((s) => !m || !m.has(s));
  }

  /**
   * Advance at most one frame. Returns true if the sim stepped, false if we're waiting
   * on a peer (the caller should render a "waiting" state but keep rendering/input alive).
   */
  tick() {
    // Owe a batch for every frame through frame+delay. On the first tick this also
    // bootstraps the initial `delay` frames with empty batches, so peers immediately have
    // something to consume and nobody deadlocks waiting for turn 0.
    while (this.nextLocalFrame <= this.frame + this.delay) {
      const f = this.nextLocalFrame++;
      const d = this.#drainLocal();
      this.#batch(f).set(this.localSlot, d);
      if (this.slots.length > 1) this.send({ t: 'turn', f, d });
    }

    if (this.missing(this.frame).length) {
      if (!this.stalledSince) this.stalledSince = Date.now();
      return false;
    }
    this.stalledSince = 0;

    // Apply in slot order so every peer applies in the same order.
    const m = this.turns.get(this.frame);
    for (const s of this.slots) this.#apply(s, m.get(s));
    this.turns.delete(this.frame);

    this.x.openbw_step();
    this.frame++;
    return true;
  }

  /** How long we've been blocked on a peer, in ms (0 when running). */
  stalledMs() { return this.stalledSince ? Date.now() - this.stalledSince : 0; }

  /**
   * Stop waiting on a slot. NOTE: with more than two players every peer must drop the
   * same slot on the same frame or they diverge; for 1v1 dropping the peer just means
   * continuing solo, so a local decision is safe.
   */
  dropSlot(slot) {
    this.slots = this.slots.filter((s) => s !== slot);
    this.stalledSince = 0;
  }
}
