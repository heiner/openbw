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
const MAP_LOCAL = './maps/Weave_v1.scx';
const MAP_REMOTE = '';

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
async function loadAssets() {
  const db = await idbOpen();
  const out = [];
  for (let i = 0; i < MPQS.length; i++) {
    const { key, local, url, size } = MPQS[i];
    let buf = await idbGet(db, key);
    if (buf) {
      setMsg(`Loading ${key} from cache…`);
    } else {
      // Prefer a same-origin local mirror (dev convenience); otherwise fetch
      // from the Internet Archive. Production hosts no game files, so the
      // local probe 404s and the archive.org path is used.
      let mirror = null;
      try { const r = await fetch(local); if (r.ok) mirror = await r.arrayBuffer(); } catch {}
      if (mirror) {
        setMsg(`Loading ${key} from local mirror…`);
        buf = mirror;
      } else {
        setMsg(`Downloading ${key} (${(size / 1e6) | 0} MB) from the Internet Archive…`);
        buf = await fetchWithProgress(url, size, setBar);
      }
      setMsg(`Caching ${key}…`);
      await idbPut(db, key, buf);
    }
    out.push(new Uint8Array(buf));
    setBar((i + 1) / (MPQS.length + 1));
  }
  setMsg('Loading map…');
  let mapBuf = null;
  try { const r = await fetch(MAP_LOCAL); if (r.ok) mapBuf = await r.arrayBuffer(); } catch {}
  if (!mapBuf && MAP_REMOTE) mapBuf = await (await fetch(MAP_REMOTE)).arrayBuffer();
  if (!mapBuf) throw new Error('No map available — set MAP_REMOTE to a hosted .scx URL.');
  out.push(new Uint8Array(mapBuf));
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
  };
}

