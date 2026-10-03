"""Character portraits from the game's Knowledge (codex) entries - read-only.

How the game links a character to the picture its Knowledge entry shows
(CrimsonDesert.exe RVAs, image base 0x140000000):

  characterinfo record  ._knowledgeInfo   (u32 knowledgeinfo key; in memory a u16 handle at +0x1EA,
                                           read at 0x14F136A by 0x151E6C0, characterinfo reader 0x14F01A0)
  knowledgeinfo record  ._uiTextureName   (u32 stringinfo key; in memory +0x12,
                                           read at 0x150D802 by 0x1520520, knowledgeinfo reader 0x150D740)
  stringinfo record     the texture id, e.g. "cd_knowledgeimage_Knowledge_GreyFur_Max"
                        (stringinfo key = hashlittle(text, 810718), case kept)
  file                  ui/texture/image/knowledgeimage/<texture id lower-case>.dds
                        (the UI resolves textureid(...) by file stem, see changecharacternoticepanel.css)

The game itself walks exactly this chain at 0xF164A5..0xF1659F (characterinfo getter 0x389570 ->
+0x1EA -> knowledgeInfo getter 0x433650 -> +0x12 -> stringinfo getter 0x388E00 -> +0x18 string),
falling back to "cd_knowledgeimage_Knowledge_Unknown_Portrait" (0xF1664B).

Record layouts used here (file order = reader order):
  knowledgeinfo: u32 _key | u32 len + _stringKey | u8 _isBlocked | u32 _uiTextureName | u8 _isDefault |
                 u32 _factionInfo | u32 _factionNodeInfo | u32 _skillInfo | u32 n + n*u32 _characterInfoList ...
  characterinfo: ... u32 _vanishTickCount | u32 _uiPortraitPath | u32 _symbolImage |
                 4 skill lists (u32 n + n*(u32 skill, u32 level)) | u32 n + n*u32 _interactionInfoList |
                 f32 _interactionDistance | u32 _defaultActionActionIndex | u32 n + n*u32 _defaultShareValueIndex |
                 f32 _characterWeight | u8 _battleOrderType | u8 _characterType | u32 _uiMapTextureInfo |
                 u8 _mapIconDisplayType | u32 _knowledgeInfo | u8 _knowledgeObtainType ...
  stringinfo:    u32 key | u32 0 | u8 0 | u32 len + text

Images: 512x512 DDS, BC1 (DXT1) for the Greymanes, Kliff, Damiane, Oongka; BC3 (DXT5) for a few
(knowledge_unique_aldric). The archive stores them "Partial": a plain DDS header whose reserved
dwords at 0x20 hold (stored size, size) of the top mip; when they differ the top mip is LZ4.
unpack_dds() returns a standard DDS that Pillow opens.

Usage:
  python portraits.py                       list every roster character (+ Kliff, Damiane, Oongka)
  python portraits.py --out <dir> [--size N] [--jpeg] [--keep-dds]
                                            also write <dir>/<key hex>_<name>.png (or .jpg)
  python portraits.py --hud                 the health window's pictures: build/portraits/<KEY>.jpg
                                            (head and shoulders, 192 px; game art - not for the repository)
"""
import argparse
import io
import os
import re
import struct
import sys

import tables  # noqa: E402  (tools/tables.py)
import game_files  # noqa: E402  (DLLCrimson/tools/game_files.py, put on the path by tables.py)

HERE = os.path.dirname(os.path.abspath(__file__))
ROSTER_H = os.path.join(HERE, '..', 'asi', 'src', 'roster.h')
HUD_OUT = os.path.join(HERE, '..', 'build', 'portraits')
HUD_SIZE = 192
HUD_CROP = (104, 16, 408, 320)     # head and shoulders of the 512 x 512 pictures
EXTRA = [(0x1, 'Kliff'), (0x4, 'Damiane'), (0x6, 'Oongka')]
UNKNOWN = 'cd_knowledgeimage_Knowledge_Unknown_Portrait'   # the game's fallback (0xF1664B)


def u32(b, p):
    return struct.unpack_from('<I', b, p)[0]


def read_roster(path=ROSTER_H):
    out = []
    for m in re.finditer(r'\{\s*0x([0-9A-Fa-f]+)\s*,\s*"([^"]*)"\s*\}', open(path, encoding='utf-8').read()):
        out.append((int(m.group(1), 16), m.group(2)))
    return out


