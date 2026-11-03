"""Read current sources and the immutable baseline; emit review diffs only."""
from pathlib import Path
import difflib, hashlib, json, zipfile

ROOT=Path(__file__).resolve().parents[2]
BASE=Path('/archive/20261013_c1')
OUT=ROOT/'delivery/20261013_c1/diff'
OUT.mkdir(parents=True,exist_ok=True)
sha=lambda data:hashlib.sha256(data).hexdigest()
with zipfile.ZipFile(BASE/'baseline.zip') as archive:
    names=set(archive.namelist())
    # Baseline archive stores project-relative names, not another nested ZIP.
    if 'rtl/top/radar_capture_bd.v' not in names:
        raise RuntimeError('Unexpected baseline layout; inspect it before comparison')
    changes=[]; diffs=[]
    for tree in ('rtl','constraints','sim','scripts/m2_closure'):
        for path in sorted((ROOT/tree).rglob('*')):
            if not path.is_file() or path.suffix not in {'.v','.sv','.vh','.xdc','.py','.tcl','.json'}:
                continue
            name=path.relative_to(ROOT).as_posix(); now=path.read_bytes()
            before=archive.read(name) if name in names else b''
            if now==before:continue
            changes.append({'path':name,'kind':'modified' if name in names else 'added',
                            'before_sha256':sha(before) if name in names else None,
                            'after_sha256':sha(now)})
            diffs.extend(difflib.unified_diff(before.decode('utf-8-sig').splitlines(True),
                         now.decode('utf-8-sig').splitlines(True),
                         fromfile='baseline/'+name if name in names else '/dev/null',
                         tofile='current/'+name))
    protected={}
    for name in ('constraints/radar_pins.xdc',
                 'Radar_XCZU2CG_M2.srcs/sources_1/bd/radar_system/ui/bd_93d232e8.ui'):
        a=sha(archive.read(name));b=sha((ROOT/name).read_bytes())
        if a!=b:raise RuntimeError('Protected pin/layout bytes changed: '+name)
        protected[name]={'baseline_sha256':a,'current_sha256':b,'status':'UNCHANGED'}
(OUT/'before_after.diff').write_text(''.join(diffs),encoding='utf-8')
(OUT/'source_changes.json').write_text(json.dumps(changes,indent=2),encoding='utf-8')
(OUT/'protected_hashes.json').write_text(json.dumps(protected,indent=2),encoding='utf-8')
print(json.dumps({'changed_files':len(changes),'protected':list(protected),'out':str(OUT)}))
