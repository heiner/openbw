// web/openbw.js — browser host for the OpenBW sandbox wasm module.
//
// Responsibilities:
//   * fetch the 3 game MPQs from the Internet Archive on first run and cache
//     them in IndexedDB (so no copyrighted bytes are ever hosted by the site);
//   * provide a minimal WASI shim + the env file bridge the wasm imports;
//   * step the sim on a fixed timer and render on requestAnimationFrame,
//     blitting the framebuffer to a <canvas>; forward input to the wasm exports.

// Build id — CI replaces the placeholder with the commit SHA for cache-busting.
const BUILD = '__BUILD__';

// BOT_LOG (?debug or ?botlog) mirrors a bot's own log output (e.g. McRave's per-frame
// profiler) into the console.
const BOT_LOG = /(\?|&)(debug|botlog)\b/.test(location.search);

// Archive index mapping must match web/wasm_main.cpp's js_file_reader:
//   0 StarDat  1 BrooDat  2 Patch_rt  3 map
const ARCHIVE_BASE =
  'https://archive.org/download/sc-classic-installer_202311/' +
  'StarCraft%20Portable.zip/Starcraft%20Brood%20War%2F';
const MPQS = [
  { key: 'stardat',  local: './data/STARDAT.MPQ',  url: ARCHIVE_BASE + 'STARDAT.MPQ',  size: 66184491 },
  { key: 'broodat',  local: './data/BROODAT.MPQ',  url: ARCHIVE_BASE + 'BROODAT.MPQ',  size: 23519727 },
  { key: 'patch_rt', local: './data/patch_rt.mpq', url: ARCHIVE_BASE + 'patch_rt.mpq', size: 636958   },
];
// The melee map: a CC BY 4.0 map committed under web/maps/ and deployed with the
// site (see web/maps/ATTRIBUTION.md). MAP_REMOTE is an optional fallback, used only
// if a build ever ships without the map file.
// Map metadata for the lobby (needed before the map parses; cross-checked at load). `file`
// is fetched directly, so it can be a local path or a CORS URL — the pro/classic maps stream
// from the bwmapdb GitHub repo, so only CC-licensed Weave is bundled. Weave is the default:
// it renders cleanly, whereas OpenBW leaves black gaps on some maps.
const BWDB = 'https://raw.githubusercontent.com/Lucifirius/bwmapdb/master/F0%20-%20Melee/';
const MAPS = [
  { name: 'Weave',            file: './maps/Weave_v1.scx',                          starts: 4, w: 64,  h: 64,  tileset: 'Desert'   },
  { name: 'Lost Temple',      file: BWDB + '(4)Lost%20Temple.scm',                  starts: 4, w: 128, h: 128, tileset: 'Jungle'   },
  { name: 'Python',           file: BWDB + '~Pro/(4)Python1.3.scx',                 starts: 4, w: 128, h: 128, tileset: 'Jungle'   },
  { name: 'Luna',             file: BWDB + '~Pro/(4)Luna%20The%20Final%20PGT.scx',  starts: 4, w: 128, h: 128, tileset: 'Twilight' },
  { name: 'Nostalgia',        file: BWDB + '~Pro/(4)Nostalgia.scm',                 starts: 4, w: 128, h: 128, tileset: 'Badlands' },
  { name: 'Neo Lost Temple',  file: BWDB + '~Pro/(4)Neo%20Lost%20Temple.scx',       starts: 4, w: 128, h: 128, tileset: 'Jungle'   },
  { name: 'Longinus',         file: BWDB + '~Pro/(4)Longinus.scx',                  starts: 3, w: 128, h: 128, tileset: 'Jungle'   },
  { name: 'The Hunters',      file: BWDB + '(8)The%20Hunters.scm',                  starts: 8, w: 128, h: 128, tileset: 'Jungle'   },
  { name: 'Big Game Hunters', file: BWDB + '(8)Big%20Game%20Hunters.scm',           starts: 8, w: 128, h: 128, tileset: 'Jungle'   },
  { name: 'Blood Bath',       file: BWDB + '(4)Blood%20Bath.scm',                   starts: 4, w: 64,  h: 64,  tileset: 'Space'    },
];
const TILESETS = ['Badlands', 'Space', 'Installation', 'Ashworld', 'Jungle', 'Desert', 'Ice', 'Twilight'];
// Pick `n` distinct start slots at random.
function pickStartSlots(n, starts) {
  const pool = [...Array(Math.max(starts, n)).keys()];
  for (let i = pool.length - 1; i > 0; i--) {
    const j = (Math.random() * (i + 1)) | 0;
    [pool[i], pool[j]] = [pool[j], pool[i]];
  }
  return pool.slice(0, n);
}
const MAP_LOCAL = MAPS[0].file;
const MAP_REMOTE = '';

// Fetched map bytes, kept so the lobby can hash a map without re-downloading it.
const mapCache = new Map();
async function fetchMap(file) {
  if (mapCache.has(file)) return mapCache.get(file);
  const r = await fetch(file);
  if (!r.ok) throw new Error('Map not available: ' + file);
  const bytes = new Uint8Array(await r.arrayBuffer());
  mapCache.set(file, bytes);
  return bytes;
}
// Peers must run byte-identical terrain or their sims diverge immediately, so the map is
// hash-verified rather than trusted by filename.
// A quick non-cryptographic hash (cyrb53) of the map bytes — enough to catch peers loading
// different terrain, and pure JS so it works on any origin. crypto.subtle would need a
// secure context (https or localhost), which broke multiplayer over a plain-http LAN IP.
function cyrb53(bytes) {
  let h1 = 0xdeadbeef, h2 = 0x41c6ce57;
  for (let i = 0; i < bytes.length; i++) {
    h1 = Math.imul(h1 ^ bytes[i], 2654435761);
    h2 = Math.imul(h2 ^ bytes[i], 1597334677);
  }
  h1 = Math.imul(h1 ^ (h1 >>> 16), 2246822507) ^ Math.imul(h2 ^ (h2 >>> 13), 3266489909);
  h2 = Math.imul(h2 ^ (h2 >>> 16), 2246822507) ^ Math.imul(h1 ^ (h1 >>> 13), 3266489909);
  return (4294967296 * (2097151 & h2) + (h1 >>> 0)).toString(16).padStart(14, '0');
}
async function mapHash(file) {
  return cyrb53(await fetchMap(file));
}

const $ = (id) => document.getElementById(id);
const setBar = (f) => { $('bar').firstElementChild.style.width = (f * 100).toFixed(1) + '%'; };
const setMsg = (m) => { $('msg').textContent = m; };

