"""Who is on stage for each official Guitar Hero and Rock Band song, from the games' own song lists, as
assets/song_facts.tsv: what a custom song that is one of them (a Clone Hero rip) has lost, for its band
(custom/song_facts.h).

    build/venv/bin/python tools/song_facts.py pull <cache>
    build/venv/bin/python tools/song_facts.py build <cache> assets/song_facts.tsv

pull copies each game's song list (a few MB each) into <cache>/<game>/ from the NAS (mounted at
/Volumes/terramox, PS3 zips read over `ssh nasmox`); build reads them. Sources:

    gh1, gh2, gh80s  config/gen/songs.dtb in the PS2 discs' archives: band (female_singer,
                     metal_keyboard, no singer) and anim_tempo
    rb1, rb2, rb3    songs.dta, the main checkout's .planning/leaderboards/tools/rb-dta: vocal_gender,
                     anim_tempo, tracks
    gh3, gha         the PS2 discs' DATAP.WAD qb.pak/.pab: singer, keyboard
    ghwt             the PC install's qb.pak/.pab and qs.pak (its strings): singer
    gh5, bh, wor     the PS3 zips' QB.PAK/.PAB and QS.PAK: singer. Warriors of Rock also lists GH World
                     Tour's DLC, Metallica's, Smash Hits' and Van Halen's songs.

Each line of the table, tab-split: `s`, artist key, title key, gender (f, m, or n for no singer), keys
(1 a keyboard part), anim tempo (fast, medium or -); then `a`, artist key, gender for an artist all of
whose songs have one. A key is a name as song_facts.cpp's factKey(): ASCII, accents off, lowercase, `&` as
"and", a leading "the", `(...)`, `[...]` and anything from "feat" on left out, letters and digits only.

The PAK/QB layout is Queen Bee's (Nanook's QueenBeeParser, Pak/PakFormat.cs).
"""

import re
import struct
import subprocess
import sys
import unicodedata
import zlib
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

NAS = '/Volumes/terramox/Games'
NAS_SSH = '/terramox/Games'
PS3 = 'Standalone/Rhythm/Guitar Hero/Archive/ps3'
PS3_PAK = 'PS3_GAME/USRDIR/DATA/COMPRESSED/PS3/PAK'
HARMONIX = {
    'gh1': 'Standalone/Rhythm/Guitar Hero/Roms/ps2/gh1/Guitar Hero (USA).chd',
    'gh2': 'Standalone/Rhythm/Guitar Hero/Roms/ps2/gh2/Guitar Hero II (USA).chd',
    'gh80s': 'Standalone/Rhythm/Guitar Hero/Roms/ps2/gh80s/Guitar Hero Encore - Rocks the 80s (USA).chd',
}
NEVERSOFT_PS2 = {
    'gh3': 'Emulation/roms/ps2/Guitar Hero III - Legends of Rock (USA).chd',
    'gha': 'Emulation/roms/ps2/Guitar Hero - Aerosmith (USA) (En,Fr).chd',
}
GHWT = 'Standalone/Rhythm/Guitar Hero/Play/GHWT/DATA/PAK'
NEVERSOFT_PS3 = {
    'gh5': 'Guitar Hero 5 [BLUS30292].zip',
    'bh': 'Band Hero [BLUS30372].zip',
    'wor': 'Guitar Hero Warriors of Rock [BLUS30487].zip',
}
ROCK_BAND = ('rb1', 'rb2', 'rb3')


def main_checkout():
    """The main checkout, where .planning is, from a worktree too."""
    common = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--git-common-dir'], capture_output=True, text=True,
                            check=True).stdout.strip()
    return (ROOT / common).resolve().parent


# Names, as the game matches them.

def key(s):
    s = unicodedata.normalize('NFKD', (s or '').replace('\\L', '').strip()).encode('ascii', 'ignore').decode().lower()
    s = re.sub(r'\(.*?\)|\[.*?\]', '', s).replace('&', 'and')
    s = re.sub(r'^the\s+', '', s)
    s = re.sub(r'\b(feat|featuring|ft)\b.*', '', s)
    return re.sub(r'[^a-z0-9]', '', s)


# Neversoft PAK/QB.

def qkey(s):
    return zlib.crc32(s.lower().replace('/', '\\').encode()) ^ 0xFFFFFFFF


def unchunk(b):
    """GHWT on: zlib in 'CHNK' blocks, each its header's length, its raw deflate's, the next's offset."""
    if b[:4] != b'CHNK':
        return b
    out, p = bytearray(), 0
    while True:
        head, size, nxt = struct.unpack_from('>3I', b, p + 4)
        out += zlib.decompress(b[p + head:p + head + size], -15)
        if nxt == 0xFFFFFFFF:
            return bytes(out)
        p += nxt


