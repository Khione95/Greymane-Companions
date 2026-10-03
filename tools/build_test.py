"""Test 3 (Test 2 with the story Greymanes' records fixed): can Greymanes be recruited, and do the game's own Follow /
Standby / Disband orders then work on them? A DMM v3 JSON mod.

Every camp comrade (the 38 the game hires through its scout missions:
Gilbert, Morrow, Ryan, ...) gets Recruit, Follow, Standby and Disband, so
whoever is at the camp can be tried. And five story Greymanes:

Two ways of recruiting, on different Greymanes:
  A (Duane, Yann, Jian): the game's own unused "Recruit" option (Npc_Employ).
  B (Shane, Naira): a copy of the talk option (Npc_Dialog1, which hands out a
    drop set) that hands out a hire drop set "Mercenary(<character>)" (made
    like Ryan's) and reads "Recruit".
Each also becomes a comrade (the mercenary type byte: melee 0x46, ranged
0x47, as the camp's comrades) with Ryan's "Comrade Found" message, and gets
Follow, Standby, Disband and Follow into Combat (on / off).

Records are edited as whole blobs (DMM: set _blob_b64 / new_record).
Output: dist/Greymane Companions Test 3.zip (a folder with the JSON).
Usage: python build_test.py"""
import base64
import json
import os
import struct
import zipfile

import tables

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'dist')
NAME = 'Greymane Companions Test 3'
CAMP_EXTRA = ['Npc_Employ', 'Npc_Order_Follow', 'Npc_Order_Wait', 'Npc_Order_Disband']

MELEE, RANGE = 0x46, 0x47
RYAN = 0x199                       # a camp comrade (melee), the template
ANCHOR = bytes.fromhex('3e8f7aa1')  # 13 bytes before the mercenary type, once in each record
DIALOG = 0xAC                      # Npc_Dialog1: hands out DropSet_Friendly_Talk
TALK_DROPSET = 0x90F562
EMPLOY = 0xF4292                   # Npc_Employ "Recruit"
ORDERS = ['Npc_Order_Follow', 'Npc_Order_Wait', 'Npc_Order_Disband', 'Npc_Hyosi_Add', 'Npc_Hyosi_Remove']

# (character key, mercenary type, way)
TESTS = [
    (0x195, MELEE, 'A'),    # Duane
    (0x192, MELEE, 'A'),    # Yann
    (0x19B, MELEE, 'A'),    # Jian
    (0x19A, RANGE, 'B'),    # Shane
    (0x19C, MELEE, 'B'),    # Naira
]
NEW_DROPSET = 0x1360001            # above the game's highest (0x134FF28)
NEW_INTERACTION = 0xF5001          # above the game's highest (0xF4391)


def u32(v):
    return struct.pack('<I', v)


def text(s):
    b = s.encode()
    return u32(len(b)) + b


def loc_string(field, owner, ident):
    """A LocalizableString: kind, field, owner key, the text id as decimal text."""
    kind = {0x32: 3, 0x60: 6}[field]
    return bytes([kind]) + u32(field) + u32(owner) + text(str(ident))


def type_position(raw):
    i = raw.find(ANCHOR)
    if i < 0 or raw.find(ANCHOR, i + 1) >= 0:
        raise SystemExit('the mercenary type anchor is not found exactly once')
    return i + 13


def interaction_list(raw, known):
    """(position, keys) of the interaction list: a count and that many known
    interaction keys, then the interaction distance (a float)."""
    found = []
    for p in range(0, len(raw) - 8):
        n = struct.unpack_from('<I', raw, p)[0]
        if not 1 <= n <= 64 or p + 4 + 4 * n + 4 > len(raw):
            continue
        keys = struct.unpack_from(f'<{n}I', raw, p + 4)
        if all(k in known for k in keys):
            distance = struct.unpack_from('<f', raw, p + 4 + 4 * n)[0]
            if 0.5 <= distance <= 50:
                found.append((p, list(keys)))
    if len(found) != 1:
        raise SystemExit(f'interaction list found {len(found)} times')
    return found[0]


def add_interactions(raw, extra):
    p, keys = interaction_list(raw, KNOWN_INTERACTIONS)
    old = len(keys)
    keys += [k for k in extra if k not in keys]
    return raw[:p] + u32(len(keys)) + b''.join(u32(k) for k in keys) + raw[p + 4 + 4 * old:]


# An empty LocalizableString: kind 0, field, owner and a text length of 0 (13
# bytes; Test 2 took it for one byte, and the game could not read the rest).
EMPTY_LOC = bytes(13)


def recruit_character(raw, kind, extra, hire_message):
    t = type_position(raw)
    if raw[t] != 0xFF or raw[t + 1:t + 14] != EMPTY_LOC:
        raise SystemExit('not a plain character (type or hire message set already)')
    raw = raw[:t] + bytes([kind]) + hire_message + raw[t + 14:]
    return add_interactions(raw, extra)


def camp_comrades(chars):
    """The characters the game makes comrades: melee or ranged, with a hire message."""
    out = []
    for key, raw in chars.raw.items():
        i = raw.find(ANCHOR)
        if i >= 0 and raw.find(ANCHOR, i + 1) < 0 and raw[i + 13] in (MELEE, RANGE) and raw[i + 14] == 3:
            out.append(key)
    return sorted(out)


