"""Deterministic raw16 complex fixture and independent float chain reference."""
from pathlib import Path
import argparse,json
import numpy as np
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();out=Path(a.output);out.mkdir(parents=True,exist_ok=True)
rng=np.random.default_rng(6843)
n=np.arange(256); raw=np.zeros((96,4,256),complex)
for chirp in range(96):
    loop,slot=divmod(chirp,3);tx=[0,2,1][slot]
    for rx in range(4):
        raw[chirp,rx]=11000*np.exp(2j*np.pi*(37*n/256+3*(loop+slot/3)/32)+(tx*4+rx)*.27j)
        raw[chirp,rx]+=1700*np.exp(2j*np.pi*(81*n/256-5*(loop+slot/3)/32)-(tx*4+rx)*.13j)
        raw[chirp,rx]+=rng.normal(0,20,256)+1j*rng.normal(0,20,256)
        raw[chirp,rx]+=317-213j
iq=np.stack((np.rint(raw.real),np.rint(raw.imag)),axis=-1).astype(np.int16)
with (out/'input.hex').open('w') as f:
    for i,q in iq.reshape(-1,2):f.write(f'{int(q)&0xffffffff:08x}{int(i)&0xffffffff:08x}\n')
z=iq[...,0].astype(float)+1j*iq[...,1]
range_fft=np.fft.fft((z-z.mean(axis=-1,keepdims=True))*np.hanning(256),axis=-1)/(32768*256)
cube=np.empty((256,12,32),complex)
for chirp in range(96):
    loop,slot=divmod(chirp,3);tx=[0,2,1][slot]
    cube[:,tx*4:tx*4+4,loop]=range_fft[chirp].T
doppler=np.fft.fft(cube*np.hanning(32),axis=-1)/32
np.save(out/'raw_iq.npy',iq);np.save(out/'range_reference.npy',range_fft)
np.save(out/'cube_reference.npy',doppler)
np.save(out/'spectrum_reference.npy',np.sum(abs(range_fft)**2,axis=(0,1)))
np.save(out/'rd_reference.npy',np.sum(abs(doppler)**2,axis=1))
(out/'fixture.json').write_text(json.dumps({'seed':6843,'geometry':[96,4,256],'range_bins':[37,81],'doppler_bins':[3,-5],'input_amplitudes':[11000,1700]},indent=2))
print(out)
