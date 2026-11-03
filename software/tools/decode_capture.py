#!/usr/bin/env python3
"""Decode one complete @R4 export from a serial text log, verify CRC, save bin+JSON.

No ADC/IQ parsing. Default mode requires a single completed export. Use --session
and --occurrence (1-based) to choose explicitly when a log contains multiple.
An interrupted export is ignored ONLY if a subsequent BEGIN restarts it; an
invalid/duplicate/out-of-order record inside a completed export is an error.
A trailing incomplete export is rejected, even if an earlier export completed.
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import sys
import zlib

class DecodeError(ValueError):
    pass

@dataclass
class Capture:
    session: int
    size: int
    crc32: int
    metadata: dict[str, str] = field(default_factory=dict)
    payload: bytearray = field(default_factory=bytearray)
    begin_line: int = 0
    end_line: int = 0

MANDATORY = {"software", "profile_id", "firmware_sha256_expected", "cfg_sha256_expected", "review_sha256", "xsa_sha256_expected", "bit_sha256_expected", "transport", "geometry", "cpu_times_us", "sys_observed_ticks", "snapshot_generations", "snapshot_status", "dma_before_reset"} | {f"rx_word_{i:02d}" for i in range(12)}

def captures_from_log(text: str) -> list[Capture]:
    complete: list[Capture] = []
    current: Capture | None = None
    for lineno, raw in enumerate(text.splitlines(), 1):
        # Permit a timestamp prefix from serial monitors. Never split/execute it.
        pos = raw.find("@R4")
        if pos < 0:
            continue
        line = raw[pos:].strip()
        try:
            if line.startswith("@R4BEGIN "):
                m = re.fullmatch(r"@R4BEGIN (\d+) (\d+) ([0-9a-fA-F]{8})", line)
                if not m:
                    raise DecodeError("malformed BEGIN")
                sid, size = int(m[1]), int(m[2])
                if not 1 <= sid <= 0xFFFFFFFF or not 8 <= size <= 8388608 or size % 8:
                    raise DecodeError("BEGIN session/size out of range")
                current = Capture(sid, size, int(m[3], 16), begin_line=lineno)
            elif line.startswith("@R4META "):
                if current is None:
                    raise DecodeError("META without BEGIN")
                m = re.fullmatch(r"@R4META ([a-zA-Z0-9_]+) (.+)", line)
                if not m or current.payload or m[1] in current.metadata:
                    raise DecodeError("bad/duplicate/late metadata")
                current.metadata[m[1]] = m[2]
            elif line.startswith("@R4DATA "):
                if current is None:
                    raise DecodeError("DATA without BEGIN")
                m = re.fullmatch(r"@R4DATA (\d+) (\d+) ([0-9a-fA-F]{2,256})", line)
                if not m or len(m[3]) % 2:
                    raise DecodeError("malformed DATA")
                if int(m[1]) != current.session or int(m[2]) != len(current.payload):
                    raise DecodeError("session/offset mismatch (missing, duplicate or reordered DATA)")
                data = bytes.fromhex(m[3])
                if len(current.payload) + len(data) > current.size:
                    raise DecodeError("data exceeds declared size")
                current.payload.extend(data)
            elif line.startswith("@R4END "):
                if current is None:
                    raise DecodeError("END without BEGIN")
                m = re.fullmatch(r"@R4END (\d+) (\d+) ([0-9a-fA-F]{8})", line)
                if not m or (int(m[1]),int(m[2]),int(m[3],16)) != (current.session,current.size,current.crc32):
                    raise DecodeError("END does not match BEGIN")
                if len(current.payload) != current.size or zlib.crc32(current.payload) != current.crc32:
                    raise DecodeError("length/CRC32 verification failed")
                if not MANDATORY <= current.metadata.keys():
                    raise DecodeError("required metadata missing: " + ",".join(sorted(MANDATORY-current.metadata.keys())))
                for key in ("firmware_sha256_expected", "cfg_sha256_expected", "review_sha256", "xsa_sha256_expected", "bit_sha256_expected"):
                    if not re.fullmatch(r"[0-9a-f]{64}", current.metadata[key]):
                        raise DecodeError("invalid identity metadata: " + key)
                geom = current.metadata["geometry"].split()
                if len(geom) != 5 or int(geom[0])*int(geom[1]) != current.size:
                    raise DecodeError("geometry does not match data length")
                export_version = re.fullmatch(r"M2-SW4\.1-v(\d+)\.(\d+)", current.metadata["software"])
                if export_version and tuple(map(int, export_version.groups())) >= (1, 2):
                    if not {"qualification", "review_bits", "validation_scope"} <= current.metadata.keys():
                        raise DecodeError("new export is missing its qualification/evidence scope")
                qualification = current.metadata.get("qualification")
                if qualification is not None:
                    if qualification not in {"QUALIFIED", "FIRST_PACKET_TEST", "FINITE_FRAME_TEST"}:
                        raise DecodeError("unknown capture qualification")
                    bits = current.metadata.get("review_bits", "").split()
                    if len(bits) != 2 or any(not re.fullmatch(r"[0-9a-f]{2}", x) for x in bits):
                        raise DecodeError("missing or invalid review bits")
                    approvals, source_reviews = map(lambda x: int(x, 16), bits)
                    if qualification == "FIRST_PACKET_TEST":
                        if current.metadata.get("validation_scope") != "INPUT_TIMING_AND_WIRE_FORMAT_PENDING":
                            raise DecodeError("first-packet experiment must retain pending timing/wire scope")
                        if (approvals & 0x83) != 0x83 or source_reviews != 7 or geom != ["131072", "1", "8", "0", "2"]:
                            raise DecodeError("invalid first-packet experiment contract")
                    elif qualification == "FINITE_FRAME_TEST":
                        if (approvals & 0x83) != 0x83 or source_reviews != 7 or current.metadata.get("validation_scope") != "INPUT_TIMING_AND_WIRE_FORMAT_PENDING":
                            raise DecodeError("invalid finite-frame trial evidence scope")
                        finite = current.metadata.get("finite_source", "").split()
                        allowed = [(["399360", "1", "8", "0", "2"], ["1", "1", "96", "256", "15"]),
                                   (["266240", "1", "8", "0", "2"], ["1", "1", "64", "256", "15"]),
                                   (["262144", "1", "8", "0", "2"], ["1", "0", "64", "256", "15"]),
                                   (["262144", "16", "8", "0", "2"], ["16", "0", "64", "256", "15"])]
                        if (geom, finite) not in allowed:
                            raise DecodeError("unsupported finite-frame geometry/source metadata")
                    elif approvals != 0xff or current.metadata.get("validation_scope") != "PROFILE_REVIEWED_NOT_LIVE_MEASUREMENT":
                        raise DecodeError("invalid qualified profile review metadata")
                current.end_line=lineno
                complete.append(current)
                current=None
            else:
                raise DecodeError("unknown R4 record")
        except (ValueError, IndexError) as exc:
            raise DecodeError(f"line {lineno}: {exc}") from exc
    if current is not None:
        raise DecodeError(
            f"incomplete trailing export: session {current.session}, BEGIN line "
            f"{current.begin_line}, received {len(current.payload)}/{current.size} bytes, "
            "END missing; refusing to reuse an earlier completed export")
    if not complete:
        raise DecodeError("no completed, verified export (BEGIN/DATA/END all required)")
    return complete

def save_capture(capture: Capture, prefix: Path, overwrite: bool = False) -> tuple[Path, Path]:
    binary = prefix.with_suffix(".bin")
    metadata = prefix.with_suffix(".json")
    if not overwrite and (binary.exists() or metadata.exists()):
        raise DecodeError("output exists; use --force or another prefix")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    meta = dict(schema=1, session=capture.session, bytes=capture.size, crc32=f"{capture.crc32:08x}",
                begin_line=capture.begin_line, end_line=capture.end_line, metadata=capture.metadata,
                validation="SERIAL_EXPORT_LENGTH_CRC_ONLY", adc_semantics="NOT_VERIFIED")
    meta["qualification"] = capture.metadata.get("qualification", "LEGACY_UNSPECIFIED")
    meta["input_timing"] = "NOT_VERIFIED_BY_EXPORT"
    # JSON becomes the final completion marker. Temporary files never appear as
    # valid exports; no successful return before both replacements complete.
    tmpb = binary.with_name(binary.name+".tmp")
    tmpj = metadata.with_name(metadata.name+".tmp")
    tmpb.write_bytes(capture.payload)
    tmpj.write_text(json.dumps(meta, indent=2, ensure_ascii=False)+"\n", encoding="utf-8")
    tmpb.replace(binary)
    tmpj.replace(metadata)
    return binary, metadata

def main() -> int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    ap.add_argument("--out", required=True, type=Path, help="output prefix, not directory")
    ap.add_argument("--session", type=int)
    ap.add_argument("--occurrence", type=int, default=None)
    ap.add_argument("--force", action="store_true")
    a=ap.parse_args()
    try:
        captures=captures_from_log(a.log.read_text(encoding="utf-8-sig", errors="strict"))
        selected=[c for c in captures if a.session is None or c.session==a.session]
        if a.occurrence is None:
            if len(selected)!=1:
                raise DecodeError(f"{len(selected)} matching complete exports; select --session/--occurrence explicitly")
            capture=selected[0]
        else:
            if not 1 <= a.occurrence <= len(selected):
                raise DecodeError("occurrence out of range")
            capture=selected[a.occurrence-1]
        b,j=save_capture(capture,a.out,a.force)
        print(f"PASS session={capture.session} bytes={capture.size} CRC32={capture.crc32:08x}\n{b}\n{j}\nRAW16 transport retained; not an ADC/frame proof.")
        return 0
    except (OSError, UnicodeError, DecodeError) as exc:
        print("ERROR: "+str(exc), file=sys.stderr)
        return 2

if __name__=="__main__":
    raise SystemExit(main())
