"""The game's static data tables, read from its archives (read-only) with
DLLCrimson's tools/game_files.py (through Crimson Browser Sharp's index).

A table is gamedata/binarystaticinfo__/bin/<name>.staticinfoheader (a count,
then each record's key and its offset in the body) and .staticinfobody (the
records one after another). A record starts with its key and its string key
(u32 length + text)."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.expanduser('~'), 'Desktop', 'DLLCrimson', 'tools'))
import game_files  # noqa: E402


def hash_name(name):
    """The game's key for a name (hashlittle of the lower-case name, 810718)."""
    return game_files._hash_little(name.lower().encode(), 810718)


class Table:
    def __init__(self, name):
        self.name = name
        files = {}
        for e in game_files.entries(r'^gamedata/binarystaticinfo__/bin/' + name + r'\.staticinfo(header|body)$'):
            files[e['path'].rsplit('.', 1)[1]] = game_files.read(e)
        head, body = files['staticinfoheader'], files['staticinfobody']

        # The count is a u16 or a u32; the key fills what is left of each entry.
        for base in (2, 4):
            n = int.from_bytes(head[:base], 'little')
            if n and (len(head) - base) % n == 0 and (len(head) - base) // n > 4:
                break
        self.key_size = (len(head) - base) // n - 4
        entries = []
        for i in range(n):
            p = base + i * (self.key_size + 4)
            key = int.from_bytes(head[p:p + self.key_size], 'little')
            entries.append((key, struct.unpack_from('<I', head, p + self.key_size)[0]))
        ends = sorted(o for _, o in entries) + [len(body)]
        following = {ends[i]: ends[i + 1] for i in range(len(ends) - 1)}
        self.raw = {key: body[o:following[o]] for key, o in entries}
        self.names = {}
        for key, raw in self.raw.items():
            p = self.key_size
            length = struct.unpack_from('<I', raw, p)[0] if len(raw) >= p + 4 else 0
            if 0 < length < 300:
                self.names[key] = raw[p + 4:p + 4 + length].decode('utf-8', 'replace')
        self.by_name = {v: k for k, v in self.names.items()}

    def key(self, name):
        return self.by_name[name]