class Qb:
    """One PAK's files and their QB, big endian (PC .xen, PS3) or little (PS2, its own type codes)."""

    KIND = {1: 'int', 2: 'float', 3: 'str', 4: 'wstr', 5: 'f2', 6: 'f3', 7: 'script', 0x0A: 'struct', 0x0C: 'array',
            0x0D: 'key', 0x1A: 'key', 0x1B: 'strptr', 0x1C: 'key'}

    def __init__(self, little):
        self.e = '<' if little else '>'

    def u32(self, b, o):
        return struct.unpack_from(self.e + 'I', b, o)[0]

    def files(self, pak, pab, pab_only):
        """Each file's name key and bytes: offsets into the .pab alone (GH5 on), else from the header entry
        into pak and pab."""
        pak, pab = unchunk(pak), unchunk(pab) if pab else b''
        data = pab if pab_only else pak + pab
        out, pos = [], 0
        while pos + 32 <= len(pak):
            ext, off, size, _full, _fn, name, _parent, flags = struct.unpack_from(self.e + '8I', pak, pos)
            if ext in (0x2CB3EF3B, 0xB524565F) or (size == 0 and ext == 0):
                break
            at = off if pab_only else pos + off
            out.append((name, data[at:at + size]))
            pos += 32 + (160 if flags & 0x20 else 0)
        return out

    def section_code(self, typ):
        if self.e == '<':
            return (typ >> 16) & 0xFF if typ & 0xFF00 == 0x0400 else -1
        return (typ >> 8) & 0xFF if typ & 0xFFFF00FF == 0x00200000 else -1

    def item_code(self, typ):
        if self.e == '<':
            return ((typ >> 8) & 0xFF) >> 1
        return (typ >> 16) & 0x7F if typ & 0x00800000 else (typ >> 8) & 0xFF

    def text(self, b, o, wide):
        if wide:
            e = o
            while b[e:e + 2] != b'\0\0':
                e += 2
            return b[o:e].decode('utf-16-be' if self.e == '>' else 'utf-16-le', 'replace')
        return b[o:b.index(b'\0', o)].decode('latin1')

    def struct_(self, b, o, depth=0):
        d = {}
        if self.u32(b, o) != (0x00010000 if self.e == '<' else 0x00000100):
            return d
        p = self.u32(b, o + 4)
        while p and depth < 20:
            typ, k, val, nxt = struct.unpack_from(self.e + '4I', b, p)
            kind = self.KIND.get(self.item_code(typ))
            if kind in ('str', 'wstr'):
                val = self.text(b, val, kind == 'wstr')
            elif kind == 'float':
                val = struct.unpack(self.e + 'f', struct.pack(self.e + 'I', val))[0]
            elif kind == 'struct':
                val = self.struct_(b, val, depth + 1)
            elif kind == 'array':
                val = self.array(b, val)
            elif kind == 'key':
                val = ('#', val)
            d[k] = val
            p = nxt
        return d

    def array(self, b, o):
        typ, n = self.u32(b, o), self.u32(b, o + 4)
        if n == 0:
            return []
        base = o + 8 if n == 1 else self.u32(b, o + 8)
        if typ == (0x000A0100 if self.e == '<' else 0x00010A00):
            ptrs = [self.u32(b, o + 8)] if n == 1 else [self.u32(b, base + 4 * i) for i in range(n)]
            return [self.struct_(b, p) for p in ptrs]
        return [self.u32(b, base + 4 * i) for i in range(n)]

    def sections(self, b):
        p, out = 28, []
        while p + 20 <= len(b):
            typ, k, fk, val, _nxt = struct.unpack_from(self.e + '5I', b, p)
            code = self.section_code(typ)
            if code not in self.KIND:
                break
            out.append((k, self.struct_(b, val) if self.KIND[code] == 'struct' else None))
            q = p + 20  # the next section: the next of a section's type with the same file key
            while q + 20 <= len(b) and not (self.section_code(self.u32(b, q)) in self.KIND and self.u32(b, q + 8) == fk):
                q += 4
            p = q
        return out


def strings(qs):
    """A QS pak's strings by key: UTF-16 lines `<key hex> "<text>"`."""
    out = {}
    for _, d in Qb(False).files(qs, None, False):
        if d[:2] == b'\xff\xfe':
            t = d.decode('utf-16-le', 'replace')
        elif d[:2] == b'\xfe\xff':
            t = d.decode('utf-16-be', 'replace')
        else:
            continue
        for m in re.finditer(r'([0-9a-fA-F]{8}) "([^"]*)"', t):
            out[int(m.group(1), 16)] = m.group(2)
    return out


