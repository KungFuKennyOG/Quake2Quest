"""Export a decoded .ghb skinned character to JSON (+ per-frame vertex arrays and bolt tracks)."""
import sys,json,struct,math
sys.path.insert(0,'/tmp/claude-0/-home-claude/78e201ce-a82e-5df6-a319-9073f72cc0bd/scratchpad/tools')
import ghbskin as s, ghbstream as gs, ghbdesc as gd
def bolts(f):
    r=gs.parse(f,verbose=False);d=open(f,'rb').read();o=gd.desc(f)
    A=d[o['a']:][:o['base']];nfr=r['i13c'];out={}
    for n in r['nodes']:
        inv,sc,org=n['v'];mn,mx=n['bounds'][2],n['bounds'][3]
        w=[round(math.log2(round((mx[i]-mn[i])/sc[i]+1))) if sc[i]>0 and mx[i]>mn[i] else 0 for i in range(3)]
        tr=[]
        for fr in range(nfr):
            dw=struct.unpack_from('<I',A,n['bulk_ofs']+fr*8+4)[0];sh=0;p=[]
            for i in range(3):
                p.append(((dw>>sh)&((1<<w[i])-1))*sc[i]+org[i]);sh+=w[i]
            tr.append(p)
        out[n['name']]=tr
    return r,out
def export(f,path):
    m=s.load(f);r,b=bolts(f);dec=lambda x:~x if x<0 else x
    names=[n['name'] for n in r['nodes_108']]
    tris=[(mat,[ (dec(m['cor'][c][0]),dec(m['cor'][c][2])) for c in t]) for mat,t in s.tris(m)]
    json.dump(dict(src=r['src'],surfaces=names,uv=m['uv'],tris=tris,frames=[[list(map(lambda x:round(x,4),v)) for v in fr] for fr in m['frames']],bolts=b,sequences=[o['name'] for o in r['objs']]),open(path,'w'))
if __name__=='__main__':export(sys.argv[1],sys.argv[2])
