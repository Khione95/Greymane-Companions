import os, re, struct, sys
B = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tables/gamedata/binarystaticinfo__/bin')

def load(t):
    h = open(os.path.join(B, t + '.staticinfoheader'), 'rb').read()
    b = open(os.path.join(B, t + '.staticinfobody'), 'rb').read()
    n = struct.unpack_from('<H', h)[0]
    ks = (len(h) - 2) // n - 4 if n else 0
    if n and (len(h) - 2) % n:
        # maybe u32 count
        n = struct.unpack_from('<I', h)[0]; ks = (len(h) - 4) // n - 4; base = 4
    else:
        base = 2
    ents = []
    for i in range(n):
        k = int.from_bytes(h[base + i*(ks+4): base + i*(ks+4)+ks], 'little')
        o = struct.unpack_from('<I', h, base + i*(ks+4)+ks)[0]
        ents.append((k, o))
    offs = sorted(o for _, o in ents) + [len(b)]
    nxt = {offs[i]: offs[i+1] for i in range(len(offs)-1)}
    recs = []
    for k, o in ents:
        r = b[o:nxt[o]]
        # name: key then u32 len + str
        name = None
        p = ks
        if len(r) >= p + 4:
            L = struct.unpack_from('<I', r, p)[0]
            if 0 < L < 200 and p + 4 + L <= len(r):
                name = r[p+4:p+4+L].decode('utf-8', 'replace')
        recs.append({'key': k, 'off': o, 'raw': r, 'name': name})
    return ks, recs

def strs(r, n=3):
    return [m.group().decode('latin1') for m in re.finditer(rb'[\x20-\x7e]{%d,}' % n, r)]

if __name__ == '__main__':
    t = sys.argv[1]
    ks, recs = load(t)
    print(t, 'keysize', ks, 'records', len(recs))
    for r in recs[: int(sys.argv[2]) if len(sys.argv) > 2 else 99999]:
        print(hex(r['key']), r['name'], len(r['raw']), '|', ' '.join(s for s in strs(r['raw'], 4) if s != r['name'])[:300])