NAMES = {qkey(n): n for n in 'singer female male none keyboard true false title artist name checksum'.split()}


def neversoft_songs(game, d):
    """Every song in a game's QB: each struct with a checksum, name and title, in a QB that names a singer."""
    little = (d / 'qb.pak.ps2').exists()
    qb = Qb(little)
    if little:
        files = qb.files((d / 'qb.pak.ps2').read_bytes(), (d / 'qb.pab.ps2').read_bytes(), False)
    elif (d / 'QB.PAK.PS3').exists():
        files = qb.files((d / 'QB.PAK.PS3').read_bytes(), (d / 'QB.PAB.PS3').read_bytes(), True)
    else:
        files = qb.files((d / 'qb.pak.xen').read_bytes(), (d / 'qb.pab.xen').read_bytes(), False)
    qs = next((d / n for n in ('qs.pak.xen', 'QS.PAK.PS3') if (d / n).exists()), None)
    words = strings(qs.read_bytes()) if qs else {}
    singer = qkey('singer').to_bytes(4, 'little' if little else 'big')

    def named(v):
        if isinstance(v, tuple):
            return NAMES.get(v[1]) or words.get(v[1]) or '%08x' % v[1]
        return v

    out = []
    for _, b in files:
        if singer not in b:
            continue
        try:
            sections = qb.sections(b)
        except (struct.error, ValueError, IndexError):
            continue
        for _, st in sections:
            for v in (st or {}).values():
                if isinstance(v, dict) and all(qkey(n) in v for n in ('checksum', 'name', 'title')):
                    s = {NAMES.get(k, '%08x' % k): named(x) for k, x in v.items() if not isinstance(x, (dict, list))}
                    out.append(dict(game=game, artist=s.get('artist'), title=s.get('title'),
                                    singer=s.get('singer'), keyboard=s.get('keyboard') == 'true'))
    return out


# Harmonix DTA.

def dta(text):
    toks = re.findall(r'"(?:[^"\\]|\\.)*"|\(|\)|[^\s()]+', re.sub(r';[^\n]*', '', text))
    i = 0

    def node():
        nonlocal i
        out = []
        while i < len(toks):
            t = toks[i]
            i += 1
            if t == '(':
                out.append(node())
            elif t == ')':
                return out
            else:
                out.append(t.strip('"') if t.startswith('"') else t)
        return out

    return node()


def field(entry, name):
    for x in entry:
        if isinstance(x, list) and x and x[0] == name:
            return x[1:]
    return None


def tempo(v):
    v = (v or [None])[0]
    return {'kTempoFast': 'fast', '64': 'fast', 'kTempoMedium': 'medium', '32': 'medium', 'kTempoSlow': 'medium',
            '16': 'medium'}.get(v)


def harmonix_songs(game, text):
    out = []
    for e in dta(text):
        if not isinstance(e, list) or not field(e, 'artist') or not field(e, 'name') or field(e, 'fake'):
            continue
        row = dict(game=game, artist=field(e, 'artist')[0], title=field(e, 'name')[0], tempo=tempo(field(e, 'anim_tempo')))
        if game.startswith('rb'):
            tracks = (field(field(e, 'song') or [], 'tracks') or [[]])[0]
            parts = [t[0] for t in tracks if isinstance(t, list) and t]
            gender = (field(e, 'vocal_gender') or [None])[0]
            row.update(singer=gender if 'vocals' in parts else 'none', keyboard='keys' in parts)
        else:
            # GH2's female_singer and metal_keyboard, GH1's SINGER_FEMALE_METAL and KEYBOARD_METAL.
            band = field(e, 'band')
            members = [str(m).lower() for m in band or []]
            singers = [m for m in members if 'singer' in m]
            row.update(singer='male' if band is None else 'female' if any('female' in m for m in singers) else 'male' if singers else 'none',
                       keyboard=any('keyboard' in m for m in members))
        out.append(row)
    return out


# Pulling each game's list.

