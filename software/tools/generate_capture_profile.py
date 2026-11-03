#!/usr/bin/env python3
"""Generate a reviewed, finite SW4 profile from actual firmware/config bytes.

This verifies identities and structural constraints; it does not measure IO
voltages/timing, identify the board's flash or prove an ADC interpretation.
All approval statements must be supported by the user's review evidence.
FIRST_PACKET_TEST retains pending dynamic checks and admits only one bounded
128 KiB trial after electrical/identity/geometry approval and file-backed
protocol, wire-format and startup-preamble source compatibility reviews.
FINITE_FRAME_TEST binds a reviewed 2TX legacy or 3TX single-HSI geometry;
it retains pending input timing and ADC alignment checks.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

APPROVALS = (
    "board_revision_and_electrical_verified",
    "actual_flashed_firmware_matches_file",
    "uart_version_and_commands_verified",
    "two_lane_600mbps_raw16_wire_format_verified",
    "source_timing_and_termination_verified",
    "idle_clock_or_start_preamble_verified",
    "sensor_stop_and_reset_non_autostart_verified",
    "finite_transport_geometry_and_metadata_reviewed",
)
QUALIFICATIONS = {"QUALIFIED": 0, "FIRST_PACKET_TEST": 1, "FINITE_FRAME_TEST": 2, "STREAM_FRAME_TEST": 3}
TRIAL_APPROVAL_MASK = 0x83
SOURCE_REVIEWS = ("uart_protocol", "wire_format", "startup_preamble")
VERBS = frozenset("flushCfg dfeDataOutputMode channelCfg adcCfg adcbufCfg profileCfg chirpCfg frameCfg advFrameCfg subFrameCfg lowPower lvdsStreamCfg guiMonitor cfarCfg peakGrouping multiObjBeamForming calibDcRangeSig clutterRemoval compRangeBiasAndRxChanPhase measureRangeBiasAndRxChanPhase extendedMaxVelocity bpmCfg CQRxSatMonitor CQSigImgMonitor analogMonitor aoaFovCfg cfarFovCfg calibData".split())
REQUIRED_VERBS = {"flushCfg", "dfeDataOutputMode", "channelCfg", "adcCfg", "adcbufCfg", "profileCfg", "chirpCfg", "lvdsStreamCfg"}
MAX_BYTES = 8 * 1024 * 1024
# The finite-frame experiment freezes the existing waveform. Supporting a new
# waveform is a separate review, not an arbitrary cfg-to-size guess.
FINITE_FIXED_COMMANDS = {
    "flushCfg": ("flushCfg",),
    "dfeDataOutputMode": ("dfeDataOutputMode 1",),
    "adcCfg": ("adcCfg 2 1",),
    "adcbufCfg": ("adcbufCfg -1 0 1 1 1",),
    "profileCfg": ("profileCfg 0 60 7 3 24 0 0 166 1 256 12500 0 0 158",),
    "bpmCfg": ("bpmCfg -1 0 0 0",),
    "lowPower": ("lowPower 0 0",),
}

class ProfileError(ValueError):
    pass

def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def integer(review: dict, key: str, lo: int, hi: int) -> int:
    value = review.get(key)
    if type(value) is not int or not lo <= value <= hi:
        raise ProfileError(f"{key} must be an integer in [{lo}, {hi}]")
    return value

def printable(value: object, field: str, lo: int, hi: int) -> str:
    if not isinstance(value, str) or not lo <= len(value) <= hi or any(not 32 <= ord(c) <= 126 for c in value):
        raise ProfileError(f"{field} must be {lo}..{hi} printable ASCII characters")
    return value

def cfg_commands(raw: bytes) -> list[str]:
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ProfileError("cfg must be UTF-8/ASCII") from exc
    result: list[str] = []
    started = False
    for lineno, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith(("%", "#", "//")):
            continue
        if started:
            raise ProfileError(f"cfg:{lineno}: command after sensorStart; supply a single complete configuration")
        if not 1 <= len(line) <= 239 or any(not 32 <= ord(c) <= 126 for c in line) or ";" in line:
            raise ProfileError(f"cfg:{lineno}: invalid character/length; no inline comments or command separators")
        if line == "sensorStart":
            started = True
            continue  # NEVER included in the configuration command table.
        if line == "sensorStop":
            if result:
                raise ProfileError(f"cfg:{lineno}: sensorStop is allowed only before configuration")
            continue  # supervisor owns stop separately, with a real ACK.
        verb = line.split(" ", 1)[0]
        if verb not in VERBS:
            raise ProfileError(f"cfg:{lineno}: unsupported verb {verb!r}; review the firmware/protocol, do not bypass")
        result.append(line)
    if not 1 <= len(result) <= 96:
        raise ProfileError("cfg must contain 1..96 configuration lines")
    seen = {line.split(" ", 1)[0] for line in result}
    if not REQUIRED_VERBS <= seen or not ({"frameCfg", "advFrameCfg"} & seen):
        missing = sorted(REQUIRED_VERBS - seen)
        raise ProfileError(f"cfg is not a full reviewed TI configuration; missing {missing}, plus frameCfg/advFrameCfg is required")
    if result[0] != "flushCfg":
        raise ProfileError("first configuration line must be exactly flushCfg")
    if sum(line == "flushCfg" for line in result) != 1:
        raise ProfileError("exactly one flushCfg is required")
    return result

def source_reviews(review: dict, review_dir: Path | None) -> tuple[int, list[dict]]:
    """Bind each compatibility claim to existing local evidence, without executing it."""
    if review_dir is None:
        raise ProfileError("trial qualification requires the review file directory for local evidence resolution")
    reviews = review.get("source_reviews")
    if not isinstance(reviews, dict):
        raise ProfileError("trial qualification requires source_reviews")
    bound = []
    for key in SOURCE_REVIEWS:
        item = reviews.get(key)
        if not isinstance(item, dict) or item.get("approved") is not True:
            raise ProfileError(f"unapproved source compatibility review: {key}")
        if not isinstance(item.get("summary"), str) or len(item["summary"].strip()) < 12:
            raise ProfileError(f"source review summary missing: {key}")
        refs = item.get("evidence")
        if not isinstance(refs, list) or not refs:
            raise ProfileError(f"local source evidence is required: {key}")
        for ref in refs:
            if not isinstance(ref, dict) or not isinstance(ref.get("path"), str) or not ref["path"].strip():
                raise ProfileError(f"invalid local evidence path: {key}")
            raw_path = ref["path"]
            if raw_path.startswith(("\\\\", "//")) or "://" in raw_path:
                raise ProfileError(f"evidence must be a local file, not a network reference: {key}")
            expected = ref.get("sha256")
            if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
                raise ProfileError(f"invalid evidence SHA-256: {key}")
            path = Path(raw_path)
            if not path.is_absolute():
                path = review_dir / path
            try:
                path = path.resolve(strict=True)
                if not path.is_file():
                    raise ProfileError(f"evidence is not a file: {key}: {raw_path}")
                actual = sha(path.read_bytes())
            except OSError as exc:
                raise ProfileError(f"cannot read local evidence: {key}: {raw_path}: {exc}") from exc
            if actual != expected:
                raise ProfileError(f"evidence SHA-256 mismatch: {key}: {raw_path}")
            bound.append({"review": key, "path": raw_path, "sha256": actual})
    return 7, bound


def finite_geometry(commands: list[str], size: int, count: int) -> dict:
    """Strict supported SDK 3.5 geometry, independently checked on target too."""
    by_verb: dict[str, list[str]] = {}
    for line in commands:
        by_verb.setdefault(line.split(" ", 1)[0], []).append(line)
    if "advFrameCfg" in by_verb or "subFrameCfg" in by_verb:
        raise ProfileError("FINITE_FRAME_TEST supports frameCfg only")
    for verb, expected in FINITE_FIXED_COMMANDS.items():
        if sorted(by_verb.get(verb, [])) != sorted(expected):
            raise ProfileError(f"FINITE_FRAME_TEST requires the frozen {verb} command(s)")
    channel = by_verb.get("channelCfg", [])
    if channel == ["channelCfg 15 5 0"]:
        tx_sequence = [0, 2]
    elif channel == ["channelCfg 15 7 0"]:
        tx_sequence = [0, 2, 1]
    else:
        raise ProfileError("FINITE_FRAME_TEST requires reviewed 4RX/2TX or 4RX/3TX channelCfg")
    expected_chirps = [f"chirpCfg {i} {i} 0 0 0 0 0 {1 << tx}" for i, tx in enumerate(tx_sequence)]
    if sorted(by_verb.get("chirpCfg", [])) != sorted(expected_chirps):
        raise ProfileError("FINITE_FRAME_TEST requires TX0/TX2 or TX0/TX2/TX1 TDM order")
    tx_count = len(tx_sequence)
    frame = by_verb.get("frameCfg", [])
    if frame == [f"frameCfg 0 {tx_count-1} 32 1 100 1 0"]:
        frames = 1
    elif tx_count == 2 and frame == ["frameCfg 0 1 32 16 100 1 0"]:
        frames = 16
    elif tx_count == 3 and frame in (["frameCfg 0 2 32 8 100 1 0"], ["frameCfg 0 2 32 16 100 1 0"]):
        frames = int(frame[0].split()[4])
    else:
        raise ProfileError("FINITE_FRAME_TEST requires reviewed 1/8/16 source frames at the frozen 100ms geometry")
    lvds = by_verb.get("lvdsStreamCfg", [])
    if lvds == ["lvdsStreamCfg -1 1 1 0"]:
        hsi = 1
    elif lvds == ["lvdsStreamCfg -1 0 1 0"]:
        hsi = 0
    else:
        raise ProfileError("FINITE_FRAME_TEST requires ADC-only hardware stream and no software stream")
    # SDK 3.5 HSI uses 52 base header bytes rounded to 16B for this HW stream.
    # This is a prediction; the receiver must still validate every actual header.
    payload = 256 * 4 * 2 * 2
    record = payload + (64 if hsi else 0)
    frame_bytes = record * (tx_count * 32)
    if (tx_count == 2 and hsi and frames != 1) or (tx_count == 3 and (not hsi or frames not in (1, 8, 16))) or (size, count) != (frame_bytes, frames):
        raise ProfileError("FINITE_FRAME_TEST permits 3TX HSI 399360x1/x8/x16, or legacy 2TX HSI 266240x1 / ADC 262144x1 or x16")
    return {"source_frames": frames, "hsi_header_enabled": hsi,
            "chirps_per_frame": tx_count * 32, "samples_per_rx": 256, "rx_mask": 15,
            "rx_channels": 4, "tx_sequence": tx_sequence, "tx_mask": sum(1 << tx for tx in tx_sequence),
            "tx_count": tx_count, "loops": 32, "frame_period_ms": 100,
            "adc_sample_swap": 1, "channel_interleave": 1,
            "adc_bytes_per_chirp": payload, "expected_header_bytes": 64 if hsi else 0,
            "wire_bytes_per_chirp": record, "wire_bytes_per_frame": frame_bytes}


def geometry_header(manifest: dict) -> str:
    """Pure constants shared by the target DSP/scheduler; generated, not inferred."""
    g = manifest["finite_geometry"]
    fields = {
        "PROFILE_ID": json.dumps(manifest["profile_id"]),
        "CFG_SHA256": json.dumps(manifest["cfg_sha256"]),
        "CHIRPS": f'{g["chirps_per_frame"]}U',
        "RX": f'{g["rx_channels"]}U', "TX": f'{g["tx_count"]}U',
        "TX_MASK": f'{g["tx_mask"]}U', "LOOPS": f'{g["loops"]}U',
        "TX_SEQUENCE": '{' + ','.join(f'{tx}U' for tx in g["tx_sequence"]) + '}',
        "SAMPLES": f'{g["samples_per_rx"]}U',
        "RECORD_BYTES": f'{g["wire_bytes_per_chirp"]}U',
        "FRAME_BYTES": f'{g["wire_bytes_per_frame"]}U',
    }
    return ('/* SPDX-License-Identifier: MIT; GENERATED by generate_capture_profile.py. */\n'
            '#ifndef RADAR_CAPTURE_GEOMETRY_H\n#define RADAR_CAPTURE_GEOMETRY_H\n' +
            ''.join(f'#define RADAR_CAPTURE_GEOMETRY_{key} {value}\n' for key, value in fields.items()) +
            '#endif\n')


def stream_geometry(commands: list[str], size: int, count: int) -> dict:
    if count not in (0, 16, 64) or size != 399360:
        raise ProfileError("STREAM_FRAME_TEST requires 399360 bytes and16/64/0 frames")
    frame = [x for x in commands if x.startswith("frameCfg ")]
    if frame != [f"frameCfg 0 2 32 {count} 100 1 0"]:
        raise ProfileError("STREAM_FRAME_TEST requires exact3TX frozen waveform/frame count")
    normalized = ["frameCfg 0 2 32 16 100 1 0" if x.startswith("frameCfg ") else x for x in commands]
    geometry = finite_geometry(normalized, size, 16)
    geometry["source_frames"] = count
    geometry["stream_window_frames"] = 18
    geometry["raw_slots"] = 16
    return geometry

def generate(firmware: bytes, cfg: bytes, review_raw: bytes,
             review_dir: Path | None = None) -> tuple[str, dict]:
    if not firmware:
        raise ProfileError("firmware file is empty")
    try:
        r = json.loads(review_raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProfileError(f"invalid review JSON: {exc}") from exc
    if not isinstance(r, dict) or type(r.get("schema")) is not int or r.get("schema") != 1:
        raise ProfileError("review schema must be 1")
    qualification_name = r.get("qualification", "QUALIFIED")
    if not isinstance(qualification_name, str) or qualification_name not in QUALIFICATIONS:
        raise ProfileError("qualification must be QUALIFIED, FIRST_PACKET_TEST or FINITE_FRAME_TEST")
    qualification = QUALIFICATIONS[qualification_name]
    if r.get("firmware_sha256") != sha(firmware) or r.get("cfg_sha256") != sha(cfg):
        raise ProfileError("review hashes do not match the ACTUAL input bin/cfg bytes; review again after any change")
    evidence = r.get("approvals")
    if not isinstance(evidence, dict):
        raise ProfileError("review.approvals is required")
    approvals = 0
    for index, key in enumerate(APPROVALS):
        item = evidence.get(key)
        if not isinstance(item, dict) or type(item.get("approved")) is not bool:
            raise ProfileError(f"approval must be a boolean: {key}")
        if (qualification == 0 or TRIAL_APPROVAL_MASK & (1 << index)) and not item["approved"]:
            raise ProfileError(f"unapproved: {key}")
        if not isinstance(item.get("evidence"), str) or len(item["evidence"].strip()) < 12:
            raise ProfileError(f"evidence reference/description missing: {key}")
        if item["approved"]:
            approvals |= 1 << index
    source_flags, source_evidence = source_reviews(r, review_dir) if qualification != 0 else (0, [])
    name = printable(r.get("name"), "name", 1, 64)
    version = printable(r.get("version_substring"), "version_substring", 5, 64)
    if not re.match(r"\d+(?:\.\d+)+", version):
        raise ProfileError("version_substring must begin with the actual dotted numeric SDK version, not a generic banner")
    lanes = integer(r, "lanes", 2, 2)
    rate = integer(r, "lane_mbps", 600, 600)
    bits = integer(r, "word_bits", 16, 16)
    fmt = integer(r, "format_flags", 0, 31)
    size = integer(r, "packet_bytes", 8, 8388600)
    count = integer(r, "packet_count", 0 if qualification == 3 else 1, 128)
    skip = integer(r, "skip_pairs", 0, 0xFFFFFFFF)
    if size % 8 or (qualification != 3 and (size * count > MAX_BYTES or ((size + 65535) // 65536) * count > 128)):
        raise ProfileError("finite geometry must fit both the 128x64KiB BD/RX pool and separate 8MiB result store; packet_bytes multiple of 8")
    modes = {"IDLE_CLOCK": 1, "REVIEWED_PREAMBLE": 2}
    mode = modes.get(r.get("startup_mode"))
    if mode is None:
        raise ProfileError("startup_mode must be IDLE_CLOCK or REVIEWED_PREAMBLE")
    receive = integer(r, "receive_timeout_us", 100000, 4000000)
    snapshot = integer(r, "snapshot_timeout_us", 1000, 1000000)
    delay = r.get("use_delay")
    if type(delay) is not bool:
        raise ProfileError("use_delay must be true/false")
    taps = r.get("taps")
    if not isinstance(taps, list) or len(taps) != 3 or any(type(x) is not int or not 0 <= x <= 511 for x in taps):
        raise ProfileError("taps must contain three absolute integers 0..511 (FR, DATA0, DATA1)")
    if delay and mode != 1:
        raise ProfileError("delay application requires IDLE_CLOCK and actual RX baseline before start")
    if qualification == 1 and (size != 131072 or count != 1 or fmt != 8 or skip != 0 or delay or mode != 2):
        raise ProfileError("FIRST_PACKET_TEST requires 131072x1, format_flags=8, skip_pairs=0, no delay and REVIEWED_PREAMBLE")
    commands = cfg_commands(cfg)
    geometry = None
    if qualification in (2, 3):
        if fmt != 8 or skip != 0 or delay or mode != 2:
            raise ProfileError("FINITE_FRAME_TEST requires format_flags=8, skip_pairs=0, no delay and REVIEWED_PREAMBLE")
        geometry = stream_geometry(commands, size, count) if qualification == 3 else finite_geometry(commands, size, count)
    identity = {"schema": 1, "firmware_sha256": sha(firmware), "cfg_sha256": sha(cfg), "review_sha256": sha(review_raw),
                "qualification": qualification_name, "source_review_flags": source_flags}
    pid = sha(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode())[:16]
    q = lambda value: json.dumps(value, ensure_ascii=True)
    header = """/* SPDX-License-Identifier: MIT
 * GENERATED from actual supplied files and explicit evidence review.
 * Expected identities only, not live flash/readback or electrical measurement.
 * Regenerate after ANY firmware/cfg/review change. Do not hand-edit approvals.
 */
