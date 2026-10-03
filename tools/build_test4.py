"""Test 4: the Recruit and Dismiss talk options on the camp comrades, for the
plugin to act on. A DMM v3 JSON mod (it replaces Test 3: uninstall that first).

Two new interactions (research/notes.md, gca wf4/wf5):
  Recruit 0xF5010 - a copy of the game's Follow (Npc_Order_Follow) that reads
    "Recruit" (Npc_Employ's text) and shows only while she does NOT follow you
    (CheckAccompanyType None instead of FollowDefensive / Wait). It keeps
    Follow's AI event, so her own AI can start following at once.
  Dismiss 0xF5011 - a copy of Standby (Npc_Order_Wait) that reads "Disband"
    (Npc_Order_Disband's text), shows while she follows you in any way (Not
    CheckAccompanyType None) and sends no AI event: the plugin takes her off the
    list. On Follow's key (InteractionA), as the game's Follow is.
The 38 camp comrades (Gilbert, Morrow, ...) get Recruit, Dismiss, and the
game's own Follow and Standby (for after she is recruited).
Output: dist/Greymane Companions Test 4.zip (a folder with the JSON).
Usage: python build_test4.py"""
import base64
import json
import os
import struct
import zipfile

import build_test
import tables

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'dist')
NAME = 'Greymane Companions Test 4'

FOLLOW, WAIT, EMPLOY, DISBAND = 0xF4251, 0xF4253, 0xF4292, 0xF425B
RECRUIT, DISMISS = 0xF5010, 0xF5011          # above the game's highest (0xF4391); the plugin knows them
RECRUIT_NAME, DISMISS_NAME = 'Npc_Greymane_Recruit', 'Npc_Greymane_Dismiss'

# CheckAccompanyType(FollowDefensive) || CheckAccompanyType(Wait) -> (None) || (None): not on the list.
FOLLOW_COND = bytes.fromhex('0397010300' '0397010100')
RECRUIT_COND = bytes.fromhex('0397010000' '0397010000')
# (!Dead && (Acc==FollowOffensive || Acc==FollowDefensive)) && !CCTime -> (!Dead && !(Acc==None)) && !CCTime.
WAIT_TARGET = bytes.fromhex('01 01 02 03 10 00 00 00 00 03 97 01 02 00 03 97 01 03 00 02 03 2f 00 00')
DISMISS_TARGET = bytes.fromhex('01 01 02 03 10 00 00 00 02 03 97 01 00 00 02 03 2f 00 00')
EVENT_FOLLOW, EVENT_WAIT = 0xF5BFEE82, 0xEEBE079C
# _inputKeyMapName: "InteractionB" (Standby's) -> "InteractionA" (Follow's).
KEY_B = bytes.fromhex('51 c3') + struct.pack('<I', 0x5C3952A4)
KEY_A = bytes.fromhex('51 c3') + struct.pack('<I', 0xFC15F4E2)

u32 = build_test.u32
text = build_test.text
loc_string = build_test.loc_string


def label(owner):
    return loc_string(0x60, owner, (owner << 32) | 0x60)


def renamed(raw, key, name):
    old = struct.unpack_from('<I', raw, 4)[0]
    return u32(key) + text(name) + raw[8 + old:]


def replace_once(raw, old, new, what):
    if raw.count(old) != 1:
        raise SystemExit(f'{what}: found {raw.count(old)} times, not once')
    return raw.replace(old, new)


def recruit_record(follow):
    raw = renamed(follow, RECRUIT, RECRUIT_NAME)
    raw = replace_once(raw, label(FOLLOW), label(EMPLOY), 'Follow label')
    raw = replace_once(raw, FOLLOW_COND, RECRUIT_COND, 'Follow condition')
    replace_once(raw, u32(EVENT_FOLLOW), u32(EVENT_FOLLOW), 'Follow event')
    return raw


def dismiss_record(wait):
    raw = renamed(wait, DISMISS, DISMISS_NAME)
    raw = replace_once(raw, label(WAIT), label(DISBAND), 'Standby label')
    raw = replace_once(raw, WAIT_TARGET, DISMISS_TARGET, 'Standby condition')
    raw = replace_once(raw, u32(EVENT_WAIT), bytes(4), 'Standby event')
    raw = replace_once(raw, KEY_B, KEY_A, 'Standby key')
    return raw


def b64(raw):
    return base64.b64encode(raw).decode()


def build():
    chars = tables.Table('characterinfo')
    inter = tables.Table('interactioninfo')
    build_test.KNOWN_INTERACTIONS = set(inter.raw)

    for key, name in ((RECRUIT, RECRUIT_NAME), (DISMISS, DISMISS_NAME)):
        if key in inter.raw or name in inter.by_name:
            raise SystemExit(f'{hex(key)} / {name} is taken')

    recruit = recruit_record(inter.raw[FOLLOW])
    dismiss = dismiss_record(inter.raw[WAIT])
    inter_intents = [
        {'op': 'new_record', 'new_key': RECRUIT, 'label': 'Recruit (Greymane camp comrades)',
         'template': {'key': RECRUIT, '_blob_fallback': True, '_blob_b64': b64(recruit)}},
        {'op': 'new_record', 'new_key': DISMISS, 'label': 'Dismiss (Greymane camp comrades)',
         'template': {'key': DISMISS, '_blob_fallback': True, '_blob_b64': b64(dismiss)}},
    ]

    char_intents, plan = [], []
    for key in build_test.camp_comrades(chars):
        name = chars.names[key]
        raw = build_test.add_interactions(chars.raw[key], [RECRUIT, DISMISS, FOLLOW, WAIT])
        char_intents.append({'op': 'set', 'key': key, 'entry': name, 'field': '_blob_b64', 'new': b64(raw),
                             'label': f'{name}: Recruit, Dismiss, Follow, Standby'})
        plan.append(f'{name} ({hex(key)}): + Recruit / Dismiss / Follow / Standby')

    mod = {
        'format': 3, 'format_minor': 1,
        'modinfo': {'author': 'Khione', 'title': NAME, 'version': '0.4',
                    'description': 'Test: the Greymane camp comrades get Recruit and Dismiss (with the Greymane '
                                   'Companions plugin), and the game\'s Follow and Standby.'},
        'targets': [
            {'file': 'interactioninfo.pabgb', 'intents': inter_intents},
            {'file': 'characterinfo.pabgb', 'intents': char_intents},
        ],
    }
    plan.append(f'Recruit {hex(RECRUIT)}: {len(recruit)} bytes; Dismiss {hex(DISMISS)}: {len(dismiss)} bytes')
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
