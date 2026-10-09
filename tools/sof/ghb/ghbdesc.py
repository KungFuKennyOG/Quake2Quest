import sys,struct
sys.path.insert(0,'/tmp/claude-0/-home-claude/78e201ce-a82e-5df6-a319-9073f72cc0bd/scratchpad/tools')
import ghbstream as g
def desc(f):
    r=g.parse(f);d=open(f,'rb').read()
    s=g.Stream(d[:r['a']],r['pos_after_L'])
    o={'file':f,'a':r['a'],'b':r['b']}
    o['marker']=(s.int(),s.int(),s.int())
    o['c']=[s.int() for _ in range(20)]
    o['bounds']=g.read_bounds(s)
    o['v4']=[s.vec3() for _ in range(4)]
    has=s.bool(); o['has']=has
    if has:
        o['d0']=s.int();o['d1']=s.int()
        n=s.int();o['arr']=[s.int() for _ in range(n)]
        o['p8']=s.int();o['base']=s.int()
        m=s.int();o['recs']=[]
        for i in range(m):
            o['recs'].append(dict(i0=s.int(),i1=s.int(),sc=s.vec3(),org=s.vec3(),bits=(s.raw(1)[0],s.raw(1)[0],s.raw(1)[0]),s0=s.int(),s1=s.int()))
    o['a4']=s.int();o['tailsz']=s.int();o['rest']=d[s.p:r['a']]
    o['pos']=s.p
    return o
if __name__=='__main__':
    for f in sys.argv[1:]:
        o=desc(f)
        for k,v in o.items():
            if k=='recs':
                print('recs',len(v))
                for x in v[:6]:print('  ',x)
            elif k=='arr': print('arr',len(v),v[:40])
            else: print(k,v if not isinstance(v,bytes) else v.hex())
