"""Writes PKR_TEST_PLAN files (every map, every person/sign next to it), split in n chunks: python3 gen_sweep_plans.py [n]"""
# test plans: every map, every person/sign/item next to it
import json,struct,sys,os
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
lay={l.get('id'):l for l in json.load(open('data/layouts/layouts.json'))['layouts'] if l.get('id')}
g=json.load(open('data/maps/map_groups.json'))
plans=[]
for gi,gr in enumerate(g['group_order']):
    for mi,m in enumerate(g[gr]):
        d=json.load(open('data/maps/%s/map.json'%m)); L=lay.get(d['layout'])
        if not L: continue
        blk=open(L['blockdata_filepath'],'rb').read(); w=L['width']; h=L['height']
        def free(x,y):
            return 0<=x<w and 0<=y<h and not (struct.unpack_from('<H',blk,(y*w+x)*2)[0]>>10)&3
        occ={(o['x'],o['y']) for o in d.get('object_events',[])}
        def near(cx,cy):
            for r in range(0,30):
                for y in range(cy-r,cy+r+1):
                    for x in range(cx-r,cx+r+1):
                        if free(x,y) and (x,y) not in occ: return x,y
            return None
        st=near(w//2,h//2)
        if not st: continue
        steps=['note map %s'%m,'warp %d %d %d %d'%(gi,mi,st[0],st[1]),'walk 120']
        targets=[(o['x'],o['y'],'obj') for o in d.get('object_events',[])]+[(b['x'],b['y'],'bg') for b in d.get('bg_events',[])]
        for (tx,ty,k) in targets:
            for dx,dy,face in ((0,1,2),(-1,0,4),(1,0,3),(0,-1,1)):
                x,y=tx+dx,ty+dy
                if free(x,y) and (x,y) not in occ:
                    steps+=['note %s %s %d,%d'%(m,k,tx,ty),'warp %d %d %d %d'%(gi,mi,x,y),'face %d'%face,'mash 300','back 120']
                    break
        plans.append((m,steps))
n=int(sys.argv[1]) if len(sys.argv)>1 else 16
OUT=os.environ.get('PKR_TEST_DIR','/tmp/pkr')
os.makedirs(OUT,exist_ok=True)
chunks=[[] for _ in range(n)]
for i,(m,st) in enumerate(plans): chunks[i%n]+=st
for i,c in enumerate(chunks): open(os.path.join(OUT,'plan%02d.txt'%i),'w').write('\n'.join(c)+'\n')
print(len(plans),'maps', sum(len(c) for c in chunks),'steps')
