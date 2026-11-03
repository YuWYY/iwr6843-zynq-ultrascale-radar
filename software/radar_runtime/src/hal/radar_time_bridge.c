/* SPDX-License-Identifier: MIT */
#include "radar_time_bridge.h"
#include <string.h>

static uint32_t rd(RadarTimeBridge *b,uint32_t a){return b->io.read(b->io.context,a);}
static void wr(RadarTimeBridge *b,uint32_t a,uint32_t v){b->io.write(b->io.context,a,v);}
static uint64_t distance(uint64_t a,uint64_t b){return a>b?a-b:b-a;}
static uint64_t tick_us(uint64_t n,uint32_t hz)
{return (n/hz)*UINT64_C(1000000)+(n%hz)*UINT64_C(1000000)/hz;}
static uint32_t drift_us(uint64_t n)
{return (uint32_t)((n*RADAR_TIME_DRIFT_PPM+999999U)/1000000U);}
void radar_time_bridge_invalidate(RadarTimeBridge *b,int reason)
{if(b){b->valid=0;b->error=reason;++b->invalidations;}}
static int fail(RadarTimeBridge *b,int error)
{radar_time_bridge_invalidate(b,error);return error;}
void radar_time_bridge_init(RadarTimeBridge *b,const RadarDspIo *io)
{if(b){memset(b,0,sizeof *b);if(io)b->io=*io;}}
static int capability(RadarTimeBridge *b)
{
    if(!b || !b->io.mapped || !b->io.read || !b->io.write || !b->io.barrier || !b->io.now)
        return RADAR_TIME_E_CAPABILITY;
    if(rd(b,0)!=UINT32_C(0x52445031) || rd(b,4)!=RADAR_TIME_ABI)
        return fail(b,RADAR_TIME_E_CAPABILITY);
    return 0;
}
int radar_time_bridge_begin(RadarTimeBridge *b,uint32_t epoch)
{
    int rc;if(!b || !epoch)return RADAR_TIME_E_ARGUMENT;
    if((rc=capability(b)))return rc;
    if(rd(b,RADAR_TIME_STATUS)&1U)return fail(b,RADAR_TIME_E_OWNER);
    b->valid=0;b->epoch=epoch;
    wr(b,RADAR_TIME_EPOCH_TOKEN,epoch);b->io.barrier(b->io.context);
    wr(b,RADAR_TIME_CMD,2U);b->io.barrier(b->io.context);
    if(rd(b,RADAR_TIME_CURRENT_EPOCH)!=epoch || !(rd(b,RADAR_TIME_STATUS)&1U) || rd(b,0x100U))
        return fail(b,RADAR_TIME_E_EPOCH);
    return radar_time_bridge_refresh(b);
}
int radar_time_bridge_end(RadarTimeBridge *b,uint32_t epoch,int stopped)
{
    uint32_t current;int rc;if(!b || !epoch)return RADAR_TIME_E_ARGUMENT;
    if(!stopped)return RADAR_TIME_E_OWNER;
    if((rc=capability(b)))return rc;
    if(epoch!=b->epoch)
        return fail(b,RADAR_TIME_E_EPOCH);
    current=rd(b,RADAR_TIME_CURRENT_EPOCH);
    if(current!=epoch){
        /* A confirmed stopped/reset source cannot retain an owner that the
         * PL itself erased. Release only this zero-epoch/unowned case;
         * keep the mapping/error invalid and never adopt another epoch. */
        if(!current && !(rd(b,RADAR_TIME_STATUS)&1U)){
            radar_time_bridge_invalidate(b,RADAR_TIME_E_EPOCH);return 0;
        }
        return fail(b,RADAR_TIME_E_EPOCH);
    }
    wr(b,RADAR_TIME_CMD,3U);b->io.barrier(b->io.context);
    if(rd(b,RADAR_TIME_STATUS)&1U)return fail(b,RADAR_TIME_E_OWNER);
    return 0;
}
int radar_time_bridge_refresh(RadarTimeBridge *b)
{
    uint64_t before,after,ticks,mid,elapsed,nominal,budget;
    uint32_t gen0,gen1,gen2,epoch,hz,uncertainty;int rc;
    if((rc=capability(b)))return rc;
    if(!b->epoch || rd(b,RADAR_TIME_CURRENT_EPOCH)!=b->epoch)
        return fail(b,RADAR_TIME_E_EPOCH);
    gen0=rd(b,RADAR_TIME_GENERATION);
    before=b->io.now(b->io.context);wr(b,RADAR_TIME_CMD,1U);b->io.barrier(b->io.context);
    gen1=rd(b,RADAR_TIME_GENERATION);
    ticks=(uint64_t)rd(b,RADAR_TIME_TICKS_LO)|((uint64_t)rd(b,RADAR_TIME_TICKS_HI)<<32);
    epoch=rd(b,RADAR_TIME_SNAPSHOT_EPOCH);hz=rd(b,0x124U);
    gen2=rd(b,RADAR_TIME_GENERATION);after=b->io.now(b->io.context);
    if(gen1!=gen0+1U || gen1!=gen2 || !(rd(b,RADAR_TIME_STATUS)&2U))
        return fail(b,RADAR_TIME_E_SNAPSHOT);
    if(epoch!=b->epoch || rd(b,RADAR_TIME_CURRENT_EPOCH)!=epoch)
        return fail(b,RADAR_TIME_E_EPOCH);
    if(!hz || after<before || (b->valid && hz!=b->hz))return fail(b,RADAR_TIME_E_CLOCK);
    /* Include microsecond quantization plus one SYS tick rounding. */
    if(after-before>2U*(RADAR_TIME_MAX_UNCERTAINTY_US-2U))return fail(b,RADAR_TIME_E_UNCERTAINTY);
    uncertainty=(uint32_t)((after-before+1U)/2U)+2U;mid=before+(after-before)/2U;
    if(b->valid){
        if(mid<b->anchor_us || ticks<b->anchor_tick)return fail(b,RADAR_TIME_E_CLOCK);
        elapsed=mid-b->anchor_us;
        if(elapsed>RADAR_TIME_MAX_AGE_US)return fail(b,RADAR_TIME_E_STALE);
        nominal=tick_us(ticks-b->anchor_tick,hz);
        budget=(uint64_t)uncertainty+b->anchor_uncertainty_us+drift_us(elapsed)+2U;
        if(distance(elapsed,nominal)>budget)return fail(b,RADAR_TIME_E_CLOCK);
    }
    b->anchor_tick=ticks;b->anchor_us=mid;b->bracket_before_us=before;b->bracket_after_us=after;
    b->anchor_uncertainty_us=uncertainty;b->hz=hz;b->generation=gen1;
    b->valid=1;b->error=0;++b->refreshes;return 0;
}
int radar_time_bridge_map(RadarTimeBridge *b,uint32_t epoch,uint64_t ticks,uint64_t now,
                          uint64_t *ps_us,uint32_t *uncertainty_us)
{
    uint64_t age,delta,mapped;uint32_t uncertainty;
    if(!b || !ps_us || !uncertainty_us)return RADAR_TIME_E_ARGUMENT;
    *ps_us=0;*uncertainty_us=0;
    if(!b->valid)return b->error?b->error:RADAR_TIME_E_SNAPSHOT;
    if(epoch!=b->epoch || rd(b,RADAR_TIME_CURRENT_EPOCH)!=epoch)
        return fail(b,RADAR_TIME_E_EPOCH);
    if(now<b->anchor_us)return fail(b,RADAR_TIME_E_CLOCK);
    age=now-b->anchor_us;
    delta=tick_us(distance(ticks,b->anchor_tick),b->hz);
    if(age>RADAR_TIME_MAX_AGE_US || delta>RADAR_TIME_MAX_AGE_US)
        return fail(b,RADAR_TIME_E_STALE);
    uncertainty=b->anchor_uncertainty_us+drift_us(age>delta?age:delta)+2U;
    if(uncertainty>RADAR_TIME_MAX_UNCERTAINTY_US)return fail(b,RADAR_TIME_E_UNCERTAINTY);
    if(ticks>=b->anchor_tick)mapped=b->anchor_us+delta;
    else {if(delta>b->anchor_us)return fail(b,RADAR_TIME_E_CLOCK);mapped=b->anchor_us-delta;}
    if(mapped>now && mapped-now>uncertainty)return fail(b,RADAR_TIME_E_CLOCK);
    *ps_us=mapped;*uncertainty_us=uncertainty;return 0;
}