#ifndef RADAR_CAPTURE_PROFILE_DATA_H
#define RADAR_CAPTURE_PROFILE_DATA_H
static const char *const radar_profile_commands[] = {
"""
    header += "".join(f"    {q(line)},\n" for line in commands)
    header += "};\nstatic const RadarCaptureProfile radar_profile_data = {\n"
    fields = [
        f".schema=1U, .approvals=0x{approvals:08x}U,",
        f".qualification={qualification}U, .source_review_flags=0x{source_flags:08x}U,",
        f".name={q(name)}, .profile_id={q(pid)},",
        f".firmware_sha256={q(identity['firmware_sha256'])},",
        f".cfg_sha256={q(identity['cfg_sha256'])},",
        f".review_sha256={q(identity['review_sha256'])},",
        f".version_substring={q(version)},",
        f".lanes={lanes}U, .lane_mbps={rate}U, .word_bits={bits}U, .format_flags={fmt}U,",
        f".packet_bytes={size}U, .packet_count={count}U, .skip_pairs={skip}U,",
        f".startup_mode={mode}U, .receive_timeout_us={receive}U, .snapshot_timeout_us={snapshot}U,",
        f".use_delay={int(delay)}U, .taps={{{taps[0]}U,{taps[1]}U,{taps[2]}U}},",
        f".command_count={len(commands)}U, .commands=radar_profile_commands,",
        '.start_command="sensorStart", .stop_command="sensorStop"',
    ]
    header += "".join("    " + line + "\n" for line in fields) + "};\n#endif\n"
    manifest = dict(identity, profile_id=pid, name=name, configuration_lines=len(commands),
                    packet_bytes=size, packet_count=count, total_bytes=size*count,
                    startup_mode=r["startup_mode"], approvals=approvals, source_evidence=source_evidence,
                    evidence_note=("STREAM_FRAME_TEST_NEW_FLOW_HARDWARE_ACCEPTANCE_PENDING" if qualification == 3 else
                                   "FINITE_FRAME_TEST_INPUT_TIMING_AND_ADC_ALIGNMENT_PENDING" if qualification == 2 else
                                   "FIRST_PACKET_TEST_PENDING_DYNAMIC_AND_INPUT_TIMING" if qualification == 1 else
                                   "USER_REVIEW_NOT_AUTOMATIC_MEASUREMENT"))
    if geometry is not None:
        manifest["finite_geometry"] = geometry
    return header, manifest

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--firmware", required=True, type=Path)
    ap.add_argument("--cfg", required=True, type=Path)
    ap.add_argument("--review", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path, help="normally radar_runtime/src/config/radar_capture_profile_data.h")
    ap.add_argument("--force", action="store_true", help="replace existing generated/default profile after review")
    args = ap.parse_args()
    try:
        text, manifest = generate(args.firmware.read_bytes(), args.cfg.read_bytes(), args.review.read_bytes(), args.review.parent)
        paths = [args.output, args.output.with_suffix(".manifest.json")]
        contents = [text, json.dumps(manifest, indent=2, ensure_ascii=False)+"\n"]
        if "finite_geometry" in manifest:
            paths.append(args.output.with_name("radar_capture_geometry.h"))
            contents.append(geometry_header(manifest))
        if not args.force and any(path.exists() for path in paths):
            raise ProfileError("output exists; use --force only after backup/review")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        for path, content in zip(paths, contents):
            temp = path.with_name(path.name+".tmp")
            temp.write_text(content, encoding="utf-8", newline="\n")
            temp.replace(path)
        print(json.dumps(manifest, indent=2, ensure_ascii=False))
        print("Generated; rebuild the application. On board: capture confirm " + manifest["profile_id"])
        return 0
    except (OSError, ProfileError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        return 2

if __name__ == "__main__":
    raise SystemExit(main())