// ---------------------------------------------------------------------------
// IndexedDB cache for the MPQs
// ---------------------------------------------------------------------------
function idbOpen() {
  return new Promise((res, rej) => {
    const r = indexedDB.open('openbw', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('assets');
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
}
function idbGet(db, key) {
  return new Promise((res, rej) => {
    const r = db.transaction('assets').objectStore('assets').get(key);
    r.onsuccess = () => res(r.result || null);
    r.onerror = () => rej(r.error);
  });
}
function idbPut(db, key, val) {
  return new Promise((res, rej) => {
    const tx = db.transaction('assets', 'readwrite');
    tx.objectStore('assets').put(val, key);
    tx.oncomplete = () => res();
    tx.onerror = () => rej(tx.error);
  });
}

async function fetchWithProgress(url, expected, onProgress) {
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`fetch ${url}: HTTP ${resp.status}`);
  const total = +resp.headers.get('content-length') || expected || 0;
  const reader = resp.body.getReader();
  const chunks = [];
  let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    got += value.length;
    if (total) onProgress(got / total);
  }
  const buf = new Uint8Array(got);
  let o = 0;
  for (const c of chunks) { buf.set(c, o); o += c.length; }
  return buf.buffer;
}

// Returns [stardat, broodat, patch_rt, map] as Uint8Arrays.
async function loadAssets(mapFile = MAP_LOCAL) {
  const db = await idbOpen();
  const out = new Array(MPQS.length);

  // Fetch the three MPQs concurrently. archive.org throttles per-connection, so
  // three parallel downloads pull far more aggregate bandwidth than the old
  // serial loop, and IndexedDB writes are independent so there's no contention.
  // The bar is the sum of bytes across all three, leaving the final slice for the map.
  const totalBytes = MPQS.reduce((s, m) => s + m.size, 0);
  const got = new Array(MPQS.length).fill(0);
  const MPQ_SPAN = MPQS.length / (MPQS.length + 1);
  const renderBar = () =>
    setBar(Math.min(1, got.reduce((a, b) => a + b, 0) / totalBytes) * MPQ_SPAN);
  setMsg(`Downloading game data (${(totalBytes / 1e6) | 0} MB)…`);

  await Promise.all(MPQS.map(async ({ key, local, url, size }, i) => {
    let buf = await idbGet(db, key);
    if (!buf) {
      // Same-origin local mirror is a dev convenience; production hosts no game
      // files, so this 404s and the archive.org URL is used.
      let mirror = null;
      try { const r = await fetch(local); if (r.ok) mirror = await r.arrayBuffer(); } catch {}
      buf = mirror || await fetchWithProgress(url, size, (frac) => { got[i] = frac * size; renderBar(); });
      await idbPut(db, key, buf);
    }
    got[i] = size; renderBar();
    out[i] = new Uint8Array(buf);
  }));

  setMsg('Loading map…');
  // fetchMap memoises, so a map the lobby already hashed isn't downloaded twice.
  let mapBytes = null;
  try { mapBytes = await fetchMap(mapFile); } catch {}
  if (!mapBytes && MAP_REMOTE) mapBytes = new Uint8Array(await (await fetch(MAP_REMOTE)).arrayBuffer());
  if (!mapBytes) throw new Error('No map available — set MAP_REMOTE to a hosted .scx URL.');
  out.push(mapBytes);
  setBar(1);
  return out;
}

// ---------------------------------------------------------------------------
// WASI shim (only what wasm_main/backend actually import)
// ---------------------------------------------------------------------------
function makeWasi(getMemory) {
  const OK = 0, EBADF = 8;
  const dv = () => new DataView(getMemory().buffer);
  const dec = new TextDecoder();
  return {
    clock_time_get(id, precision, out) {
      const ns = BigInt(Math.round(performance.now() * 1e6));
      dv().setBigUint64(out, ns, true);
      return OK;
    },
    fd_write(fd, iovs, iovs_len, nwritten) {
      const view = dv();
      let total = 0, s = '';
      for (let i = 0; i < iovs_len; i++) {
        const p = iovs + i * 8;
        const off = view.getUint32(p, true), len = view.getUint32(p + 4, true);
        s += dec.decode(new Uint8Array(getMemory().buffer, off, len));
        total += len;
      }
      view.setUint32(nwritten, total, true);
      if (s) (fd === 2 ? console.error : console.log)('[wasm] ' + s.replace(/\n$/, ''));
      return OK;
    },
    fd_fdstat_get(fd, out) {
      const view = dv();
      view.setUint8(out, 2);          // filetype = character device (stdout/stderr)
      view.setUint16(out + 2, 0, true);
      view.setBigUint64(out + 8, 0n, true);
      view.setBigUint64(out + 16, 0n, true);
      return OK;
    },
    fd_fdstat_set_flags: () => OK,
    fd_close: () => OK,
    fd_seek: () => EBADF,
    fd_read: () => EBADF,
    path_open: () => EBADF,
    fd_prestat_get: () => EBADF,      // no preopens
    fd_prestat_dir_name: () => EBADF,
    proc_exit(code) { throw new Error('wasm proc_exit(' + code + ')'); },
    // The bot module (BWAPI server + ZZZKBot) pulls in a bit more of libc than the
    // clean build: it reads env vars (none set -> defaults), touches the filesystem
    // (there is none -> fails cleanly), and uses random_get. None affect the sim.
    environ_sizes_get(count_out, size_out) {
      const view = dv(); view.setUint32(count_out, 0, true); view.setUint32(size_out, 0, true); return OK;
    },
    environ_get: () => OK,            // empty environment
    random_get(buf, len) {
      crypto.getRandomValues(new Uint8Array(getMemory().buffer, buf, len)); return OK;
    },
    poll_oneoff(subs, events, n, nevents_out) { dv().setUint32(nevents_out, 0, true); return OK; },
    path_unlink_file: () => EBADF,
    path_remove_directory: () => EBADF,
    path_rename: () => EBADF,
  };
}

// ---------------------------------------------------------------------------
// Input mapping (browser -> SDL-style codes the engine expects)
// ---------------------------------------------------------------------------
const SCANCODE = {
  Escape: 41, Space: 44,
  ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82,
  ControlLeft: 224, ShiftLeft: 225, ControlRight: 228, ShiftRight: 229,
};

// A user-chosen key (KeyboardEvent.code) that acts exactly like Escape — for setups where
// the physical Esc is awkward, and ahead of pointer-lock/fullscreen modes where the browser
// reserves Esc for itself. Vim users' Caps Lock→Esc habit is the canonical example.
let altEsc = '';
try { altEsc = localStorage.getItem('openbw-alt-esc') || ''; } catch {}

function wireInput(canvas, x) {
  const xy = (e) => {
    const r = canvas.getBoundingClientRect();
    return [Math.round((e.clientX - r.left) * canvas.width / r.width),
            Math.round((e.clientY - r.top) * canvas.height / r.height)];
  };
  const sdlButton = (b) => (b === 0 ? 1 : b === 1 ? 2 : b === 2 ? 3 : 0);

  // Reflect a mouse event's modifier flags into the engine's key-state, which is
  // authoritative at click time. On macOS this lets Cmd (meta) stand in for Ctrl
  // for select-all-of-type — there Ctrl+click is hijacked by the OS as a right-click.
  const syncMods = (e) => {
    x.openbw_key(e.ctrlKey || e.metaKey ? 1 : 0, 0, 224);   // virtual Ctrl
    x.openbw_key(e.shiftKey ? 1 : 0, 0, 225);               // virtual Shift
  };

  canvas.addEventListener('mousemove', (e) => { const [px, py] = xy(e); x.openbw_mouse_move(px, py); });
  // Park the cursor off-screen when it leaves the canvas so edge-scrolling stops.
  canvas.addEventListener('mouseleave', () => x.openbw_mouse_move(-1, -1));
  canvas.addEventListener('mousedown', (e) => {
    syncMods(e); const [px, py] = xy(e); x.openbw_mouse_button(1, sdlButton(e.button), px, py, e.detail || 1);
  });
  window.addEventListener('mouseup', (e) => {
    syncMods(e); const [px, py] = xy(e); x.openbw_mouse_button(0, sdlButton(e.button), px, py, e.detail || 1);
  });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  // Forward keys to the engine without shadowing the browser's own shortcuts.
  // The game only binds unmodified keys plus Ctrl+digit (control groups); anything
  // held with Cmd (meta) or Alt — Cmd+1 to switch tabs, Cmd+R to reload, … — is
  // left for the browser. The modifier keys themselves are always forwarded so the
  // engine can track held state (for select-all, shift-queue, group assign).
  const MOD_SC = new Set([224, 225, 228, 229]);
  const key = (down) => (e) => {
    if (document.activeElement && document.activeElement.tagName === 'INPUT') return;   // typing in chat
    if (altEsc && e.code === altEsc) {
      // Fire a complete Esc press. Caps Lock is a lock key — macOS delivers keydown when
      // the lock turns on but only keyup when it turns off — so accept either edge for it;
      // ordinary keys act on keydown alone (both edges would double-fire the cancel).
      if (down || e.code === 'CapsLock') { x.openbw_key(1, 0, 41); x.openbw_key(0, 0, 41); }
      e.preventDefault();
      return;
    }
    const sc = SCANCODE[e.code] || 0;
    const sym = e.key.length === 1 ? e.key.toLowerCase().charCodeAt(0) : 0;
    if (!sc && !sym) return;
    if (MOD_SC.has(sc)) { x.openbw_key(down ? 1 : 0, 0, sc); return; }   // track, don't preventDefault
    const isDigit = sym >= 48 && sym <= 57;
    const gameBinds = (!e.metaKey && !e.altKey && !e.ctrlKey)          // unmodified keys
                   || (e.ctrlKey && !e.metaKey && !e.altKey && isDigit); // Ctrl+digit = control groups
    if (!gameBinds) return;   // hand Cmd+1, Cmd+R, Ctrl+T, … back to the browser
    x.openbw_key(down ? 1 : 0, sym, sc);
    e.preventDefault();
  };
  window.addEventListener('keydown', key(true));
  window.addEventListener('keyup', key(false));

  // Trackpad / wheel scrolling pans the camera.
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    const scale = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? canvas.height : 1;
    x.openbw_pan(Math.round(e.deltaX * scale), Math.round(e.deltaY * scale));
  }, { passive: false });

  // --- Touch: gestures onto the mouse behaviours the engine already knows.
  //   short tap            → select                (left click)
  //   double tap           → select all of type    (double left click)
  //   drag                 → pan the camera
  //   long-press then drag → box select            (left-button drag)
  //   long-press then lift → smart order            (right click: move / attack / gather)
  //   two-finger tap       → smart order            (right click — the trackpad's gesture)
  // The HTML command card keeps working, so tapping a verb (Attack…) then a target is the
  // discoverable order path; two-finger tap (tablet) and long-press (one-handed phone) are
  // the right-click shortcuts. Everything routes to existing exports — no engine changes.
  const LONG_MS = 350, DBL_MS = 300, MOVE_PX = 12, MOVE2 = MOVE_PX * MOVE_PX;
  const d2 = (ax, ay, bx, by) => (ax - bx) ** 2 + (ay - by) ** 2;
  let tt = null;                            // the active gesture, or null
  let lastTap = { t: -1e9, x: 0, y: 0 };    // for double-tap detection
  // Park the engine cursor off-screen after a gesture: otherwise its last position lingers
  // and, if that was near a screen edge, edge-scroll keeps running with no finger down.
  const park = () => x.openbw_mouse_move(-1, -1);

  canvas.addEventListener('touchstart', (e) => {
    e.preventDefault();
    const t = e.changedTouches[0];
    const [px, py] = xy(t);
    if (tt) {
      // A second finger while the first is still a pending tap → two-finger tap = right
      // click, mirroring the trackpad. Cancels the first finger's single-tap/long-press.
      if (tt.mode === 'pending' && tt.id2 === undefined) {
        clearTimeout(tt.timer);
        tt.mode = 'two'; tt.id2 = t.identifier; tt.x02 = px; tt.y02 = py;
      }
      return;
    }
    tt = { id: t.identifier, x0: px, y0: py, x: px, y: py, mode: 'pending', armed: false, id2: undefined, up: 0 };
    tt.timer = setTimeout(() => { if (tt) tt.armed = true; }, LONG_MS);   // long-press fires
  }, { passive: false });

  canvas.addEventListener('touchmove', (e) => {
    if (!tt) return;
    e.preventDefault();
    if (tt.mode === 'two' || tt.mode === 'dead') {
      // Movement of either finger means it's a pan/pinch, not a tap — cancel the order.
      for (const c of e.changedTouches) {
        if (c.identifier !== tt.id && c.identifier !== tt.id2) continue;
        const [cx, cy] = xy(c), first = c.identifier === tt.id;
        if (d2(cx, cy, first ? tt.x0 : tt.x02, first ? tt.y0 : tt.y02) > MOVE2) tt.mode = 'dead';
      }
      return;
    }
    const t = [...e.changedTouches].find((c) => c.identifier === tt.id);
    if (!t) return;
    const [px, py] = xy(t);
    const far = d2(px, py, tt.x0, tt.y0) > MOVE2;
    if (tt.mode === 'pending' && far) {
      clearTimeout(tt.timer);
      if (tt.armed) {                       // long-press held, now dragging → box select
        tt.mode = 'box';
        x.openbw_mouse_move(tt.x0, tt.y0);
        x.openbw_mouse_button(1, 1, tt.x0, tt.y0, 1);   // left down at the press point
        x.openbw_mouse_move(px, py);
      } else {                              // moved before the long-press → pan
        tt.mode = 'pan';
      }
    } else if (tt.mode === 'pan') {
      x.openbw_pan(tt.x - px, tt.y - py);   // drag the world with the finger
    } else if (tt.mode === 'box') {
      x.openbw_mouse_move(px, py);          // grow the selection rectangle
    }
    tt.x = px; tt.y = py;
  }, { passive: false });

  const endTouch = (e) => {
    if (!tt) return;
    e.preventDefault();
    const t = [...e.changedTouches].find((c) => c.identifier === tt.id || c.identifier === tt.id2);
    if (!t) return;                         // a finger we aren't tracking
    if (tt.mode === 'two' || tt.mode === 'dead') {
      if (++tt.up < 2) return;              // wait until both fingers have lifted
      if (tt.mode === 'two') {              // clean two-finger tap → smart order at the anchor
        x.openbw_mouse_move(tt.x0, tt.y0);
        x.openbw_mouse_button(1, 3, tt.x0, tt.y0, 1);
        x.openbw_mouse_button(0, 3, tt.x0, tt.y0, 1);
      }
      tt = null; park();
      return;
    }
    if (t.identifier !== tt.id) return;     // single-finger modes only track the primary
    clearTimeout(tt.timer);
    const [px, py] = xy(t);
    if (tt.mode === 'box') {
      x.openbw_mouse_button(0, 1, px, py, 1);           // left up completes the box
    } else if (tt.mode === 'pending' && tt.armed) {
      x.openbw_mouse_button(1, 3, tt.x0, tt.y0, 1);     // long-press, no drag → right click
      x.openbw_mouse_button(0, 3, tt.x0, tt.y0, 1);
    } else if (tt.mode === 'pending') {
      const now = performance.now();
      const dbl = now - lastTap.t < DBL_MS && d2(px, py, lastTap.x, lastTap.y) < MOVE2 * 4;
      const clicks = dbl ? 2 : 1;                        // clicks=2 → engine select-all-of-type
      x.openbw_mouse_move(px, py);
      x.openbw_mouse_button(1, 1, px, py, clicks);
      x.openbw_mouse_button(0, 1, px, py, clicks);
      lastTap = { t: now, x: px, y: py };
    }
    // 'pan' → nothing to emit on release
    tt = null;
    park();
  };
  canvas.addEventListener('touchend', endTouch, { passive: false });
  canvas.addEventListener('touchcancel', () => { if (tt) { clearTimeout(tt.timer); tt = null; park(); } },
                          { passive: false });
}

