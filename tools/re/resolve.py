import os,struct,re
B=os.environ.get('RE_DIR','.')+'/'  # dir holding ota_0.bin and all_decomp.c; outputs resolved.c
d=open(B+'ota_0.bin','rb').read()
n=d[1]; off=24; segs=[]
for i in range(n):
    a,l=struct.unpack('<II',d[off:off+8]); segs.append((a,l,off+8)); off+=8+l
def rd(a,n=4):
    for s,l,o in segs:
        if s<=a<s+l: return d[o+a-s:o+a-s+n]
    return None
def u32(a):
    b=rd(a); return struct.unpack('<I',b)[0] if b and len(b)==4 else None
def cstr(a):
    for s,l,o in segs:
        if s<=a<s+l:
            e=d.find(b'\0',o+a-s,o+l)
            t=d[o+a-s:e]
            if 1<=len(t)<200 and all(32<=c<127 or c in(9,10,27) for c in t): return re.sub(r'\x1b\[[0-9;]*m','',t.decode()).replace('\n','\\n')
    return None
if __name__=='__main__':
    t=open(B+'all_decomp.c').read()
    def sub(m):
        kind,addr=m.group(1),int(m.group(2),16); v=u32(addr)
        if v is None: return m.group(0)
        if kind=='PTR_DAT_':
            s=cstr(v); return '"%s"'%s if s else 'P(0x%x)'%v
        if kind=='PTR_FUN_': return 'F_%x'%v
        if kind=='DAT_': return 'K(0x%x)'%v
        if kind=='PTR_DAT_': return 'P(0x%x)'%v
    t=re.sub(r'\b(PTR_DAT_|PTR_FUN_|DAT_)(4[0-9a-f]{7})\b',sub,t)
    open(B+'resolved.c','w').write(t)
