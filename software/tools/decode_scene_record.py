#!/usr/bin/env python3
"""Decode stopped/frozen RSCENE01 export logs, never opens a serial port.

All bytes must be covered by completed CRC-checked pages. Identical page retries
are accepted, conflicting bytes/identities are rejected. Three heatmaps are
samples, not a full input sequence for structural-map algorithm replay.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import zlib

HEADER, FRAME, TIMING, HEAT = 4096, 2560, 320, 100352
DIAG_UINT = ('version reject_flags valid_buckets required_buckets map_elapsed_ms '
    'radial_candidates persistent_cells independent_groups point_like_groups '
    'candidate_overflow fit_trials best_support_cells best_support_groups stable_count '
    'tentative_count stale_count ambiguous_count range_only_count rejected_points '
    'capacity_count processing_us budget_overruns reserved0 reserved1').split()
DIAG_FLOAT = ('best_span_m best_residual_m best_occupancy best_temporal_support '
    'best_single_point_ratio range_mad_limit_m association_range_m association_u').split()
POINT_FIELDS = ('range_bin doppler_bin range_m velocity_mps azimuth_deg elevation_deg '
    'horizontal_angle_deg x_m y_m z_m snr_db coherence power_dbfs flags').split()

def require(condition, message):
    if not condition:
        raise ValueError(message)

def crc(data):
    return zlib.crc32(data) & 0xffffffff

def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def u64(data, offset):
    return struct.unpack_from('<Q', data, offset)[0]

def ctext(data, offset, length):
    value = data[offset:offset + length]
    require(b'\0' in value, f'unterminated manifest field at {offset}')
    return value.split(b'\0', 1)[0].decode('ascii')

def json_safe(value):
    """Unknown coordinates remain unknown; binary image keeps exact IEEE bits."""
    if isinstance(value,float) and not math.isfinite(value):
        return None
    if isinstance(value,dict):
        return {k:json_safe(v) for k,v in value.items()}
    if isinstance(value,(list,tuple)):
        return [json_safe(v) for v in value]
    return value

def fields(line):
    return dict(re.findall(r'\b([a-z_]+)=([0-9a-f]+)\b', line))

def collect_pages(paths):
    identity = None
    image = seen = covered = None
    pages, incomplete = [], []
    for path in paths:
        pending = None
        for number, line in enumerate(Path(path).read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            where = f'{path}:{number}'
            if '@SCENEBEGIN ' in line:
                f = fields(line)
                try:
                    current = tuple(int(f[k], 16 if k == 'crc' else 10) for k in
                                    ('schema', 'epoch', 'bytes', 'crc', 'frames', 'timing', 'heatmaps'))
                    start, count = int(f['start']), int(f['count'])
                except (KeyError, ValueError) as exc:
                    raise ValueError(f'{where}: malformed BEGIN') from exc
                require(current[0] == 1 and 0 < current[2] <= 0x600000, f'{where}: unsupported image')
                require(0 <= start < current[2] and 0 < count <= 65536 and start + count <= current[2], f'{where}: page bounds')
                if identity is None:
                    identity = current
                    image, seen, covered = bytearray(current[2]), bytearray(current[2]), bytearray(current[2])
                require(identity == current, f'{where}: mixed capture/image identities')
                if pending:
                    incomplete.append(pending)
                pending = {'start': start, 'count': count, 'next': start, 'source': where}
            elif '@SCENEDATA ' in line:
                require(pending is not None, f'{where}: DATA outside a page')
                match = re.search(r'@SCENEDATA epoch=(\d+) offset=(\d+) bytes=(\d+) crc=([0-9a-fA-F]{8}) ([0-9a-fA-F]+)\s*$', line)
                require(match is not None, f'{where}: malformed/truncated DATA')
                epoch, offset, size = map(int, match.group(1, 2, 3))
                payload = bytes.fromhex(match.group(5))
                require(epoch == identity[1] and offset == pending['next'] and 0 < size <= 128 and len(payload) == size,
                        f'{where}: chunk sequence/identity/size')
                require(offset + size <= pending['start'] + pending['count'], f'{where}: chunk outside requested page')
                require(crc(payload) == int(match.group(4), 16), f'{where}: chunk CRC mismatch')
                for j, value in enumerate(payload, offset):
                    require(not seen[j] or image[j] == value, f'{where}: conflicting retry bytes')
                    image[j], seen[j] = value, 1
                pending['next'] += size
            elif '@SCENEEND ' in line:
                require(pending is not None, f'{where}: END outside a page')
                f = fields(line)
                require(int(f.get('epoch', '-1')) == identity[1] and int(f.get('bytes', '-1')) == identity[2]
                        and int(f.get('crc', '0'), 16) == identity[3], f'{where}: END identity')
                require(int(f.get('next', '-1')) == pending['next'] == pending['start'] + pending['count'], f'{where}: incomplete page END')
                covered[pending['start']:pending['next']] = b'\1' * pending['count']
                pages.append(pending)
                pending = None
        if pending:
            incomplete.append(pending)
    require(identity is not None and covered is not None and all(covered), 'missing bytes or no completed page coverage')
    require(crc(image) == identity[3], 'whole exported image CRC mismatch')
    return bytes(image), {'envelope': dict(zip(('schema', 'epoch', 'bytes', 'crc32', 'frames', 'timing', 'heatmaps'), identity)),
                          'completed_pages': pages, 'incomplete_pages_retained': incomplete}

def decode_timing(row):
    out = {'sequence': u64(row, 0)}
    names = ('frame_id acquisition_id frame_index backend crc32 frame_flags stages job_id config_epoch '
             'published_revision queue_depth queue_high_water presentation_count error transport_sequence transport_bytes transport_hz transport_status').split()
    out.update(zip(names, struct.unpack_from('<13Ii4I', row, 8)))
    names = ('transport_first_tick transport_last_tick observed_ps_us batch_begin_ps_us armed_ps_us halt_ps_us '
             'copy_begin_ps_us copy_complete_ps_us ready_ps_us processing_begin_ps_us front_begin_ps_us front_end_ps_us '
             'submit_begin_ps_us submit_end_ps_us done_ps_us post_done_ps_us published_ps_us pl_cycles').split()
    out.update(zip(names, struct.unpack_from('<18Q', row, 80)))
    names = ('sequence frame_id data_revision view_revision page coalesced render_begin_ps_us render_end_ps_us queue_ps_us ack_ps_us').split()
    out['first_ack'] = dict(zip(names, struct.unpack_from('<6I4Q', row, 224)))
    out.update(zip(('mapped_us', 'cpu_us', 'uncertainty_us', 'raw_slot', 'raw_lease', 'task_slot', 'task_lease'), struct.unpack_from('<2Q5I', row, 280)))
    return out

def decode_image(image, expected_duration=None):
    require(len(image) >= HEADER and image[:8] == b'RSCENE01', 'bad record image magic')
    require(tuple(u32(image, x) for x in (8, 12, 16, 20, 24, 28, 32, 36)) ==
            (1, HEADER, FRAME, TIMING, HEAT, 0x01020304, 205, 121), 'unsupported schema/layout/endianness')
    require(crc(image[:4092]) == u32(image, 4092), 'manifest CRC mismatch')
    require(crc(image[HEADER:]) == u32(image, 120), 'payload CRC mismatch')
    n, nt, nh = (u32(image, x) for x in (52, 56, 60))
    require(n <= nt <= 2048 and nh <= 3 and len(image) == HEADER + n*FRAME + nt*TIMING + nh*HEAT == u32(image, 80), 'record counts/length mismatch')
    manifest = {'schema': 1, 'duration_s': u32(image, 40), 'installation_epoch': u32(image, 44), 'epoch': u32(image, 48),
        'frame_count': n, 'timing_count': nt, 'heatmap_count': nh, 'first_us': u64(image, 64), 'last_us': u64(image, 72),
        'image_bytes': len(image), 'error': struct.unpack_from('<i', image, 84)[0], 'incomplete': bool(u32(image, 88)),
        'presented': u32(image, 92), 'skipped': u32(image, 96), 'dp_flags': u32(image, 100), 'source_state': u32(image, 104),
        'first_frame': u32(image, 108), 'last_frame': u32(image, 112), 'missed_heatmaps': u32(image, 116),
        'payload_crc32': f'{u32(image, 120):08x}', 'image_crc32': f'{crc(image):08x}', 'image_sha256': hashlib.sha256(image).hexdigest()}
    manifest['reflection_id_seed']=u32(image,128)
    require(manifest['reflection_id_seed']>0,'missing initial reflection ID seed')
    for name, offset, size in [('software_version',256,64),('build_id',320,64),('profile_id',384,32),('cfg_sha256',416,80),
            ('array_id',496,48),('calibration_id',544,64),('bit_sha256',608,80),('xsa_sha256',688,80),
            ('firmware_expected_sha256',768,80),('firmware_version_observed',848,256),('scene_parameters',1104,1808),('evidence_boundary',2912,256)]:
        manifest[name] = ctext(image, offset, size)
    require(manifest['duration_s'] in (60, 180), 'unexpected duration')
    if expected_duration is not None:
        require(manifest['duration_s'] == expected_duration, 'wrong requested duration')
    frames = []
    for i in range(n):
        row = image[HEADER+i*FRAME:HEADER+(i+1)*FRAME]
        require(row[:4] == b'RSF1' and u32(row,4) == FRAME and crc(row[:124]+row[128:]) == u32(row,124), f'frame {i}: magic/CRC')
        out = dict(zip(('record_index epoch frame_id transport_sequence raw_crc32 frame_flags').split(), struct.unpack_from('<6I',row,8)))
        out['timestamp_us'],out['observed_us'] = struct.unpack_from('<2Q',row,32)
        names=('time_error_us installation_epoch spatial_valid calibrated candidate_count point_count truncated_count xyz_count '
               'scene_flags reset_reason track_count reflection_count wall_count dsp_flags scene_valid rule_version').split()
        out.update(zip(names,struct.unpack_from('<16I',row,48)))
        out['hsi_session_id'],out['clipped_components'] = struct.unpack_from('<QI',row,112)
        require(out['record_index']==i+1 and out['epoch']==manifest['epoch'] and out['installation_epoch']==manifest['installation_epoch'],f'frame {i}: identity')
        require(out['frame_flags']&7 == 7 and out['timestamp_us']>0 and out['time_error_us']<=1000,f'frame {i}: certification/time')
        require(out['point_count']<=32 and out['track_count']<=8 and out['reflection_count']<=8 and out['wall_count']<=4,f'frame {i}: bounded counts')
        if frames:
            require(out['frame_id']==frames[-1]['frame_id']+1 and out['transport_sequence']==frames[-1]['transport_sequence']+1
                    and 0<out['timestamp_us']-frames[-1]['timestamp_us']<=500000,f'frame {i}: sequence/time discontinuity')
        out['points']=[dict(zip(POINT_FIELDS,struct.unpack_from('<Ii11fI',row,128+j*56))) for j in range(out['point_count'])]
        out['tracks']=[]
        for j in range(out['track_count']):
            ident,flags,x,y,age,speed=struct.unpack_from('<IIffIf',row,1920+j*24)
            out['tracks'].append(dict(id=ident,label=flags&255,confirmed=bool(flags&256),measured=bool(flags&512),predicted=bool(flags&1024),x_m=x,y_m=y,age_ms=age,motion_speed_mps=speed))
        out['reflections']=[]
        for j in range(out['reflection_count']):
            ident,packed,r,u,age,mad=struct.unpack_from('<IIffIf',row,2112+j*24)
            out['reflections'].append(dict(id=ident,state=packed&255,flags=packed>>8,range_m=r,u=u,age_ms=age,range_mad_m=mad))
        out['wall_candidates']=[dict(zip(('x0_m y0_m x1_m y1_m residual_m support_fraction cells age_ms').split(),struct.unpack_from('<6f2I',row,2304+j*32))) for j in range(out['wall_count'])]
        out['diagnostics']=dict(zip(DIAG_UINT+DIAG_FLOAT,struct.unpack_from('<24I8f',row,2432)))
        frames.append(out)
    timing=[decode_timing(image[HEADER+n*FRAME+i*TIMING:HEADER+n*FRAME+(i+1)*TIMING]) for i in range(nt)]
    timing_by_id={x['frame_id']:x for x in timing}
    require(len(timing_by_id)==nt,'duplicate timing identity')
    for f in frames:
        t=timing_by_id.get(f['frame_id'])
        require(t is not None and t['acquisition_id']==f['epoch'] and t['transport_sequence']==f['transport_sequence']
                and t['crc32']==f['raw_crc32'] and t['frame_flags']==f['frame_flags'],f"frame {f['frame_id']}: timing identity mismatch")
    if frames:
        require((frames[0]['frame_id'],frames[-1]['frame_id'],frames[0]['timestamp_us'],frames[-1]['timestamp_us'])==
                (manifest['first_frame'],manifest['last_frame'],manifest['first_us'],manifest['last_us']),'manifest frame/time bounds')
    heats=[]
    for i in range(nh):
        start=HEADER+n*FRAME+nt*TIMING+i*HEAT
        row=image[start:start+HEAT]
        require(row[:4]==b'RSH1' and u32(row,4)==i+1 and crc(row[:252])==u32(row,252),'heat header CRC/identity')
        require(crc(row[256:99476])==u32(row,44) and u32(row,24)==205 and u32(row,28)==121,'heat payload CRC/geometry')
        frame_index=u32(row,8)-1;require(0<=frame_index<n,'heat frame index')
        f=frames[frame_index]
        require((u32(row,12),u64(row,16),u32(row,36),u32(row,40))==(f['frame_id'],f['timestamp_us'],f['raw_crc32'],f['epoch']),'heat/frame identity')
        schedule=u32(row,32);require(schedule in (10,30 if manifest['duration_s']==60 else 90,manifest['duration_s']-5),'heat schedule')
        require(0<=f['timestamp_us']-manifest['first_us']-schedule*1000000<=500000,'heat sample outside time window')
        heats.append(dict(index=i+1,frame_index=frame_index,frame_id=f['frame_id'],timestamp_us=f['timestamp_us'],schedule_s=schedule,
                          shape=[205,121],dtype='little-endian float32',payload=row[256:99476]))
    manifest['validated']=True
    manifest['complete_duration_record']=bool(n and not manifest['incomplete'] and not manifest['error'] and nh==3 and manifest['last_us']-manifest['first_us']>=manifest['duration_s']*1000000)
    manifest['evidence_scope']='WIRE_INTEGRITY_AND_OUTPUT_RECORD_ONLY; POINT_ONLY_REFLECTION_REPLAY_POSSIBLE; THREE_HEATMAP_SAMPLES_CANNOT_REPLAY_COMPLETE_STRUCTURE_SEQUENCE; NO_PHYSICAL_ACCURACY_CLAIM'
    return manifest,json_safe(frames),timing,heats

def host_path(value):
    path=Path(value)
    if not path.exists() and re.match(r'^[A-Za-z]:[\\/]',str(value)) and Path('/mnt').exists():
        path=Path('/mnt')/str(value)[0].lower()/str(value)[3:].replace('\\','/')
    return path

def bind_launch(manifest,binding):
    """Consume the actual schema2 sealed startup identity, not a new assertion."""
    require(binding.get('schema_version')==2,'unsupported sealed launch identity')
    require(binding.get('build_id')==manifest['build_id'] and binding.get('application',{}).get('build_id')==manifest['build_id'],'launch build identity mismatch')
    inputs={x['name']:x for x in binding.get('inputs',[])}
    require(all(x in inputs for x in ('app','bit','xsa')),'sealed launch missing app/bit/xsa')
    for name in ('app','bit','xsa'):
        entry=inputs[name];path=host_path(entry['path'])
        require(path.is_file(),f'sealed input missing: {path}')
        data=path.read_bytes()
        require(hashlib.sha256(data).hexdigest()==entry['sha256'],f'sealed {name} changed since launch identity')
        if name=='app':
            require(manifest['software_version'].encode('ascii') in data and manifest['build_id'].encode('ascii') in data,'record software/version not in sealed ELF')
        else:
            require(entry['sha256']==manifest[f'{name}_sha256'],f'launch {name} hash mismatch')
    selected=[x for x in binding.get('source_files',[]) if str(x['path']).replace('\\','/').endswith('/config/profiles/radar_3tx_hsi_stream0.cfg')]
    require(len(selected)==1 and selected[0]['sha256']==manifest['cfg_sha256'],'selected continuous profile not bound to sealed source identity')
    return {'elf_sha256':inputs['app']['sha256'],'bit_sha256':inputs['bit']['sha256'],'xsa_sha256':inputs['xsa']['sha256'],
            'build_id':manifest['build_id'],'software_version':manifest['software_version'],'selected_cfg':selected[0],
            'scope':'CONSISTENCY_WITH_SEALED_LAUNCH_IDENTITY; ACTUAL_BOOT_PROOF_REMAINS_IN_BOARD_STARTUP_LOG'}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs',nargs='+',type=Path)
    parser.add_argument('--output-dir',required=True,type=Path)
    parser.add_argument('--launch-identity',required=True,type=Path)
    parser.add_argument('--expected-duration',type=int,choices=(60,180))
    args=parser.parse_args()
    image,pages=collect_pages(args.logs)
    manifest,frames,timing,heats=decode_image(image,args.expected_duration)
    require(tuple(pages['envelope'][k] for k in ('epoch','frames','timing','heatmaps'))==
            tuple(manifest[k] for k in ('epoch','frame_count','timing_count','heatmap_count')),'envelope/manifest identity mismatch')
    binding=json.loads(args.launch_identity.read_text(encoding='utf-8-sig'))
    manifest['launch_identity']=bind_launch(manifest,binding)
    manifest['launch_identity_path']=str(args.launch_identity.resolve())
    manifest['launch_identity_sha256']=hashlib.sha256(args.launch_identity.read_bytes()).hexdigest()
    manifest['launch_binding_verified']=True
    manifest['export']=pages
    manifest['inputs']=[{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in args.logs]
    out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    (out/'image.bin').write_bytes(image)
    for name,rows in [('frames',frames),('timing',timing)]:
        (out/f'{name}.jsonl').write_text(''.join(json.dumps(row,allow_nan=False)+'\n' for row in rows),encoding='utf-8')
    for h in heats:
        h['file']=f"heatmap_{h['index']:02d}.f32le"
        (out/h['file']).write_bytes(h.pop('payload'))
    manifest['heatmaps']=heats
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps({'validated':True,'frames':len(frames),'timing':len(timing),'heatmaps':len(heats),'complete_duration_record':manifest['complete_duration_record'],'image_sha256':manifest['image_sha256'],'output_dir':str(out)}))
    return 0

if __name__=='__main__':
    raise SystemExit(main())