// ---------------------------------------------------------------------------
// Input mapping (browser -> SDL-style codes the engine expects)
// ---------------------------------------------------------------------------
const SCANCODE = {
  Escape: 41,
  ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82,
  ControlLeft: 224, ShiftLeft: 225, ControlRight: 228, ShiftRight: 229,
};

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
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
async function boot(race) {
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
  muteBtn.style.display = 'block';
  muteBtn.onclick = () => {
    muted = !muted;
    try { localStorage.setItem('openbw-muted', muted ? '1' : '0'); } catch {}
    applyMute();
    resumeAudio();
  };
  applyMute();

  // Controls popup toggled by the "?" button; dismissed by clicking elsewhere.
  const helpBtn = $('helpbtn'), helpPop = $('help');
  helpBtn.style.display = 'block';
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

  const assets = await loadAssets();

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
    ...audio,
  };

  setMsg('Starting engine…');
  const imports = { wasi_snapshot_preview1: makeWasi(getMemory), env };
  // In dev, a unique URL + no-store defeats Chrome's aggressive HTTP/compiled-
  // wasm caching so rebuilds are picked up. In production we want the opposite:
  // let the browser cache and reuse the compiled module across visits.
  const DEV = location.hostname === 'localhost' || location.hostname === '127.0.0.1';
  // BUILD is replaced by CI with the commit SHA (see .github/workflows/pages.yml), so a
  // new deploy fetches a fresh wasm URL instead of a stale cached one. Locally it stays
  // the '__BUILD__' placeholder and we cache-bust per load instead.
  const wasmReq = () => DEV ? fetch('./openbw.wasm?t=' + Date.now(), { cache: 'no-store' })
                            : fetch('./openbw.wasm?v=' + BUILD);
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
  x.openbw_init(...winSize(), race, 0);
  wireInput(canvas, x);

  // Settings popup (⚙, top-left). Order/rally lines are off by default; the choice
  // persists. Wired here because it drives the wasm, which is now instantiated.
  const settingsBtn = $('settingsbtn'), settingsPop = $('settings'), optLines = $('opt-lines');
  settingsBtn.style.display = 'block';
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
  document.addEventListener('click', (e) => {
    if (settingsPop.style.display === 'block' && e.target !== settingsBtn && !settingsPop.contains(e.target))
      settingsPop.style.display = 'none';
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
  let paused = false, stepMs = 42;   // 42 ms ≈ 24 Hz "fastest" game speed
  try { stepMs = +localStorage.getItem('openbw-speed') || 42; } catch {}

  // Local input is serialised to BW command bytes rather than applied directly; the
  // lockstep queue drains them, schedules them, and applies every player's batch before
  // stepping. Single-player is just a one-player session with zero input delay, so solo
  // play runs the exact code path multiplayer will — versioned by net.js's BUILD query so
  // a deploy can't pair a fresh openbw.js with a stale net.js.
  const MY_SLOT = 0;
  const { Lockstep } = await import('./net.js?v=' + BUILD);
  const lockstep = new Lockstep({ x, memory, slots: [MY_SLOT], localSlot: MY_SLOT, delay: 0 });

  let stepTimer;
  const stepLoop = () => {
    if (!paused) lockstep.tick();
    stepTimer = setTimeout(stepLoop, stepMs);
  };
  stepTimer = setTimeout(stepLoop, stepMs);

  // Pause/resume, shown like a video player (⏸ while running, ▶ while paused). The
  // ︎ text selector keeps the glyphs monochrome. A big "Paused" banner also shows.
  const pauseBtn = $('pausebtn'), pausedEl = $('paused');
  pauseBtn.style.display = 'block';
  const applyPause = () => {
    pauseBtn.textContent = paused ? '▶︎' : '⏸︎';
    pauseBtn.title = paused ? 'Resume' : 'Pause';
    pausedEl.style.display = paused ? 'flex' : 'none';
  };
  pauseBtn.onclick = () => { paused = !paused; applyPause(); };
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
  let lastCard = '';
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
    if (!text) { cardEl.innerHTML = ''; return; }
    const lines = text.split('\n');
    // Title line: name \t "HP x/y" \t <second stat, e.g. Shields / Minerals — may be empty>.
    const t0 = lines[0].split('\t');
    let html = `<span class="title">${t0[0]}</span>`;
    if (t0[1]) html += `<span class="stat">${t0[1]}</span>`;
    if (t0[2]) html += `<span class="stat">${t0[2]}</span>`;
    const ri = getResIcons();   // mineral / gas sprite icons for the cost popup
    for (let i = 1; i < lines.length; i++) {
      const f = lines[i].split('\t');   // KEY, Label, enabled(1/0), iconUnitId(-1 = none)
      if (f.length < 2) continue;
      const off = f[2] === '0' ? ' off' : '';
      const id = f.length > 3 ? parseInt(f[3], 10) : -1;
      const url = id >= 0 ? iconURL(id) : '';
      const ico = url ? `<img class="ico" src="${url}">` : '';
      // Highlight the hotkey letter inside the label (first match — usually the first
      // letter, else an inner letter like "Medi[c]"). Fall back to a badge if absent.
      const key = f[0].toLowerCase(), label = f[1];
      const k = label.toLowerCase().indexOf(key);
      const labelHtml = k >= 0
        ? `${label.slice(0, k)}<b>${label[k]}</b>${label.slice(k + 1)}`
        : `<b>${f[0].toUpperCase()}</b> ${label}`;
      // Resource cost: tooltip on hover, and mark red when unaffordable.
      const minC = f.length > 4 ? +f[4] : 0, gasC = f.length > 5 ? +f[5] : 0;
      const poor = f.length > 6 && f[6] === '0' ? ' poor' : '';
      // Immediate cost popup shown on hover (native title tooltips are too slow to notice),
      // fronting each amount with the same delivery sprite used in the resource HUD.
      const mIco = ri[0] ? `<img class="cico" src="${ri[0]}">` : '';
      const gIco = ri[1] ? `<img class="cico" src="${ri[1]}">` : '';
      const cost = (minC || gasC)
        ? `<span class="cost"><b class="m">${mIco}${minC}</b>${gasC ? ` <b class="g">${gIco}${gasC}</b>` : ''}</span>`
        : '';
      // Wrap the label in one span so the flex `gap` on .cmd spaces only the icon from
      // the label — not the highlighted <b> from the rest of the word ("B uild").
      html += `<span class="cmd${off}${poor}" data-key="${f[0]}">${ico}<span class="lbl">${labelHtml}</span>${cost}</span>`;
    }
    cardEl.innerHTML = html;
  };

  // Clicking a card button does the same as pressing its key (shift-click queues).
  cardEl.addEventListener('click', (e) => {
    const cmd = e.target.closest('.cmd');
    if (!cmd || cmd.classList.contains('off')) return;
    const key = cmd.dataset.key;
    if (!key) return;
    x.openbw_key(e.shiftKey ? 1 : 0, 0, 225);   // reflect shift for queuing
    const sym = key.charCodeAt(0);
    x.openbw_key(1, sym, 0); x.openbw_key(0, sym, 0);   // frame loop processes it
  });

  // Producer status: "<progress%>\t<name(training)>\t<queued>…". The progress ticks
  // every frame, so only rebuild the chips when the item list changes and just move the
  // bar otherwise — rebuilding the whole thing each frame flickers and eats clicks.
  const statusEl = $('status');
  let lastStatusKey = null, statusBar = null;
  const updateStatus = () => {
    const ptr = x.openbw_status();
    const text = ptr ? readCString(ptr) : '';
    const p = text ? text.split('\t') : [];
    const key = p.slice(1).join('\t');   // the chips (names), independent of progress
    if (key !== lastStatusKey) {
      lastStatusKey = key;
      statusBar = null;
      if (!text) { statusEl.innerHTML = ''; return; }
      let html = `<span class="bar"><i></i></span>`;
      // p[1..] are the queued units in order; chip i maps to build-queue slot i-1.
      for (let i = 1; i < p.length; i++)
        html += `<span class="q${i === 1 ? ' cur' : ''}" data-slot="${i - 1}" title="Click to cancel">${p[i]}</span>`;
      statusEl.innerHTML = html;
      statusBar = statusEl.querySelector('.bar > i');
    }
    if (statusBar) statusBar.style.width = (Math.max(0, Math.min(100, parseInt(p[0], 10) || 0))) + '%';
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
    x.openbw_render();
    const w = x.openbw_framebuffer_width(), h = x.openbw_framebuffer_height(), ptr = x.openbw_framebuffer();
    if (w && h && ptr) {
      if (w !== iw || h !== ih) { iw = w; ih = h; canvas.width = w; canvas.height = h; image = ctx.createImageData(w, h); }
      image.data.set(new Uint8Array(memory.buffer, ptr, w * h * 4));
      ctx.putImageData(image, 0, 0);
    }
    updateCard();
    updateStatus();
    updateResources();
    updateError();
    updateCursor();
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

// Let the user pick a race (also satisfies the "user gesture" some browsers want).
$('controls').style.display = 'block';
setMsg('Choose a race to begin.');
// Warn on mobile: no touch controls yet, and the first run pulls ~90 MB.
if (/Mobi|Android|iPhone|iPad|iPod/i.test(navigator.userAgent) ||
    (window.matchMedia && matchMedia('(pointer: coarse)').matches))
  $('mobilewarn').style.display = 'block';
for (const b of document.querySelectorAll('#controls button')) {
  b.addEventListener('click', () => {
    $('controls').style.display = 'none';
    boot(+b.dataset.race).catch((err) => { setMsg('Error: ' + err.message); console.error(err); });
  }, { once: true });
}
