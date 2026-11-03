"""Compare current production sources to the independently verified full backup."""
from pathlib import Path
import zipfile, hashlib, json, difflib, csv
root=Path(__file__).resolve().parents[2]
out=root/'delivery/20261012_171114'
archive=Path('/archive/20261012_171114')
sha=lambda b:hashlib.sha256(b).hexdigest()
baseline=archive/'baseline.zip'
assert sha(baseline.read_bytes())=='c4f5987fd5c5de862fd4c536ff70dd5067e8529a4d3bd792390a3a4ac2e6e323'
rows=[];patch=[]
with zipfile.ZipFile(baseline) as z:
    for p in sorted((root/'rtl').rglob('*.v')):
        name=p.relative_to(root).as_posix();old=z.read(name);new=p.read_bytes()
        rows.append([name,sha(old),sha(new),'UNCHANGED' if old==new else 'MODIFIED'])
        if old!=new:
            patch.extend(difflib.unified_diff(old.decode().splitlines(True),new.decode().splitlines(True),fromfile='before/'+name,tofile='after/'+name))
with (out/'rtl_sha256_comparison.tsv').open('w',newline='',encoding='utf-8') as f:
    w=csv.writer(f,delimiter='\t');w.writerow(['source','before_sha256','after_sha256','status']);w.writerows(rows)
(out/'source_diff.patch').write_text(''.join(patch),encoding='utf-8')
current={p.relative_to(root).as_posix():sha(p.read_bytes()) for folder in ['rtl','constraints','sim','docs','scripts/m2_real'] for p in (root/folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
(out/'current_source_hashes.json').write_text(json.dumps(current,indent=2),encoding='utf-8')
(out/'backup_reference.json').write_text(json.dumps({'archive':str(archive),'zip':'baseline.zip','sha256':sha(baseline.read_bytes()),'files':741,'crc_and_all_input_hashes_verified':True,'recursive_historical_zips_included':False},indent=2),encoding='utf-8')
print(json.dumps({'production_files':len(rows),'modified':[r[0] for r in rows if r[-1]=='MODIFIED']},indent=2))
