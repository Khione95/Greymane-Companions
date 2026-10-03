import json, struct, ci
names = {k:(s,e) for k,s,e in json.load(open('char_engnames.json',encoding='utf-8'))}
il = json.load(open('ilists.json',encoding='utf-8'))
out=[]
for r in ci.RECS:
    try: h=ci.head(r)
    except Exception: continue
    if h['faction?']==0xf4240:
        k=r['key']; s,e=names.get(k,(r['name'],''))
        out.append((k,s,e,h['npc_info'],h['equip_info']))
print(len(out))
for k,s,e,npc,eq in sorted(out,key=lambda x:x[2] or ''):
    print(f"{k:>8} {hex(k):>8} {e!r:28} {s:50} npc={npc:#x}")
