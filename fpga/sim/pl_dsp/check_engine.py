"""Compare production native-XFFT RTL with AMD's independent bit-accurate DLL
and the existing chain's float normalization. Never substitutes a fake FFT."""
from pathlib import Path
import argparse,ctypes as C,os,json,math
import numpy as np
p=argparse.ArgumentParser();p.add_argument('fixture');p.add_argument('vendor');p.add_argument('--model-only',action='store_true');p.add_argument('--candidate-reference');a=p.parse_args()
root=Path(a.fixture);vendor=Path(a.vendor);dll_dir=os.add_dll_directory(str(vendor))
lib=C.CDLL(str(vendor/'libIp_xfft_v9_1_bitacc_cmodel.dll'))
I=C.c_int;D=C.c_double;P=C.POINTER
class Generics(C.Structure):_fields_=[(s,I) for s in ['nfft','arch','has_nfft','float','input_width','twiddle_width','scaling','bfp','rounding','nssr','inverse']]
class Inputs(C.Structure):_fields_=[('nfft',I),('re',P(D)),('re_size',I),('im',P(D)),('im_size',I),('schedule',P(I)),('schedule_size',I),('direction',I)]
class Outputs(C.Structure):_fields_=[('re',P(D)),('re_size',I),('im',P(D)),('im_size',I),('exponent',I),('overflow',I)]
lib.xilinx_ip_xfft_v9_1_create_state.argtypes=[Generics];lib.xilinx_ip_xfft_v9_1_create_state.restype=C.c_void_p
lib.xilinx_ip_xfft_v9_1_bitacc_simulate.argtypes=[C.c_void_p,Inputs,P(Outputs)];lib.xilinx_ip_xfft_v9_1_bitacc_simulate.restype=I
lib.xilinx_ip_xfft_v9_1_destroy_state.argtypes=[C.c_void_p]
class FFT:
    def __init__(self,n):
        self.n=n;self.log=int(math.log2(n));self.state=lib.xilinx_ip_xfft_v9_1_create_state(Generics(self.log,2,0,0,24,24,1,0,1,1,0))
        if not self.state:raise RuntimeError('vendor create failed')
        self.scale=(I*self.log)(*([1]*self.log))
    def run(self,z):
        re=np.ascontiguousarray(z[:,0]/2**23,dtype=np.float64);im=np.ascontiguousarray(z[:,1]/2**23,dtype=np.float64)
        ore=np.zeros(self.n);oim=np.zeros(self.n)
        inp=Inputs(self.log,re.ctypes.data_as(P(D)),self.n,im.ctypes.data_as(P(D)),self.n,self.scale,len(self.scale),1)
        out=Outputs(ore.ctypes.data_as(P(D)),self.n,oim.ctypes.data_as(P(D)),self.n,0,0)
        if lib.xilinx_ip_xfft_v9_1_bitacc_simulate(self.state,inp,C.byref(out)) or out.overflow:raise RuntimeError('vendor FFT failed/overflow')
        return np.rint(np.stack((ore,oim),axis=-1)*2**23).astype(np.int64)
    def close(self):lib.xilinx_ip_xfft_v9_1_destroy_state(self.state)
def rne(x,s):
    q=x>>s;rem=x-(q<<s);half=1<<(s-1)
    return q+((rem>half)|((rem==half)&((q&1)!=0)))
def words(name):return np.array([int(s,16) for s in (root/name).read_text().splitlines()],dtype=np.uint64)
def complex_slots(name,shape):
    w=words(name);i=(w&np.uint64(0xffffffff)).astype(np.uint32).view(np.int32).astype(np.int64)
    q=(w>>np.uint64(32)).astype(np.uint32).view(np.int32).astype(np.int64)
    if np.max(abs(i))>=2**23 or np.max(abs(q))>=2**23:raise AssertionError('sign-extension/24bit slots')
    return np.stack((i,q),axis=-1).reshape(*shape,2)
iq=np.load(root/'raw_iq.npy').astype(np.int64)<<7
win=np.rint(np.hanning(256)*2**24).astype(np.int64)
slow=np.rint(np.hanning(32)*2**24).astype(np.int64)
f=FFT(256);g=FFT(32);r=np.empty_like(iq)
for chirp in range(96):
    for rx in range(4):
        z=iq[chirp,rx];z=z-rne(np.sum(z,axis=0),8);z=rne(z*win[:,None],24)
        r[chirp,rx]=f.run(z)
