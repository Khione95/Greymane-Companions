import json, struct, ci, st, re
names = {k:(s,e) for k,s,e in json.load(open('char_engnames.json',encoding='utf-8'))}
il = json.load(open('ilists.json',encoding='utf-8'))
MT={0x40:'Main',0x46:'Melee',0x47:'Range',0x48:'Worker',0x49:'GuestWorker',0x4a:'Shop',0xff:'-'}
_,DS=st.load('dropsetinfo')
dsn=[(r['key'],r['name']) for r in DS if r['name'] and 'Mercenary(' in r['name']]
def mtype(r):
    raw=r['raw']; k=r['key']
    pat=b'\x03\x32\x00\x00\x00'+struct.pack('<I',k)
    i=raw.find(pat)
    if i>0: return raw[i-1], i-1, True
    return None,None,False
sel=[]
for r in ci.RECS:
    try: h=ci.head(r)
    except Exception: continue
    k=r['key']; s,e=names.get(k,(r['name'],''))
    if h['faction?']==0xf4240 or s in ('NHW_Unique_Nairah_DualSword_444','NHM_Unique_Yann_DualDagger_443','NHM_Unique_Yann_DualDagger_621','NHM_Unique_GreyFur_Bram','NHM_Unique_GreyFur_Gunnar','NHM_Unique_GreyFur_Richard','NHM_Unique_Macduff_TwoHandSword_400'):
        if 'Contribution' in s or e in ('Greymane',) or "'s " in e or 'Friend' in e or 'Family' in e: continue
        t,off,hm=mtype(r)
        ils=il.get(hex(k),[None,None,[]])[2]
        flags=[x for x in ils if x.startswith(('Npc_Order','Npc_Hyosi','Mercenary_','Npc_Employ'))]
        ds=[hex(dk) for dk,dn in dsn if dn and (('('+s+')') in dn or ('('+s+';') in dn or ('('+s+')' in dn))]
        sel.append((e,s,k,t,hm,flags,ds,h['faction?']))
for e,s,k,t,hm,flags,ds,f in sorted(sel,key=lambda x:(x[3] or 0,x[0])):
    print(f"{e:18} {s:42} {k:#9x} type={MT.get(t,t) if t is not None else '?':6} fac={'G' if f==0xf4240 else hex(f)} ds={ds} {flags}")
