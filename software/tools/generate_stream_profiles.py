#!/usr/bin/env python3
"""Generate distinct stream16/64/continuous identities from reviewed batch16.

Approval bits are inherited without upgrading their evidence. Source waveform,
firmware and wiring are unchanged; new stream execution remains unverified.
"""
import argparse
import json
from pathlib import Path
from generate_capture_profile import generate, sha

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--firmware', required=True, type=Path)
    ap.add_argument('--profiles', type=Path, default=Path(__file__).resolve().parents[1] / 'radar_runtime/src/config/profiles')
    args = ap.parse_args()
    base = args.profiles
    review = json.loads((base / 'radar_3tx_hsi_batch16.review.json').read_text())
    cfg = (base / 'radar_3tx_hsi_batch16.cfg').read_text()
    firmware = args.firmware.read_bytes()
    for count in (16, 64, 0):
        r = json.loads(json.dumps(review))
        text = cfg.replace('frameCfg 0 2 32 16 100 1 0', f'frameCfg 0 2 32 {count} 100 1 0')
        raw = text.encode('utf-8')
        r.update(name=f'TI35_F20_3TX_HSI_STREAM{count}', qualification='STREAM_FRAME_TEST',
                 packet_count=count, cfg_sha256=sha(raw))
        for key, value in r['approvals'].items():
            if not value['approved']:
                value['evidence'] = 'New stream receive flow has not completed hardware acceptance. Unchanged waveform and existing finite evidence do not prove streaming stop, refill or timing.'
        r['approvals']['finite_transport_geometry_and_metadata_reviewed']['evidence'] = (
            'Unchanged3TX/4RX, TX0/2/1,32loops,96HSI records and399360 bytes/frame. '
            '128 BDs support an18-frame rolling window;16 RAW slots use explicit leases. '
            f'Source count{count} uses separate STREAM qualification; runtime flow acceptance pending.')
        for value in r['source_reviews'].values():
            value['summary'] = ('Source compatibility inherited from the exact SDK3.5 and finite3TX review; '
                                f'only numFrames changes to{count}. Runtime stream acceptance remains pending.')
        review_raw = (json.dumps(r, indent=2) + '\n').encode()
        header, manifest = generate(firmware, raw, review_raw, base)
        header = header.replace('RADAR_CAPTURE_PROFILE_DATA_H', f'RADAR_CAPTURE_STREAM{count}_H')
        header = header.replace('radar_profile_commands', f'radar_stream{count}_commands')
        header = header.replace('radar_profile_data', f'radar_stream{count}_data')
        (base / f'radar_3tx_hsi_stream{count}.cfg').write_bytes(raw)
        (base / f'radar_3tx_hsi_stream{count}.review.json').write_bytes(review_raw)
        (base / f'radar_capture_stream{count}.h').write_text(header, newline='\n')
        (base / f'radar_capture_stream{count}.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', newline='\n')
        print(count, manifest['profile_id'], manifest['cfg_sha256'])

if __name__ == '__main__':
    main()
