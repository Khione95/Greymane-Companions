"""The names the game shows for characters (English): a characterinfo record's
LocalizableStrings (kind, field, owner key, the text id as decimal text) looked
up in gamedata/stringtable/binary__/eng/character.paloc ("paloc", sizes, then
LZ4: per entry u32 length + text key (digits), u32 length + text).

  display_names(chars, keys) -> {key: name}
  python display_names.py      prints the roster's names"""
import os
import re
import struct
import sys

import lz4.block

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'DLLCrimson', 'tools'))
import game_files  # noqa: E402


def paloc(name):
    e = next(game_files.entries(r'stringtable/binary__/eng/' + name + r'\.paloc$'))
    data = game_files.read(e)
    size = struct.unpack_from('<I', data, 12)[0]
    text = lz4.block.decompress(data[17:], uncompressed_size=size)
    out = {}
    p = 0
    while p + 4 <= len(text):
        n = struct.unpack_from('<I', text, p)[0]
        if 0 < n <= 32 and text[p + 4:p + 4 + n].isdigit():
            key = text[p + 4:p + 4 + n]
            q = p + 4 + n
            m = struct.unpack_from('<I', text, q)[0] if q + 4 <= len(text) else 0
            if 0 <= m < 4000:
                out[key.decode()] = text[q + 4:q + 4 + m].decode('utf-8', 'replace')
                p = q + 4 + m
                continue
        p += 1
    return out


def loc_ids(raw, owner):
    """The text ids of the record's LocalizableStrings, in order."""
    ids = []
    for m in re.finditer(re.escape(struct.pack('<I', owner)), raw):
        at = m.end()
        if at + 4 > len(raw):
            continue
        n = struct.unpack_from('<I', raw, at)[0]
        if 0 < n <= 24 and raw[at + 4:at + 4 + n].isdigit():
            ids.append(raw[at + 4:at + 4 + n].decode())
    return ids


def display_names(chars, keys):
    texts = paloc('character')
    names = {}
    for key in keys:
        for ident in loc_ids(chars.raw[key], key):
            if texts.get(ident):
                names[key] = texts[ident]
                break
    return names


def main():
    import build_test
    import tables
    chars = tables.Table('characterinfo')
    keys = build_test.camp_comrades(chars) + [k for k, _, _ in build_test.TESTS]
    names = display_names(chars, keys)
    for key in keys:
        print(f'{key:06X}  {chars.names.get(key, "?"):40s}  {names.get(key, "?")}')


if __name__ == '__main__':
    main()
