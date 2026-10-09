import sys,struct
sys.path.insert(0,'/tmp/claude-0/-home-claude/78e201ce-a82e-5df6-a319-9073f72cc0bd/scratchpad/tools')
import numpy as np, ghbskin as s, ghbdesc as gd
def vnormals(m,fi):
    P=np.array(m['frames'][fi]);N=np.zeros_like(P)
    for mat,t in s.tris(m):
        i=[ (c if False else None) for c in t]
        v=[(m['cor'][c][0] if m['cor'][c][0]>=0 else ~m['cor'][c][0]) for c in t]
        n=np.cross(P[v[1]]-P[v[0]],P[v[2]]-P[v[0]])
        for k in v:N[k]+=n
    L=np.linalg.norm(N,axis=1,keepdims=True);L[L==0]=1
    return N/L
def norm_indices(f,m):
    o=m['o'];d=open(f,'rb').read();B=d[o['a']:][o['base']:]
    out=[];nv=o['d0']
    for r in o['recs']:
        G=r['i1'];s1=r['s1']
        per=G if G<3 else (2 if G<5 else 3)
        out.append(np.frombuffer(B[s1:s1+nv*per],dtype=np.uint8).reshape(nv,per))
    return out
