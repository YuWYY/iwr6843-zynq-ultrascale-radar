#!/usr/bin/env python3
"""Audit a complete stopped STREAM run without trimming frames or using cfg time.

Input is the saved serial log: one STREAM START, STOPPED, final idle STATUS,
and all paginated @STREAMBEGIN/@STREAM/@STREAMEND records. No hardware access.
Exit0 means PASS_INSTRUMENTED_GATES; it does not prove total CPU coverage,
classification accuracy, RF-to-screen latency or physical target detection.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
from typing import Any

SCHEMA = 'M2_STREAM_TIMING_AUDIT_V1'
RECORD_REQUIRED = set('record frame epoch seq crc flags stages raw lease task tasklease first last mapped uncertainty observed ready front submitted done published ack cpu render queue high revision page error bytes hz tstatus ackseq ackrevision ackframe renderbegin renderend queued coalesced'.split())
STATUS_REQUIRED = set('running epoch verified processed presented ready owners high error records overwritten skipped_display pending_display crc_errors hsi_errors dma_errors metadata_errors dp_errors dp_active health_cpu_us health_max_us'.split())
STOP_REQUIRED = set('verified processed presented remaining residual error records'.split())
HEX_FIELDS = {'crc', 'flags', 'stages', 'tstatus', 'dp_errors'}
ZERO_ERRORS = ('crc_errors', 'hsi_errors', 'dma_errors', 'metadata_errors', 'dp_errors')
TOKENS = re.compile(r'(?<!\w)([A-Za-z_][A-Za-z_0-9]*)=([^\s;]+)')
REQUIRED_STAGES = 1 | 2 | 4 | 8 | 16 | 32 | 256

def percentile(values: list[int], fraction: float = .95) -> int | None:
    """Nearest-rank quantile; no interpolation or outlier removal."""
    if not values:
        return None
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)]

def distribution(values: list[int]) -> dict[str, Any]:
    return {'count': len(values), 'minimum': min(values) if values else None,
            'mean': sum(values) / len(values) if values else None,
            'p95': percentile(values), 'maximum': max(values) if values else None}

def numbers(text: str) -> tuple[dict[str, int], list[str]]:
    result: dict[str, int] = {}
    bad: list[str] = []
    for key, value in TOKENS.findall(text):
        try:
            parsed = int(value, 16 if key in HEX_FIELDS else 0 if value.lower().startswith(('0x', '-0x')) else 10)
        except ValueError:
            # START backend is intentionally nonnumeric; required numeric
            # fields still fail their explicit presence check below.
            continue
        if key in result:
            bad.append('duplicate_field:' + key)
        result[key] = parsed
    return result, bad

def audit_text(text: str) -> dict[str, Any]:
    issues: list[dict[str, Any]] = []
    records: list[dict[str, int]] = []
    starts: list[tuple[int, dict[str, int]]] = []
    stops: list[tuple[int, dict[str, int]]] = []
    statuses: list[tuple[int, dict[str, int]]] = []
    batches: list[dict[str, Any]] = []
    active: dict[str, Any] | None = None

    def issue(code: str, line: int | None = None, detail: Any = None, *, error: bool = False) -> None:
        entry: dict[str, Any] = {'code': code, 'kind': 'FAIL' if error else 'INCOMPLETE'}
        if line is not None:
            entry['line'] = line
        if detail is not None:
            entry['detail'] = detail
        issues.append(entry)

    def required(data: dict[str, int], fields: set[str], kind: str, line: int) -> bool:
        missing = sorted(fields - data.keys())
        if missing:
            issue(kind + '_missing_fields', line, missing)
        return not missing

    for line_no, raw in enumerate(text.splitlines(), 1):
        line = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', raw)
        data, bad = numbers(line)
        relevant = '@STREAM' in line or '[STREAM ' in line
        if relevant and bad:
            issue('malformed_fields', line_no, bad)
        if '[STREAM START]' in line:
            starts.append((line_no, data))
            if 'backend=PL_BASELINE' not in line:
                issue('wrong_backend', line_no, error=True)
        elif '[STREAM STOPPED]' in line:
            required(data, STOP_REQUIRED, 'stopped', line_no)
            stops.append((line_no, data))
        elif '[STREAM STATUS]' in line:
            statuses.append((line_no, data))
        elif '@STREAMBEGIN ' in line:
            if active is not None:
                issue('unterminated_export', line_no)
            required(data, {'first', 'count', 'records', 'overwritten', 'epoch'}, 'export_begin', line_no)
            active = {'begin': data, 'line': line_no, 'rows': []}
        elif '@STREAMEND ' in line:
            if active is None:
                issue('orphan_export_end', line_no)
            else:
                required(data, {'next', 'records', 'epoch'}, 'export_end', line_no)
                active['end'] = data
                active['end_line'] = line_no
                batches.append(active)
                active = None
        elif '@STREAM ' in line:
            required(data, RECORD_REQUIRED, 'record', line_no)
            data['_line'] = line_no
            records.append(data)
            if active is None:
                issue('record_outside_export', line_no)
            else:
                active['rows'].append(data)
        if '[STREAM ERROR]' in line or '[STREAM FAIL]' in line or '[DISPLAY] DPDMA error' in line:
            issue('runtime_error_line', line_no, raw, error=True)
        if '[STREAM LIVE]' in line and data.get('errors', 0):
            issue('live_error_counter', line_no, data['errors'], error=True)

    if active is not None:
        issue('unterminated_export', active['line'])
    if len(starts) != 1:
        issue('one_complete_run_required', detail={'starts': len(starts)})
    if len(stops) != 1:
        issue('one_stopped_marker_required', detail={'stops': len(stops)})
    final = statuses[-1][1] if statuses else {}
    if not statuses:
        issue('final_status_missing')
    else:
        required(final, STATUS_REQUIRED, 'status', statuses[-1][0])
        if final.get('running', 1):
            issue('run_still_active', error=True)
        if stops and statuses[-1][0] < stops[-1][0]:
            issue('final_status_precedes_stop')
    stop = stops[-1][1] if stops else {}
    if stops and starts and stops[0][0] < starts[0][0]:
        issue('stop_precedes_start')
    if starts and records and any(r['_line'] < starts[0][0] for r in records):
        issue('records_outside_selected_run')

    for batch in batches:
        b, e, rows = batch['begin'], batch['end'], batch['rows']
        if stops and batch['line'] < stops[-1][0]:
            issue('export_before_stopped', batch['line'])
        if b.get('count') != len(rows) or e.get('next') != b.get('first', -1) + b.get('count', -1):
            issue('export_length_mismatch', batch['line'])
        if b.get('records') != e.get('records') or b.get('epoch') != e.get('epoch'):
            issue('export_identity_changed', batch['line'])
        if b.get('records') != final.get('records') or b.get('epoch') != final.get('epoch'):
            issue('export_status_mismatch', batch['line'])
        if b.get('overwritten', 1):
            issue('export_overwritten', batch['line'], error=True)
        if [r.get('record') for r in rows] != list(range(b.get('first', 0) + 1, b.get('first', 0) + 1 + len(rows))):
            issue('export_record_order_or_identity', batch['line'])
    if not batches:
        issue('export_envelopes_missing')

    unique: dict[int, dict[str, int]] = {}
    duplicate_exports = 0
    for r in records:
        if 'record' not in r:
            continue
        if r['record'] in unique:
            old = {k: v for k, v in unique[r['record']].items() if k != '_line'}
            new = {k: v for k, v in r.items() if k != '_line'}
            if old != new:
                issue('conflicting_duplicate_record', r['_line'], r['record'], error=True)
            else:
                duplicate_exports += 1
        else:
            unique[r['record']] = r
    rows = [unique[i] for i in sorted(unique)]
    n = final.get('records', -1)
    if not 1 <= n <= 8192 or sorted(unique) != list(range(1, n + 1)):
        issue('record_coverage_missing', detail={'expected': n, 'unique_exported': len(rows)})
    valid = [r for r in rows if RECORD_REQUIRED <= r.keys()]
    if final.get('overwritten', 1):
        issue('records_overwritten', detail=final.get('overwritten'), error=True)
    for name in ('verified', 'processed'):
        if final.get(name) != n:
            issue('count_coverage_' + name, detail={'count': final.get(name), 'records': n})
        if stop.get(name) != final.get(name):
            issue('count_changed_after_stop_' + name)
    if stop.get('records') != n:
        issue('stopped_records_mismatch')
    for name in ('error', 'remaining', 'residual'):
        if stop.get(name, 1):
            issue('stop_' + name, detail=stop.get(name), error=True)
    for name in ('error', 'ready', 'owners', 'pending_display', *ZERO_ERRORS):
        if name in final and final[name]:
            issue('final_' + name, detail=final[name], error=True)
    if final.get('dp_active') != 1:
        issue('display_not_active', error=True)
    if 'health_cpu_us' in final and 'health_max_us' in final and not 0 <= final['health_max_us'] <= final['health_cpu_us']:
        issue('invalid_health_cpu_aggregate', error=True)

    previous: dict[str, int] | None = None
    leases_raw: set[tuple[int, int]] = set()
    leases_task: set[tuple[int, int]] = set()
    ack_sequences: set[int] = set()
    ack_rows: list[dict[str, int]] = []
    for r in valid:
        line = r['_line']
        if r['epoch'] != final.get('epoch') or not r['epoch'] or r['seq'] != r['record']:
            issue('frame_epoch_or_sequence', line, error=True)
        if r['flags'] & 3 != 3 or r['flags'] & 0x80000010:
            issue('invalid_frame_integrity_flags', line, r['flags'], error=True)
        if r['stages'] & REQUIRED_STAGES != REQUIRED_STAGES or r['stages'] & 128 or r['error']:
            issue('incomplete_or_failed_processing', line, error=True)
        if r['bytes'] != 399360 or r['hz'] <= 0 or r['tstatus'] & 0xA0000000 or r['last'] < r['first']:
            issue('transport_metadata_invalid', line, error=True)
        if not (0 <= r['raw'] < 16 and 0 <= r['task'] < 2 and r['lease'] > 0 and r['tasklease'] > 0):
            issue('invalid_slot_lease', line, error=True)
        for seen, key in ((leases_raw, (r['raw'], r['lease'])), (leases_task, (r['task'], r['tasklease']))):
            if key in seen:
                issue('reused_slot_lease', line, error=True)
            seen.add(key)
        stamps = [r[k] for k in ('observed', 'ready', 'front', 'submitted', 'done', 'published')]
        if stamps[0] <= 0 or stamps != sorted(stamps):
            issue('processing_timestamp_order', line, error=True)
        if r['queue'] < 0 or r['high'] < r['queue'] or r['high'] >= 8:
            issue('queue_at_overload_or_invalid', line, error=True)
        if r['cpu'] < 0 or r['render'] < 0:
            issue('negative_cpu_or_render', line, error=True)
        if r['flags'] & 36 != 36 or not 0 <= r['uncertainty'] <= 1000 or r['mapped'] <= 0:
            issue('untrusted_time_mapping', line, error=True)
        elif r['mapped'] > r['observed'] + r['uncertainty']:
            issue('mapped_first_after_dma_observation', line, error=True)
        if previous is not None:
            if r['frame'] != previous['frame'] + 1 or r['first'] <= previous['last'] or r['hz'] != previous['hz']:
                issue('frame_or_transport_discontinuity', line, error=True)
            for key in ('observed', 'published', 'mapped', 'revision'):
                if r[key] <= previous[key]:
                    issue('nonmonotonic_' + key, line, error=True)
            if r['hz'] > 0:
                delta = (r['first'] - previous['first']) * 1e6 / r['hz']
                if abs((r['mapped'] - previous['mapped']) - delta) > r['uncertainty'] + previous['uncertainty'] + 2:
                    issue('mapped_clock_discontinuity', line, error=True)
        previous = r
        if r['ack']:
            if not r['stages'] & 64 or not r['ackseq'] or r['ackframe'] != r['frame'] or r['ackrevision'] != r['revision']:
                issue('ack_identity_mismatch', line, error=True)
            if r['ackseq'] in ack_sequences:
                issue('duplicate_ack_sequence', line, error=True)
            ack_sequences.add(r['ackseq'])
            order = [r[k] for k in ('published', 'renderbegin', 'renderend', 'queued', 'ack')]
            if order != sorted(order) or r['render'] != r['renderend'] - r['renderbegin']:
                issue('ack_or_render_timestamp_order', line, error=True)
            if r['cpu'] < r['queued'] - r['renderbegin']:
                issue('cpu_omits_render_queue', line, error=True)
            if not 0 <= r['page'] < 4:
                issue('invalid_ack_page', line, error=True)
            if ack_rows and (r['ack'] <= ack_rows[-1]['ack'] or r['ackseq'] <= ack_rows[-1]['ackseq']):
                issue('nonmonotonic_ack', line, error=True)
            ack_rows.append(r)
        elif r['stages'] & 64 or r['ackseq'] or r['ackframe'] or r['ackrevision']:
            issue('ack_missing_timestamp', line, error=True)

    presented = len(ack_rows)
    if final.get('presented') != presented:
        issue('presented_count_mismatch', detail={'status': final.get('presented'), 'records': presented})
    if stop.get('presented', 0) > final.get('presented', -1):
        issue('presentation_counter_regressed')
    latest_ack_frame = ack_rows[-1]['frame'] if ack_rows else -1
    skipped = sum(not r['ack'] and r['frame'] < latest_ack_frame for r in valid)
    pending = sum(not r['ack'] and r['frame'] >= latest_ack_frame for r in valid)
    if final.get('skipped_display') != skipped or final.get('pending_display') != pending or presented + skipped + pending != len(valid):
        issue('missing_ack_accounting', detail={'observed_skipped': skipped, 'observed_pending': pending,
                                              'status_skipped': final.get('skipped_display'), 'status_pending': final.get('pending_display')})
    observed = [r['observed'] for r in valid]
    published = [r['published'] for r in valid]
    acks = [r['ack'] for r in ack_rows]
    observed_span = observed[-1] - observed[0] if len(observed) > 1 else 0
    window_us = max([*published, *acks], default=0) - min(observed, default=0)
    if window_us <= 0:
        window_us = 0
    interval = [b-a for a, b in zip(acks, acks[1:])]
    age = [r['ack']-r['mapped'] for r in ack_rows if r['mapped'] > 0 and r['flags'] & 36 == 36 and 0 <= r['uncertainty'] <= 1000]
    age_upper = [r['ack']-r['mapped']+r['uncertainty'] for r in ack_rows if r['mapped'] > 0 and r['flags'] & 36 == 36 and 0 <= r['uncertainty'] <= 1000]
    cpu = [r['cpu'] for r in valid]
    def rate(count: int) -> float | None:
        return count*1e6/window_us if window_us else None
    rates = {'acquisition_hz': rate(len(valid)), 'processing_hz': rate(len(published)), 'unique_display_hz': rate(presented)}
    q = [r['queue'] for r in valid]
    quarter = max(1, len(q)//4)
    queue_first = sum(q[:quarter])/len(q[:quarter]) if q else None
    queue_last = sum(q[-quarter:])/len(q[-quarter:]) if q else None
    gates: dict[str, bool] = {
        'full_untrimmed_observation_600s': observed_span >= 600_000_000,
        'acquisition_ge_9_5hz': rates['acquisition_hz'] is not None and rates['acquisition_hz'] >= 9.5,
        'processing_ge_9_5hz': rates['processing_hz'] is not None and rates['processing_hz'] >= 9.5,
        'new_display_ge_9hz': rates['unique_display_hz'] is not None and rates['unique_display_hz'] >= 9,
        'display_interval_p95_le_150ms': bool(interval) and percentile(interval) <= 150_000,
        'display_interval_max_le_250ms': bool(interval) and max(interval) <= 250_000,
        'mapped_first_to_ack_p95_upper_le_300ms': len(age_upper) == presented and bool(age_upper) and min(age) >= 0 and percentile(age_upper) <= 300_000,
        'recorded_kernel_cpu_max_le_80ms': bool(cpu) and max(cpu) <= 80_000,
        'queue_no_sustained_growth': queue_first is not None and queue_last <= queue_first+1 and max(q, default=8) < 8,
    }
    # Never let a short run, incomplete export, missing metadata or missing
    # final ownership accounting become PASS despite good surviving samples.
    any_failure = any(i['kind'] == 'FAIL' for i in issues)
    if issues:
        status = 'FAIL' if any_failure else 'INCOMPLETE'
    elif not all(gates.values()):
        status = 'FAIL_GATES'
    else:
        status = 'PASS_INSTRUMENTED_GATES'
    return {
        'schema': SCHEMA, 'status': status, 'passed': status == 'PASS_INSTRUMENTED_GATES',
        'scope': 'Untrimmed captured STREAM run; instrumented timing and count integrity only',
        'definitions': {'time_unit': 'us', 'percentile': 'nearest_rank', 'trimmed_records': 0,
            'rate_window': 'first DMA-observed frame to last publication or unique first DP ACK, whichever is later',
            'latency': 'mapped SYS first accepted handshake to first actual DP ACK; conservative upper bound adds mapping uncertainty',
            'not_adc_to_screen': True, 'configuration_period_used_as_clock': False,
            'queue_rule': 'last-quarter mean <= first-quarter mean+1 frame; maximum ready depth below overload8; final ownership empty'},
        'coverage': {'expected_records': n, 'exported_rows': len(records), 'unique_records': len(rows),
            'complete_numeric_records': len(valid), 'identical_duplicate_exports': duplicate_exports,
            'export_pages': len(batches), 'first_frame': valid[0]['frame'] if valid else None,
            'last_frame': valid[-1]['frame'] if valid else None, 'epoch': final.get('epoch'),
            'presented': presented, 'skipped_display': skipped, 'pending_display': pending,
            'raw_coalesced_at_last_ack': ack_rows[-1]['coalesced'] if ack_rows else None,
            'raw_coalesced_counter_is_not_skipped_frame_count': True},
        'metrics': {'observation_span_us': observed_span, 'whole_window_us': window_us, **rates,
            'display_interval_us': distribution(interval), 'mapped_first_to_ack_us': distribution(age),
            'mapped_first_to_ack_upper_us': distribution(age_upper), 'recorded_kernel_cpu_us': distribution(cpu),
            'queue_first_quarter_mean': queue_first, 'queue_last_quarter_mean': queue_last,
            'queue_maximum': max(q) if q else None},
        'cpu_coverage': {'full_cpu_coverage': False,
            'included_in_record': ['RX copy and first CRC/HSI', 'front packing and second CRC', 'PL driver submit/poll',
                'range import, spatial and scene postprocessing', 'display render through queue'],
            'health_cpu_us_aggregate': final.get('health_cpu_us'), 'health_max_us': final.get('health_max_us'),
            'not_per_frame': ['periodic health guard'],
            'not_proven_instrumented': ['main-loop orchestration', 'UART/background work', 'interrupt CPU', 'all task admission/clock-map overhead'],
            'full_cpu_80ms_gate': 'NOT_PROVEN'},
        'gates': gates, 'issues': issues, 'final_status': final, 'stopped_summary': stop,
        'limits': ['No classification accuracy or physical detection claim', 'No KEY3 validation',
                   'No absolute RF/ADC-to-screen latency claim', 'No total CPU-budget proof']}

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log', type=Path, nargs='+', help='Complete run log followed by its export chunks, in saved order')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    chunks = [p.read_bytes() for p in args.log]
    report = audit_text('\n'.join(c.decode('utf-8', errors='replace') for c in chunks))
    report['input_files'] = [{'path': str(p.resolve()), 'bytes': len(c), 'sha256': hashlib.sha256(c).hexdigest()}
                             for p, c in zip(args.log, chunks)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False)+'\n', encoding='utf-8')
    print(json.dumps({'status': report['status'], 'metrics': report['metrics'], 'gates': report['gates'],
                      'issue_count': len(report['issues']), 'report': str(args.output.resolve())}, indent=2))
    return 0 if report['passed'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
