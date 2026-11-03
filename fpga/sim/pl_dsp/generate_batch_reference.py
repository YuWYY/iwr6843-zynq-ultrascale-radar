"""Independent vendor-FFT and temporal expectations for 64 sequential jobs.
Addresses and 100ms timestamps are test assumptions, never production settings.
"""
from pathlib import Path
import argparse,ctypes as C,hashlib,json,os,struct,sys
import numpy as np
p=argparse.ArgumentParser();p.add_argument('output');p.add_argument('fixture');p.add_argument('models');p.add_argument('vendor');a=p.parse_args()
out=Path(a.output);out.mkdir(parents=True,exist_ok=True);fixture=Path(a.fixture);vendor=Path(a.vendor)
sys.path.insert(0,a.models)
from numeric_matrix import fixed_pipeline,Generics,Inputs,Outputs,I,P,cases,DEFAULT_RAW,DEFAULT_CFG
from temporal_model import Header,Cell,begin_frame,process_cell,BASELINE_FALLBACK,elapsed_alpha
dll_dir=os.add_dll_directory(str(vendor));lib=C.CDLL(str(vendor/'libIp_xfft_v9_1_bitacc_cmodel.dll'))
lib.xilinx_ip_xfft_v9_1_create_state.argtypes=[Generics];lib.xilinx_ip_xfft_v9_1_create_state.restype=C.c_void_p
lib.xilinx_ip_xfft_v9_1_bitacc_simulate.argtypes=[C.c_void_p,Inputs,P(Outputs)];lib.xilinx_ip_xfft_v9_1_bitacc_simulate.restype=I
lib.xilinx_ip_xfft_v9_1_destroy_state.argtypes=[C.c_void_p]
def write_words(path,words):path.write_text(''.join(f'{int(w):016x}\n' for w in words))
def complex_words(iq):
    z=iq.reshape(-1,2);return (z[:,0].astype(np.uint64)&np.uint64(0xffffffff))|((z[:,1].astype(np.uint64)&np.uint64(0xffffffff))<<np.uint64(32))