def hire_dropset(template, template_char, key, char, char_name):
    """Ryan's hire drop set for another character."""
    old_name = template[4:8 + struct.unpack_from('<I', template, 4)[0]]
    name = text(f'Mercenary({char_name})')
    body = template[8 + len(old_name) - 4:]
    body = body.replace(b'\x01' + u32(template_char), b'\x01' + u32(char), 1)
    body = body.replace(b'\xff\xff' + u32(template_char), b'\xff\xff' + u32(char), 1)
    if not body.endswith(old_name + b'\x00'):
        raise SystemExit('the hire drop set does not end with its name')
    body = body[:-len(old_name) - 1] + name + b'\x00'
    return u32(key) + name + body


def recruit_interaction(template, key, name, dropset):
    """A copy of the talk option that hands out the hire drop set and reads Recruit."""
    old = template[4:8 + struct.unpack_from('<I', template, 4)[0]]
    raw = u32(key) + text(name) + template[4 + len(old):]
    talk_label = loc_string(0x60, DIALOG, (DIALOG << 32) | 0x60)
    recruit_label = loc_string(0x60, EMPLOY, (EMPLOY << 32) | 0x60)
    if raw.count(talk_label) != 1 or raw.count(u32(TALK_DROPSET)) != 1:
        raise SystemExit('the talk option is not as expected')
    raw = raw.replace(talk_label, recruit_label).replace(u32(TALK_DROPSET), u32(dropset))
    return raw


def b64(raw):
    return base64.b64encode(raw).decode()


def build():
    chars = tables.Table('characterinfo')
    inter = tables.Table('interactioninfo')
    drops = tables.Table('dropsetinfo')
    global KNOWN_INTERACTIONS
    KNOWN_INTERACTIONS = set(inter.raw)
    if NEW_INTERACTION in inter.raw or NEW_DROPSET in drops.raw:
        raise SystemExit('a new key is taken')

    ryan = chars.raw[RYAN]
    t = type_position(ryan)
    hire_message = ryan[t + 1:t + 1 + 13 + struct.unpack_from('<I', ryan, t + 10)[0]]
    if hire_message != loc_string(0x32, RYAN, (RYAN << 32) | 0x32):
        raise SystemExit("Ryan's hire message is not as expected")
    ryan_dropset = drops.key('Mercenary(NHM_Unique_Ryan_DualDagger_409)')
    orders = [inter.key(n) for n in ORDERS]

    char_intents, drop_intents, inter_intents, plan = [], [], [], []
    for n, (key, kind, way) in enumerate(TESTS):
        name = chars.names[key]
        if way == 'A':
            extra = [EMPLOY] + orders
        else:
            dkey, ikey = NEW_DROPSET + n, NEW_INTERACTION + n
            drop = hire_dropset(drops.raw[ryan_dropset], RYAN, dkey, key, name)
            option = recruit_interaction(inter.raw[DIALOG], ikey, f'Npc_Recruit_{name}', dkey)
            drop_intents.append({'op': 'new_record', 'new_key': dkey, 'label': f'Mercenary({name})',
                                 'template': {'key': dkey, '_blob_fallback': True, '_blob_b64': b64(drop)}})
            inter_intents.append({'op': 'new_record', 'new_key': ikey, 'label': f'Recruit {name}',
                                  'template': {'key': ikey, '_blob_fallback': True, '_blob_b64': b64(option)}})
            extra = [ikey] + orders
        raw = recruit_character(chars.raw[key], kind, extra, hire_message)
        char_intents.append({'op': 'set', 'key': key, 'entry': name, 'field': '_blob_b64', 'new': b64(raw),
                             'label': f'{name}: comrade ({"melee" if kind == MELEE else "ranged"}), recruit way {way}'})
        plan.append(f'{name} ({hex(key)}): {"melee" if kind == MELEE else "ranged"}, way {way}')

    camp_extra = [inter.key(n) for n in CAMP_EXTRA]
    for key in camp_comrades(chars):
        name = chars.names[key]
        raw = add_interactions(chars.raw[key], camp_extra)
        char_intents.append({'op': 'set', 'key': key, 'entry': name, 'field': '_blob_b64', 'new': b64(raw),
                             'label': f'{name}: Recruit, Follow, Standby, Disband'})
        plan.append(f'{name} ({hex(key)}): camp comrade, + Recruit / Follow / Standby / Disband')

    mod = {
        'format': 3, 'format_minor': 1,
        'modinfo': {'author': 'Khione', 'title': NAME, 'version': '0.3',
                    'description': 'Test: camp comrades (Gilbert, Morrow, ...) and five story Greymanes get Recruit, '
                                   'Follow, Standby and Disband.'},
        'targets': [
            {'file': 'dropsetinfo.pabgb', 'intents': drop_intents},
            {'file': 'interactioninfo.pabgb', 'intents': inter_intents},
            {'file': 'characterinfo.pabgb', 'intents': char_intents},
        ],
    }
    return mod, plan


def main():
    mod, plan = build()
    os.makedirs(OUT, exist_ok=True)
    archive = os.path.join(OUT, f'{NAME}.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(f'{NAME}/{NAME}.json', json.dumps(mod, indent=2))
    print('\n'.join(plan))
    print('test mod:', archive)


if __name__ == '__main__':
    main()