class Data:
    def __init__(self):
        self.chars = tables.Table('characterinfo')
        self.know = tables.Table('knowledgeinfo')
        self.interactions = set(tables.Table('interactioninfo').raw)
        self.strings = {}
        for k, r in tables.Table('stringinfo').raw.items():
            self.strings[k] = r[13:13 + u32(r, 9)].decode('utf-8', 'replace')
        self.textures = {}   # file stem -> path, every UI texture (the UI resolves texture ids by stem)
        for e in game_files.entries(r'^ui/texture/.*\.dds$'):
            stem = e['path'].rsplit('/', 1)[1][:-4]
            if stem not in self.textures or '/image/' in e['path']:
                self.textures[stem] = e
        # reverse map: character key -> knowledge keys whose _characterInfoList names it
        self.listed_in = {}
        for k in self.know.raw:
            for c in self.knowledge(k)['characters']:
                self.listed_in.setdefault(c, []).append(k)

    def hash_name(self, h, kn=0):
        """A texture-id hash as text: from stringinfo, else a guess from the knowledge name."""
        if not h:
            return '-'
        if h in self.strings:
            s = self.strings[h]
            return f'{s} ({"file" if s.lower() in self.textures else "no file"})'
        if kn in self.know.names:
            guess = 'cd_portraitimage_' + self.know.names[kn]
            if tables.game_files._hash_little(guess.encode(), 810718) == h:
                return f'{guess} (hash {h:#x}, not in stringinfo, {"file" if guess.lower() in self.textures else "no file"})'
        return f'hash {h:#x} (not in stringinfo)'

    def knowledge(self, key):
        r = self.know.raw[key]
        p = 4 + 4 + u32(r, 4)            # after _key and _stringKey
        tex = u32(r, p + 1)               # after _isBlocked
        p += 1 + 4 + 1 + 4 + 4 + 4        # _isBlocked _uiTextureName _isDefault _factionInfo _factionNodeInfo _skillInfo
        n = u32(r, p)
        chars = list(struct.unpack_from(f'<{n}I', r, p + 4)) if n < 256 else []
        return {'name': self.know.names.get(key, '?'), 'texture_key': tex,
                'texture': self.strings.get(tex), 'characters': chars}

    def character_fields(self, key):
        """(_knowledgeInfo, _uiPortraitPath, _symbolImage) of a characterinfo record, found from its
        interaction list (as build_test.py finds it) and parsed forward / solved backward."""
        r = self.chars.raw[key]
        lists = []
        for p in range(0, len(r) - 8):
            n = u32(r, p)
            if not 0 <= n <= 64 or p + 4 + 4 * n + 4 > len(r):
                continue
            keys = struct.unpack_from(f'<{n}I', r, p + 4)
            if n and not all(k in self.interactions for k in keys):
                continue
            dist = struct.unpack_from('<f', r, p + 4 + 4 * n)[0]
            if not 0.5 <= dist <= 50:
                continue
            q = p + 4 + 4 * n + 4                     # _defaultActionActionIndex
            q += 4
            if q + 4 > len(r):
                continue
            cnt = u32(r, q)                           # _defaultShareValueIndex
            if cnt > 64:
                continue
            q += 4 + 4 * cnt
            q += 4 + 1 + 1                            # _characterWeight _battleOrderType _characterType
            q += 4 + 1                               # _uiMapTextureInfo _mapIconDisplayType
            if q + 5 > len(r):
                continue
            kn = u32(r, q)
            if kn and kn not in self.know.raw:
                continue
            if r[q + 4] > 16:
                continue                              # _knowledgeObtainType is a small enum
            lists.append((p, n, kn, q, self._before_interactions(r, p)))
        # A list can also match from inside the skill lists before it (a skill level that is also an
        # interaction key): same end, same knowledge field - keep one per knowledge position.
        # A stray "count 0" match elsewhere has no skill lists ending at it: drop it.
        by_at = {}
        for c in lists:
            by_at.setdefault(c[3], []).append(c)
        if len(by_at) > 1:
            by_at = {at: cs for at, cs in by_at.items() if any(c[1] for c in cs)} or by_at
        if len(by_at) > 1:
            by_at = {at: cs for at, cs in by_at.items() if any(c[4] for c in cs)} or by_at
        if len(by_at) != 1:
            return {'error': f'interaction list found at {len(by_at)} places', 'candidates': lists}
        cs = sorted(next(iter(by_at.values())), key=lambda c: (not c[4], -c[0]))
        p, n, kn, at, sols = cs[0]
        portrait = symbol = None
        if sols:                                      # the nearest: four empty lists when there are none
            portrait, symbol = sols[-1][1], sols[-1][2]
        return {'knowledge': kn, 'knowledge_at': at, 'interaction_list_at': p, 'interactions': n,
                'portrait_key': portrait, 'symbol_key': symbol, 'portrait_solutions': len(sols)}

    def _before_interactions(self, r, p):
        """_uiPortraitPath / _symbolImage: the positions q where u32 q, u32 q+4 and then four skill
        lists (u32 n + n*(u32 skill, u32 level)) end exactly at p. These fields hold the hash of a
        texture id (hashlittle 810718), which need not be in stringinfo."""
        sols = []
        for q in range(max(4, p - 4096), p - 23):
            pos = q + 8
            ok = True
            for _ in range(4):
                if pos + 4 > p:
                    ok = False
                    break
                c = u32(r, pos)
                if c > 200:
                    ok = False
                    break
                pos += 4 + 8 * c
            if ok and pos == p:
                sols.append((q, u32(r, q), u32(r, q + 4)))
        return sols