#ifndef RADAR_TIME_BRIDGE_HOST_ONLY
static RadarDspDriver owner;
static RadarTimeBridge stream_bridge;
int radar_stream_time_begin(uint32_t epoch)
{
    int rc;radar_dsp_platform_init(&owner);
    radar_time_bridge_init(&stream_bridge,&owner.io);
    rc=radar_time_bridge_begin(&stream_bridge,epoch);owner.capable=rc==0;return rc;
}
int radar_stream_time_end(uint32_t epoch)
{return radar_time_bridge_end(&stream_bridge,epoch,1);}
int radar_stream_time_pop(RadarTransportTime *t,uint64_t *us,uint32_t *uncertainty)
{
    uint64_t now;int rc;
    if(!t || !us || !uncertainty)return RADAR_TIME_E_ARGUMENT;
    *us=0;*uncertainty=0;
    if(!stream_bridge.valid)return stream_bridge.error?stream_bridge.error:RADAR_TIME_E_SNAPSHOT;
    now=stream_bridge.io.now(stream_bridge.io.context);
    if(now<stream_bridge.anchor_us || now-stream_bridge.anchor_us>=RADAR_TIME_REFRESH_US){
        rc=radar_time_bridge_refresh(&stream_bridge);if(rc)return rc;
        now=stream_bridge.io.now(stream_bridge.io.context);
    }
    rc=radar_dsp_metadata_pop(&owner,t);if(rc<=0)return rc;
    if(!t->valid)return RADAR_TIME_E_SNAPSHOT;
    rc=radar_time_bridge_map(&stream_bridge,stream_bridge.epoch,t->first_tick,now,us,uncertainty);
    return rc?rc:1;
}
const RadarTimeBridge *radar_stream_time_state(void){return &stream_bridge;}
#endif
