#!/usr/bin/env python3
"""Replace the entire application src tree, with backup and rollback.

Dry-run unless --apply. Preserve local src/app.yaml and .clangd, vitis-comp.json,
_ide, Platform, BSP, XSA, bit and launch configuration. Never download or build.
A tree swap removes old flat sources/caches rather than compiling two copies.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import sys
import uuid

class InstallError(ValueError):
    pass

KEEP_LOCAL = ('app.yaml', '.clangd')

def reject_links(path: Path) -> None:
    if path.is_symlink() or (path.exists() and any(p.is_symlink() for p in path.rglob('*'))):
        raise InstallError(f'symlink inside managed tree is not supported: {path}')

def install(application: Path, source: Path, apply: bool=False,
            clean_build: bool=False, replace_profile: bool=False) -> dict:
    # Reject top-level links before resolve() erases that fact.
    reject_links(source)
    if application.is_symlink():
        raise InstallError('application root must not be a symlink')
    app=application.resolve(); src=source.resolve(); target=app/'src'
    if not (src/'capture/radar_session.c').is_file() or not (src/'linker/lscript_sw4.ld').is_file():
        raise InstallError('source is not the complete classified SW4.1 src tree')
    if not (app/'vitis-comp.json').is_file() or not target.is_dir():
        raise InstallError('--application must contain vitis-comp.json and src/')
    if (app/'vitis-comp.json').is_symlink():
        raise InstallError('component metadata is a symlink')
    if src==app or src==target or src in app.parents or app in src.parents:
        raise InstallError('source and application must be separate, non-nested trees')
    reject_links(target)
    if clean_build:
        reject_links(app/'build')
    info=json.loads((app/'vitis-comp.json').read_text(encoding='utf-8-sig'))
    if info.get('cpuInstance')!='psu_cortexa53_0' or info.get('domainRealName',info.get('domain'))!='standalone_psu_cortexa53_0':
        raise InstallError('requires the existing A53-0 standalone component')
    files=sorted(p.relative_to(src).as_posix() for p in src.rglob('*') if p.is_file())
    if not files or any('/.cache/' in f or f.endswith(('.o','.elf','.bit','.xsa')) for f in files):
        raise InstallError('empty source or generated artifacts inside production source')
    incoming_profile=(src/'config/radar_capture_profile_data.h').read_bytes()
    local_profile=next((target/f for f in ('config/radar_capture_profile_data.h',
                      'radar_capture_profile_data.h') if (target/f).is_file()), None)
    different_profile=bool(local_profile and local_profile.read_bytes()!=incoming_profile)
    plan=dict(mode='APPLY' if apply else 'DRY_RUN',application=str(app),source=str(src),files=files,
        operation='FULL_SRC_TREE_REPLACEMENT_NOT_OVERLAY',clean_build=clean_build,
        different_local_profile=different_profile,
        preserved=['src/app.yaml','src/.clangd','vitis-comp.json','_ide','Platform/BSP/XSA/bit/FSBL'],
        removed_from_active_src=['old flat C/H files','lscript.ld','lscript_sw0.ld',
                                 '.cache/','.compile_commands/','compile_commands.json'])
    if not apply:
        return plan
    if different_profile and not replace_profile:
        raise InstallError('local capture profile differs; no files changed. Review it first. '
                           'Use --replace-profile only to explicitly archive and replace it.')
    ident=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'_'+uuid.uuid4().hex[:8]
    backup=app.parent/'_radar_sw41_backups'/(app.name+'_'+ident)
    stage=app/('.radar_src_stage_'+ident)
    backup.mkdir(parents=True,exist_ok=False)
    plan['backup']=str(backup)
    old_moved=new_installed=build_moved=False
    try:
        shutil.copytree(src,stage)
        for f in KEEP_LOCAL:
            if (target/f).is_file(): shutil.copy2(target/f,stage/f)
        shutil.copy2(app/'vitis-comp.json',backup/'vitis-comp.json')
        # Rename each whole tree; the backup is outside the Vitis component.
        target.rename(backup/'src'); old_moved=True
        stage.rename(target); new_installed=True
        if clean_build and (app/'build').exists():
            (app/'build').rename(backup/'previous_build'); build_moved=True
        plan['installed_sha256']={f:hashlib.sha256((target/f).read_bytes()).hexdigest() for f in files}
        (backup/'INSTALL.json').write_text(json.dumps(plan,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        return plan
    except Exception as exc:
        # Only remove a tree that this invocation just created. Restore old
        # source/build if a later rename/copy fails (e.g. Windows file locks).
        try:
            if build_moved: (backup/'previous_build').rename(app/'build')
            if new_installed: shutil.rmtree(target)
            if old_moved: (backup/'src').rename(target)
            if stage.exists(): shutil.rmtree(stage)
        except Exception as rollback_exc:
            raise InstallError(f'installation failed: {exc}; rollback also failed: {rollback_exc}; '
                               f'old files are under {backup}. Do not build until restored.') from exc
        raise InstallError(f'installation rolled back: {exc}; backup metadata: {backup}') from exc

def main() -> int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--application',required=True,type=Path)
    ap.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'radar_runtime/src')
    ap.add_argument('--apply',action='store_true')
    ap.add_argument('--clean-build',action='store_true',help='archive old application build only; preserve platform')
    ap.add_argument('--replace-profile',action='store_true',help='explicitly replace a differing local profile after backup')
    a=ap.parse_args()
    try:
        print(json.dumps(install(a.application,a.source,a.apply,a.clean_build,a.replace_profile),ensure_ascii=False,indent=2))
        return 0
    except (OSError,ValueError) as exc:
        print('ERROR: '+str(exc),file=sys.stderr);return 2

if __name__=='__main__':raise SystemExit(main())