// ---------------------------------------------------------------------------
// Boot
// A headless bot replica: its own memory + WASI over the shared assets, silent (the
// rendered replica handles sound). Seats the full melee and attaches its bot. Used as the
// shadow sim for the second bot in a spectated bot-vs-bot — the Lockstep keeps it in sync.
async function makeBotReplica(assets, moduleName, slots, botSlot, w, h) {
  const DEV = BUILD === '__BUILD__' || location.hostname === 'localhost' || location.hostname === '127.0.0.1';
  let memory;
  const env = {
    js_file_size: (i) => assets[i].length,
    js_read_data: (i, dst, off, n) => { new Uint8Array(memory.buffer).set(assets[i].subarray(off, off + n), dst); },
    js_bot_log: (ptr, len) => { if (BOT_LOG) console.log('%c[bot ' + botSlot + ']', 'color:#c90', new TextDecoder().decode(new Uint8Array(memory.buffer, ptr, len))); },
    js_sound_load: () => -1, js_sound_play: () => {}, js_sound_is_playing: () => 0, js_sound_stop: () => {}, js_sound_set_volume: () => {},
  };
  const req = () => DEV ? fetch('./' + moduleName + '?t=' + Date.now(), { cache: 'no-store' }) : fetch('./' + moduleName + '?v=' + BUILD);
  let instance;
  try { ({ instance } = await WebAssembly.instantiateStreaming(req(), { wasi_snapshot_preview1: makeWasi(() => memory), env })); }
  catch { ({ instance } = await WebAssembly.instantiate(await (await req()).arrayBuffer(), { wasi_snapshot_preview1: makeWasi(() => memory), env })); }
  const x = instance.exports;
  memory = x.memory;
  x._initialize();
  const pairs = new Int32Array(slots.length * 2);
  slots.forEach((s, i) => { pairs[2 * i] = s.slot; pairs[2 * i + 1] = s.race; });
  const p = x.openbw_in_ptr(pairs.byteLength);
  new Uint8Array(memory.buffer, p, pairs.byteLength).set(new Uint8Array(pairs.buffer));
  x.openbw_init_mp(w, h, botSlot, slots.length);
  x.openbw_bot_attach(botSlot);
  return { x, memory };
}