def unpack_dds(entry):
    """The archive's DDS ("Partial") -> a standard DDS file (bytes)."""
    with open(os.path.join(game_files.GAME, entry['archive'], f'{entry["paz"]}.paz'), 'rb') as f:
        f.seek(entry['offset'])
        data = f.read(entry['packed'])
    if entry['encryption'] != 'None':
        data = game_files.chacha(data, entry['path'])
    if data[:4] != b'DDS ':
        raise ValueError('not a DDS')
    head = 128 + (20 if data[84:88] == b'DX10' else 0)
    h, w = u32(data, 12), u32(data, 16)
    fourcc = data[84:88]
    block = 8 if fourcc in (b'DXT1', b'BC4U', b'ATI1') else 16
    top = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
    stored = u32(data, 0x20)
    out = bytearray(data[:head])
    if entry['packed'] == entry['size'] or stored in (0, top):
        out += data[head:]
    else:
        import lz4.block
        out += lz4.block.decompress(data[head:head + stored], uncompressed_size=top)
        out += data[head + stored:]
    out[0x20:0x4C] = bytes(0x2C)              # the archive's size table (reserved dwords)
    if len(out) != entry['size']:
        raise ValueError(f'{entry["path"]}: unpacked {len(out)} bytes, expected {entry["size"]}')
    return bytes(out)


def dds_info(dds):
    return {'w': u32(dds, 16), 'h': u32(dds, 12), 'mips': u32(dds, 28), 'fourcc': dds[84:88].decode('ascii', 'replace')}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', help='write decoded images here (PNG, or JPEG with --jpeg)')
    ap.add_argument('--size', type=int, default=0, help='longest side in pixels (default: as stored, 512)')
    ap.add_argument('--jpeg', action='store_true')
    ap.add_argument('--keep-dds', action='store_true', help='also write the unpacked .dds')
    ap.add_argument('--hud', action='store_true', help="the health window's pictures (build/portraits)")
    a = ap.parse_args()
    if a.hud:
        a.out, a.jpeg = HUD_OUT, True

    d = Data()
    roster = read_roster()
    people = roster + [x for x in EXTRA if x[0] not in {k for k, _ in roster}]
    found = 0
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    print(f'{"key":>8}  {"name":10} {"knowledge":>10} {"knowledge name":28} {"texture id -> file":70} format   listed-in / dialog portrait')
    for key, name in people:
        if key not in d.chars.raw:
            print(f'{key:#8x}  {name:10} not in characterinfo')
            continue
        f = d.character_fields(key)
        listed = [hex(k) for k in d.listed_in.get(key, [])]
        if 'error' in f:
            print(f'{key:#8x}  {name:10} {f["error"]}  listed in {listed}')
            continue
        kn = f['knowledge']
        portrait = d.hash_name(f['portrait_key'], kn)
        if not kn:
            print(f'{key:#8x}  {name:10} {"0":>10} {"(no knowledge entry)":28} {UNKNOWN} (fallback)  listed in {listed}  portrait {portrait}')
            continue
        k = d.knowledge(kn)
        tex = k['texture'] or '?'
        e = d.textures.get(tex.lower())
        fmt = ''
        if e:
            dds = unpack_dds(e)
            i = dds_info(dds)
            fmt = f'{i["fourcc"]} {i["w"]}x{i["h"]}'
            found += 1
            if a.out:
                base = os.path.join(a.out, f'{key:06X}' if a.hud else f'{key:06x}_{name}')
                if a.keep_dds:
                    open(base + '.dds', 'wb').write(dds)
                from PIL import Image
                with Image.open(io.BytesIO(dds)) as im:
                    im = im.convert('RGBA')
                    if a.hud:
                        im = im.resize((512, 512)).crop(HUD_CROP).resize((HUD_SIZE, HUD_SIZE), Image.LANCZOS)
                    elif a.size:
                        im.thumbnail((a.size, a.size))
                    if a.jpeg:
                        im.convert('RGB').save(base + '.jpg', 'JPEG', quality=88, optimize=True)
                    else:
                        im.save(base + '.png', optimize=True)
        path = e['path'] if e else '(no file)'
        print(f'{key:#8x}  {name:10} {kn:#10x} {k["name"]:28} {tex} -> {path}  {fmt}  '
              f'{"listed" if key in k["characters"] else "NOT listed"} in its _characterInfoList; '
              f'listed in {listed}; _uiPortraitPath {portrait}')
    print(f'\n{found} of {len(people)} characters have a knowledge picture')


if __name__ == '__main__':
    main()
