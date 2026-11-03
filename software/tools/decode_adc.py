#!/usr/bin/env python3
"""Strict decoder for this project's SDK 3.5.0.4 / two-lane RAW16 capture.

Supports reviewed TX0/TX2/TX1 96-chirp HSI frames and legacy TX0/TX2
64-chirp frames, RX0..3, 256 complex samples, adcbufCfg sampleSwap=1.
HSI requires one finite frame; legacy ADC-only permits 1 or 16 frames.
No header search, best-looking I/Q selection, or error concealment is performed.

Source contract: TI hsiprotocol.h, hsiheader.c, cbuff_lvds.c and
cbuff_transfer.c from mmwave_sdk_03_05_00_04; see this turn's adc_source_notes.md.
"""
from __future__ import annotations

import argparse
from array import array
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib


class DecodeError(ValueError):
    """Input violates the fixed, reviewed stream contract."""


HSI_IDS = frozenset((
    0x0CDA0ADC0CDA0ADC, 0x09CC0CC909CC0CC9, 0x08CC0CC808CC0CC8,
    0x0F48084F0F48084F, 0x076C0C67076C0C67, 0x0DDA0ADD0DDA0ADD,
    0x0EDA0ADE0EDA0ADE, 0x0FDA0ADF0FDA0ADF,
))
SDK_VERSION = 0x0744  # 3 << 9 | 5 << 6 | 0 << 3 | 4
HEADER_BYTES = 64
PAYLOAD_BYTES = 4096
RECORD_BYTES = HEADER_BYTES + PAYLOAD_BYTES
CHIRPS = 64
RX_COUNT = 4
SAMPLES = 256


@dataclass(frozen=True)
class Geometry:
    mode: str
    frames: int
    cfg_sha256: str
    tx_sequence: tuple[int, ...] = (0, 2)

    @property
    def chirps(self) -> int:
        return len(self.tx_sequence) * 32

    @property
    def input_bytes(self) -> int:
        return self.frames * self.chirps * (RECORD_BYTES if self.mode == "hsi" else PAYLOAD_BYTES)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_cfg(data: bytes, mode: str) -> Geometry:
    """Reject other ADC formats/sequence shapes instead of guessing their order."""
    if mode not in ("hsi", "adc"):
        raise DecodeError("mode must be hsi or adc")
    try:
        text = data.decode("utf-8-sig")
    except UnicodeError as exc:
        raise DecodeError("cfg must be UTF-8 text") from exc
    commands: dict[str, list[list[str]]] = {}
    for line in text.splitlines():
        line = line.split("%", 1)[0].split("#", 1)[0].strip()
        if line:
            name, *args = line.split()
            commands.setdefault(name, []).append(args)

    def one(name: str, length: int) -> list[str]:
        rows = commands.get(name, [])
        if len(rows) != 1 or len(rows[0]) != length:
            raise DecodeError(f"cfg requires exactly one {name} with {length} arguments")
        return rows[0]

    def exact(name: str, values: list[int]) -> None:
        try:
            actual = [int(v, 10) for v in one(name, len(values))]
        except ValueError as exc:
            raise DecodeError(f"invalid integer in {name}") from exc
        if actual != values:
            raise DecodeError(f"unsupported {name}: expected {' '.join(map(str, values))}")

    exact("dfeDataOutputMode", [1])
    channel = one("channelCfg", 3)
    if channel == ["15", "5", "0"]:
        tx_sequence = (0, 2)
    elif channel == ["15", "7", "0"] and mode == "hsi":
        tx_sequence = (0, 2, 1)
    else:
        raise DecodeError("only reviewed 4RX/2TX legacy or 4RX/3TX HSI channelCfg is supported")
    exact("adcCfg", [2, 1])
    exact("adcbufCfg", [-1, 0, 1, 1, 1])
    exact("lvdsStreamCfg", [-1, 1 if mode == "hsi" else 0, 1, 0])
    profile = one("profileCfg", 14)
    frame = one("frameCfg", 7)
    try:
        if [float(v) for v in profile] != [0, 60, 7, 3, 24, 0, 0, 166, 1, 256, 12500, 0, 0, 158]:
            raise DecodeError("only the reviewed profile-0 waveform is supported")
        if [int(v) for v in frame[:3]] != [0, len(tx_sequence)-1, 32]:
            raise DecodeError("frameCfg must select the reviewed TX sequence with 32 loops")
        frames = int(frame[3])
        if frames not in (1, 16) or (mode == "hsi" and frames != 1):
            raise DecodeError("HSI requires numFrames=1; ADC requires finite numFrames=1 or 16")
        if float(frame[4]) != 100 or int(frame[5]) != 1 or float(frame[6]) != 0:
            raise DecodeError("frame period must be 100 ms, software trigger=1, trigger delay=0")
        chirps = [[int(v) for v in row] for row in commands.get("chirpCfg", [])]
        if sorted(chirps) != [[i, i, 0, 0, 0, 0, 0, 1 << tx] for i, tx in enumerate(tx_sequence)]:
            raise DecodeError("chirpCfg must use reviewed profile-0 TX0/TX2 or TX0/TX2/TX1 order")
        if "bpmCfg" in commands and any(len(row) != 4 or int(row[1]) != 0
                                        for row in commands["bpmCfg"]):
            raise DecodeError("BPM must be disabled")
    except (ValueError, IndexError) as exc:
        if isinstance(exc, DecodeError):
            raise
        raise DecodeError("invalid numeric cfg field") from exc
    if "subFrameCfg" in commands or "advFrameCfg" in commands:
        raise DecodeError("advanced/subframe configuration is not supported")
    if commands.get("sensorStart") != [[]]:
        raise DecodeError("cfg requires exactly one plain sensorStart")
    return Geometry(mode, frames, sha256(data), tx_sequence)