// ---------------------------------------------------------------------------
// session: { slots: [{slot, race}], mySlot, link, delay }
// Single-player is one slot with no link and zero delay; 1v1 is two slots, a PeerLink,
// and a few frames of input delay. Both peers must pass an identical `slots` list.
// Bot-vs-bot spectate adds { spectate:true, bot:{slot,module}, bot2:{slot,module} }.
async function boot(session) {
  // Create the audio context now — inside the race-button click gesture — so the
  // browser's autoplay policy doesn't leave it suspended; resume on later input too.
  const audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  const resumeAudio = () => audioCtx.resume();
  addEventListener('pointerdown', resumeAudio);
  addEventListener('keydown', resumeAudio);

  // Master gain node: every sound routes through it, so muting is a single knob.
  const masterGain = audioCtx.createGain();
  masterGain.connect(audioCtx.destination);
  const muteBtn = $('mute');
  let muted = false;
  try { muted = localStorage.getItem('openbw-muted') === '1'; } catch {}
  const applyMute = () => {
    masterGain.gain.value = muted ? 0 : 1;
    muteBtn.textContent = muted ? '🔇' : '🔊';
    muteBtn.title = muted ? 'Unmute sound' : 'Mute sound';
  };
  $('topbar').style.display = 'flex';
  muteBtn.onclick = () => {
    muted = !muted;
    try { localStorage.setItem('openbw-muted', muted ? '1' : '0'); } catch {}
    applyMute();
    resumeAudio();
  };
  applyMute();

  // Controls popup toggled by the "?" button; dismissed by clicking elsewhere.
  const helpBtn = $('helpbtn'), helpPop = $('help');
  helpBtn.onclick = (e) => {
    e.stopPropagation();
    $('settings').style.display = 'none';
    helpPop.style.display = helpPop.style.display === 'block' ? 'none' : 'block';
  };
  document.addEventListener('click', (e) => {
    if (helpPop.style.display === 'block' && e.target !== helpBtn && !helpPop.contains(e.target)) {
      helpPop.style.display = 'none';
    }
  });

  const assets = await loadAssets(session.mapFile || MAP_LOCAL);

  let memory;
  const getMemory = () => memory;

  // Web Audio bridge for native_sound (web/wasm_backend.cpp): decode WAV bytes
  // to AudioBuffers keyed by id, then play/stop them per channel. pan is always
  // 0 from the engine, so we don't bother panning.
  const soundBuffers = [];   // id -> AudioBuffer | null while decoding
  const soundChannels = [];  // channel -> { source, gain, playing }
  const pendingPlay = {};    // id -> {channel, volume} requested before decode finished
  const gainFor = (v) => Math.min(1, Math.max(0, v / 128));
  const playBuffer = (buf, channel, volume) => {
    const prev = soundChannels[channel];
    if (prev && prev.source) { try { prev.source.stop(); } catch {} }
    const source = audioCtx.createBufferSource();
    source.buffer = buf;
    const gain = audioCtx.createGain();
    gain.gain.value = gainFor(volume);
    source.connect(gain).connect(masterGain);
    const rec = { source, gain, playing: true };
    source.onended = () => { if (soundChannels[channel] === rec) rec.playing = false; };
    soundChannels[channel] = rec;
    source.start();
  };
  const audio = {
    js_sound_load(ptr, size) {
      const id = soundBuffers.length;
      soundBuffers.push(null);
      const wav = new Uint8Array(memory.buffer, ptr, size).slice();   // copy out of wasm memory
      audioCtx.decodeAudioData(wav.buffer).then((b) => {
        soundBuffers[id] = b;
        // A play requested while still decoding would be silently dropped
        // (a one-shot sound like a selection ack) — fire it now that it's ready.
        const p = pendingPlay[id];
        if (p) { delete pendingPlay[id]; playBuffer(b, p.channel, p.volume); }
      }, () => {});
      return id;
    },
    js_sound_play(id, channel, volume) {
      if (id < 0) return;
      if (soundBuffers[id]) playBuffer(soundBuffers[id], channel, volume);
      else pendingPlay[id] = { channel, volume };   // decode still in flight
    },
    js_sound_is_playing: (channel) => (soundChannels[channel] && soundChannels[channel].playing ? 1 : 0),
    js_sound_stop(channel) {
      const c = soundChannels[channel];
      if (c && c.source) { try { c.source.stop(); } catch {} c.playing = false; }
    },
    js_sound_set_volume(channel, volume) {
      const c = soundChannels[channel];
      if (c) c.gain.gain.value = gainFor(volume);
    },
  };

  const env = {
    js_file_size: (index) => assets[index].length,
    js_read_data: (index, dst, offset, n) => {
      new Uint8Array(memory.buffer).set(assets[index].subarray(offset, offset + n), dst);
    },
    // A bot module (e.g. McRave) routes its Logger here; mirror it to the console in debug.
    js_bot_log: (ptr, len) => { if (BOT_LOG) console.log('%c[bot]', 'color:#f90', new TextDecoder().decode(new Uint8Array(memory.buffer, ptr, len))); },
    ...audio,
  };

  setMsg('Starting engine…');
  const imports = { wasi_snapshot_preview1: makeWasi(getMemory), env };
  // In dev, a unique URL + no-store defeats Chrome's aggressive HTTP/compiled-
  // wasm caching so rebuilds are picked up. In production we want the opposite:
  // let the browser cache and reuse the compiled module across visits.
  // Any build whose SHA wasn't stamped in by CI is a local/dev build — including one served
  // to a phone over a LAN IP. Treat it as dev so the wasm is fetched fresh (the ?v=__BUILD__
  // cache-buster is a constant locally, so a plain fetch would serve a stale, skewed wasm).
  const DEV = BUILD === '__BUILD__' || location.hostname === 'localhost' || location.hostname === '127.0.0.1';
  // BUILD is replaced by CI with the commit SHA (see .github/workflows/pages.yml), so a
  // new deploy fetches a fresh wasm URL instead of a stale cached one. Locally it stays
  // the '__BUILD__' placeholder and we cache-bust per load instead.
  // vs-Computer loads a bot-enabled module (sandbox + BWAPI server + the chosen bot);
  // each exports the same surface plus openbw_bot_*, so everything else is identical.
  const wasmName = session.bot ? session.bot.module : 'openbw.wasm';
  const wasmReq = () => DEV ? fetch('./' + wasmName + '?t=' + Date.now(), { cache: 'no-store' })
                            : fetch('./' + wasmName + '?v=' + BUILD);
  let instance;
  try {
    ({ instance } = await WebAssembly.instantiateStreaming(wasmReq(), imports));
  } catch (e) {
    // dev servers may not send application/wasm — fall back to plain instantiate
    const bytes = await (await wasmReq()).arrayBuffer();
    ({ instance } = await WebAssembly.instantiate(bytes, imports));
  }
  const x = instance.exports;
  memory = x.memory;

  const canvas = $('screen');
  const ctx = canvas.getContext('2d');
  const winSize = () => [Math.max(320, window.innerWidth | 0), Math.max(240, window.innerHeight | 0)];

  x._initialize();
  const { slots, mySlot, link = null, delay = 0 } = session;
  if (slots.length > 1) {
    // Hand the slot list to the wasm as int32 [slot, race] pairs via the scratch buffer.
    const pairs = new Int32Array(slots.length * 2);
    slots.forEach((s, i) => { pairs[2 * i] = s.slot; pairs[2 * i + 1] = s.race; });
    const p = x.openbw_in_ptr(pairs.byteLength);
    new Uint8Array(memory.buffer, p, pairs.byteLength).set(new Uint8Array(pairs.buffer));
    x.openbw_init_mp(...winSize(), mySlot, slots.length);
  } else {
    x.openbw_init(...winSize(), slots[0].race, slots[0].slot);
  }
  // Attach the BWAPI read-view so the bot can drive its slot (vs-Computer only).
  if (session.bot) x.openbw_bot_attach(session.bot.slot);
  // Bot-vs-bot spectate: reveal the whole map and stand up the second bot on a shadow
  // replica that the Lockstep keeps bit-identical. This one (x) is the rendered spectator
  // view — full UI, sound, pause, input — with the human as an onlooker (Lockstep bot2 path).
  let shadow = null, bot2 = null;
  if (session.spectate && session.bot2) {
    x.openbw_reveal_map();
    x.openbw_set_spectator(1);   // no unit voices; resource HUD follows the selected unit
    shadow = await makeBotReplica(assets, session.bot2.module, slots, session.bot2.slot, ...winSize());
    bot2 = { slot: session.bot2.slot };
  }
  wireInput(canvas, x);

  // Settings popup (⚙, top-left). Order/rally lines are off by default; the choice
  // persists. Wired here because it drives the wasm, which is now instantiated.
  const settingsBtn = $('settingsbtn'), settingsPop = $('settings'), optLines = $('opt-lines');
  let showLines = false;
  try { showLines = localStorage.getItem('openbw-order-lines') === '1'; } catch {}
  optLines.checked = showLines;
  x.openbw_set_order_lines(showLines ? 1 : 0);
  settingsBtn.onclick = (e) => {
    e.stopPropagation();
    helpPop.style.display = 'none';
    settingsPop.style.display = settingsPop.style.display === 'block' ? 'none' : 'block';
  };
  optLines.onchange = () => {
    try { localStorage.setItem('openbw-order-lines', optLines.checked ? '1' : '0'); } catch {}
    x.openbw_set_order_lines(optLines.checked ? 1 : 0);
  };
  // Alternative Esc key: click the button, press the key to bind (Esc itself clears).
  // Stored as KeyboardEvent.code; the key handler routes it as a full Esc press.
  const altEscBtn = $('opt-altesc');
  const altEscShow = () => {
    altEscBtn.textContent = altEsc ? altEsc.replace(/^(Key|Digit)/, '') : 'none';
  };
  altEscShow();
  altEscBtn.onclick = (e) => {
    e.stopPropagation();
    altEscBtn.textContent = 'press a key…';
    window.addEventListener('keydown', (ev) => {
      ev.preventDefault(); ev.stopPropagation();
      altEsc = ev.code === 'Escape' ? '' : ev.code;
      try { localStorage.setItem('openbw-alt-esc', altEsc); } catch {}
      altEscShow();
    }, { capture: true, once: true });
  };
  // Save the recorded game as a StarCraft .rep. Our command stream already is BW's action
  // format, so the wasm just serialises what it recorded — opens in BW and replay tools.
  const saveReplay = (btn) => {
    const ptr = x.openbw_save_replay(), len = x.openbw_replay_len();
    if (!ptr || !len) { if (btn) btn.textContent = 'Nothing recorded'; return; }
    const bytes = new Uint8Array(memory.buffer, ptr, len).slice();   // copy: the buffer is reused
    const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
    const a = document.createElement('a');
    const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-');
    a.href = url; a.download = `openbw-${stamp}.rep`; a.click();
    URL.revokeObjectURL(url);
    if (btn) { const t = btn.textContent; btn.textContent = `Saved (${(len / 1024) | 0} KB)`;
               setTimeout(() => { btn.textContent = t; }, 2500); }
  };

  // End-of-game score screen: outcome, per-player unit/building/resource/score totals from
  // the sim, and the save-replay button (a replay is only worth keeping once it's done).
  const RACE_NAME = ['Zerg', 'Terran', 'Protoss'];
  const PLAYER_COLORS = ['#f40404', '#0c48cc', '#2cb494', '#88409c', '#f88c14', '#703014', '#cce0d0', '#fcfc38'];
  let gameOverShown = false;
  const toMainMenu = () => location.replace(location.origin + location.pathname);
  const showGameOver = (title, won) => {
    if (gameOverShown) return;
    gameOverShown = true;
    // openbw_stats returns a C-string pointer, not a JS string — read it out of wasm memory.
    const ptr = x.openbw_stats();
    const rows = (ptr ? readCString(ptr) : '').split('\n');
    const secs = Math.round((+(rows.shift() || '0\t0').split('\t')[1] || 0) / 24);   // ~24 fps
    let html = '<tr><th>Player</th><th>Units</th><th>Buildings</th><th>Minerals</th><th>Gas</th><th>Score</th></tr>';
    for (const line of rows) {
      if (!line) continue;
      const [isMe, race, color, units, bldgs, min, gas, score] = line.split('\t').map(Number);
      const sw = `<span class="swatch" style="background:${PLAYER_COLORS[color] || '#888'}"></span>`;
      html += `<tr class="${isMe ? 'me' : ''}"><td>${sw}${RACE_NAME[race] || 'Player'}</td>` +
              `<td>${units}</td><td>${bldgs}</td><td>${min}</td><td>${gas}</td><td>${score}</td></tr>`;
    }
    html += `<tr><td colspan="6" style="color:#64748b;text-align:center;padding-top:12px">` +
            `Game length ${(secs/60)|0}:${String(secs%60).padStart(2,'0')}</td></tr>`;
    $('go-stats').innerHTML = html;
    const t = $('go-title');
    t.textContent = title;
    t.className = won === true ? 'win' : won === false ? 'lose' : '';
    $('gameover').classList.add('show');
    // The game's decided — you can't concede one you've already won or lost, so the
    // settings menu's "Resign" turns into "Back to main menu".
    resignArmed = false;
    resignBtn.textContent = 'Back to main menu';
    resignBtn.classList.replace('danger', 'backmenu');
  };
  $('go-replay').onclick = (e) => saveReplay(e.currentTarget);
  $('go-close').onclick = () => { x.openbw_reveal_map(); $('gameover').classList.remove('show'); };
  $('go-new').onclick = toMainMenu;

  // Debug (?debug in the URL): force a win/lose to exercise the game-over screen without
  // playing to elimination. Bypasses the competitive gate, so it works in single-player.
  let debugResourcesPending = false;
  if (/(\?|&)debug\b/.test(location.search)) {
    $('opt-debug').style.display = 'block';
    $('dbg-victory').onclick = () => { settingsPop.style.display = 'none'; x.openbw_debug_outcome(1); };
    $('dbg-defeat').onclick = () => { settingsPop.style.display = 'none'; x.openbw_debug_outcome(2); };
    $('dbg-resources').onclick = () => { settingsPop.style.display = 'none'; x.openbw_debug_resources(2000, 1000); };
    // Not while spectating: a cheat mutates only the rendered replica, desyncing the shadow.
    debugResourcesPending = !session.spectate;   // start rich; applied after the first step sets the melee start
  }

  // Chat (Enter to open). Messages go into the same log the sim uses for eliminations, via
  // openbw_post_message. In a 1v1 they're relayed to the peer over the control channel; in
  // single-player debug the input doubles as a cheat console.
  const isDebug = /(\?|&)debug\b/.test(location.search);
  const chatInput = $('chatinput');
  const postLog = (text) => {
    const bytes = new TextEncoder().encode(text);
    const p = x.openbw_in_ptr(bytes.length);
    new Uint8Array(memory.buffer, p, bytes.length).set(bytes);
    x.openbw_post_message(bytes.length);
  };
  const CHEATS = {
    'show me the money':     () => (x.openbw_debug_resources(10000, 10000), 'resources granted'),
    'black sheep wall':      () => (x.openbw_reveal_map(), 'map revealed'),
    'there is no cow level': () => (x.openbw_debug_outcome(1), 'victory'),
    'game over man':         () => (x.openbw_debug_outcome(2), 'defeat'),
  };
  const sendChat = (raw) => {
    const text = raw.trim();
    if (!text) return;
    if (!link && isDebug) {
      const cheat = CHEATS[text.toLowerCase()];
      if (cheat) { postLog('Cheat: ' + cheat()); return; }
    }
    if (link) link.sendControl({ t: 'chat', text });
    postLog('You: ' + text);
  };
  const closeChat = () => { chatInput.value = ''; chatInput.blur(); chatInput.style.display = 'none'; };
  window.addEventListener('keydown', (e) => {
    if (document.activeElement === chatInput) {
      if (e.key === 'Enter') { sendChat(chatInput.value); closeChat(); e.preventDefault(); }
      else if (e.key === 'Escape') { closeChat(); e.preventDefault(); }
      return;
    }
    if (e.key === 'Enter') { chatInput.style.display = 'block'; chatInput.focus(); e.preventDefault(); }
  });

  // Resign: one confirming click (no blocking dialog), then concede via the sim.
  const resignBtn = $('opt-resign');
  let resignArmed = false;
  resignBtn.onclick = () => {
    if (gameOverShown) { settingsPop.style.display = 'none'; toMainMenu(); return; }
    if (!resignArmed) { resignArmed = true; resignBtn.textContent = 'Confirm — resign?'; return; }
    resignArmed = false; resignBtn.textContent = 'Resign game';
    settingsPop.style.display = 'none';
    x.openbw_resign();
  };
  document.addEventListener('click', (e) => {
    if (settingsPop.style.display === 'block' && e.target !== settingsBtn && !settingsPop.contains(e.target))
      settingsPop.style.display = 'none';
    if (resignArmed && e.target !== resignBtn) { resignArmed = false; resignBtn.textContent = 'Resign game'; }
  });

  // Follow the window size (debounced). The frame loop below picks up the new
  // framebuffer dimensions automatically.
  let resizeTimer = 0;
  window.addEventListener('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => x.openbw_resize(...winSize()), 150);
  });

  $('overlay').style.display = 'none';

  // Fixed-timestep simulation on a timer, rendering on requestAnimationFrame.
  // A self-rescheduling timeout (rather than setInterval) advances one frame per fire
  // — so returning to a hidden tab resumes at normal speed instead of fast-forwarding a
  // backlog — while letting the interval change on the fly for the speed control. rAF is
  // paused while hidden, so we simply don't render then. Pausing just skips the step;
  // rendering and input keep running, so you can still look around and give orders.
  let paused = false, waiting = false, stepMs = 42;   // 42 ms ≈ 24 Hz "fastest" game speed
  try { stepMs = +localStorage.getItem('openbw-speed') || 42; } catch {}

  // Local input is serialised to BW command bytes rather than applied directly; the
  // lockstep queue drains them, schedules them, and applies every player's batch before
  // stepping. Single-player is just a one-player session with zero input delay, so solo
  // play runs the exact code path multiplayer will — versioned by net.js's BUILD query so
  // a deploy can't pair a fresh openbw.js with a stale net.js.
  const { Lockstep } = await import('./net.js?v=' + BUILD);
  const lockstep = new Lockstep({
    x, memory,
    slots: slots.map((s) => s.slot),
    localSlot: mySlot,
    delay,
    send: (msg) => { if (link && msg.t === 'turn') link.sendTurn(msg.f, msg.d); },
    bot: session.bot || null,
    shadow, bot2,   // set only when spectating a bot-vs-bot match
  });
  let dropped = null;   // set when the peer is gone
  let over = false;     // game finished (win / lose / disconnect) — stop stepping
  const dropBtn = $('dropbtn');
  if (link) {
    const peerSlot = slots.find((s) => s.slot !== mySlot).slot;
    link.frameOf = () => lockstep.frame;

    // End the multiplayer game cleanly: stop stepping, release the peer, and show a final
    // banner. Used for a disconnect, a host-authority kick, and a manual drop after a stall.
    const endMp = (banner) => {
      if (over) return;
      over = true;
      dropped = dropped || banner;
      lockstep.dropSlot(peerSlot);
      dropBtn.style.display = 'none';
      try { link.close(); } catch {}
      showGameOver(banner, /Victory/.test(banner));   // opponent left is a win; others neutral
    };
    // The host is the authority: a peer sending turns for illegal frames, or whose sim has
    // diverged, is dropped rather than allowed to stall or corrupt the game.
    const kick = (why) => {
      if (over) return;
      console.warn('[mp] dropping peer:', why);
      if (session.isHost) link.sendControl({ t: 'kick', why });
      endMp(`Opponent dropped — ${why}`);
    };
    // The channel closed or failed mid-game: in a 1v1, the opponent leaving is a win.
    link.onClose = () => { if (!over) endMp('Opponent left — Victory!'); };
    // Manual escape hatch shown after a long stall.
    dropBtn.onclick = () => endMp('Opponent left — Victory!');

    // setTurnHandler (not a plain assignment) so turns that arrived while we were still
    // loading get replayed instead of dropped — they are never retransmitted.
    link.setTurnHandler((frame, bytes) => {
      const r = lockstep.receiveTurn(peerSlot, frame, bytes);
      if (!r.ok && session.isHost) kick(r.why);
      else if (!r.ok) console.warn('[mp] bad turn from host:', r.why);
    });

    // Periodic desync probe: both peers hash their sim at the same frames and compare.
    link.onControl = (msg) => {
      if (msg.t === 'kick') { endMp(`Disconnected — ${msg.why || 'kicked'}`); return; }
      if (msg.t === 'chat') { postLog('Opponent: ' + String(msg.text || '').slice(0, 80)); return; }
      if (msg.t !== 'sum') return;
      const mine = mySums.get(msg.f);
      if (mine === undefined) { peerSums.set(msg.f, msg.h); return; }
      mySums.delete(msg.f);
      if (mine !== msg.h && session.isHost) kick(`desync at frame ${msg.f}`);
      else if (mine !== msg.h) endMp('Desynced — game ended');
    };
  }
  const mySums = new Map(), peerSums = new Map();
  const SUM_EVERY = 240;   // ~10 s at "fastest"
  {
    const m = MAPS.find((e) => e.file === (session.mapFile || MAP_LOCAL));
    const actual = x.openbw_start_locations();
    if (m && actual && m.starts !== actual)
      console.warn(`[map] ${m.name}: table says ${m.starts} start locations, map has ${actual}`);
    // Same cross-check for the lobby's size/tileset (from the openbw_map_* exports).
    if (m) {
      const w = x.openbw_map_w(), h = x.openbw_map_h(), ts = TILESETS[x.openbw_map_tileset()];
      if (m.w !== w || m.h !== h || m.tileset !== ts)
        console.warn(`[map] ${m.name}: table says ${m.w}×${m.h} ${m.tileset}, map is ${w}×${h} ${ts}`);
    }
  }
  if (DEV) window.__bw = { x, memory, lockstep, link };   // dev-only debugging handle

  // Pace the sim off wall-clock instead of one frame per timer tick, so a hiccup is made
  // up rather than permanently lost. Frames are never skipped — lockstep requires every
  // peer to simulate every frame in order — we only choose when to run them, so
  // determinism is unaffected. The debt is capped so a long stall resumes promptly
  // instead of fast-forwarding for ages.
  //
  // NOTE: this does NOT rescue a backgrounded tab. Browsers throttle hidden-tab timers to
  // ~1 Hz, and lockstep deliberately never lets a peer run more than `delay` frames ahead
  // of the other, so a hidden peer can only burst ~delay frames per wake-up (~5 fps at
  // delay 4) and drags its opponent down with it. That bound is the correctness property,
  // not a bug; raising `delay` trades input lag for it. In real play both windows are
  // visible and this never applies — but two tabs in one window will crawl.
  const MAX_CATCHUP = 32, MAX_DEBT_MS = 2000;
  // After our own verdict we keep feeding the lockstep for a grace window, so a finished
  // player doesn't starve the peer's turns before it reaches its own verdict. Without this
  // the loser goes `over`, stops sending turns, and the winner stalls on "Waiting for
  // opponent" a few frames short of detecting the win.
  const OVER_GRACE_MS = 5000;
  let stepTimer, stepClock = performance.now(), lastProgress = performance.now(), overSince = 0;
  const stepLoop = () => {
    if (!paused && (!over || (link && performance.now() - overSince < OVER_GRACE_MS))) {
      const now = performance.now();
      if (now - stepClock > MAX_DEBT_MS) stepClock = now - MAX_DEBT_MS;
      let budget = Math.min(Math.floor((now - stepClock) / stepMs), MAX_CATCHUP);
      let advanced = 0;
      while (budget-- > 0 && lockstep.tick()) advanced++;
      stepClock += advanced * stepMs;      // only consume the time we actually simulated
      const stepped = advanced > 0;
      if (stepped) lastProgress = now;
      if (stepped && debugResourcesPending) { x.openbw_debug_resources(2000, 1000); debugResourcesPending = false; }
      // Victory/defeat comes straight out of the shared sim, so both peers reach the
      // same verdict on the same frame without exchanging anything. Stop stepping once
      // it's decided — rendering and panning keep working so you can look around.
      if (stepped && !over) {
        const o = x.openbw_outcome();
        if (o) { over = true; overSince = now; showGameOver(o === 1 ? 'Victory!' : 'Defeat', o === 1); }
      }
      // Hysteresis: only claim we're waiting after a sustained gap with no progress.
      // Reacting to a single late turn made the banner flicker. After a long stall, offer
      // to drop the opponent (their turns never resumed).
      const stall = (link && !over) ? (now - lastProgress) : 0;
      waiting = stall > 1500;
      dropBtn.style.display = stall > 8000 ? 'block' : 'none';
      updateBanner();
      // Desync probe: both peers hash the same frames and compare. Sampled after the step,
      // so both label it with the same post-step frame number.
      if (stepped && link && !dropped && !over && lockstep.frame % SUM_EVERY === 0) {
        const h = x.openbw_checksum() >>> 0;
        const peer = peerSums.get(lockstep.frame);
        if (peer === undefined) mySums.set(lockstep.frame, h);
        else {
          peerSums.delete(lockstep.frame);
          if (peer !== h) {
            console.warn('[mp] desync', lockstep.frame, h, peer);
            if (session.isHost) link.sendControl({ t: 'kick', why: `desync at frame ${lockstep.frame}` });
            over = true; overSince = now; dropped = 'desync'; setBanner('Desync detected — game ended', 0);
          }
        }
        link.sendControl({ t: 'sum', f: lockstep.frame, h });
      }
    }
    // Tick faster than the frame interval so catch-up stays responsive; the loop itself
    // decides how many frames are actually due.
    stepTimer = setTimeout(stepLoop, Math.max(8, stepMs >> 1));
  };
  stepTimer = setTimeout(stepLoop, stepMs);

  // Pause/resume, shown like a video player (⏸ while running, ▶ while paused). The
  // ︎ text selector keeps the glyphs monochrome. A big "Paused" banner also shows.
  const pauseBtn = $('pausebtn'), pausedEl = $('paused');
  const pausedSpan = pausedEl.querySelector('span');
  // No pause in multiplayer: lockstep means a local pause just stops feeding turns and
  // stalls the opponent, who sees "Waiting for opponent" until you resume. Hiding it
  // reflows the flex row, so the remaining icons shift left rather than leaving a gap.
  pauseBtn.style.display = link ? 'none' : '';
  // One centre banner serves every state: an explicit message (disconnect/desync) wins,
  // otherwise paused, otherwise blocked waiting on a peer's turn.
  let bannerText = null, bannerOverride = '', bannerTimer = 0;
  const updateBanner = () => {
    const t = bannerOverride || (paused ? 'Paused' : waiting ? 'Waiting for opponent…' : '');
    if (t === bannerText) return;
    bannerText = t;
    pausedSpan.textContent = t;
    pausedEl.style.display = t ? 'flex' : 'none';
  };
  // ms = 0 keeps it up permanently (a fatal state); otherwise it clears itself.
  const setBanner = (t, ms) => {
    bannerOverride = t;
    clearTimeout(bannerTimer);
    if (ms) bannerTimer = setTimeout(() => { bannerOverride = ''; updateBanner(); }, ms);
    updateBanner();
  };
  const applyPause = () => {
    pauseBtn.classList.toggle('is-paused', paused);
    pauseBtn.title = paused ? 'Resume' : 'Pause';
    updateBanner();
  };
  pauseBtn.onclick = () => {
    paused = !paused;
    // On resume, forget the wall-clock debt from the pause so the step loop doesn't
    // fast-forward to "catch up".
    if (!paused) stepClock = performance.now();
    applyPause();
    x.openbw_set_paused?.(paused ? 1 : 0);   // freeze game input in the sim too
  };
  applyPause();

  // Game speed lives in the settings popup; the value is the ms per sim frame.
  const optSpeed = $('opt-speed');
  optSpeed.value = String(stepMs);
  optSpeed.onchange = () => {
    stepMs = +optSpeed.value || 42;
    try { localStorage.setItem('openbw-speed', String(stepMs)); } catch {}
  };

  // Command card overlay: openbw_card() returns "title\nKEY\tLabel\n…".
  const cardEl = $('card');
  const dec = new TextDecoder();
  let lastCard = '', lastBtnSig = null;
  const readCString = (ptr) => {
    const mem = new Uint8Array(memory.buffer);
    let end = ptr; while (mem[end]) end++;
    return dec.decode(mem.subarray(ptr, end));
  };
  // Render a unit's sprite icon (by unit id) to a cached data URL. The engine hands
  // back RGBA pixels in wasm memory valid only until the next openbw_icon call, so we
  // copy them into a canvas immediately and memoize the resulting data URL by id.
  const iconCache = new Map();
  const iconCanvas = document.createElement('canvas');
  const ictx = iconCanvas.getContext('2d');
  const iconURL = (id) => {
    if (iconCache.has(id)) return iconCache.get(id);
    const ptr = x.openbw_icon(id), w = x.openbw_icon_w(), h = x.openbw_icon_h();
    let url = '';
    if (ptr && w && h) {
      iconCanvas.width = w; iconCanvas.height = h;
      const img = ictx.createImageData(w, h);
      img.data.set(new Uint8Array(memory.buffer, ptr, w * h * 4));
      ictx.putImageData(img, 0, 0);
      url = iconCanvas.toDataURL();
    }
    iconCache.set(id, url);
    return url;
  };

  const updateCard = () => {
    const ptr = x.openbw_card();
    const text = ptr ? readCString(ptr) : '';
    if (text === lastCard) return;
    lastCard = text;
    if (!text) { cardEl.innerHTML = ''; lastBtnSig = null; return; }
    const lines = text.split('\n');
    // Title line: name \t "HP x/y" \t <second stat, e.g. Shields / Minerals — may be empty>.
    // It ticks every frame whenever the unit's HP/energy changes — e.g. a building whose HP
    // counts up during construction. Rebuilding the whole card each tick replaces the button
    // nodes, and a click needs its mousedown and mouseup on the *same* node — so a mid-build
    // rebuild between press and release silently ate the click (why "the Cancel button does
    // nothing" while its hotkey worked). Build the title and the button grid separately and
    // only touch the button DOM when the button set itself changes.
    const t0 = lines[0].split('\t');
    let titleHtml = `<span class="title">${t0[0]}</span>`;
    if (t0[1]) titleHtml += `<span class="stat">${t0[1]}</span>`;
    if (t0[2]) titleHtml += `<span class="stat">${t0[2]}</span>`;
    if (t0[3]) titleHtml += `<span class="fx">${t0[3]}</span>`;   // status effects (blind, parasite, …)
    const btnSig = lines.slice(1).join('\n');
    if (btnSig === lastBtnSig) {
      // Buttons unchanged: refresh only the title spans, leaving the .cmd nodes (and any
      // in-flight click on them) intact.
      const firstCmd = cardEl.querySelector('.cmd');
      while (cardEl.firstChild && cardEl.firstChild !== firstCmd) cardEl.removeChild(cardEl.firstChild);
      if (firstCmd) firstCmd.insertAdjacentHTML('beforebegin', titleHtml);
      else cardEl.innerHTML = titleHtml;
      return;
    }
    lastBtnSig = btnSig;
    const ri = getResIcons();   // mineral / gas sprite icons for the cost popup
    let html = titleHtml;
    for (let i = 1; i < lines.length; i++) {
      const f = lines[i].split('\t');   // KEY, Label, enabled(1/0), iconUnitId(-1 = none)
      if (f.length < 2) continue;
      const off = f[2] === '0' ? ' off' : '';
      const id = f.length > 3 ? parseInt(f[3], 10) : -1;
      const url = id >= 0 ? iconURL(id) : '';
      const ico = url ? `<img class="ico" src="${url}">` : '';
      // Highlight the hotkey letter inside the label (first match — usually the first
      // letter, else an inner letter like "Medi[c]"). Fall back to a badge if absent.
      // Key 0x1b is Escape (Cancel, as in the original) — show it as an "Esc" badge and
      // encode data-key as a numeric entity so the char survives in the attribute.
      const esc27 = f[0].charCodeAt(0) === 27;
      const dataKey = esc27 ? '&#27;' : f[0];
      const key = f[0].toLowerCase(), label = f[1];
      const k = esc27 ? -1 : label.toLowerCase().indexOf(key);
      const labelHtml = esc27
        ? `<b>Esc</b> ${label}`
        : k >= 0
          ? `${label.slice(0, k)}<b>${label[k]}</b>${label.slice(k + 1)}`
          : `<b>${f[0].toUpperCase()}</b> ${label}`;
      // Resource cost: tooltip on hover, and mark red when unaffordable.
      const minC = f.length > 4 ? +f[4] : 0, gasC = f.length > 5 ? +f[5] : 0;
      const poor = f.length > 6 && f[6] === '0' ? ' poor' : '';
      // Immediate cost popup shown on hover (native title tooltips are too slow to notice),
      // fronting each amount with the same delivery sprite used in the resource HUD.
      const mIco = ri[0] ? `<img class="cico" src="${ri[0]}">` : '';
      const gIco = ri[1] ? `<img class="cico" src="${ri[1]}">` : '';
      const esc = (s) => s.replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
      // Popup on hover: what it does (for upgrades/research/abilities) above the cost; a grayed
      // build instead shows the prerequisite it's waiting on.
      const desc = f.length > 8 ? f[8] : '';
      const costInner = (minC || gasC)
        ? `<b class="m">${mIco}${minC}</b>${gasC ? ` <b class="g">${gIco}${gasC}</b>` : ''}`
        : '';
      const cost = (desc || costInner)
        ? `<span class="cost">${desc ? `<div class="d">${esc(desc)}</div>` : ''}${costInner ? `<div>${costInner}</div>` : ''}</span>`
        : '';
      const req = f.length > 7 ? f[7] : '';
      const popup = off && req ? `<span class="req">Requires ${esc(req)}</span>` : cost;
      // Light up the button matching the unit's current order (Move while moving, etc.).
      const active = f.length > 9 && f[9] === '1' ? ' active' : '';
      // Wrap the label in one span so the flex `gap` on .cmd spaces only the icon from
      // the label — not the highlighted <b> from the rest of the word ("B uild").
      html += `<span class="cmd${off}${poor}${active}" data-key="${dataKey}">${ico}<span class="lbl">${labelHtml}</span>${popup}</span>`;
    }
    cardEl.innerHTML = html;
  };

  // Clicking a card button does the same as pressing its key (shift-click queues).
  // Grayed buttons still dispatch — the sim decides (a spell blocked on energy raises the
  // "Not enough energy" toast; other unavailable commands are simply consumed).
  cardEl.addEventListener('click', (e) => {
    const cmd = e.target.closest('.cmd');
    if (!cmd) return;
    const key = cmd.dataset.key;
    if (!key) return;
    x.openbw_key(e.shiftKey ? 1 : 0, 0, 225);   // reflect shift for queuing
    const sym = key.charCodeAt(0);
    x.openbw_key(1, sym, 0); x.openbw_key(0, sym, 0);   // frame loop processes it
  });

  // Group wireframes: one 32x32 tile per selected unit when 2+ are selected (BW's
  // multi-selection row). openbw_wires() lists "id\tname\thp\tmax\tstate" per unit; the
  // canvases are rebuilt only when ids or damage states change (a rebuild between mousedown
  // and mouseup eats the click — same lesson as the command card), while the HP tooltips
  // update in place every tick.
  const wiresEl = $('wires');
  let lastWireSig = '';
  const updateWires = () => {
    const t = readCString(x.openbw_wires());
    // 64 = single selection's big wireframe, 32 = group tiles. Feature-detected: three wasm
    // modules share this JS, and an older module mid-dev must degrade, not kill the loop.
    const box = x.openbw_wire_box ? x.openbw_wire_box() : 32;
    const rows = t ? t.trim().split('\n').map((l) => l.split('\t')) : [];
    const sig = box + '|' + rows.map((r) => r[0] + ':' + r[4]).join(',');
    if (sig !== lastWireSig) {
      lastWireSig = sig;
      wiresEl.innerHTML = '';
      rows.forEach((r, i) => {
        const ptr = x.openbw_wire_rgba(i);
        if (!ptr) return;
        const c = document.createElement('canvas');
        c.width = box; c.height = box;
        const g = c.getContext('2d');
        const img = g.createImageData(box, box);
        img.data.set(new Uint8Array(memory.buffer, ptr, box * box * 4));
        g.putImageData(img, 0, 0);
        c.style.width = c.style.height = (box === 64 ? 96 : 38) + 'px';
        c.dataset.id = r[0];
        c.onclick = () => x.openbw_debug_select(+c.dataset.id, 0);   // click = select just this unit
        wiresEl.appendChild(c);
      });
    }
    // tooltips track HP without touching the canvas nodes
    const kids = wiresEl.children;
    for (let i = 0; i < rows.length && i < kids.length; i++)
      kids[i].title = `${rows[i][1]} — HP ${rows[i][2]}/${rows[i][3]}`;
  };

  // Producer status: "<progress%>\t<name(training)>\t<queued>…". The progress ticks
  // every frame, so only rebuild the chips when the item list changes and just move the
  // bar otherwise — rebuilding the whole thing each frame flickers and eats clicks.
  const statusEl = $('status');
  let lastStatusKey = null, statusBar = null, lastBarPct = -1;
  const BAR_W = 61;   // internal width; quantised the same way the wasm renderer expects
  const updateStatus = () => {
    const ptr = x.openbw_status();
    const text = ptr ? readCString(ptr) : '';
    const p = text ? text.split('\t') : [];
    const key = p.slice(1).join('\t');   // the chips (names), independent of progress
    if (key !== lastStatusKey) {
      lastStatusKey = key;
      statusBar = null; lastBarPct = -1;
      if (!text) { statusEl.innerHTML = ''; return; }
      // Progress bar is a <canvas> rendered from the same health-bar palette the engine
      // draws under units, so it matches the original exactly.
      let html = `<canvas class="bar"></canvas>`;
      // p[1..] are the queued units in order; chip i maps to build-queue slot i-1.
      for (let i = 1; i < p.length; i++)
        html += `<span class="q${i === 1 ? ' cur' : ''}" data-slot="${i - 1}" title="Click to cancel">${p[i]}</span>`;
      statusEl.innerHTML = html;
      statusBar = statusEl.querySelector('.bar');
    }
    if (statusBar) {
      const pct = Math.max(0, Math.min(100, parseInt(p[0], 10) || 0));
      if (pct === lastBarPct) return;
      lastBarPct = pct;
      const bp = x.openbw_progress_bar(pct, BAR_W);
      const w = x.openbw_progress_bar_w(), h = x.openbw_progress_bar_h();
      if (!bp || !w || !h) return;
      if (statusBar.width !== w || statusBar.height !== h) { statusBar.width = w; statusBar.height = h; }
      const bctx = statusBar.getContext('2d');
      const img = bctx.createImageData(w, h);
      img.data.set(new Uint8Array(memory.buffer, bp, w * h * 4));
      bctx.putImageData(img, 0, 0);
    }
  };
  // Cancel a queued item (resources refunded). mousedown, not click: acts on the press
  // and stays reliable even if the chip list happens to rebuild mid-interaction.
  statusEl.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    const q = e.target.closest('.q');
    if (q && q.dataset.slot !== undefined) x.openbw_cancel(+q.dataset.slot);
  });

  // Resource HUD: "minerals\tgas\tsupply_used\tsupply_max".
  const resEl = $('resources');
  let lastRes = '';
  // The mineral/gas icons are the sprites a worker carries when delivering — a blue
  // mineral chunk and a green gas container (per race). They never change, so build
  // the data URLs once (after the game is up, so the palette is loaded).
  const resIconURL = (which) => {
    const ptr = x.openbw_res_icon(which), w = x.openbw_icon_w(), h = x.openbw_icon_h();
    if (!ptr || !w || !h) return '';
    iconCanvas.width = w; iconCanvas.height = h;
    const img = ictx.createImageData(w, h);
    img.data.set(new Uint8Array(memory.buffer, ptr, w * h * 4));
    ictx.putImageData(img, 0, 0);
    return iconCanvas.toDataURL();
  };
  // [0]=mineral chunk, [1]=gas container, [2]=supply provider. Cached once the palette
  // is loaded; shared by the HUD and the command-card cost popup.
  let resIcons = null;
  const getResIcons = () => {
    if (resIcons) return resIcons;
    const r = [resIconURL(0), resIconURL(1), resIconURL(2)];
    if (r[0] && r[1] && r[2]) resIcons = r;
    return r;
  };
  const updateResources = () => {
    const ptr = x.openbw_resources();
    const text = ptr ? readCString(ptr) : '';
    if (text === lastRes) return;
    lastRes = text;
    if (!text) { resEl.innerHTML = ''; return; }
    const ri = getResIcons();
    const [min, gas, used, max] = text.split('\t');
    const cap = (+used >= +max) ? ' cap' : '';
    const ico = (u, cls) => u ? `<img class="rico" src="${u}">` : `<span class="dot ${cls}"></span>`;
    resEl.innerHTML =
      `<span class="r min">${ico(ri[0], 'min')}${min}</span>` +
      `<span class="r gas">${ico(ri[1], 'gas')}${gas}</span>` +
      `<span class="r sup">${ico(ri[2], 'sup')}<span class="${cap}">${used}/${max}</span></span>`;
  };

  // Blocked-command toast: openbw_error() returns "seq\tmessage"; flash the message
  // whenever seq changes (e.g. "Not enough minerals" when a build is unaffordable).
  const toastEl = $('toast');
  let lastErrSeq = -1, toastTimer = 0;
  const updateError = () => {
    const ptr = x.openbw_error();
    const text = ptr ? readCString(ptr) : '';
    if (!text) return;
    const tab = text.indexOf('\t');
    const seq = +text.slice(0, tab), msg = text.slice(tab + 1);
    if (seq === lastErrSeq) return;
    lastErrSeq = seq;
    if (!msg) return;
    toastEl.textContent = msg;
    toastEl.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => toastEl.classList.remove('show'), 2200);
  };

  // System message log (eliminations…): openbw_messages() is a newline-separated list the
  // sim ages out, so re-render only when it changes. textContent keeps it injection-safe.
  const msgEl = $('messages');
  let lastMsgText = null;
  const updateMessages = () => {
    const ptr = x.openbw_messages();
    const text = ptr ? readCString(ptr) : '';
    if (text === lastMsgText) return;
    lastMsgText = text;
    msgEl.textContent = '';
    if (!text) return;
    for (const line of text.split('\n')) {
      const d = document.createElement('div');
      d.className = 'msg';
      d.textContent = line;
      msgEl.appendChild(d);
    }
  };

  let iw = 0, ih = 0, image = null;
  // Swap the canvas cursor to match state: while edge-scrolling, a directional resize
  // arrow pointing the way we're panning; otherwise the pointer mode (0 normal,
  // 1 targeting, 2 placing).
  const EDGE_CURSOR = ['', 'n-resize', 'ne-resize', 'e-resize', 'se-resize',
                       's-resize', 'sw-resize', 'w-resize', 'nw-resize'];
  // 0 normal, 1 targeting, 2 placing, 3 hover-unit, 4 targeting-over-unit.
  const MODE_CURSOR = ['default', 'crosshair', 'cell', 'pointer', 'crosshair'];
  let lastCursor = '';
  const updateCursor = () => {
    const edge = x.openbw_edge();
    const cur = edge ? EDGE_CURSOR[edge] : (MODE_CURSOR[x.openbw_cursor()] || 'default');
    if (cur === lastCursor) return;
    lastCursor = cur;
    canvas.style.cursor = cur;
  };

  function frame() {
    x.openbw_render(performance.now());
    const w = x.openbw_framebuffer_width(), h = x.openbw_framebuffer_height(), ptr = x.openbw_framebuffer();
    if (w && h && ptr) {
      if (w !== iw || h !== ih) { iw = w; ih = h; canvas.width = w; canvas.height = h; image = ctx.createImageData(w, h); }
      image.data.set(new Uint8Array(memory.buffer, ptr, w * h * 4));
      ctx.putImageData(image, 0, 0);
    }
    updateCard();
    updateWires();
    updateStatus();
    updateResources();
    updateError();
    updateMessages();
    updateCursor();
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

// Bot vs Bot (spectate). Two bot MODULES are two independent BWAPI singletons, so we run
// two deterministic sim replicas: each frame we tick each bot on its OWN replica, then
// apply BOTH bots' commands to BOTH replicas and step them. Identical sim + identical
// command stream => bit-identical (asserted via openbw_checksum). Replica A is rendered
// with fog off (spectator); replica B just produces its bot's orders. Human input is
// camera-only — a stray command to one replica alone would desync them.
// Map choice applies to both single-player and multiplayer, so it lives outside the
// multiplayer panel.
const mapSelect = $('map-select');

// The title doubles as a way back to a clean URL. An invite link leaves #i=... in the
// address bar, so a joiner who reloads re-enters the join flow with the *old* code —
// clicking here drops the fragment and query and forces a genuine reload (dropping a
// fragment alone is a same-document navigation, which would keep the stale page state).
{
  const bare = location.origin + location.pathname;
  const home = $('home');
  home.href = bare;
  home.addEventListener('click', (e) => {
    e.preventDefault();
    if (location.href !== bare) location.replace(bare);
    location.reload();
  });
}

// Solo lobby: pick your race and, for each other start location on the map, an
// opponent — ZZZKBot (Zerg) or None. (One bot for now; the rest are empty.) The
// slot rows rebuild when the map changes, since the map decides how many there are.
$('controls').style.display = 'block';
setMsg('Set up your game.');
// Warn on mobile: no touch controls yet, and the first run pulls ~90 MB.
if (/Mobi|Android|iPhone|iPad|iPod/i.test(navigator.userAgent) ||
    (window.matchMedia && matchMedia('(pointer: coarse)').matches))
  $('mobilewarn').style.display = 'block';

const YOU_RACES = [[1, 'Terran'], [0, 'Zerg'], [2, 'Protoss'], [-1, 'Random']];   // value = race_t, -1 = random
// Computer opponents: dropdown value -> which wasm module + which race it plays.
// race_t: Zerg 0, Terran 1, Protoss 2. McRave is multi-race (one module per race).
const OPPONENTS = {
  zzzk:       { label: 'ZZZKBot (Zerg)',    module: 'openbw-bot.wasm',    race: 0 },
  'mcrave-p': { label: 'McRave (Protoss)',  module: 'openbw-mcrave.wasm', race: 2 },
  'mcrave-t': { label: 'McRave (Terran)',   module: 'openbw-mcrave.wasm', race: 1 },
  'mcrave-z': { label: 'McRave (Zerg)',     module: 'openbw-mcrave.wasm', race: 0 },
};
const DEFAULT_OPP = 'mcrave-p';
const slotsBox = $('slots');
const curMap = () => MAPS.find((m) => m.file === mapSelect.value) || MAPS[0];

function renderMapInfo() {
  const m = curMap();
  $('map-info').textContent = `${m.w}×${m.h} · ${m.tileset} · ${m.starts} players`;
}
function updateHint() {
  if ($('spectate').checked) { $('lobby-hint').textContent = 'Watch two bots play.'; return; }
  const anyBot = slotsBox._opps.some((s) => s.value !== 'none');
  $('lobby-hint').textContent = anyBot ? '' : 'No opponent — sandbox mode (no win or loss).';
}
// (Re)build the You + opponent rows for the current map's start-location count. When
// spectating, slot 0 ("You") becomes a second bot picker.
function buildSlots() {
  const m = curMap();
  const spectate = $('spectate').checked;
  slotsBox.innerHTML = '';
  const you = document.createElement('select');   // slot 0: You, or Bot 1 when spectating
  if (spectate) { for (const [val, o] of Object.entries(OPPONENTS)) you.add(new Option(o.label, val)); you.value = 'zzzk'; }
  else { for (const [v, name] of YOU_RACES) you.add(new Option(name, v)); you.value = '1'; }
  const youLbl = document.createElement('span'); youLbl.className = 'lbl you'; youLbl.textContent = spectate ? 'Bot 1' : 'You';
  slotsBox.append(youLbl, you);
  const opps = [];
  for (let s = 1; s < m.starts; s++) {
    const lbl = document.createElement('span'); lbl.className = 'lbl';
    lbl.textContent = spectate && s === 1 ? 'Bot 2' : 'Slot ' + (s + 1);
    const sel = document.createElement('select'); sel.dataset.slot = String(s);
    for (const [val, o] of Object.entries(OPPONENTS)) sel.add(new Option(o.label, val));
    sel.add(new Option('None', 'none'));
    sel.value = s === 1 ? DEFAULT_OPP : 'none';   // default to a 1v1 vs the bot
    // One bot for now: picking any bot here clears any other bot slot back to None.
    sel.onchange = () => {
      if (sel.value !== 'none') for (const o of opps) if (o !== sel && o.value !== 'none') o.value = 'none';
      updateHint();
    };
    slotsBox.append(lbl, sel);
    opps.push(sel);
  }
  slotsBox._you = you; slotsBox._opps = opps;
  updateHint();
}
// addEventListener (not .onchange) so the multiplayer panel's own map handler coexists.
mapSelect.addEventListener('change', () => { renderMapInfo(); buildSlots(); });
$('spectate').addEventListener('change', buildSlots);   // toggling swaps "You" for a 2nd bot
renderMapInfo(); buildSlots();

$('start-game').onclick = () => {
  $('controls').style.display = 'none';
  const file = mapSelect.value;
  const starts = (MAPS.find((m) => m.file === file) || {}).starts || 2;
  // Bot vs Bot: two bots on two random start locations, watched as a spectator.
  if ($('spectate').checked) {
    const b1 = OPPONENTS[slotsBox._you.value];
    const s2 = slotsBox._opps[0] && slotsBox._opps[0].value !== 'none' ? slotsBox._opps[0].value : DEFAULT_OPP;
    const b2 = OPPONENTS[s2];
    const picks = pickStartSlots(2, starts);
    const slots = [{ slot: picks[0], race: b1.race }, { slot: picks[1], race: b2.race }];
    // A normal game (full UI) rendered as a spectator: bot 1 drives slot 0 here, bot 2 runs
    // on a shadow replica boot() spins up. See the spectate handling in boot().
    const session = { slots, mySlot: picks[0], mapFile: file, spectate: true,
                      bot:  { slot: picks[0], module: b1.module },
                      bot2: { slot: picks[1], module: b2.module } };
    boot(session).catch((e) => { setMsg('Error: ' + e.message); console.error(e); });
    return;
  }
  // Randomise which start location each player gets (slot = start location = colour).
  const botSel = slotsBox._opps.find((s) => s.value !== 'none');
  const picks = pickStartSlots(botSel ? 2 : 1, starts);
  // resolveRace turns the Random sentinel (-1) into a concrete race.
  const slots = [{ slot: picks[0], race: resolveRace(+slotsBox._you.value) }];
  const session = { slots, mySlot: picks[0], mapFile: file };
  if (botSel) {
    const o = OPPONENTS[botSel.value];
    slots.push({ slot: picks[1], race: o.race });
    session.bot = { slot: picks[1], module: o.module };
  }
  // No bot -> a single occupied slot -> the sim runs non-competitive (no auto win/lose).
  boot(session).catch((e) => { setMsg('Error: ' + e.message); console.error(e); });
};

// Load any .scx/.scm from disk (read locally, never uploaded): You on slot 0, a computer
// opponent on slot 1 if one is selected above.
$('map-file').addEventListener('change', async (e) => {
  const f = e.target.files && e.target.files[0];
  if (!f) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer());
    const id = 'custom:' + f.name;
    mapCache.set(id, bytes);   // fetchMap returns this straight from cache — no network
    const slots = [{ slot: 0, race: resolveRace(+slotsBox._you.value) }];
    const session = { slots, mySlot: 0, mapFile: id };
    const botSel = slotsBox._opps.find((s) => s.value !== 'none');
    if (botSel) { const o = OPPONENTS[botSel.value]; slots.push({ slot: 1, race: o.race }); session.bot = { slot: 1, module: o.module }; }
    $('controls').style.display = 'none';
    boot(session).catch((err) => { setMsg('Error: ' + err.message); console.error(err); });
  } catch (err) { setMsg('Error reading map: ' + err.message); console.error(err); }
});

