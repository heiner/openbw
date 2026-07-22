// web/openbw.js — browser host for the OpenBW sandbox wasm module.
//
// Responsibilities:
//   * fetch the 3 game MPQs from the Internet Archive on first run and cache
//     them in IndexedDB (so no copyrighted bytes are ever hosted by the site);
//   * provide a minimal WASI shim + the env file bridge the wasm imports;
//   * step the sim on a fixed timer and render on requestAnimationFrame,
//     blitting the framebuffer to a <canvas>; forward input to the wasm exports.

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
const MAP_URL = './maps/Benzene.scx';

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
  const map = new Uint8Array(await (await fetch(MAP_URL)).arrayBuffer());
  out.push(map);
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

  canvas.addEventListener('mousemove', (e) => { const [px, py] = xy(e); x.openbw_mouse_move(px, py); });
  canvas.addEventListener('mousedown', (e) => {
    const [px, py] = xy(e); x.openbw_mouse_button(1, sdlButton(e.button), px, py, e.detail || 1);
  });
  window.addEventListener('mouseup', (e) => {
    const [px, py] = xy(e); x.openbw_mouse_button(0, sdlButton(e.button), px, py, e.detail || 1);
  });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  const key = (down) => (e) => {
    const sc = SCANCODE[e.code] || 0;
    const sym = e.key.length === 1 ? e.key.toLowerCase().charCodeAt(0) : 0;
    if (sc || sym) { x.openbw_key(down ? 1 : 0, sym, sc); e.preventDefault(); }
  };
  window.addEventListener('keydown', key(true));
  window.addEventListener('keyup', key(false));
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
async function boot(race) {
  const assets = await loadAssets();

  let memory;
  const getMemory = () => memory;
  const env = {
    js_file_size: (index) => assets[index].length,
    js_read_data: (index, dst, offset, n) => {
      new Uint8Array(memory.buffer).set(assets[index].subarray(offset, offset + n), dst);
    },
  };

  setMsg('Starting engine…');
  const imports = { wasi_snapshot_preview1: makeWasi(getMemory), env };
  // In dev, a unique URL + no-store defeats Chrome's aggressive HTTP/compiled-
  // wasm caching so rebuilds are picked up. In production we want the opposite:
  // let the browser cache and reuse the compiled module across visits.
  const DEV = location.hostname === 'localhost' || location.hostname === '127.0.0.1';
  const wasmReq = () => DEV ? fetch('./openbw.wasm?t=' + Date.now(), { cache: 'no-store' })
                            : fetch('./openbw.wasm');
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

  // Follow the window size (debounced). The frame loop below picks up the new
  // framebuffer dimensions automatically.
  let resizeTimer = 0;
  window.addEventListener('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => x.openbw_resize(...winSize()), 150);
  });

  $('overlay').style.display = 'none';

  // Fixed-timestep simulation on a timer, rendering on requestAnimationFrame.
  // The timer keeps firing (throttled) while the tab is hidden and advances one
  // frame per call, so returning to the tab resumes at normal speed instead of
  // fast-forwarding a backlog. rAF is paused while hidden, so we simply don't
  // render then.
  setInterval(() => x.openbw_step(), 42);   // ~24 Hz "fastest" game speed

  let iw = 0, ih = 0, image = null;
  function frame() {
    x.openbw_render();
    const w = x.openbw_framebuffer_width(), h = x.openbw_framebuffer_height(), ptr = x.openbw_framebuffer();
    if (w && h && ptr) {
      if (w !== iw || h !== ih) { iw = w; ih = h; canvas.width = w; canvas.height = h; image = ctx.createImageData(w, h); }
      image.data.set(new Uint8Array(memory.buffer, ptr, w * h * 4));
      ctx.putImageData(image, 0, 0);
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

// Let the user pick a race (also satisfies the "user gesture" some browsers want).
$('controls').style.display = 'block';
setMsg('Choose a race to begin.');
for (const b of document.querySelectorAll('#controls button')) {
  b.addEventListener('click', () => {
    $('controls').style.display = 'none';
    boot(+b.dataset.race).catch((err) => { setMsg('Error: ' + err.message); console.error(err); });
  }, { once: true });
}