c=np.empty((256,12,32,2),dtype=np.int64)
for chirp in range(96):
    loop,slot=divmod(chirp,3);tx=[0,2,1][slot];c[:,tx*4:tx*4+4,loop]=r[chirp].transpose(1,0,2)
for rb in range(256):
    for channel in range(12):c[rb,channel]=g.run(rne(c[rb,channel]*slow[:,None],24))
f.close();g.close()
np.save(root/'vendor_range_integer.npy',r);np.save(root/'vendor_cube_integer.npy',c)
if a.model_only:
    print('PASS vendor bit-accurate model generated384 range and3072 Doppler transforms');raise SystemExit(0)
rtl_r=complex_slots('rtl_range.hex',(96,4,256));rtl_c=complex_slots('rtl_cube.hex',(256,12,32))
spec=np.sum(r*r,axis=(0,1,3),dtype=np.uint64);rd=np.sum(c*c,axis=(1,3),dtype=np.uint64)
result={'bit_exact_range':bool(np.array_equal(r,rtl_r)),'bit_exact_cube':bool(np.array_equal(c,rtl_c)),
 'bit_exact_spectrum':bool(np.array_equal(spec,words('rtl_spectrum.hex'))),'bit_exact_rd':bool(np.array_equal(rd.ravel(),words('rtl_rd.hex'))),
 'range_max_integer_error':int(np.max(abs(r-rtl_r))),'cube_max_integer_error':int(np.max(abs(c-rtl_c)))}
actual=(rtl_c[...,0]+1j*rtl_c[...,1])/2**22*(256/127.5)*32
reference=np.load(root/'cube_reference.npy')*(256/127.5)*32
result['cube_FS_normalized_RMS']=float(np.sqrt(np.mean(abs(actual-reference)**2)))
scale=(256/127.5)**2/384
reference_s=np.load(root/'spectrum_reference.npy')*scale
actual_s=words('rtl_spectrum.hex').astype(float)/2**44*scale
mask=reference_s>1e-8
result['spectrum_above_minus80dB_max_error_dB']=float(np.max(abs(10*np.log10(actual_s[mask]/reference_s[mask]))))
# Independent original-CFAR integer arithmetic, including all ties and clipping.
rank=[];threshold=1038676
for rb in range(6,205):
    for k in range(32):
        cut=int(rd[rb,k])
        if not cut:continue
        ismax=all(int(rd[rr,kk])<cut or (int(rd[rr,kk])==cut and (rr,kk)>(rb,k)) for rr in range(max(6,rb-1),min(204,rb+1)+1) for kk in [(k-1)%32,k,(k+1)%32] if (rr,kk)!=(rb,k))
        if not ismax:continue
        cells=[int(rd[rr,(k+dk)%32]) for rr in range(max(6,rb-10),min(204,rb+10)+1) for dk in range(-5,6) if not(abs(rr-rb)<=2 and abs(dk)<=1)]
        total=sum(cells);count=len(cells)
        if (cut*count<<16)>total*threshold:rank.append((cut,rb,k,total,count))
rank.sort(key=lambda x:(-x[0],x[1],x[2]));cw=words('rtl_candidates.hex')
reported_count=int(cw[0])&0xffffffff;reported_total=int(cw[0])>>32
expected=rank[:32];records=[]
for j in range(reported_count):
    w=[int(x) for x in cw[8+4*j:12+4*j]];records.append((w[1],w[0]&0xffffffff,w[0]>>32,w[2],w[3]&0xffffffff))
expected_total=len(rank)
if a.candidate_reference:
    temporal=json.loads(Path(a.candidate_reference).read_text());expected=[tuple(x) for x in temporal['expected_records']];expected_total=temporal['expected_total']
result['cfar_exact']=records==expected and reported_count==len(expected) and reported_total==expected_total
result['candidate_count']=reported_count;result['candidate_total']=reported_total
result['pass']=all(result[k] for k in ['bit_exact_range','bit_exact_cube','bit_exact_spectrum','bit_exact_rd','cfar_exact']) and result['cube_FS_normalized_RMS']<=1e-5 and result['spectrum_above_minus80dB_max_error_dB']<=.1
(root/'comparison.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
if not result['pass']:raise SystemExit(1)