// --- multiplayer: 1v1 over a copy-paste WebRTC link (direct-cable style) ------------
// The host makes an offer; the joiner opens the link (which auto-fills it) and returns a
// response the host pastes back. The joiner never pastes anything when they arrive via a
// link, so the UI only ever shows them a copy-out box. No signalling server is involved.
//
// After the channel opens both sit in a lobby until the host starts a 5s countdown; races
// lock for the final 2s. The host is authoritative for the final slot list, and the map is
// hash-verified so peers can't silently run different terrain.
const MP_DELAY = 4;        // input delay in frames (~170 ms at "fastest")
const COUNTDOWN = 5, LOCK_AT = 2;

const mp = {
  setup: $('mp-setup'),
  host: $('mp-host'), join: $('mp-join'),
  relay: $('mp-relay'),
  relayUrl: $('mp-relay-url'), relayHost: $('mp-relay-host'), relayJoin: $('mp-relay-join'),
  stepInvite: $('mp-step-invite'), inviteHint: $('mp-invite-hint'),
  inInvite: $('mp-in-invite'), accept: $('mp-accept'),
  stepShare: $('mp-step-share'), shareHint: $('mp-share-hint'), out: $('mp-out'), copy: $('mp-copy'),
  stepAnswer: $('mp-step-answer'), inAnswer: $('mp-in-answer'), connect: $('mp-connect'),
  lobby: $('mp-lobby'), start: $('mp-start'), status: $('mp-status'),
  s: [$('mp-s0'), $('mp-s1')], n: [$('mp-n0'), $('mp-n1')], r: [$('mp-r0'), $('mp-r1')],
  giMap: $('mp-gi-map'), giSpeed: $('mp-gi-speed'),
};