def raw16_to_memory(data: bytes) -> bytes:
    """Undo complex two-lane Format0 for ALL CBUFF data, including HSI headers.

    CBUFF complex Format0 0x76325410 gives lane0 words [0,1,4,5], lane1
    [2,3,6,7]. FPGA RAW16 stores lane0 then lane1 per beat, so groups are
    memory [0,2,1,3]. This permutation is its own inverse. No I/Q swap here.
    """
    if len(data) % 8:
        raise DecodeError("RAW16 byte count must be a multiple of 8")
    words = array("h")
    words.frombytes(data)
    # Swapping complete 16-bit objects is endian independent.
    words[1::4], words[2::4] = words[2::4], words[1::4]
    return words.tobytes()


def memory_qi_to_iq(data: bytes) -> bytes:
    """adcbufCfg sampleSwap=1 memory [Q,I] -> little-endian int16 [I,Q]."""
    if len(data) % 4:
        raise DecodeError("complex ADC data must contain complete I/Q pairs")
    words = array("h")
    words.frombytes(data)
    words[0::2], words[1::2] = words[1::2], words[0::2]
    return words.tobytes()


def parse_header(data: bytes, chirp: int) -> dict:
    """Parse an already lane-unmapped 64-byte header; fail at the first error."""
    prefix = f"chirp {chirp} offset {chirp * RECORD_BYTES}: "

    def require(condition: bool, reason: str) -> None:
        if not condition:
            raise DecodeError(prefix + reason)

    require(len(data) == HEADER_BYTES, "short HSI header")
    ident, lsw, msw, reserved = struct.unpack_from("<QHHI", data)
    version, header_words = struct.unpack_from("<HH", data, 16)
    platform, interleave, size, dtype, rx_mask, fmt = struct.unpack_from("<6B", data, 20)
    chirp_mode, adc_words, cp_words, *extra = struct.unpack_from("<9H", data, 26)
    cq_words, user_words = extra[:3], extra[3:]
    require(ident in HSI_IDS, "unknown HSI session ID (no resynchronization attempted)")
    require((lsw & 0xF000) == 0 and (msw & 0xF000) == 0, "length field uses reserved bits")
    total_length = lsw | (msw << 12)
    require(total_length == RECORD_BYTES - 16, "totalLength must be 4144, excluding datacard16")
    require(reserved == 0, "datacard reserved field must be zero")
    require(version == SDK_VERSION, "SDK version must be 3.5.0.4 (0x0744)")
    require(header_words == HEADER_BYTES // 2, "headerSize must be 32 CBUFF units (64 bytes)")
    require(platform == 4, "platform must be XWR68XX=4")
    require(interleave == 2, "HSI data must be noninterleaved=2")
    require(size == 0, "HSI ADC width must be 16 bits (dataSize=0)")
    require(dtype == 2, "HSI dataType must be complex=2")
    require(rx_mask == 15, "HSI RX mask must enable RX0..3")
    require(fmt == 1, "HSI dataFmt must be ADC-only=1")
    require(chirp_mode == 1, "HSI chirpMode must be 1")
    require(adc_words == 512, "adcDataSize must be 512 CBUFF units per RX")
    require(cp_words == 0 and cq_words == [0, 0, 0] and user_words == [0, 0, 0],
            "unexpected CP/CQ/user payload")
    require(data[44:52] == b"\0" * 8, "unexpected appExtensionHeader for stock SDK demo")
    require(data[52:64] == b"\x0f" * 12, "HSI padding differs from SDK 0x0F padding")
    return {"chirp_index": chirp, "offset": chirp * RECORD_BYTES,
            "session_id": f"0x{ident:016x}", "total_length_excluding_datacard": total_length,
            "header_bytes": HEADER_BYTES, "payload_bytes": PAYLOAD_BYTES,
            "sdk_version": "3.5.0.4", "rx_mask": rx_mask, "chirp_mode": chirp_mode}


def decode(data: bytes, geometry: Geometry) -> tuple[bytes, dict]:
    if len(data) != geometry.input_bytes:
        raise DecodeError(f"length mismatch: cfg requires {geometry.input_bytes} bytes; got {len(data)}")
    memory = raw16_to_memory(data)
    records = []
    if geometry.mode == "hsi":
        adc = bytearray()
        for chirp in range(geometry.chirps):
            offset = chirp * RECORD_BYTES
            header = parse_header(memory[offset:offset + HEADER_BYTES], chirp)
            if records and header["session_id"] != records[0]["session_id"]:
                raise DecodeError(f"chirp {chirp}: HSI session ID changed within this finite frame")
            records.append(header)
            adc.extend(memory[offset + HEADER_BYTES:offset + RECORD_BYTES])
        iq = memory_qi_to_iq(adc)
    else:
        iq = memory_qi_to_iq(memory)
    report = {
        "schema": 1, "decoder": "M2_ADC_HSI_FIXED_SDK35_V2", "mode": geometry.mode,
        "cfg_sha256": geometry.cfg_sha256, "input_bytes": len(data), "input_sha256": sha256(data),
        "input_crc32": f"{zlib.crc32(data):08x}", "iq_bytes": len(iq), "iq_sha256": sha256(iq),
        "iq_dtype": "little-endian signed int16", "iq_components": ["I", "Q"],
        "shape": [geometry.frames, geometry.chirps, RX_COUNT, SAMPLES, 2],
        "axis_order": ["configured_frame", "chirp", "rx", "complex_sample", "iq_component"],
        "rx_order": [0, 1, 2, 3],
        "tx_sequence": list(geometry.tx_sequence), "loops": 32,
        "tx_for_chirp": [geometry.tx_sequence[c % len(geometry.tx_sequence)] for c in range(geometry.chirps)],
        "tdm_view_shape": [geometry.frames, 32, len(geometry.tx_sequence), RX_COUNT, SAMPLES, 2],
        "tdm_view_axis_order": ["configured_frame", "loop", "tx_slot", "rx", "complex_sample", "iq_component"],
        "tx_mapping_basis": "FIXED_CFG_AND_CHIRP_INDEX; HSI_HAS_NO_TX_ID",
        "mapping": "RAW16 [Q0,Q1,I0,I1] -> complex [I0+jQ0,I1+jQ1]",
        "hsi_structure": f"PASS_{geometry.chirps}_SEQUENTIAL_RECORDS_FROM_OFFSET_ZERO" if records else "NOT_PRESENT",
        "hsi_records": records, "hsi_id_is_frame_counter": False,
        "transport_valid": None, "transport_scope": "NOT_PROVIDED",
        "adc_alignment": ("HSI_CHIRP_BOUNDARIES_VALIDATED_FRAME_ORIGIN_REQUIRES_FINITE_CAPTURE_EVIDENCE"
                          if records else "NOT_PROVEN_BY_ADC_ONLY_LENGTH"),
        "frame_count_basis": "DECLARED_FINITE_CFG_AND_EXACT_LENGTH; NO_HSI_FRAME_COUNTER",
        "adc_signal_valid": "NOT_ESTABLISHED_BY_PARSER",
        "input_timing": "NOT_VERIFIED_BY_PARSER",
        "limits": ["No analog calibration, signal plausibility or lane error-rate claim.",
                   "No new physical capture is implied by parsing a file.",
                   "Requires unchanged stock SDK two-lane Format0 and FPGA RAW16 lane/bit mapping.",
                   "Finite frame origin also requires ARM before sensorStart, zero skip, and successful capture.",
                   "ADC-only mode requires separate prior HSI and capture evidence for frame alignment."],
    }
    return iq, report


def bind_transport(report: dict, exported: dict) -> None:
    """Bind an existing decode_capture report; CRC evidence never proves ADC truth."""
    if not isinstance(exported, dict) or exported.get("validation") != "SERIAL_EXPORT_LENGTH_CRC_ONLY":
        raise DecodeError("transport report is not a verified decode_capture export report")
    metadata = exported.get("metadata")
    if not isinstance(metadata, dict):
        raise DecodeError("transport report has no capture metadata")
    if exported.get("bytes") != report["input_bytes"] or exported.get("crc32") != report["input_crc32"]:
        raise DecodeError("transport byte count/CRC32 does not match input")
    if metadata.get("cfg_sha256_expected") != report["cfg_sha256"]:
        raise DecodeError("transport expected cfg SHA-256 does not match supplied cfg")
    geom = str(metadata.get("geometry", "")).split()
    try:
        if len(geom) != 5 or int(geom[0]) * int(geom[1]) != report["input_bytes"]:
            raise DecodeError("transport packet geometry does not match input")
        # Runtime R4 metadata uses RAW16 format_flags=8, skip=0. Nonzero skip discards source
        # words and cannot satisfy the offset-zero finite-frame contract.
        if int(geom[2]) != 8 or int(geom[3]) != 0:
            raise DecodeError("transport must use the frozen RAW16 format_flags=8, skip=0")
    except ValueError as exc:
        if isinstance(exc, DecodeError):
            raise
        raise DecodeError("invalid transport geometry") from exc
    report["transport_valid"] = True
    report["transport_scope"] = "EXPORT_LENGTH_CRC32_AND_EXPECTED_CFG_HASH_MATCH_ONLY"
    report["transport_session"] = exported.get("session")
    report["transport_qualification"] = exported.get("qualification", "UNSPECIFIED")
    report["firmware_sha256_expected"] = metadata.get("firmware_sha256_expected")


def save(iq: bytes, report: dict, prefix: Path, force: bool = False) -> tuple[Path, Path]:
    binary = prefix.with_name(prefix.name + ".iq_i16le.bin")
    metadata = prefix.with_name(prefix.name + ".adc.json")
    if not force and (binary.exists() or metadata.exists()):
        raise DecodeError("output exists; choose another prefix or use --force")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    tmpb = binary.with_name(binary.name + ".tmp")
    tmpj = metadata.with_name(metadata.name + ".tmp")
    report["iq_file"] = str(binary.resolve())
    tmpb.write_bytes(iq)
    tmpj.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    tmpb.replace(binary)
    tmpj.replace(metadata)  # Report is the completion marker.
    return binary, metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--mode", required=True, choices=("hsi", "adc"))
    parser.add_argument("--cfg", required=True, type=Path)
    parser.add_argument("--output", "--out", required=True, type=Path, help="output filename prefix")
    parser.add_argument("--transport", type=Path, help="optional decode_capture JSON from this binary export")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    try:
        geometry = parse_cfg(args.cfg.read_bytes(), args.mode)
        iq, report = decode(args.input.read_bytes(), geometry)
        report["input_file"] = str(args.input.resolve())
        report["cfg_file"] = str(args.cfg.resolve())
        if args.transport:
            bind_transport(report, json.loads(args.transport.read_text(encoding="utf-8-sig")))
            report["transport_file"] = str(args.transport.resolve())
        binary, metadata = save(iq, report, args.output, args.force)
        print(f"PASS_FIXED_FORMAT mode={geometry.mode} frames={geometry.frames} input_bytes={report['input_bytes']}")
        print(binary)
        print(metadata)
        print("Parser success does not prove ADC signal validity or physical input timing.")
        return 0
    except (OSError, DecodeError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
