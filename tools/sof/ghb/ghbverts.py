import sys,struct
sys.path.insert(0,'/tmp/claude-0/-home-claude/78e201ce-a82e-5df6-a319-9073f72cc0bd/scratchpad/tools')
import ghbdesc as gd

class Bits:
    def __init__(s,b,off=0): s.b=b; s.p=off*8
    def r(s,n):
        if n==0: return 0
        v=0
        for i in range(n):  # LSB-first
            v|=((s.b[s.p>>3]>>(s.p&7))&1)<<i; s.p+=1
        return v
    def sm(s,n):  # magnitude then sign bit
        m=s.r(n); neg=s.r(1); return -m if neg else m

def decode_record(B,rec,nverts):
    """returns q[v][f][c] ints, bit position at end"""
    G=rec['i1']; bits=rec['bits']
    br=Bits(B,rec['s0'])
    q=[]
    d8=1
    for v in range(nverts):
        if (1<<d8)<=v-1: d8+=1
        cur=[[0,0,0] for _ in range(G)]
        if G<2:
            for c in range(3): cur[0][c]=br.r(bits[c])
        else:
            mode=br.r(2)
            if mode==0:
                wd=[0,0,0]
                for c in range(3):
                    cur[0][c]=br.r(bits[c]); wd[c]=br.r(4)
                for f in range(1,G):
                    for c in range(3):
                        if wd[c]: cur[f][c]=cur[f-1][c]+br.r(wd[c]+1)-(1<<wd[c])
                        else: cur[f][c]=cur[f-1][c]
            else:
                ref=br.r(d8) if d8 else 0
                e0=br.r(4)
                a8=[0,0,0];base=[0,0,0]
                for c in range(3):
                    a8[c]=br.r(4)
                    if e0: base[c]=br.sm(e0)
                for f in range(G):
                    for c in range(3):
                        r=q[ref][f][c]
                        cur[f][c]=r+base[c]+(br.r(a8[c]) if a8[c] else 0)
        q.append(cur)
    return q,br.p

if __name__=='__main__':
    f=sys.argv[1]
    o=gd.desc(f);d=open(f,'rb').read()
    bulk=d[o["a"]:];B=bulk[o["base"]:]
    nv=o['d0']
    for k,r in enumerate(o['recs']):
        try:
            q,p=decode_record(B,r,nv)
            used=(p+7)//8+4
            print(k,'G',r['i1'],'bytes used',used,'expected',r['s1'],'OK' if used==r['s1'] else 'MISMATCH')
        except Exception as e:
            print(k,'ERR',e)