// Random is a lobby choice, not something the engine understands. The host resolves it
// the instant the countdown ends and puts the concrete races in the authoritative `start`
// message, so both peers init from an identical list and neither rolls its own dice.
const RACES = [{ v: 1, n: 'Terran' }, { v: 0, n: 'Zerg' }, { v: 2, n: 'Protoss' }, { v: -1, n: 'Random' }];
const fillRaces = (sel) => { for (const r of RACES) sel.add(new Option(r.n, r.v)); };
const resolveRace = (r) => (r === -1 ? [0, 1, 2][(Math.random() * 3) | 0] : r);
const mpSay = (t, cls = '') => { mp.status.textContent = t; mp.status.className = cls; };
const mpShow = (el, on) => { el.style.display = on ? 'block' : 'none'; };

// Only list maps the site actually serves. Production ships just the one CC-licensed
// map; a local checkout has the others too. Probing keeps the dropdown honest in both
// (and picks up any licensed map added later) instead of advertising a 404. MAPS[0] is
// the always-shipped default, so it's listed immediately and the rest fill in as their
// HEAD requests confirm — the UI is never empty and never offers a map that isn't there.
mapSelect.add(new Option(MAPS[0].name, MAPS[0].file));
for (const sel of mp.r) fillRaces(sel);
Promise.all(MAPS.slice(1).map(async (m) => {
  try { return (await fetch(m.file, { method: 'HEAD' })).ok ? m : null; } catch { return null; }
})).then((found) => {
  for (const m of found) if (m) mapSelect.add(new Option(m.name, m.file));
});

