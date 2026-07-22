#!/usr/bin/env python3
"""
scdat.py - fetch StarCraft 1.16.1 MPQs from archive.org and extract arr\\*.dat files.

Zero dependencies (pure Python 3.8+). Includes a complete MPQ v1 reader:
Storm hash/decryption, sector handling, and decompression for PKWARE DCL
"implode" (ported from zlib's contrib/blast.c by Mark Adler), zlib, and bzip2.

Usage:
  python3 scdat.py fetch                      # patch_rt.mpq (637 KB - has the 5 core dats)
  python3 scdat.py fetch broodat stardat      # the bigger archives too
  python3 scdat.py extract patch_rt.mpq BROODAT.MPQ STARDAT.MPQ -o out/
  python3 scdat.py probe patch_rt.mpq         # which known files exist in an archive

Sources: Internet Archive item 'sc-classic-installer_202311', served per-member
out of 'StarCraft Portable.zip' via the view_archive endpoint (no Range support
there, but patch_rt is tiny). Sizes/CRC32s below come from the zip's central
directory and are verified after download.
"""
import argparse, bz2, os, struct, sys, urllib.request, zlib

ITEM = 'sc-classic-installer_202311'
ZIP  = 'StarCraft%20Portable.zip'
DIR  = 'Starcraft%20Brood%20War'
BASE = f'https://archive.org/download/{ITEM}/{ZIP}/{DIR}%2F'

MEMBERS = {  # name -> (url-tail, size, crc32)
    'patch_rt.mpq':  ('patch_rt.mpq',  636958,    0xbd6e74e2),
    'broodat':       ('BROODAT.MPQ',   23519727,  0xd97eb359),
    'stardat':       ('STARDAT.MPQ',   66184491,  0x1cd201b1),
    'broodwar-cd':   ('BroodWar.mpq',  571847688, 0x712f7ef1),
    'starcraft-cd':  ('StarCraft.mpq', 601389682, 0xc2898267),
}
ALIASES = {'patch_rt': 'patch_rt.mpq', 'patch': 'patch_rt.mpq',
           'broodat.mpq': 'broodat', 'stardat.mpq': 'stardat'}

KNOWN_FILES = [
    'arr\\units.dat', 'arr\\weapons.dat', 'arr\\upgrades.dat',
    'arr\\techdata.dat', 'arr\\flingy.dat', 'arr\\sprites.dat',
    'arr\\images.dat', 'arr\\orders.dat', 'arr\\sfxdata.dat',
    'arr\\portdata.dat', 'arr\\mapdata.dat',
    'rez\\stat_txt.tbl', 'scripts\\iscript.bin', 'scripts\\aiscript.bin',
    '(listfile)',
]

# --------------------------------------------------------------------------
# PKWARE Data Compression Library "explode", ported from zlib contrib/blast.c
# (c) Mark Adler, zlib license.
# --------------------------------------------------------------------------
MAXBITS = 13

_LITLEN = [11,124,8,7,28,7,188,13,76,4,10,8,12,10,12,10,8,23,8,9,7,6,7,8,7,6,
           55,8,23,24,12,11,7,9,11,12,6,7,22,5,7,24,6,11,9,6,7,22,7,11,38,7,9,
           8,25,11,8,11,9,12,8,12,5,38,5,38,5,11,7,5,6,21,6,10,53,8,7,24,10,
           27,44,253,253,253,252,252,252,13,12,45,12,45,12,61,12,45,44,173]
_LENLEN  = [2, 35, 36, 53, 38, 23]
_DISTLEN = [2, 20, 53, 230, 247, 151, 248]
_BASE  = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264]
_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8]

def _construct(rep):
    """Expand run-length coded lengths; build canonical Huffman (count, symbol)."""
    length = []
    for b in rep:
        length.extend([b & 15] * ((b >> 4) + 1))
    count = [0] * (MAXBITS + 1)
    for l in length:
        count[l] += 1
    offs = [0, 0]
    for l in range(1, MAXBITS):
        offs.append(offs[l] + count[l])
    symbol = [0] * sum(count[1:])
    for sym, l in enumerate(length):
        if l:
            symbol[offs[l]] = sym
            offs[l] += 1
    return count, symbol

_LITCODE, _LENCODE, _DISTCODE = _construct(_LITLEN), _construct(_LENLEN), _construct(_DISTLEN)

class _Bits:
    """LSB-first bit reader."""
    def __init__(self, data):
        self.data, self.pos, self.buf, self.cnt = data, 0, 0, 0
    def bits(self, need):
        val = self.buf
        while self.cnt < need:
            val |= self.data[self.pos] << self.cnt
            self.pos += 1
            self.cnt += 8
        self.buf = val >> need
        self.cnt -= need
        return val & ((1 << need) - 1)
    def decode(self, h):
        count, symbol = h
        code = first = index = 0
        for ln in range(1, MAXBITS + 1):
            code |= self.bits(1) ^ 1          # codes are stored inverted
            c = count[ln]
            if code < first + c:
                return symbol[index + (code - first)]
            index += c
            first = (first + c) << 1
            code <<= 1
        raise ValueError('bad Huffman code')

