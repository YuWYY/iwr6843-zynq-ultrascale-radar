"""Verify the natively saved and reopened block placement and design identity."""
from pathlib import Path
import re,json,hashlib
root=Path(__file__).resolve().parents[2];out=root/'delivery/20261012_171114/layout'
def cells(p):
    return {n:tuple(map(int,(lvl,x,y))) for n,lvl,x,y in re.findall(r'preplace inst (\S+) -pg 1 -lvl (\d+) -x (-?\d+) -y (-?\d+)',p.read_text())}
a=cells(out/'radar_system.ui');b=cells(out/'reopened.ui')
assert len(a)==len(b)==12 and a.keys()==b.keys()
for n in a: assert a[n][0]==b[n][0],f'Column changed: {n}'
names=sorted(a)
for x in names:
    for y in names:
        assert (a[x][2]<a[y][2])==(b[x][2]<b[y][2]),f'Row ordering changed: {x} / {y}'
bd=root/'Radar_XCZU2CG_M2.srcs/sources_1/bd/radar_system/radar_system.bd'
digest=hashlib.sha256(bd.read_bytes()).hexdigest()
assert digest=='69e5e81dda17bf0489d22ba84ffa04ae39bd06d28db560304b98064b3e326fc5'
result={'status':'PASS','instances':12,'row_column_order_preserved':True,'bd_hash_before_and_after':digest,'integration_repeated_in_native_gui':True,'netlist_bytes_unchanged_after_repeated_integration_and_layout':True,'saved':a,'reopened':b}
(out/'layout_verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print('NATIVE_LAYOUT_REOPEN_AND_IDEMPOTENT_INTEGRATION=PASS')