// Race is picked in the lobby, not before hosting — the handshake only carries the
// starting value, and either side can change it until the countdown locks.
const START_RACE = 1;   // Terran
let mpLink = null, mpRole = null, mpMap = MAPS[0].file, mpPeerRace = START_RACE, mpLocked = false;

// Choosing a role is one-way until it fails or the game starts. Without this, clicking
// Host and Join in any order left both flows' panels on screen at once.
function mpHideSteps() {
  for (const el of [mp.stepInvite, mp.stepShare, mp.stepAnswer, mp.lobby]) mpShow(el, false);
}
// Mark a handshake step finished: a green tick, faded, and its controls made inert so it
// reads as history rather than something still to do.
function mpStepDone(el) {
  el.classList.add('done');
  for (const c of el.querySelectorAll('input, button')) c.disabled = true;
}
function mpResetSteps() {
  for (const el of [mp.stepInvite, mp.stepShare, mp.stepAnswer]) {
    el.classList.remove('done');
    for (const c of el.querySelectorAll('input, button')) c.disabled = false;
  }
}
function mpSetRole(role) {
  if (mpRole) return false;                 // already committed
  mpRole = role;
  mp.host.disabled = mp.join.disabled = true;
  mpHideSteps();
  return true;
}
function mpResetRole() {                    // let the user try again after a failure
  mp.inInvite.readOnly = false;
  mp.accept.style.display = '';
  mp.inviteHint.textContent = '1. Paste the invite code your friend sent:';
  mpRole = null;
  if (mpLink) { try { mpLink.close(); } catch {} mpLink = null; }
  mp.host.disabled = mp.join.disabled = false;
  mapSelect.disabled = false;
  mpResetSteps();
  mpHideSteps();
}