def explode(data, expected=None):
    """Decompress a PKWARE DCL imploded buffer."""
    br = _Bits(data)
    lit = br.bits(8)                          # 0 = raw literals, 1 = coded
    if lit > 1:
        raise ValueError('bad implode header')
    dct = br.bits(8)                          # log2(dict size) - 6, in 4..6
    if not 4 <= dct <= 6:
        raise ValueError('bad implode dictionary size')
    out = bytearray()
    while True:
        if br.bits(1):
            sym = br.decode(_LENCODE)
            ln = _BASE[sym] + br.bits(_EXTRA[sym])
            if ln == 519:                     # end-of-stream code
                break
            sh = 2 if ln == 2 else dct
            dist = (br.decode(_DISTCODE) << sh) + br.bits(sh) + 1
            if dist > len(out):
                raise ValueError('distance too far back')
            for _ in range(ln):               # overlap-safe byte copy
                out.append(out[-dist])
        else:
            out.append(br.decode(_LITCODE) if lit else br.bits(8))
        if expected is not None and len(out) >= expected:
            break                             # Storm stops at the sector size
    return bytes(out[:expected] if expected else out)

# --------------------------------------------------------------------------
# MPQ v1 reader (Storm-compatible)
# --------------------------------------------------------------------------
def _make_crypt_table():
    ct, seed = [0] * 0x500, 0x00100001
    for i in range(0x100):
        idx = i
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            t1 = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            ct[idx] = t1 | (seed & 0xFFFF)
            idx += 0x100
    return ct

_CT = _make_crypt_table()

