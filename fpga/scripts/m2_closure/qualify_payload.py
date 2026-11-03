"""Require real finite nonnegative held-payload max-delay and bus-skew results."""
from pathlib import Path
import argparse
import json
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_identity import GateError, read_json, require
from release_gate import payload_metrics, bus_skew, reset_metrics

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('reports',type=Path)
    ap.add_argument('--stage',choices=['synth','route'],default='route')
    args=ap.parse_args()
    try:
        metrics=read_json(args.reports/'constraint_metrics.json')
        require(metrics.get('verification_stage') == args.stage, 'Constraint report stage mismatch')
        rows=payload_metrics(metrics)
        resets=reset_metrics(metrics,args.stage)
        skews=bus_skew((args.reports/'bus_skew.rpt').read_text(encoding='utf-8-sig'),rows)
        print(json.dumps({'PAYLOAD_TIMING':'PASS','PHY_RESET_TIMING':resets['status'],'PHYSICAL_SIGNOFF':'PASS' if args.stage=='route' else 'NOT_RUN','reset_endpoints':len(resets['endpoints']),'channels':len(rows),'bus_skew':skews},indent=2))
        return 0
    except (GateError,OSError,ValueError,KeyError,TypeError) as exc:
        print(f'PAYLOAD_TIMING_FAIL: {exc}',file=sys.stderr)
        return 1

if __name__=='__main__':raise SystemExit(main())