const mpNet = () => import('./net.js?v=' + BUILD);

const mpBoot = (slots, mySlot) => {
  $('controls').style.display = 'none';
  boot({ slots, mySlot, link: mpLink, delay: MP_DELAY, isHost: mpRole === 'host', mapFile: mpMap })
    .catch((err) => { setMsg('Error: ' + err.message); console.error(err); });
};

const mpRaceName = (r) => (RACES.find((x) => x.v === r) || RACES[0]).n;
const mpMine = () => (mpRole === 'host' ? 0 : 1);
const mpTheirs = () => (mpRole === 'host' ? 1 : 0);
const mpSlots = () => [{ slot: 0, race: +mp.r[0].value }, { slot: 1, race: +mp.r[1].value }];

function mpEnterLobby() {
  mpHideSteps();
  mpShow(mp.lobby, true);
  mp.giMap.textContent = (MAPS.find((m) => m.file === mpMap) || {}).name || '—';
  mp.start.style.display = mpRole === 'host' ? '' : 'none';
  // Your row is editable; your opponent's mirrors what they picked.
  mp.r[mpTheirs()].value = String(mpPeerRace);
  mp.r[mpMine()].disabled = false;
  mp.r[mpTheirs()].disabled = true;
  mp.n[mpMine()].textContent = 'You';
  mp.n[mpTheirs()].textContent = 'Opponent';
  mp.s[mpMine()].classList.add('you');
  for (const el of mp.s) el.classList.remove('open');
  mpUpdateLobby();
  mpSay('Connected.', 'ok');
}
function mpUpdateLobby() {
  mp.giSpeed.textContent = 'Fastest';
}

// Races may change freely in the lobby; both sides mirror each other until the lock.
const mpSendRace = () => {
  mpUpdateLobby();
  if (mpLink) mpLink.sendControl({ t: 'race', race: +mp.r[mpMine()].value });
};
mp.r[0].onchange = mp.r[1].onchange = mpSendRace;
mapSelect.onchange = () => { mpMap = mapSelect.value; };

function mpCountdown(n) {
  if (n <= LOCK_AT && !mpLocked) {
    mpLocked = true;
    mapSelect.disabled = mp.r[0].disabled = mp.r[1].disabled = true;
  }
  if (n <= 0) {
    if (mpRole !== 'host') return;          // the joiner starts on the host's `start`
    // Randomise who starts where. Slot drives both start location and colour, so this
    // stops the host always being top-left in the same colour every game.
    const races = mpSlots().map((s) => resolveRace(s.race));   // [host, joiner]
    const starts = (MAPS.find((m) => m.file === mpMap) || {}).starts || 2;
    const [hostSlot, joinSlot] = pickStartSlots(2, starts);
    const slots = [{ slot: hostSlot, race: races[0] }, { slot: joinSlot, race: races[1] }];
    mpLink.sendControl({ t: 'start', slots, you: joinSlot });
    mpBoot(slots, hostSlot);
    return;
  }
  mpSay(`Starting in ${n}…`, 'ok');
  if (mpRole === 'host') {
    mpLink.sendControl({ t: 'tick', n });
    setTimeout(() => mpCountdown(n - 1), 1000);
  }
}

// Lobby control messages, shared by the copy-paste (PeerLink) and relay (RelayLink)
// transports since both are just a channel underneath.
function mpOnControl(msg) {
  if (msg.t === 'race') {
    mpPeerRace = msg.race;
    if (mpRole) mp.r[mpTheirs()].value = String(msg.race);
    mpUpdateLobby();
    return;
  }
  if (msg.t === 'tick') { mpCountdown(msg.n); return; }
  if (msg.t === 'start') { mpBoot(msg.slots, msg.you); return; }
}

function mpNewLink(net, onOpen) {
  return new net.PeerLink({
    onOpen,
    onControl: mpOnControl,
    onClose: (why) => mpSay('Connection ' + why + '.', 'err'),
  });
}

// Relay transport: both players connect to the same openbw_bridge address (the host runs
// it and shares its LAN URL — the "direct cable" model, no copy-paste). The host announces
// the game with a `hello` once the peer joins; the joiner verifies the map and enters the
// lobby. Everything after connection is identical to the copy-paste path.
async function mpRelayHost(url) {
  if (!mpSetRole('host')) return;
  try {
    const net = await mpNet();
    const hash = await mapHash(mpMap);
    mpLink = new net.RelayLink({
      onOpen: () => {   // peer joined the relay
        mpLink.sendControl({ t: 'hello', v: net.PROTOCOL, map: mpMap, hash, race: +mp.r[mpMine()].value });
        mpEnterLobby();
        mpLink.sendControl({ t: 'race', race: +mp.r[mpMine()].value });
      },
      onControl: mpOnControl,
      onClose: (why) => mpSay('Connection ' + why + '.', 'err'),
    });
    await mpLink.connect(url);
    mpSay('Relay ready — waiting for a friend to join.');
  } catch (e) { mpSay(e.message, 'err'); mpResetRole(); }
}

async function mpRelayJoin(url) {
  if (!mpSetRole('join')) return;
  try {
    const net = await mpNet();
    mpLink = new net.RelayLink({
      onControl: (msg) => {
        if (msg.t === 'hello') {
          if (msg.v !== net.PROTOCOL) { mpSay(`Version mismatch (you v${net.PROTOCOL}, host v${msg.v}) — reload.`, 'err'); return; }
          mpMap = msg.map; mpPeerRace = msg.race;
          mapHash(mpMap).then((h) => {
            if (h !== msg.hash) { mpSay('Map differs from the host.', 'err'); mpLink.close(); return; }
            mpEnterLobby();
            mpLink.sendControl({ t: 'race', race: +mp.r[mpMine()].value });
          }).catch(() => mpSay('That map isn’t available here.', 'err'));
          return;
        }
        mpOnControl(msg);
      },
      onClose: (why) => mpSay('Connection ' + why + '.', 'err'),
    });
    await mpLink.connect(url);
    mpSay('Connected to relay — waiting for the host.');
  } catch (e) { mpSay(e.message, 'err'); mpResetRole(); }
}

mp.host.onclick = async () => {
  if (!mpSetRole('host')) return;         // guard *before* awaiting, or a second click slips in
  try {
    const net = await mpNet();
    mpMap = mapSelect.value;
    mpSay('Preparing invite…');
    const hash = await mapHash(mpMap);
    mpLink = mpNewLink(net, () => { mpEnterLobby(); mpLink.sendControl({ t: 'race', race: +mp.r[mpMine()].value }); });
    const code = await mpLink.createOffer({ race: START_RACE, map: mpMap, hash });
    const url = location.origin + location.pathname + '#i=' + code;
    const fits = code.length <= net.MAX_URL_CODE;
    mpShow(mp.stepShare, true);
    mp.shareHint.textContent = fits ? '1. Send this link to your friend:'
                                    : '1. Send this invite code to your friend:';
    mp.out.value = fits ? url : code;
    mpShow(mp.stepAnswer, true);
    mpSay('Waiting for their response…');
  } catch (e) { mpResetRole(); mpSay(e.message, 'err'); console.error(e); }
};

// Host: consume the joiner's response.
mp.connect.onclick = async () => {
  try {
    const info = await mpLink.acceptAnswer(mp.inAnswer.value);
    mpPeerRace = info.race ?? 1;
    mpStepDone(mp.stepShare);           // link was sent (they answered) and response accepted
    mpStepDone(mp.stepAnswer);
    mpSay('Response accepted — connecting…');
  } catch (e) { mpSay(e.message, 'err'); console.error(e); }
};

// Joiner: accept an invite (from the link, or pasted if the host sent a bare code).
async function mpAcceptInvite(code) {
  if (mpRole === 'host') return;          // can't join your own game
  if (!mpRole) mpSetRole('join');
  const net = await mpNet();
  const at = code.indexOf('#i=');
  if (at >= 0) code = code.slice(at + 3);
  if (!code.trim()) { mpSay('Paste the invite code first.', 'err'); return; }
  mpSay('Checking the map…');
  mpLink = mpNewLink(net, () => { mpEnterLobby(); mpLink.sendControl({ t: 'race', race: +mp.r[mpMine()].value }); });
  const { info, answer } = await mpLink.acceptOffer(code, { race: START_RACE });
  // Same terrain, verified by content — not by filename.
  mpMap = info.map || MAP_LOCAL;
  mapSelect.value = mpMap;
  mapSelect.disabled = true;              // the host picks the map
  const mine = await mapHash(mpMap).catch(() => null);
  if (!mine || mine !== info.hash) {
    mpSay(`Map mismatch — the host is playing a map this build doesn't have byte-for-byte. ` +
          `You both need the same version.`, 'err');
    mpResetRole();
    return;
  }
  mpPeerRace = info.race ?? 1;
  // Keep the invite on screen rather than replacing it, so both halves of the exchange
  // stay visible: what you received (now read-only) and what you send back.
  mp.inInvite.value = code;
  mp.inInvite.readOnly = true;
  mp.accept.style.display = 'none';
  mp.inviteHint.textContent = '1. Invite received from your friend:';
  mpShow(mp.stepInvite, true);
  mpStepDone(mp.stepInvite);            // nothing left to do here; step 2 is the live one
  mpShow(mp.stepShare, true);
  mp.shareHint.textContent = '2. Send this response back to the host:';
  mp.out.value = answer;
  mpSay('Waiting for the host…');
}

mp.join.onclick = () => {
  if (!mpSetRole('join')) return;
  mapSelect.disabled = true;      // the host chooses the map; yours is set from the invite
  mpShow(mp.stepInvite, true);
  mpSay('');
};

// Relay (openbw_bridge): experimental, built toward retail-interop that isn't practical yet,
// so it's hidden behind a `?relay` feature flag. Copy-paste multiplayer above still works.
if (/(\?|&)relay\b/.test(location.search)) {
  mp.relay.style.display = 'block';
  // default to the address the page was served from (so a joiner who opened the host's
  // LAN URL already has it right; the host on localhost gets ws://localhost:8100).
  mp.relayUrl.value = `ws://${location.hostname || 'localhost'}:8100`;
  mp.relayHost.onclick = () => { const u = mp.relayUrl.value.trim(); if (u) mpRelayHost(u); };
  mp.relayJoin.onclick = () => { const u = mp.relayUrl.value.trim(); if (u) { mapSelect.disabled = true; mpRelayJoin(u); } };
}
mp.accept.onclick = () => mpAcceptInvite(mp.inInvite.value).catch((e) => {
  mpResetRole(); mpSay(e.message, 'err'); console.error(e);
});
mp.start.onclick = () => { mp.start.disabled = true; mpCountdown(COUNTDOWN); };

mp.copy.onclick = async () => {
  try { await navigator.clipboard.writeText(mp.out.value); mpSay('Copied to clipboard.', 'ok'); }
  catch { mp.out.select(); mpSay('Press Cmd/Ctrl+C to copy.'); }
};

// Arriving from an invite link: the joiner pastes nothing — accept it straight away and
// show only the response they need to send back.
if (location.hash.startsWith('#i=')) {
  mpShow(mp.setup, false);
  mapSelect.disabled = true;      // arriving from an invite: the host already chose
  mpAcceptInvite(location.hash.slice(3)).catch((e) => { mpResetRole(); mpSay(e.message, 'err'); console.error(e); });
}
