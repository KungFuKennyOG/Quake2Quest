import sys,struct
sys.path.insert(0,'/tmp/claude-0/-home-claude/78e201ce-a82e-5df6-a319-9073f72cc0bd/scratchpad/tools')
import ghbdesc as gd, ghbverts as gv
def load(f):
    o=gd.desc(f);d=open(f,'rb').read();bulk=d[o['a']:]
    c=o['c'];T=c[11:20] if False else None
    cnt=c[:11];T=c[11:]
    g=bulk[o['tailsz']:]
    nuv,nsurf,ncmd,ncor=c[2],c[4],c[5],c[6]
    uv=[struct.unpack_from('<2f',g,T[0]+8*i) for i in range(nuv)]
    off=T[2]
    surf=[struct.unpack_from('<3H',g,T[3]+6*i) for i in range(nsurf)]
    cmd=list(struct.unpack_from('<%dh'%ncmd,g,T[4]))
    cor=[struct.unpack_from('<3h',g,T[5]+6*i) for i in range(ncor)]
    B=bulk[o['base']:]
    frames=[]  # list of per-frame vertex arrays
    for r in o['recs']:
        q,p=gv.decode_record(B,r,o['d0'])
        for fr in range(r['i1']):
            frames.append([tuple(q[v][fr][k]*r['sc'][k]+r['org'][k] for k in range(3)) for v in range(o['d0'])])
    return dict(o=o,uv=uv,surf=surf,cmd=cmd,cor=cor,frames=frames)
def tris(m):
    for mat,st,ln in m['surf']:
        l=m['cmd'][st:st+ln];i=0
        while i<len(l):
            k=l[i];i+=1
            if k==0:break
            n=abs(k);v=l[i:i+n];i+=n
            if len(v)<n:break
            if k<0:
                for j in range(1,n-1):yield mat,(v[0],v[j],v[j+1])
            else:
                for j in range(n-2):
                    yield mat,((v[j],v[j+1],v[j+2]) if j%2==0 else (v[j+1],v[j],v[j+2]))
