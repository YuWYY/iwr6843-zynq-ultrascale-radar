"""Bind each build to its current RTL, declarations, BD, XCI and generated HDL."""
from pathlib import Path
import argparse,hashlib,json,datetime
root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser();ap.add_argument('mode',choices=['record','check']);ap.add_argument('manifest',type=Path);args=ap.parse_args()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
if args.mode=='record':
    assert not args.manifest.exists(), 'Manifest already exists'
    inputs=list((root/'rtl').rglob('*.v'))
    inputs += [root/'constraints'/n for n in ['radar_pins.xdc','radar_clocks.xdc','radar_cdc.xdc']]
    for suffix in ['*.bd','*.xci']:
        inputs+=list((root/'Radar_XCZU2CG_M2.srcs/sources_1/bd/radar_system').rglob(suffix))
    for suffix in ['*.v','*.vhd','*.xdc','*.mem','*.coe']:
        inputs+=list((root/'Radar_XCZU2CG_M2.gen/sources_1/bd/radar_system').rglob(suffix))
    result={'recorded_at':datetime.datetime.now().astimezone().isoformat(),'top':'radar_m2_top','files':{p.relative_to(root).as_posix():sha(p) for p in sorted(set(inputs))}}
    args.manifest.parent.mkdir(parents=True,exist_ok=True);args.manifest.write_text(json.dumps(result,indent=2),encoding='utf-8')
else:
    result=json.loads(args.manifest.read_text())
    for name,digest in result['files'].items(): assert sha(root/name)==digest,f'Build input changed: {name}'
print('BUILD_INPUT_IDENTITY=PASS files='+str(len(result['files'])))