def _hash(s, htype):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for c in s.upper().replace('/', '\\').encode('latin1'):
        s1 = (_CT[(htype << 8) + c] ^ (s1 + s2)) & 0xFFFFFFFF
        s2 = (c + s1 + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
    return s1

def _decrypt(buf, key):
    s2, out = 0xEEEEEEEE, bytearray()
    for (v,) in struct.iter_unpack('<I', buf):
        s2 = (s2 + _CT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        v ^= (key + s2) & 0xFFFFFFFF
        out += struct.pack('<I', v)
        key = (((~key << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        s2 = (v + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
    return bytes(out)

F_IMPLODE, F_COMPRESS, F_ENCRYPTED, F_FIX_KEY = 0x100, 0x200, 0x10000, 0x20000
F_SINGLE_UNIT, F_EXISTS = 0x2000000, 0x80000000

class MPQ:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        magic, hsz, asz, ver, bshift, htpos, btpos, htn, btn = \
            struct.unpack_from('<4sIIHHIIII', self.data, 0)
        if magic != b'MPQ\x1a':
            raise ValueError(f'{path}: not an MPQ')
        self.sector = 512 << bshift
        self.htn = htn
        self.ht = list(struct.iter_unpack(
            '<IIHHI', _decrypt(self.data[htpos:htpos + htn * 16], _hash('(hash table)', 3))))
        self.bt = list(struct.iter_unpack(
            '<IIII', _decrypt(self.data[btpos:btpos + btn * 16], _hash('(block table)', 3))))

    def find(self, name):
        idx, a, b = _hash(name, 0), _hash(name, 1), _hash(name, 2)
        i = start = idx & (self.htn - 1)
        while True:
            n1, n2, _loc, _plat, bi = self.ht[i]
            if bi == 0xFFFFFFFF:
                return None
            if n1 == a and n2 == b and bi != 0xFFFFFFFE:
                return self.bt[bi]
            i = (i + 1) % self.htn
            if i == start:
                return None

    @staticmethod
    def _decomp(chunk, want):
        mask, body = chunk[0], chunk[1:]
        if mask == 0x08:
            return explode(body, want)
        if mask == 0x02:
            return zlib.decompress(body)[:want]
        if mask == 0x10:
            return bz2.decompress(body)[:want]
        raise NotImplementedError(f'compression mask 0x{mask:02x}')

    def extract(self, name):
        e = self.find(name)
        if e is None:
            return None
        off, csz, fsz, flags = e
        raw = self.data[off:off + csz]
        key = 0
        if flags & F_ENCRYPTED:
            key = _hash(name.replace('/', '\\').rsplit('\\', 1)[-1], 3)
            if flags & F_FIX_KEY:
                key = ((key + off) ^ fsz) & 0xFFFFFFFF

        if not flags & (F_COMPRESS | F_IMPLODE):          # plain stored file
            if flags & F_ENCRYPTED:
                raw = _decrypt(raw + b'\0' * (-len(raw) % 4), key)[:len(raw)]
            return raw[:fsz]

        if flags & F_SINGLE_UNIT:
            if flags & F_ENCRYPTED:
                raw = _decrypt(raw + b'\0' * (-len(raw) % 4), key)[:len(raw)]
            if csz == fsz:
                return raw[:fsz]
            return explode(raw, fsz) if flags & F_IMPLODE else self._decomp(raw, fsz)

        nsect = (fsz + self.sector - 1) // self.sector
        tbl = raw[:4 * (nsect + 1)]
        if flags & F_ENCRYPTED:
            tbl = _decrypt(tbl, (key - 1) & 0xFFFFFFFF)
        offs = struct.unpack(f'<{nsect + 1}I', tbl)
        out = b''
        for i in range(nsect):
            chunk = raw[offs[i]:offs[i + 1]]
            if flags & F_ENCRYPTED:
                chunk = _decrypt(chunk + b'\0' * (-len(chunk) % 4),
                                 (key + i) & 0xFFFFFFFF)[:len(chunk)]
            want = min(self.sector, fsz - i * self.sector)
            if len(chunk) == want:                        # stored sector
                out += chunk
            elif flags & F_IMPLODE and not flags & F_COMPRESS:
                out += explode(chunk, want)               # no mask byte
            else:
                out += self._decomp(chunk, want)
        if len(out) != fsz:
            raise ValueError(f'{name}: got {len(out)} bytes, expected {fsz}')
        return out

# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------
def cmd_fetch(args):
    os.makedirs(args.outdir, exist_ok=True)
    names = args.names or ['patch_rt.mpq']
    for n in names:
        key = ALIASES.get(n.lower(), n.lower())
        if key not in MEMBERS:
            sys.exit(f'unknown archive {n!r}; choose from: {", ".join(MEMBERS)}')
        tail, size, crc = MEMBERS[key]
        dest = os.path.join(args.outdir, tail.replace('%20', ' '))
        url = BASE + tail
        print(f'fetching {url}\n  -> {dest} ({size/1e6:.1f} MB)')
        req = urllib.request.Request(url, headers={'User-Agent': 'scdat/1.0'})
        c, got = 0, 0
        with urllib.request.urlopen(req, timeout=120) as r, open(dest, 'wb') as f:
            while True:
                buf = r.read(1 << 20)
                if not buf:
                    break
                f.write(buf)
                c = zlib.crc32(buf, c)
                got += len(buf)
                print(f'\r  {got/1e6:8.1f} / {size/1e6:.1f} MB', end='', flush=True)
        print()
        ok = got == size and c == crc
        print(f'  size {"OK" if got == size else f"MISMATCH ({got})"};'
              f' crc32 {c:08x} {"OK" if c == crc else f"MISMATCH (want {crc:08x})"}')
        if not ok:
            sys.exit('download verification failed')

def _open_all(paths):
    archives = []
    for p in paths:
        try:
            archives.append((p, MPQ(p)))
        except FileNotFoundError:
            print(f'  (skipping missing {p})')
    return archives

def cmd_extract(args):
    archives = _open_all(args.archives)
    os.makedirs(args.outdir, exist_ok=True)
    targets = args.files or [f for f in KNOWN_FILES if f != '(listfile)']
    for name in targets:
        for path, mpq in archives:               # priority = argument order
            data = mpq.extract(name)
            if data is not None:
                dest = os.path.join(args.outdir, name.replace('\\', '_').replace('(', '').replace(')', ''))
                open(dest, 'wb').write(data)
                print(f'{name:22} {len(data):8} bytes   <- {path}')
                break
        else:
            print(f'{name:22} -- not found in given archive(s)')

def cmd_probe(args):
    for path, mpq in _open_all(args.archives):
        print(f'== {path} (sector {mpq.sector}, {len(mpq.bt)} blocks)')
        for name in KNOWN_FILES:
            e = mpq.find(name)
            if e:
                print(f'  {name:22} {e[2]:8} bytes  flags 0x{e[3]:08x}')

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('fetch');   p.add_argument('names', nargs='*')
    p.add_argument('-o', '--outdir', default='.'); p.set_defaults(fn=cmd_fetch)
    p = sub.add_parser('extract'); p.add_argument('archives', nargs='+')
    p.add_argument('--files', nargs='*'); p.add_argument('-o', '--outdir', default='.')
    p.set_defaults(fn=cmd_extract)
    p = sub.add_parser('probe');   p.add_argument('archives', nargs='+')
    p.set_defaults(fn=cmd_probe)
    a = ap.parse_args()
    a.fn(a)
