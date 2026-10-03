import sys, os
sys.path.insert(0,'py')
import refl
def walk(o, path, hits, vals):
    if isinstance(o, dict):
        for k,v in o.items():
            if k=='@class': continue
            walk(v, path+[o.get('@class','')+'.'+k], hits, vals)
    elif isinstance(o, list):
        for i,v in enumerate(o): walk(v, path+['[%d]'%i], hits, vals)
    else:
        s=str(o).upper()
        for v in vals:
            if s in ('0X%X'%v, str(v)):
                hits.append(('/'.join(path), o))
fs=sys.argv[1:]
for f in fs:
    d=refl.decode(f)
    hits=[]
    walk(d,[],hits,[0xf43ef,0xf4409,0xf7b1a,0xf7b9a])
    print(os.path.basename(f))
    for h in hits: print('  ',h[0][-400:], h[1])
