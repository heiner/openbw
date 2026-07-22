// Headless verification of the browser wasm code path (web/openbw.wasm) under
// Node's WASI. Provides the env.js_read_data / js_file_size file bridge backed
// by the local MPQs + map, runs init + a few ticks, and dumps the rendered
// framebuffer to a PPM — proving wasm_backend.cpp renders correctly.
//
//   node web/run-wasm-node.mjs out.ppm
import { WASI } from 'node:wasi';
import { readFileSync, writeFileSync } from 'node:fs';

const out = process.argv[2] || 'wasm-shot.ppm';

// Archive index mapping must match wasm_main.cpp's js_file_reader.
const files = [
  readFileSync('web/data/STARDAT.MPQ'),
  readFileSync('web/data/BROODAT.MPQ'),
  readFileSync('web/data/patch_rt.mpq'),
  readFileSync('web/maps/Benzene.scx'),
];

const wasi = new WASI({ version: 'preview1', args: ['openbw'], preopens: {} });

let memory;
const env = {
  js_file_size: (index) => files[index].length,
  js_read_data: (index, dst, offset, n) => {
    const mem = new Uint8Array(memory.buffer);
    mem.set(files[index].subarray(offset, offset + n), dst);
  },
  // no audio in headless Node — stubs so instantiation doesn't fail
  js_sound_load: () => -1, js_sound_play: () => {}, js_sound_is_playing: () => 0,
  js_sound_stop: () => {}, js_sound_set_volume: () => {},
};

const module = await WebAssembly.compile(readFileSync('web/openbw.wasm'));
const instance = await WebAssembly.instantiate(module, {
  ...wasi.getImportObject(),
  env,
});
memory = instance.exports.memory;
wasi.initialize(instance);   // reactor: runs ctors + libc init

const x = instance.exports;
x.openbw_init(1280, 800, 1 /*terran*/, 0 /*slot*/);
for (let i = 0; i < 4; i++) x.openbw_step();
x.openbw_render();

const w = x.openbw_framebuffer_width();
const h = x.openbw_framebuffer_height();
const ptr = x.openbw_framebuffer();
const fb = new Uint8Array(memory.buffer, ptr, w * h * 4);

// RGBA -> PPM (P6)
const header = Buffer.from(`P6\n${w} ${h}\n255\n`, 'ascii');
const body = Buffer.alloc(w * h * 3);
for (let i = 0, j = 0; i < w * h; i++) {
  body[j++] = fb[i * 4 + 0];
  body[j++] = fb[i * 4 + 1];
  body[j++] = fb[i * 4 + 2];
}
writeFileSync(out, Buffer.concat([header, body]));
console.log(`wrote ${out} (${w}x${h})`);
