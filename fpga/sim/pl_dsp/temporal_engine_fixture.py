"""Prepare and compare a complete engine temporal-state transaction.

Uses the independent temporal integer model. Native FFT outputs are compared
separately by check_engine.py; this never replaces a DUT FFT.
"""
from pathlib import Path
import argparse,sys,json,struct
import numpy as np
p=argparse.ArgumentParser();p.add_argument('fixture');p.add_argument('model_directory');p.add_argument('--check',action='store_true');a=p.parse_args()
root=Path(a.fixture);sys.path.insert(0,a.model_directory)
from temporal_model import Header,Cell,MAGIC,begin_frame,process_cell
cube=np.load(root/'vendor_cube_integer.npy');rd=np.sum(cube*cube,axis=(1,3),dtype=np.uint64)
def nms(data,r,k):
    cut=int(data[r,k])
    return cut>0 and all(int(data[rr,kk])<cut or (int(data[rr,kk])==cut and (rr,kk)>(r,k)) for rr in range(max(6,r-1),min(204,r+1)+1) for kk in [(k-1)%32,k,(k+1)%32] if (rr,kk)!=(r,k))
local=np.zeros((256,32),dtype=bool)
for r in range(6,205):
    for k in range(32):local[r,k]=nms(rd,r,k)
h=Header(MAGIC,1,1,9,900000,(900000,850000,800000,750000),15,0,4)
ctx=begin_frame(h,timestamp=1000000,epoch=1,acquisition=1,frame=10,validity=7,alpha=104700000,hold=131072)
background=max(1,int(np.median(rd)))
old=[];new=[];scores=np.zeros((256,32),dtype=np.uint64);fresh=np.zeros((256,32),dtype=bool)
for j,power in enumerate(rd.ravel()):
    r,k=divmod(j,32);power=int(power)
    cell=Cell(background,tuple(power*t//10 for t in (8,9,10,11)),0)
    state,score,evidence,flags=process_cell(ctx,cell,power,bool(local[r,k]))
    assert flags==0
    old.append(cell.pack());new.append(state.pack());scores[r,k]=score;fresh[r,k]=evidence
before=h.pack()+b''.join(old);after=ctx.header.pack()+b''.join(new)
def hexwords(raw):return ''.join(f'{w:016x}\n' for w in struct.unpack('<'+'Q'*(len(raw)//8),raw))
(root/'temporal_input.hex').write_text(hexwords(before));(root/'temporal_expected.hex').write_text(hexwords(after))
rank=[]
for r in range(6,205):
    for k in range(32):
        if not fresh[r,k] or not nms(scores,r,k):continue
        idx=[(rr,(k+dk)%32) for rr in range(max(6,r-10),min(204,r+10)+1) for dk in range(-5,6) if not(abs(rr-r)<=2 and abs(dk)<=1)]
        total=sum(int(scores[pos]) for pos in idx);count=len(idx);cut=int(scores[r,k])
        if (cut*count<<16)>total*262144:rank.append((cut,r,k,int(rd[r,k]),sum(int(rd[pos]) for pos in idx),count))
rank.sort(key=lambda x:(-x[0],x[1],x[2]))
expected=[(raw,r,k,noise,count) for _,r,k,raw,noise,count in rank[:32]]
result={'expected_total':len(rank),'expected_count':len(expected),'expected_records':expected,'state_bytes':len(after)}
if a.check:
    actual=(root/'rtl_temporal.hex').read_text().splitlines();want=hexwords(after).splitlines()
    result['state_bit_exact']=actual==want
    cw=[int(s,16) for s in (root/'rtl_candidates.hex').read_text().splitlines()]
    records=[(cw[9+4*j],cw[8+4*j]&0xffffffff,cw[8+4*j]>>32,cw[10+4*j],cw[11+4*j]&0xffffffff) for j in range(cw[0]&0xffffffff)]
    result['candidate_bit_exact']=records==expected and cw[0]>>32==len(rank)
    result['pass']=result['state_bit_exact'] and result['candidate_bit_exact']
    (root/'temporal_comparison.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))
    if not result['pass']:raise SystemExit(1)
else:
    (root/'temporal_expectation.json').write_text(json.dumps(result,indent=2));print('PASS prepared temporal state and expected current-power candidate records')