def raw_words(raw):return struct.unpack('<'+'Q'*(len(raw)//8),raw)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def local_mask(data):
    result=np.zeros((256,32),dtype=bool)
    for r in range(6,205):
        for k in range(32):
            cut=int(data[r,k]);result[r,k]=cut>0 and all(int(data[rr,kk])<cut or (int(data[rr,kk])==cut and (rr,kk)>(r,k)) for rr in range(max(6,r-1),min(204,r+1)+1) for kk in [(k-1)%32,k,(k+1)%32] if (rr,kk)!=(r,k))
    return result
def detect(data,raw,gate,threshold):
    local=local_mask(data);rank=[]
    for r,k in zip(*np.nonzero(local&gate)):
        r,k=int(r),int(k);ids=[(rr,(k+dk)%32) for rr in range(max(6,r-10),min(204,r+10)+1) for dk in range(-5,6) if not(abs(rr-r)<=2 and abs(dk)<=1)]
        total=sum(int(data[pos]) for pos in ids);cut=int(data[r,k]);count=len(ids)
        if (cut*count<<16)>total*threshold:rank.append((cut,r,k,int(raw[r,k]),sum(int(raw[pos]) for pos in ids),count))
    rank.sort(key=lambda x:(-x[0],x[1],x[2]));return rank
base=np.load(fixture/'raw_iq.npy').astype(np.int64);variants=[];manifest={'status':'RUNNING','variants':[],'jobs':[],'timestamp_is_synthetic':True,'test_only_addresses':True}
for variant in range(13):
    gain=(95,100,105)[variant%3];rot=variant//3
    z=np.trunc(base*gain/100).astype(np.int64)
    for _ in range(rot):z=np.stack((-z[...,1],z[...,0]),axis=-1)
    definition={'source':'synthetic','gain_percent':gain,'rotation_quarters':rot}
    if variant==12:
        _,z,definition=next(cases(DEFAULT_RAW,DEFAULT_CFG))
    ranged,cube,stats=fixed_pipeline(z.astype(np.int16),lib)
    if stats['range_fft_overflows'] or stats['doppler_fft_overflows']:raise ValueError('unexpected overflow')
    rd=np.sum(cube*cube,axis=(1,3),dtype=np.uint64);spec=np.sum(ranged*ranged,axis=(0,1,3),dtype=np.uint64)
    prefix=out/f'variant{variant:02d}';prefix.mkdir(exist_ok=True)
    for name,words in [('input',complex_words(z)),('range',complex_words(ranged)),('cube',complex_words(cube)),('rd',rd.ravel()),('spectrum',spec)]:write_words(prefix/f'{name}.hex',words)
    variants.append((rd,local_mask(rd)));manifest['variants'].append({'variant':variant,'definition':definition,'files':{p.name:sha(p) for p in prefix.glob('*.hex')}})
    print(f'REFERENCE variant={variant}',flush=True)
initial=b''.join(struct.pack('<Q',int(s,16)) for s in (fixture/'temporal_input.hex').read_text().splitlines())
write_words(out/'initial_temporal.hex',raw_words(initial));header=Header.unpack(initial[:64]);states=[Cell.unpack(initial[64+j*64:128+j*64]) for j in range(8192)]
for job in range(64):
    variant=12 if job==63 else job%12;rd,local=variants[variant];timestamp=1000000+job*100000+(700000 if job>=32 else 0)
    epoch=3 if job==63 else (1 if job<48 else 2);acq=1+job//16;frame=10+job%16;alpha=elapsed_alpha(timestamp-header.last_us)
    context=begin_frame(header,timestamp=timestamp,epoch=epoch,acquisition=acq,frame=frame,validity=7,alpha=alpha,hold=131072)
    scores=np.zeros((256,32),dtype=np.uint64);fresh=np.zeros((256,32),dtype=bool);updated=[];flags=context.flags
    for index,power in enumerate(rd.ravel()):
        r,k=divmod(index,32);cell,score,evidence,cell_flags=process_cell(context,states[index],int(power),bool(local[r,k]));updated.append(cell);scores[r,k]=score;fresh[r,k]=evidence;flags|=cell_flags
    if flags&0x30c:raise ValueError(f'unexpected temporal error flags {flags:x}')
    fallback=bool(flags&BASELINE_FALLBACK)
    rank=detect(rd if fallback else scores,rd,np.ones_like(fresh) if fallback else fresh,1038676 if fallback else 262144)
    words=np.zeros(136,dtype=np.uint64);count=min(32,len(rank));words[0]=count|(len(rank)<<32);words[1]=len(rank)-count;words[2]=3456<<32;words[4]=(job+1)|(epoch<<32);words[5]=flags
    for j,(_,r,k,power,noise,n) in enumerate(rank[:32]):words[8+4*j:12+4*j]=[r|(k<<32),power,noise,n]
    prefix=out/f'job{job:02d}';prefix.mkdir(exist_ok=True);write_words(prefix/'candidates.hex',words)
    raw=context.header.pack()+b''.join(cell.pack() for cell in updated);write_words(prefix/'temporal.hex',raw_words(raw))
    meta=[flags,count,len(rank),alpha,timestamp,epoch,acq,frame];write_words(prefix/'meta.hex',meta)
    manifest['jobs'].append({'job':job+1,'variant':variant,'timestamp_us':timestamp,'config_epoch':epoch,'acquisition_id':acq,'frame_index':frame,'flags':flags,'candidate_count':count,'input_slots':8 if job<32 else 16,'input_slot':job%(8 if job<32 else 16),'files':{p.name:sha(p) for p in prefix.glob('*.hex')}})
    states=updated;header=context.header
    print(f'REFERENCE job={job+1} candidates={count} flags={flags:08x}',flush=True)
manifest['status']='PASS';(out/'reference_manifest.json').write_text(json.dumps(manifest,indent=2))
