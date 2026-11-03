#!/usr/bin/env python3
"""Build/verify the CURRENT closure package; never invoke Vivado or modify inputs.

build --build-dir ABS --sim-dir ABS --delivery ABS --references ABS --zip ABS
      [--include-dcp] [--readme ABS] [--backup ABS]
verify --zip ABS [--extract ABS_NEW_DIRECTORY]
verify-tree --tree ABS_EXTRACTED_ROOT

Only explicit current evidence directories are collected. No historical outputs,
runs/cache, nested archives, bitstreams or XSAs are swept into the package.
Native reopen is a separate, deliberately writable disposable-copy operation.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import zipfile

# Vivado's embedded PYTHONHOME must be ignored by the caller's python -I;
# explicitly restore only this reviewed sibling-module directory.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_identity as identity
import release_gate as release

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('m2_package_helpers', ROOT/'scripts/m2_real/package_review.py')
legacy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(legacy)
require = legacy.require
sha = legacy.digest_file
under = legacy.under
MANIFEST = 'sha256_manifest.txt'
AUDIT = 'evidence/package_audit.json'
# Native before/after BD, project and IP snapshots are review evidence too.
TEXT_SUFFIXES = legacy.EVIDENCE_SUFFIXES | {'.diff', '.patch', '.v', '.sv', '.xdc', '.bd', '.xpr', '.xci', '.py', '.ui'}


def safe(name):
    p = PurePosixPath(name)
    require(name and not p.is_absolute() and not any(x in {'', '.', '..'} for x in name.split('/')), f'Unsafe member: {name}')
    require(not any(x in name for x in '\\:\r\n\t\0'), f'Unsafe member: {name}')
    for part in p.parts:
        require(not part.endswith((' ','.')) and not re.search(r'[<>"|?*\x00-\x1f]',part), f'Unsafe Windows name: {name}')
        require(not re.fullmatch(r'(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?',part), f'Reserved Windows name: {name}')
    require(name in {'README.md', MANIFEST} or p.parts[0] in {'project', 'evidence', 'outputs'}, f'Wrong archive root: {name}')
    require(legacy.admissible(p, include_dcp=True), f'Forbidden archive input: {name}')
    require(p.suffix.lower() != '.dcp' or p.parts[0] == 'outputs', f'DCP outside outputs: {name}')


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True)+'\n').encode('utf-8')


def collect(args):
    root, build, sim, delivery = (p.resolve() for p in (args.root, args.build_dir, args.sim_dir, args.delivery))
    require(all(p.is_dir() and not legacy.reparse(p) for p in (root, build, sim, delivery)), 'Input directory missing or linked')
    require(under(build, root/'outputs') and under(sim, root/'outputs'), 'Current build/sim directories must be explicit children of ROOT/outputs')
    require(not any(p.lower().startswith('m2_real_') for d in (build, sim) for p in d.relative_to(root/'outputs').parts), 'Historical m2_real output is not a closure input')
    require(build != sim and not under(build, sim), 'Build/simulation directory overlap is ambiguous')
    xprs = list(root.glob('*.xpr'))
    require(len(xprs)==1, 'Exactly one root XPR required')
    xpr = xprs[0]
    gate_path = sim/'SIM_PASS.json'
    manifest_path = build/'build_manifest.json'
    synth_receipt_path = build/'synth_identity.json'
    route_receipt_path = build/'route_identity.json'
    native = build/'route_native_after.json'
    release_path = build/'release_status.json'
    proof_paths=[manifest_path,synth_receipt_path,route_receipt_path,native,release_path,
                 build/'route_STATUS.json',gate_path,sim/'STATUS.json',sim/'input_hashes.json',
                 root/'scripts/m2_closure/release_policy.json']
    proof_hashes={path:sha(path) for path in proof_paths}
    manifest = identity.read_json(manifest_path)
    synth = identity.read_json(synth_receipt_path)
    route = identity.read_json(route_receipt_path)
    release_record = identity.read_json(release_path)
    actual_synth = root/synth['actual_run_dcp']['path'].removeprefix('project/')
    identity.require_synth(root, native, gate_path, manifest_path, actual_synth, synth_receipt_path)
    release.verify_stage(identity.read_json(build/'route_STATUS.json'), route)
    require(route['manifest_sha256']==sha(manifest_path) and route['synth_receipt_sha256']==sha(synth_receipt_path), 'Route lineage changed')
    require(release_record.get('build_id')==manifest['build_id'] and release_record.get('route_receipt_sha256')==sha(route_receipt_path), 'Release record belongs to different route')
    report_dir = build/'reports_route'
    actual_reports = {p.name:sha(p) for p in report_dir.iterdir() if p.is_file()}
    require(actual_reports==route['reports']==release_record['report_sha256'], 'Final report set/hash changed')
    evaluated = release.evaluate(root, report_dir, identity.read_json(root/'scripts/m2_closure/release_policy.json'))
    require(all(release_record.get(k)==v for k,v in evaluated.items()), 'Release decision is stale')
    require(release_record.get('SIMULATION')=='PASS' and release_record.get('NATIVE_ROUTE_EXECUTION')=='PASS', 'Simulation/native execution not current PASS')
    require(release_record.get('DOWNLOAD_CANDIDATE')=='NO', 'This package format excludes download artifacts')
    if args.include_dcp:
        require(release_record.get('internal_status')=='PASS', 'DCP delivery requires current internal gates PASS')
    require(args.references.is_file(), 'Final native active_references.tsv is required')
    ref_hash, xpr_hash = sha(args.references), sha(xpr)
    refs = legacy.read_references(args.references, xpr, root)
    sources, original_members, folded = {}, {}, {}

    def add(name, source):
        safe(name)
        require(name.casefold() not in folded or folded[name.casefold()]==name, f'Case collision: {name}')
        require(name not in sources or sources[name]==source, f'Duplicate destination: {name}')
        if isinstance(source, Path):
            require(source.is_file() and not legacy.reparse(source), f'Missing/linked input: {source}')
            original_members.setdefault(str(source.resolve()), []).append(name)
        folded[name.casefold()]=name
        sources[name]=source

    def add_project(path):
        require(under(path.resolve(), root), f'Project path escapes root: {path}')
        add('project/'+path.relative_to(root).as_posix(), path)

    add_project(xpr)
    for rel in ('rtl','constraints','sim','docs','scripts',
                f'{xpr.stem}.srcs/sources_1/bd/radar_system',
                f'{xpr.stem}.gen/sources_1/bd/radar_system',
                f'{xpr.stem}.gen/sources_1/bd/mref/radar_capture_bd'):
        directory=root/rel
        require(directory.is_dir(), f'Required source/generated dependency tree missing: {rel}')
        for path in legacy.walk_files(directory):
            if legacy.admissible(path.relative_to(root), False): add_project(path)
    ipuser=root/f'{xpr.stem}.ip_user_files'
    for rel in ('bd/radar_system','ipstatic','mem_init_files','sim_scripts/radar_system'):
        directory=ipuser/rel
        if directory.is_dir():
            for path in legacy.walk_files(directory):
                if legacy.admissible(path.relative_to(root), False): add_project(path)
    for record in refs:
        path=Path(record['resolved'])
        internal, tool=under(path,root),under(path,identity.TOOL_ROOT)
        require(internal or tool, f'External non-tool active reference: {path}')
        if not record['exists']:
            require(record['origin']!='XPR File/Path' and not record.get('used_in','').strip(), f'Missing active input: {path}')
            record['disposition']='inactive_missing'
        elif internal:
            if record['origin']=='XPR File/Path':
                require(not Path(record['raw']).is_absolute(), f'Nonportable absolute XPR input: {record["raw"]}')
            member='project/'+path.relative_to(root).as_posix()
            if member not in sources and under(path,ipuser): add_project(path)
            require(member in sources, f'Active input omitted from package: {path}')
            record.update(disposition='packaged',member=member)
        else:
            record.update(disposition='external_tool',sha256=sha(path))
    require(sha(args.references)==ref_hash and sha(xpr)==xpr_hash, 'Reference inventory/XPR changed while collecting')

    def evidence(directory, prefix, omit=()):
        for path in legacy.walk_files(directory):
            rel=path.relative_to(directory)
            if any(rel.parts[0]==part for part in omit): continue
            if path.suffix.lower() in TEXT_SUFFIXES and legacy.admissible(rel,False): add(prefix+'/'+rel.as_posix(),path)

    evidence(sim,'evidence/sim')
    # A sim child of build is permitted but must not be duplicated as build evidence.
    omit=('reports_synth','reports_route')+((sim.relative_to(build).parts[0],) if under(sim,build) else ())
    evidence(build,'evidence/build',omit)
    for stage in ('synth','route'):
        require((build/f'reports_{stage}').is_dir(), f'Missing {stage} native reports')
        evidence(build/f'reports_{stage}',f'evidence/reports/{stage}')
    for rel in ('before','after','diff'):
        if (delivery/rel).is_dir(): evidence(delivery/rel,'evidence/diff/'+rel)
    for path in legacy.walk_files(delivery):
        rel=path.relative_to(delivery)
        if rel.parts[0] in {'before','after','diff','reopen'} or path==args.readme: continue
        if path.suffix.lower() in TEXT_SUFFIXES and legacy.admissible(rel,False): add('evidence/matrix/'+rel.as_posix(),path)
    add('evidence/active_references.tsv',args.references)
    require(args.readme.is_file() and args.readme.stat().st_size, 'Nonempty final README required')
    readme=args.readme.read_text(encoding='utf-8-sig')
    readme+='\n\n## 包内路径与重开\n\n打开 `project/'+xpr.name+'`。当前仿真证据在 `evidence/sim/`，构建身份在 `evidence/build/`，综合/布线报告在 `evidence/reports/synth/` 与 `evidence/reports/route/`，差异与基线在 `evidence/diff/`，需求矩阵及待办在 `evidence/matrix/`。`outputs/`只可能含本次内部门禁合格DCP，无bit/XSA。历史绝对路径仅用于溯源，包内成员映射见 `evidence/package_audit.json`；它们不要求原机器路径存在。\n\n解压至新目录后，以Vivado 2025.1运行 `project/scripts/m2_closure/reopen_package.tcl`，传入两个绝对参数：解压根目录、位于解压目录之外的新报告目录。该入口先核验全部包内哈希，再仅在解压副本执行原生force validate；不运行综合、实现或下载。原有SIM/构建记录用于身份溯源，若要在新路径重新运行，应使用当前closure脚本产生新记录，不伪造历史日志。\n'
    readme+='\n审阅脚本边界：`check_structure.py` 在包内从 `evidence/diff/before/radar_system.bd` 读取原始快照；它与 `audit_current.tcl` 会在解压副本的 `project/delivery/20261013_c1/` 写入新审阅报告。`audit_current.tcl` 只读打开工程，不执行 Validate 或保存。请先完成包哈希核验与专用重开验证，再按需生成这些新报告，避免将新文件误认为原包成员。\n\n`collect_changes.py` 是主线差异收集工具，明确依赖原机器外部目录 `/archive/20261013_c1/baseline.zip`，不属于可直接移机运行的验证入口；该外部 ZIP 不随本包交付。包内已有差异在 `evidence/diff/diff/`，基线身份及归档哈希在 `evidence/diff/baseline/`。本次重开须调用 `scripts/m2_closure/reopen_package.tcl`；它显式传入新包路径，不能直接使用历史 `scripts/m2_real/reopen_package.tcl` 的默认 `evidence/delivery/after/` 路径。\n'
    add('README.md',readme.encode('utf-8'))
    backup=args.backup.resolve()
    require(backup.is_dir() and not under(backup,root), 'External baseline directory required')
    for name in ('baseline_hashes.json','baseline_identity.json'): add('evidence/diff/baseline/'+name,backup/name)
    require((backup/'baseline.zip').is_file(), 'External baseline ZIP missing')
    add('evidence/diff/baseline/archive_reference.json',json_bytes({'path':str(backup/'baseline.zip'),'sha256':sha(backup/'baseline.zip'),'included':False}))
    dcps=[]
    for label,record in [('synth',synth['actual_run_dcp']),('routed',route['actual_run_routed_dcp'])]:
        require(record['path'].startswith('project/'), 'DCP receipt has non-project path')
        path=(root/record['path'][8:]).resolve()
        require(under(path,root) and path.is_file() and sha(path)==record['sha256'], f'Actual {label} DCP is stale')
        if args.include_dcp:
            member='outputs/'+label+'/'+path.name;add(member,path)
            dcps.append({'member':member,**record})

    # Bind archived bytes to the identity inputs/logs, not just to a new ZIP hash.
    bindings=[]
    sim_gate=identity.read_json(gate_path)
    expected=[('native',key[8:],val) for key,val in manifest['identity']['files'].items() if key.startswith('project/')]
    expected += [(kind,key,val) for kind in ('input_hashes','log_hashes') for key,val in sim_gate[kind].items()]
    for kind,relative,expected_hash in expected:
        path=(root/relative).resolve()
        require(under(path,root), f'Identity binding escapes project: {relative}')
        members=original_members.get(str(path),[])
        require(members, f'Identity-bound input/log omitted: {relative}')
        bindings.append({'kind':kind,'original_relative':relative,'member':members[0],'sha256':expected_hash})
    # Seal the audit inputs themselves and the exact final native reports. A
    # concurrent edit before archive copying must not produce a mixed receipt.
    for path,expected_hash in list(proof_hashes.items())+[(report_dir/name,value) for name,value in actual_reports.items()]:
        members=original_members.get(str(path.resolve()),[])
        require(members and sha(path)==expected_hash,f'Proof changed/omitted during collection: {path}')
        bindings.append({'kind':'receipt_or_report','original_relative':path.relative_to(root).as_posix(),
                         'member':members[0],'sha256':expected_hash})
    audit={'schema':'m2-closure-package-v1','build_id':manifest['build_id'],'original_root':str(root),
           'backup_root':str(backup),'xpr_member':'project/'+xpr.name,'xpr_sha256':xpr_hash,
           'references_sha256':ref_hash,'references':refs,'bindings':bindings,'dcps':dcps,
           'include_dcp':args.include_dcp,'release':release_record,
           'source_members':original_members,'native_reopen':'NOT_RUN','download_candidate':'NO'}
    add(AUDIT,json_bytes(audit))
    return sources,audit


def verify(archive_path):
    with zipfile.ZipFile(archive_path) as z:
        entries=z.infolist();names=[i.filename for i in entries]
        require(len(names)==len(set(names))==len({s.casefold() for s in names}), 'Duplicate/case-colliding archive names')
        for entry in entries:
            safe(entry.filename)
            require(not stat.S_ISLNK(entry.external_attr>>16), 'Archive symlink forbidden')
        require({'README.md',MANIFEST,AUDIT}.issubset(names), 'Missing package metadata')
        hashes={}
        for line in z.read(MANIFEST).decode().splitlines():
            match=re.fullmatch(r'([0-9a-f]{64})  (.+)',line)
            require(match is not None, 'Malformed manifest')
            value,name=match.groups();safe(name)
            require(name not in hashes and name!=MANIFEST, 'Duplicate/self manifest entry')
            hashes[name]=value
        require(set(hashes)==set(names)-{MANIFEST}, 'Manifest/member set differs')
        for name,value in hashes.items():
            with z.open(name) as f: require(legacy.digest_stream(f)==value,f'Hash failure: {name}')
        require(z.testzip() is None,'CRC failure')
        audit=json.loads(z.read(AUDIT))
        require(audit['schema']=='m2-closure-package-v1' and audit['download_candidate']=='NO','Unsupported audit')
        require(hashes[audit['xpr_member']]==audit['xpr_sha256'],'XPR differs from reference audit')
        require(hashes['evidence/active_references.tsv']==audit['references_sha256'],'Reference export changed')
        for r in audit['references']:
            if r['disposition']=='packaged':require(r['member'] in hashes,'Referenced input omitted')
            elif r['disposition']=='inactive_missing':require(r['origin']!='XPR File/Path' and not r.get('used_in','').strip(),'Active missing reference')
            else:require(r['disposition']=='external_tool','Unknown reference disposition')
        for binding in audit['bindings']:require(hashes.get(binding['member'])==binding['sha256'],f'Build/sim identity mismatch: {binding}')
        for dcp in audit['dcps']:require(hashes.get(dcp['member'])==dcp['sha256'],'DCP receipt mismatch')
        require({n for n in names if n.lower().endswith('.dcp')}=={r['member'] for r in audit['dcps']},'Unlisted DCP')
        require(not audit['dcps'] or audit['release'].get('internal_status')=='PASS','Unqualified DCP')
    return {'status':'PASS','archive_sha256':sha(archive_path),'members':len(names),'audit':audit}


def build(args):
    destination=args.zip.resolve();partial=destination.with_name(destination.name+'.partial')
    companion=destination.with_name(destination.name+'.sha256')
    require(not any(p.exists() for p in (destination,partial,companion)),'Refusing existing output/partial/hash file')
    sources,audit=collect(args)
    destination.parent.mkdir(parents=True,exist_ok=True)
    hashes={}
    with zipfile.ZipFile(partial,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,source in sorted(sources.items()):
            h=hashlib.sha256();stream=source.open('rb') if isinstance(source,Path) else io.BytesIO(source)
            with stream,z.open(legacy.zip_info(name),'w',force_zip64=True) as target:
                for block in iter(lambda:stream.read(legacy.CHUNK),b''):h.update(block);target.write(block)
            hashes[name]=h.hexdigest()
        z.writestr(legacy.zip_info(MANIFEST),''.join(f'{hashes[n]}  {n}\n' for n in sorted(hashes)).encode())
    for name,source in sources.items():
        if isinstance(source,Path):require(sha(source)==hashes[name],f'Input changed during packaging: {source}')
    result=verify(partial)
    require(not destination.exists(),'Destination appeared during build')
    partial.rename(destination)
    with companion.open('x',encoding='utf-8') as f:f.write(f"{result['archive_sha256']}  {destination.name}\n")
    return result


def verify_tree(tree,reports=None):
    tree=tree.resolve();require(tree.is_dir() and not legacy.reparse(tree),'Invalid extracted tree')
    audit=identity.read_json(tree/AUDIT)
    require(not under(tree,Path(audit['original_root']).resolve()) and not under(tree,Path(audit['backup_root']).resolve()),'Refusing original/backup tree')
    require(audit.get('schema')=='m2-closure-package-v1','Unknown extracted package schema')
    if reports is not None:
        require(reports.is_absolute(),'Native report directory must be absolute')
        if reports.exists():
            require(reports.is_dir() and not legacy.reparse(reports),'Invalid native report directory')
            require(all(p.is_file() and not legacy.reparse(p) and p.suffix.lower() in {'.log','.jou'} for p in reports.iterdir()),'Native report directory must be new except current launcher log/journal')
        require(not any(under(reports.resolve(),p) for p in (tree,Path(audit['original_root']).resolve(),Path(audit['backup_root']).resolve())), 'Native report directory cannot be inside extraction/mainline/backup')
    hashes={}
    for line in (tree/MANIFEST).read_text(encoding='utf-8').splitlines():
        match=re.fullmatch(r'([0-9a-f]{64})  (.+)',line)
        require(match is not None,'Malformed extracted manifest')
        value,name=match.groups();safe(name);path=tree.joinpath(*PurePosixPath(name).parts)
        require(name not in hashes and name!=MANIFEST,'Duplicate/self extracted manifest entry')
        hashes[name]=value
        require(under(path.resolve(),tree) and path.is_file() and not legacy.reparse(path),'Invalid extracted member')
        require(sha(path)==value,f'Extracted file differs: {name}')
    actual=set()
    for current,dirs,files in os.walk(tree,followlinks=False):
        for part in dirs+files:require(not legacy.reparse(Path(current)/part),'Linked extracted directory/file forbidden')
        for part in files:actual.add((Path(current)/part).relative_to(tree).as_posix())
    require(actual==set(hashes)|{MANIFEST},'Extracted membership differs from manifest; use a fresh verified extraction')
    require(hashes.get(audit['xpr_member'])==audit['xpr_sha256'] and hashes.get('evidence/active_references.tsv')==audit['references_sha256'],'Extracted dependency audit mismatch')
    for binding in audit['bindings']:require(hashes.get(binding['member'])==binding['sha256'],'Extracted identity binding mismatch')
    return {'status':'PASS','tree':str(tree),'project':str(tree/'project'),'audit':audit}


def extract_new(archive,target,result):
    require(target.is_absolute() and not target.exists() and target.parent.is_dir(),'Extraction requires exact absolute NEW directory and existing parent')
    target=target.resolve();audit=result['audit']
    require(not under(target,Path(audit['original_root']).resolve()) and not under(target,Path(audit['backup_root']).resolve()),'Extraction inside original/backup forbidden')
    require(sha(archive)==result['archive_sha256'],'Archive changed after verification')
    target.mkdir()
    with zipfile.ZipFile(archive) as z:
        for entry in z.infolist():
            safe(entry.filename);path=target.joinpath(*PurePosixPath(entry.filename).parts)
            require(under(path.resolve(),target),'Extraction escape')
            path.parent.mkdir(parents=True,exist_ok=True)
            with z.open(entry) as source,path.open('xb') as dest:
                for block in iter(lambda:source.read(legacy.CHUNK),b''):dest.write(block)
    verify_tree(target)
    return str(target)


def main():
    ap=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode',choices=['build','verify','verify-tree'])
    ap.add_argument('--root',type=Path,default=ROOT)
    for arg in ('build-dir','sim-dir','delivery','references','readme','zip','tree','extract','reports'):ap.add_argument('--'+arg,type=Path)
    ap.add_argument('--backup',type=Path,default=Path('/archive/20261013_c1'))
    ap.add_argument('--include-dcp',action='store_true')
    args=ap.parse_args()
    try:
        if args.mode=='build':
            require(all(getattr(args,k) is not None for k in ('build_dir','sim_dir','delivery','references','zip')),'Explicit --build-dir --sim-dir --delivery --references --zip required')
            require(all(getattr(args,k).is_absolute() for k in ('build_dir','sim_dir','delivery','references','zip')),'Use absolute build/sim/delivery/reference/archive paths')
            args.readme=args.readme or args.delivery/'README.md'
            result=build(args)
        elif args.mode=='verify':
            require(args.zip is not None,'--zip required');result=verify(args.zip)
        else:
            require(args.tree is not None,'--tree required');result=verify_tree(args.tree,args.reports)
        if args.extract:
            require(args.mode!='verify-tree','--extract requires archive mode')
            result['extracted_to']=extract_new(args.zip,args.extract,result)
        result.pop('audit',None);print(json.dumps(result,ensure_ascii=False,indent=2));return 0
    except (legacy.PackageError,identity.GateError,OSError,ValueError,KeyError,TypeError,zipfile.BadZipFile) as error:
        print(f'PACKAGE_FAIL: {error}; partial artifacts retained, no cleanup performed',file=sys.stderr);return 1


if __name__=='__main__':raise SystemExit(main())
