import st, struct
ks, RECS = st.load('characterinfo')
BY = {r['key']: r for r in RECS}
def head(r):
    raw = r['raw']; p = 4
    L = struct.unpack_from('<I', raw, p)[0]; p += 4 + L + 1
    for _ in range(2):
        t = raw[p]; p += 1
        if t:
            p += 8; L = struct.unpack_from('<I', raw, p)[0]; p += 4 + L
    p += 8
    L = struct.unpack_from('<I', raw, p)[0]; p += 4 + L
    o = {'after_edit': p}
    o['spawn_actor_type'] = raw[p]; o['none_player_sub_type'] = raw[p+1]
    o['equip_info'], o['npc_info'], o['vehicle_info'], o['call_mercenary_cool_time'] = struct.unpack_from('<4I', raw, p+2)
    o['faction?'] = struct.unpack_from('<I', raw, p+0x29)[0]
    return o