def pull(cache):
    import disc
    import inventory as I
    for game in ROCK_BAND:
        (cache / game).mkdir(parents=True, exist_ok=True)
        (cache / game / 'songs.dta').write_bytes(
            (main_checkout() / f'.planning/leaderboards/tools/rb-dta/{game}/songs/gen/songs.dta').read_bytes())
        print(game, 'songs.dta')
    for game, rel in HARMONIX.items():
        vol = I.open_volume(Path(NAS, rel))
        by = {p.upper(): (p, size, start) for p, size, start in vol.files}
        hdr = by['GEN/MAIN.HDR']
        ark = I.Ark(vol.read(hdr[2], 0, hdr[1]))
        parts = [by.get(f'GEN/MAIN_{i}.ARK') for i in range(len(ark.part_sizes))]
        entry = next(e for e in ark.entries if e[0] == 'config/gen/songs.dtb')
        p, size, usize, part, offset = entry
        _, root, _ = I.decode_dtb(I.unpack(vol.read(parts[part][2], offset, size), size, usize))
        (cache / game).mkdir(parents=True, exist_ok=True)
        (cache / game / 'songs.dta').write_text(I.render_root(root), encoding='utf-8')
        print(game, 'songs.dta')
    for game, rel in NEVERSOFT_PS2.items():
        with disc.Image(str(Path(NAS, rel))) as d:
            hed = d.read('DATAP.HED')
            wad = next(lba for path, lba, _ in d.walk() if path.upper() == 'DATAP.WAD')
            p = 0
            (cache / game).mkdir(parents=True, exist_ok=True)
            while p + 8 < len(hed):
                sector, size = struct.unpack_from('<II', hed, p)
                end = hed.index(b'\0', p + 8)
                name = hed[p + 8:end].decode('latin1')
                p = (end + 4) & ~3
                if not name:
                    break
                base = name.replace('\\', '/').split('/')[-1].lower()
                if base in ('qb.pak.ps2', 'qb.pab.ps2'):
                    (cache / game / base).write_bytes(d.read_range(wad, sector * 2048, size))
        print(game, 'qb.pak.ps2')
    (cache / 'ghwt').mkdir(parents=True, exist_ok=True)
    for name in ('qb.pak.xen', 'qb.pab.xen', 'qs.pak.xen'):
        (cache / 'ghwt' / name).write_bytes(Path(NAS, GHWT, name).read_bytes())
    print('ghwt qb.pak.xen')
    for game, z in NEVERSOFT_PS3.items():
        (cache / game).mkdir(parents=True, exist_ok=True)
        for name in ('QB.PAK.PS3', 'QB.PAB.PS3', 'QS.PAK.PS3'):
            got = subprocess.run(['ssh', 'nasmox', f'unzip -p "{NAS_SSH}/{PS3}/{z}" "{PS3_PAK}/{name}"'],
                                 capture_output=True, check=True).stdout
            (cache / game / name).write_bytes(got)
        print(game, 'QB.PAK.PS3')


# The table.

def build(cache, out):
    rows = []
    for game in (*HARMONIX, *ROCK_BAND):
        rows += harmonix_songs(game, (cache / game / 'songs.dta').read_text(encoding='utf-8', errors='replace'))
    for game in list(NEVERSOFT_PS2) + ['ghwt'] + list(NEVERSOFT_PS3):
        rows += neversoft_songs(game, cache / game)
    skip = re.compile(r'debug|tutorial|^tut_|synctest|placeholder|\btest\b|jam session|credits|guitar battle|guitar duel', re.I)
    songs = {}
    artists = defaultdict(Counter)
    for r in rows:
        a, t = key(r['artist']), key(r['title'])
        if not a or not t or skip.search(r['title'] or '') or skip.search(r['artist'] or ''):
            continue
        singer = r.get('singer')
        gender = {'female': 'f', 'none': 'n', None: None}.get(singer, 'm')  # a named character (GH3's bret) a man
        s = songs.setdefault((a, t), dict(gender=None, keys=False, tempo=None))
        if gender and s['gender'] is None:
            s['gender'] = gender
        s['keys'] |= bool(r.get('keyboard'))
        s['tempo'] = s['tempo'] or r.get('tempo')
        if gender in ('f', 'm'):
            artists[a][gender] += 1
    lines = ['# Generated by tools/song_facts.py from the games\' own song lists: do not edit.']
    for (a, t), s in sorted(songs.items()):
        if s['gender'] or s['keys'] or s['tempo']:
            lines.append('\t'.join(['s', a, t, s['gender'] or '-', '1' if s['keys'] else '0', s['tempo'] or '-']))
    for a, genders in sorted(artists.items()):
        if len(genders) == 1:
            lines.append('\t'.join(['a', a, next(iter(genders))]))
    Path(out).write_text('\n'.join(lines) + '\n')
    print(len(songs), 'songs')


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'pull':
        pull(Path(sys.argv[2]))
    elif len(sys.argv) >= 4 and sys.argv[1] == 'build':
        build(Path(sys.argv[2]), sys.argv[3])
    else:
        sys.exit(__doc__)
