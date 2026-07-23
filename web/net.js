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

  /** A peer's turn arrived. */
  receiveTurn(slot, frame, bytes) {
    this.#batch(frame).set(slot, bytes && bytes.length ? bytes : EMPTY);
  }

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
