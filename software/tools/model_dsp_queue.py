"""Explicit service-time capacity model, not a hardware throughput test.

Continuous100ms arrival is a stress assumption for the next streaming stage.
The production finite8/16 prefetch instead stops/HALTs RX when RAW is busy.
Engine simulation cycles omit real DDR arbitration; supply a nonnegative DDR
stall budget and measured PS parse/import/render occupancy separately.
"""
from collections import deque
import argparse,json,math

def simulate(service_us,frames=64,period_us=100000,capacity=16):
    if service_us<=0 or frames<1 or period_us<=0 or capacity<1:
        raise ValueError('positive service, frames, period and capacity required')
    pending=deque();finish=0.0;high=0;accepted=0;overflow=None;trace=[]
    for frame in range(frames):
        arrival=frame*period_us
        while pending and pending[0]<=arrival:pending.popleft()
        if len(pending)>=capacity:
            overflow=frame+1;break  # Never overwrite a leased or queued frame.
        finish=max(finish,arrival)+service_us;pending.append(finish)
        accepted+=1;high=max(high,len(pending));trace.append([frame+1,arrival,len(pending),finish])
    return dict(evidence='MODEL_ONLY_NOT_BOARD_THROUGHPUT',arrival_period_us=period_us,
        service_us=service_us,capacity=capacity,accepted=accepted,requested=frames,
        high_water=high,overflow_frame=overflow,drain_us=finish,
        utilization=service_us/period_us,sustainable=service_us<=period_us,
        stop_policy='STOP_PRODUCER_RETAIN_OWNED_SLOTS',trace=trace)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--engine-cycles',type=int,required=True)
    ap.add_argument('--engine-hz',type=int,required=True)
    ap.add_argument('--ps-us',type=float,required=True)
    ap.add_argument('--ddr-stall-us',type=float,required=True)
    ap.add_argument('--render-occupancy-us',type=float,required=True)
    ap.add_argument('--frames',type=int,default=64)
    ap.add_argument('--period-us',type=int,default=100000)
    ap.add_argument('--out',required=True)
    a=ap.parse_args()
    if a.engine_cycles<=0 or a.engine_hz<=0 or min(a.ps_us,a.ddr_stall_us,a.render_occupancy_us)<0:
        ap.error('cycles/hz positive; PS, DDR stalls and rendering occupancy nonnegative')
    service=a.engine_cycles*1e6/a.engine_hz+a.ps_us+a.ddr_stall_us+a.render_occupancy_us
    if not math.isfinite(service):ap.error('finite times required')
    result=simulate(service,a.frames,a.period_us)
    result['inputs']=vars(a)
    with open(a.out,'w',encoding='utf-8') as f:json.dump(result,f,indent=2)
    print(json.dumps({k:v for k,v in result.items() if k not in ('trace','inputs')}))
    return 0 if result['overflow_frame'] is None and result['sustainable'] else 2
if __name__=='__main__':raise SystemExit(main())
