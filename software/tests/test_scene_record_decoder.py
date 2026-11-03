#!/usr/bin/env python3
"""CRC/corruption/retry/identity tests using the production C recorder fixture."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('record_decoder',ROOT/'tools/decode_scene_record.py')
d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)

def rehash(image):
    struct.pack_into('<I',image,120,d.crc(image[4096:]))
    struct.pack_into('<I',image,4092,d.crc(image[:4092]))

def pages(image,start=0,stop=None):
    m,_,_,_=d.decode_image(image)
    if stop is None:stop=len(image)
    lines=[]
    for begin in range(start,stop,65536):
        end=min(begin+65536,stop)
        lines.append(f"@SCENEBEGIN schema=1 epoch={m['epoch']} bytes={len(image)} crc={d.crc(image):08x} start={begin} count={end-begin} frames={m['frame_count']} timing={m['timing_count']} heatmaps={m['heatmap_count']}")
        for at in range(begin,end,128):
            payload=image[at:min(at+128,end)]
            lines.append(f"@SCENEDATA epoch={m['epoch']} offset={at} bytes={len(payload)} crc={d.crc(payload):08x} {payload.hex()}")
        lines.append(f"@SCENEEND epoch={m['epoch']} next={end} bytes={len(image)} crc={d.crc(image):08x}")
    return '\n'.join(lines)+'\n'

class DecoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image=Path(os.environ['RADAR_RECORD_FIXTURE']).read_bytes()
        cls.valid_text=pages(cls.image)

    def collect(self,text):
        with tempfile.TemporaryDirectory() as directory:
            file=Path(directory)/'uart.txt';file.write_text(text)
            return d.collect_pages([file])

    def test_production_fixture(self):
        m,frames,timing,heats=d.decode_image(self.image,60)
        self.assertTrue(m['complete_duration_record'])
        self.assertEqual((len(frames),len(timing),len(heats)),(601,601,3))
        self.assertEqual(m['reflection_id_seed'],1)
        self.assertEqual([h['schedule_s'] for h in heats],[10,30,55])
        self.assertEqual(timing[-1]['first_ack']['frame_id'],frames[-1]['frame_id'])

    def test_complete_pages_and_identical_retry(self):
        image,meta=self.collect(self.valid_text+pages(self.image,0,65536))
        self.assertEqual(image,self.image)
        self.assertGreater(len(meta['completed_pages']),1)

    def test_incomplete_attempt_requires_completed_retry(self):
        attempt='\n'.join(self.valid_text.splitlines()[:4])+'\n'
        image,meta=self.collect(attempt+self.valid_text)
        self.assertEqual(image,self.image);self.assertEqual(len(meta['incomplete_pages_retained']),1)
        with self.assertRaises(ValueError):self.collect(attempt)

    def test_corruption_missing_page_and_conflicting_identity(self):
        corrupt=self.valid_text.replace('offset=0 bytes=128 crc=','offset=1 bytes=128 crc=',1)
        with self.assertRaisesRegex(ValueError,'sequence'):self.collect(corrupt)
        with self.assertRaises(ValueError):self.collect(self.valid_text.replace('@SCENEEND','@BROKENEND',1))
        wrong=self.valid_text.replace('epoch=7','epoch=8',1)
        with self.assertRaises(ValueError):self.collect(wrong)
        corrupted=bytearray(self.image);corrupted[5000]^=1
        with self.assertRaisesRegex(ValueError,'payload'):d.decode_image(corrupted)
        rehash(corrupted)
        with self.assertRaisesRegex(ValueError,'frame'):d.decode_image(corrupted)

    def test_unknown_coordinates_json_null(self):
        image=bytearray(self.image);point=4096+128
        for offset in (28,32,36):struct.pack_into('<f',image,point+offset,float('nan'))
        row=image[4096:4096+2560];struct.pack_into('<I',image,4096+124,d.crc(row[:124]+row[128:]))
        rehash(image)
        _,frames,_,_=d.decode_image(image)
        self.assertIsNone(frames[0]['points'][0]['z_m'])
        self.assertIn('"z_m": null',json.dumps(frames[0],allow_nan=False))

    def test_real_startup_schema_and_changed_artifact(self):
        manifest,_,_,_=d.decode_image(self.image)
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);inputs=[]
            for name in ('app','bit','xsa'):
                content=(manifest['software_version']+'\0'+manifest['build_id']).encode() if name=='app' else (name+'-test-only').encode()
                path=root/name;path.write_bytes(content)
                sha=d.hashlib.sha256(content).hexdigest();inputs.append(dict(name=name,path=str(path),sha256=sha))
                if name!='app':manifest[name+'_sha256']=sha
            binding=dict(schema_version=2,build_id=manifest['build_id'],application={'build_id':manifest['build_id']},inputs=inputs,
                         source_files=[dict(path=str(root/'config/profiles/radar_3tx_hsi_stream0.cfg'),sha256=manifest['cfg_sha256'])])
            result=d.bind_launch(manifest,binding);self.assertEqual(result['elf_sha256'],inputs[0]['sha256'])
            (root/'app').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'changed'):d.bind_launch(manifest,binding)

if __name__=='__main__':unittest.main(verbosity=2)
