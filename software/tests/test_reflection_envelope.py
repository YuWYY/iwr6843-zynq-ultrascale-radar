"""Independent dense/random probes of the generated nominal-model envelope."""
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('envelope',ROOT/'tools/generate_reflection_envelope.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class EnvelopeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.r,cls.u=m.make_tables()
    def test_generated_header_exact(self):
        text,sha=m.render(self.r,self.u)
        self.assertEqual((ROOT/'radar_runtime/src/processing/radar_reflection_envelope.h').read_text(),text)
        self.assertEqual(sha,hashlib.sha256(self.r.tobytes()+self.u.tobytes()).hexdigest())
    def test_fractional_tones_between_generation_grid(self):
        rng=np.random.default_rng(1102);n=np.arange(256);w=np.hanning(256)
        for _ in range(600):
            freq=rng.uniform(7,203);tone=np.exp(2j*np.pi*freq*n/256)
            p=np.abs(np.fft.fft((tone-tone.mean())*w))**2;anchor=np.argmax(p);p/=p.max()
            bound=self.r[np.abs(np.arange(256)-anchor)]
            self.assertTrue(np.all(p<=bound+1e-6),f'fractional frequency {freq}')
    def test_angular_lookup_and_uncertainty(self):
        rng=np.random.default_rng(6843);q=2*np.pi*m.PITCH_M*m.FC_HZ/299792458.
        for _ in range(3000):
            nominal=rng.uniform(0,2);actual=nominal+rng.uniform(-m.U_UNCERTAINTY,m.U_UNCERTAINTY)
            p=abs(np.exp(1j*q*actual*np.arange(8)).mean())**2
            self.assertLessEqual(p,float(self.u[int(nominal/m.U_STEP)])+1e-7)
    def test_power_not_amplitude_and_no_scene_prior(self):
        self.assertEqual(float(self.r[0]),1)
        self.assertEqual(float(self.u[0]),1)
        self.assertTrue(np.isfinite(self.r).all() and np.isfinite(self.u).all())
        self.assertTrue((self.r>=0).all() and (self.r<=1).all())
        self.assertGreater(float(self.u[2048]),0)
if __name__=='__main__':unittest.main()
