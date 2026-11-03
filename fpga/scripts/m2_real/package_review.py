#!/usr/bin/env python3
"""Package the reviewed M2 project without modifying its inputs or running Vivado.

Build only after final native save/export_references.tcl and the delivery README:
  python -B scripts/m2_real/package_review.py build [--include-dcp]
Verify an existing ZIP without extracting:
  python -B scripts/m2_real/package_review.py verify
Verify, then extract into exactly one NEW absolute directory (parent must exist):
  python -B scripts/m2_real/package_review.py verify --extract /tmp/M2_reopen_01

The script never deletes directories/files or overwrites an existing archive.
On failure it preserves any .partial archive or partial extraction for inspection.
Tool-installation references may remain external; other external inputs fail.
Verification establishes archive integrity, not native reopen or board operation.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
from typing import BinaryIO, Iterator
import xml.etree.ElementTree as ET
import zipfile


BUILD_ID = "20261012_171114"
ZIP_NAME = "Radar_XCZU2CG_M2_RealCapture_Review_20261012.zip"
README_NAME = "README_交付说明.md"
MANIFEST_NAME = "sha256_manifest.txt"
CHUNK = 1024 * 1024
EVIDENCE_SUFFIXES = {
    ".log", ".rpt", ".json", ".txt", ".tcl", ".hash", ".sha256",
    ".sha", ".svg", ".png", ".md", ".tsv", ".csv", ".jou", ".xml",
}
FORBIDDEN_SUFFIXES = {
    ".zip", ".7z", ".rar", ".tar", ".gz", ".bit", ".bin", ".xsa",
    ".wdb", ".pb", ".sdb", ".rlx", ".exe", ".dll", ".o", ".obj",
    ".pyc", ".pyo", ".xdbg", ".svtype", ".rtti", ".partial",
}
FORBIDDEN_PARTS = {
    ".xil", ".git", "__pycache__", "xsim.dir", "m2_ip_config_review",
    "node_modules", "oldsrcs",
}


class PackageError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PackageError(message)


def digest_stream(stream: BinaryIO) -> str:
    digest = hashlib.sha256()
    for block in iter(lambda: stream.read(CHUNK), b""):
        digest.update(block)
    return digest.hexdigest()


def digest_file(path: Path) -> str:
    with path.open("rb") as stream:
        return digest_stream(stream)


def under(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def reparse(path: Path) -> bool:
    info = path.lstat()
    return path.is_symlink() or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def walk_files(directory: Path) -> Iterator[Path]:
    """Walk without following symlinks/junctions or entering cache/snapshot trees."""
    require(directory.is_dir() and not reparse(directory), f"Not a regular directory: {directory}")
    for current, dirs, names in os.walk(directory, followlinks=False):
        here = Path(current)
        kept = []
        for name in sorted(dirs):
            child = here / name
            if name.lower() in FORBIDDEN_PARTS or name.lower().endswith((".cache", ".runs")):
                continue
            require(not reparse(child), f"Refusing directory link/junction: {child}")
            kept.append(name)
        dirs[:] = kept
        for name in sorted(names):
            path = here / name
            require(not reparse(path), f"Refusing linked input: {path}")
            if path.is_file():
                yield path


def admissible(path: Path, include_dcp: bool = False) -> bool:
    lowered = [part.lower() for part in path.parts]
    if path.suffix.lower() == ".dcp" and path.name.upper().startswith("OLD_DIAGNOSTIC"):
        return False
    if any(part in FORBIDDEN_PARTS or part.endswith((".cache", ".runs")) for part in lowered):
        return False
    if path.suffix.lower() in FORBIDDEN_SUFFIXES:
        return False
    if path.suffix.lower() == ".dcp" and not include_dcp:
        return False
    return not path.name.lower().endswith((".backup", ".bak", ".tmp", "~"))


def safe_member(name: str) -> None:
    p = PurePosixPath(name)
    require(bool(name) and not p.is_absolute(), f"Unsafe ZIP member: {name!r}")
    require(not any(part in {"", ".", ".."} for part in name.split("/")), f"Unsafe ZIP member: {name!r}")
    require(not any(char in name for char in "\\:\r\n\t\x00"), f"Unsafe ZIP member: {name!r}")
    require(not name.endswith("/"), f"Unexpected directory ZIP entry: {name}")
    require(name in {README_NAME, MANIFEST_NAME} or p.parts[0] in {"project", "evidence"}, f"Unexpected ZIP root: {name}")


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True) + "\n").encode("utf-8")


def resolve_reference(raw: str, root: Path, project_name: str) -> Path:
    aliases = {
        "PPRDIR": root,
        "PSRCDIR": root / f"{project_name}.srcs",
        "PGENDIR": root / f"{project_name}.gen",
        "PIPUSERFILESDIR": root / f"{project_name}.ip_user_files",
        "PCACHEDIR": root / f"{project_name}.cache",
        "PRUNDIR": root / f"{project_name}.runs",
    }
    text = raw.strip().replace("\\", "/")
    for name, path in aliases.items():
        text = text.replace("${" + name + "}", path.as_posix())
        text = re.sub(r"\$" + name + r"(?=/|$)", lambda _m, value=path.as_posix(): value, text)
    require("$" not in text, f"Unresolved project path variable: {raw}")
    path = Path(text)
    return (path if path.is_absolute() else root / path).resolve()


def read_references(tsv: Path, xpr: Path, root: Path) -> list[dict]:
    require(tsv.is_file(), f"Final active-reference export required: {tsv}")
    references = []
    with tsv.open("r", encoding="utf-8-sig", newline="") as stream:
        rows = csv.DictReader(stream, delimiter="\t")
        require(set(rows.fieldnames or []) >= {"path", "exists", "type", "used_in"}, "active_references.tsv must contain path, exists, type, used_in")
        for line, row in enumerate(rows, 2):
            require(bool(row.get("path")), f"Empty active reference on line {line}")
            require(row["exists"] in {"0", "1"}, f"Invalid exists field on line {line}")
            references.append({"origin": f"active_references.tsv:{line}", "raw": row["path"], "exported_exists": row["exists"] == "1", "type": row["type"], "used_in": row["used_in"]})
    require(bool(references), "Empty active-reference manifest")
    for entry in ET.parse(xpr).getroot().findall(".//FileSets/FileSet/File"):
        raw = entry.get("Path")
        require(bool(raw), "XPR File entry without Path")
        used_in = [attr.get("Val", "") for attr in entry.findall("./FileInfo/Attr") if attr.get("Name") == "UsedIn"]
        references.append({"origin": "XPR File/Path", "raw": raw, "used_in": " ".join(used_in)})
    for record in references:
        resolved = resolve_reference(record["raw"], root, xpr.stem)
        # The exported set contains file inputs. Reject even unused Downloads
        # references so the delivery cannot quietly preserve a stale dependency.
        require("downloads" not in [part.lower() for part in resolved.parts], f"Downloads reference is forbidden: {record['raw']}")
        record["resolved"] = str(resolved)
        record["exists"] = resolved.exists()
        if "exported_exists" in record:
            require(record["exported_exists"] == record["exists"], f"Reference export is stale (existence changed): {resolved}")
    return references


def collect(args: argparse.Namespace) -> tuple[dict[str, Path | bytes], dict]:
    root = args.root.resolve()
    delivery = args.delivery.resolve()
    backup = args.backup.resolve()
    require(root.is_dir(), f"Missing project: {root}")
    require(delivery.is_dir(), f"Missing delivery directory: {delivery}")
    require(backup.is_dir() and not under(backup, root), "Baseline backup must exist outside the project")
    xprs = sorted(root.glob("*.xpr"))
    require(len(xprs) == 1, f"Expected exactly one root XPR, got {len(xprs)}")
    xpr = xprs[0]
    readme = args.readme.resolve()
    require(readme.is_file() and readme.stat().st_size > 0, f"Nonempty delivery README required: {readme}")
    references_path = args.references.resolve()
    require(references_path.is_file(), f"Final active-reference export required: {references_path}")
    references_sha256 = digest_file(references_path)
    xpr_sha256 = digest_file(xpr)
    sources: dict[str, Path | bytes] = {}
    folded: dict[str, str] = {}

    def add(name: str, source: Path | bytes) -> None:
        safe_member(name)
        previous = folded.get(name.casefold())
        require(previous is None or previous == name, f"Case-insensitive ZIP collision: {previous} / {name}")
        if name in sources:
            require(sources[name] == source, f"ZIP member conflict: {name}")
            return
        if isinstance(source, Path):
            require(source.is_file() and not reparse(source), f"Invalid source: {source}")
        folded[name.casefold()] = name
        sources[name] = source

    def project_file(path: Path) -> None:
        require(under(path.resolve(), root), f"Project input escapes root: {path}")
        rel = path.relative_to(root)
        require(admissible(rel, args.include_dcp), f"Excluded active project input: {rel} (referenced DCP requires --include-dcp)")
        add("project/" + rel.as_posix(), path)

    project_file(xpr)
    for relative in (
        "rtl", "constraints", "sim", "docs", "scripts",
        f"{xpr.stem}.srcs/sources_1/bd/radar_system",
        f"{xpr.stem}.gen/sources_1/bd/radar_system",
        f"{xpr.stem}.gen/sources_1/bd/mref/radar_capture_bd",
    ):
        directory = root / relative
        require(directory.is_dir(), f"Required project tree missing: {directory}")
        for path in walk_files(directory):
            if admissible(path.relative_to(root), args.include_dcp):
                project_file(path)
    ipuser = root / f"{xpr.stem}.ip_user_files"
    # Shared HDL/init dependencies and current BD exports; do not sweep the
    # stale standalone IP directory or historical simulator build directories.
    for relative in ("bd/radar_system", "ipstatic", "mem_init_files", "sim_scripts/radar_system"):
        directory = ipuser / relative
        if directory.is_dir():
            for path in walk_files(directory):
                if admissible(path.relative_to(root), args.include_dcp):
                    project_file(path)

    refs = read_references(references_path, xpr, root)
    require(digest_file(references_path) == references_sha256 and digest_file(xpr) == xpr_sha256, "XPR/reference export changed while reading the dependency inventory")
    tool_roots = [path.resolve() for path in args.tool_root]
    missing = []
    external = []
    for record in refs:
        path = Path(record["resolved"])
        inside_project = under(path, root)
        inside_tool = any(under(path, tool) for tool in tool_roots)
        require(inside_project or inside_tool, f"Nonportable external input outside approved tool roots: {path}")
        if not record["exists"]:
            require(record["origin"] != "XPR File/Path" and not record.get("used_in", "").strip(), f"Required XPR or active source is missing: {path}")
            record["disposition"] = "missing_at_export"
            missing.append(record)
            continue
        require(path.is_file(), f"Expected file input, got directory: {path}")
        if inside_project:
            member = "project/" + path.relative_to(root).as_posix()
            # Any additional explicitly referenced current IP-user file is a
            # required dependency; other omitted inputs must be fixed upstream.
            if member not in sources and under(path, ipuser):
                project_file(path)
            require(member in sources, f"Existing referenced project input is not packaged: {path}")
            record.update(disposition="packaged", member=member)
        else:
            record["disposition"] = "external_tool_installation"
            external.append(record)

    add(README_NAME, readme)
    for path in walk_files(delivery):
        if path == readme or path.name.startswith(args.zip.name):
            continue
        if admissible(path.relative_to(delivery), args.include_dcp):
            add("evidence/delivery/" + path.relative_to(delivery).as_posix(), path)
    # The reference manifest must be included even when provided from elsewhere.
    add("evidence/active_references.tsv", references_path)
    output_dirs = []
    outputs = root / "outputs"
    if outputs.is_dir():
        for directory in sorted(outputs.glob("m2_real_*")):
            if not directory.is_dir():
                continue
            output_dirs.append(directory.name)
            for path in walk_files(directory):
                relative = path.relative_to(outputs)
                allowed_suffix = path.suffix.lower() in EVIDENCE_SUFFIXES or (args.include_dcp and path.suffix.lower() == ".dcp")
                if allowed_suffix and admissible(relative, args.include_dcp):
                    add("evidence/outputs/" + relative.as_posix(), path)
    require(bool(output_dirs), "No current outputs/m2_real_* evidence trees found")

    backup_zip = backup / "baseline.zip"
    backup_hashes = backup / "baseline_hashes.json"
    require(backup_zip.is_file() and backup_hashes.is_file(), "External baseline.zip and baseline_hashes.json are required")
    add("evidence/baseline/baseline_hashes.json", backup_hashes)
    add("evidence/baseline/backup_reference.json", json_bytes({
        "location": str(backup), "archive": str(backup_zip),
        "archive_bytes": backup_zip.stat().st_size, "archive_sha256": digest_file(backup_zip),
        "baseline_hashes_sha256": digest_file(backup_hashes),
        "backup_archive_included": False,
    }))
    audit = {
        "schema": "m2-review-package-v1", "project_root": str(root),
        "project_xpr_member": "project/" + xpr.name,
        "project_xpr_sha256": xpr_sha256,
        "active_references_sha256": references_sha256,
        "backup_outside_project": True, "backup_directory": str(backup),
        "source_timing": "PENDING", "board_electrical": "PENDING",
        "download_ready": False, "active_downloads_references": 0,
        "include_dcp": args.include_dcp, "output_directories": output_dirs,
        "references": refs, "existing_project_inputs_packaged": sum(r.get("disposition") == "packaged" for r in refs),
        "missing_at_export_count": len(missing), "external_tool_reference_count": len(external),
        "approved_external_tool_roots": [str(path) for path in tool_roots],
        "note": "Only inactive, non-XPR missing references may be recorded. Missing XPR/active inputs or non-tool external references fail packaging. Native ZIP reopen/build and physical operation require separate evidence.",
    }
    add("evidence/package_audit.json", json_bytes(audit))
    return sources, audit


def zip_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (stat.S_IFREG | 0o644) << 16
    return info


def verify(archive_path: Path) -> dict:
    require(archive_path.is_file(), f"Archive missing: {archive_path}")
    with zipfile.ZipFile(archive_path, "r") as archive:
        infos = archive.infolist()
        names = [entry.filename for entry in infos]
        require(len(names) == len(set(names)) == len({name.casefold() for name in names}), "Duplicate or case-colliding ZIP members")
        for entry in infos:
            safe_member(entry.filename)
            require(not stat.S_ISLNK(entry.external_attr >> 16), f"ZIP symlink forbidden: {entry.filename}")
            require(admissible(PurePosixPath(entry.filename), include_dcp=True), f"Forbidden ZIP member: {entry.filename}")
        require(MANIFEST_NAME in names and README_NAME in names, "ZIP missing manifest or delivery README")
        hashes = {}
        for line in archive.read(MANIFEST_NAME).decode("utf-8").splitlines():
            match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
            require(match is not None, f"Malformed SHA-256 manifest line: {line!r}")
            expected, name = match.groups()
            safe_member(name)
            require(name not in hashes and name != MANIFEST_NAME, f"Duplicate/self manifest entry: {name}")
            hashes[name] = expected
        require(set(hashes) == set(names) - {MANIFEST_NAME}, "Manifest/member set mismatch")
        for name, expected in hashes.items():
            with archive.open(name, "r") as stream:
                require(digest_stream(stream) == expected, f"SHA-256 mismatch: {name}")
        bad = archive.testzip()
        require(bad is None, f"ZIP CRC failure: {bad}")
        audit = json.loads(archive.read("evidence/package_audit.json"))
        require(audit.get("schema") == "m2-review-package-v1", "Unexpected package audit schema")
        require(audit.get("active_downloads_references") == 0, "Audit contains Downloads references")
        require(audit.get("backup_outside_project") is True, "External backup assertion missing")
        require(audit["project_xpr_member"] in hashes, "Project XPR absent")
        require(hashes[audit["project_xpr_member"]] == audit["project_xpr_sha256"], "Packaged XPR differs from dependency audit input")
        require(hashes.get("evidence/active_references.tsv") == audit["active_references_sha256"], "Packaged reference export differs from dependency audit input")
        require(audit.get("include_dcp") or not any(name.lower().endswith(".dcp") for name in names), "Unrequested DCP found in archive")
        for record in audit["references"]:
            if record.get("disposition") == "packaged":
                require(record.get("member") in hashes, f"Referenced input absent: {record}")
            elif record.get("disposition") == "missing_at_export":
                require(record["origin"] != "XPR File/Path" and not record.get("used_in", "").strip(), f"Required source is missing in dependency audit: {record}")
            else:
                require(record.get("disposition") == "external_tool_installation", f"Unknown dependency disposition: {record}")
            require("downloads" not in [part.lower() for part in record["resolved"].replace("\\", "/").split("/")], "Downloads dependency found in audit")
    return {"status": "PASS", "members": len(names), "hashed_members": len(hashes), "crc": "PASS", "archive_sha256": digest_file(archive_path), "source_timing": "PENDING", "board_electrical": "PENDING", "audit": audit}


def build(args: argparse.Namespace) -> dict:
    archive_path = args.zip.resolve()
    partial = archive_path.with_name(archive_path.name + ".partial")
    companion = archive_path.with_name(archive_path.name + ".sha256")
    for path in (archive_path, partial, companion):
        require(not path.exists(), f"Refusing to overwrite existing artifact: {path}")
    sources, _audit = collect(args)
    archive_path.parent.mkdir(parents=True, exist_ok=True)
    hashes = {}
    with zipfile.ZipFile(partial, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
        for name, source in sorted(sources.items()):
            digest = hashlib.sha256()
            stream = source.open("rb") if isinstance(source, Path) else io.BytesIO(source)
            with stream, archive.open(zip_info(name), "w", force_zip64=True) as destination:
                for block in iter(lambda: stream.read(CHUNK), b""):
                    digest.update(block)
                    destination.write(block)
            hashes[name] = digest.hexdigest()
        manifest = "".join(f"{hashes[name]}  {name}\n" for name in sorted(hashes))
        archive.writestr(zip_info(MANIFEST_NAME), manifest.encode("utf-8"))
    # Rehash all source files after writing: reject a mixed-time delivery if
    # a build/editor changed any input while the package was being produced.
    for name, source in sources.items():
        if isinstance(source, Path):
            require(source.is_file() and digest_file(source) == hashes[name], f"Source changed during packaging: {source}; partial ZIP retained")
    result = verify(partial)
    require(not archive_path.exists(), f"Archive appeared during build: {archive_path}")
    partial.rename(archive_path)
    with companion.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(f"{result['archive_sha256']}  {archive_path.name}\n")
    result["archive"] = str(archive_path)
    return result


def extract_new(archive_path: Path, target: Path, verification: dict) -> str:
    require(target.is_absolute(), "--extract must name an exact absolute NEW directory")
    require(not target.exists() and not target.is_symlink(), f"Extraction destination already exists: {target}")
    require(target.parent.is_dir(), f"Extraction parent does not exist: {target.parent}")
    target = target.resolve()
    original = Path(verification["audit"]["project_root"]).resolve()
    require(not under(target, original), "Extraction directory must be outside the original project")
    backup = Path(verification["audit"]["backup_directory"]).resolve()
    require(not under(target, backup), "Extraction directory must be outside the baseline backup")
    require(digest_file(archive_path) == verification["archive_sha256"], "Archive changed after verification")
    target.mkdir(exist_ok=False)
    with zipfile.ZipFile(archive_path, "r") as archive:
        for entry in archive.infolist():
            safe_member(entry.filename)
            path = target.joinpath(*PurePosixPath(entry.filename).parts)
            require(under(path.resolve(), target), f"Extraction path escapes target: {entry.filename}")
            path.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry, "r") as source, path.open("xb") as destination:
                for block in iter(lambda: source.read(CHUNK), b""):
                    destination.write(block)
        for line in archive.read(MANIFEST_NAME).decode("utf-8").splitlines():
            expected, name = line.split("  ", 1)
            require(digest_file(target.joinpath(*PurePosixPath(name).parts)) == expected, f"Extracted hash mismatch: {name}")
    return str(target)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=("build", "verify"))
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--delivery", type=Path, help="Defaults to ROOT/delivery/20261012_171114")
    parser.add_argument("--zip", type=Path, help="Exact ZIP path; build refuses an existing file")
    parser.add_argument("--readme", type=Path, help="Defaults to DELIVERY/README_交付说明.md")
    parser.add_argument("--references", type=Path, help="Defaults to DELIVERY/active_references.tsv")
    parser.add_argument("--backup", type=Path, default=Path("/archive") / BUILD_ID)
    parser.add_argument("--tool-root", type=Path, action="append", default=[Path("C:/Xilinx/2025.1")], help="Approved external tool installation; repeatable")
    parser.add_argument("--include-dcp", action="store_true", help="Include current DCP evidence from delivery/output trees")
    parser.add_argument("--extract", type=Path, help="After successful verification, extract into exactly this NEW absolute directory")
    args = parser.parse_args()
    args.root = args.root.resolve()
    args.delivery = args.delivery or args.root / "delivery" / BUILD_ID
    args.zip = args.zip or args.delivery / ZIP_NAME
    args.readme = args.readme or args.delivery / README_NAME
    args.references = args.references or args.delivery / "active_references.tsv"
    return args


def main() -> int:
    args = arguments()
    try:
        result = build(args) if args.mode == "build" else verify(args.zip.resolve())
        if args.extract is not None:
            result["extracted_to"] = extract_new(args.zip.resolve(), args.extract, result)
        # Full reference records are already included in the archive audit.
        result.pop("audit", None)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (PackageError, OSError, ValueError, KeyError, zipfile.BadZipFile, ET.ParseError) as error:
        print(f"PACKAGE_FAIL: {error}\nNo cleanup/delete was performed; inspect any partial artifacts.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
