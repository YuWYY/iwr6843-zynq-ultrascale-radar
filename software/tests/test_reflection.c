/* Synthetic point sequences only; no hardware and no target-class ground truth. */
#include "../radar_runtime/src/processing/radar_reflection.h"
#include "../radar_runtime/src/processing/radar_dsp_types.h"
#include "../radar_runtime/src/processing/radar_range.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static RadarReflectionProcessor p;
static RadarSpatialFrame s;
static RadarSceneContext c;
static RadarSceneFrame result;
static void initial(void)
{
    memset(&p,0,sizeof p);memset(&s,0,sizeof s);memset(&c,0,sizeof c);
    s.valid=1U;c.capture_epoch=1;c.installation_epoch=1;c.time_valid=1U;c.frame_flags=7U;
    strcpy(c.cfg_sha256,"synthetic-not-a-live-profile");strcpy(c.calibration_id,"UNCALIBRATED");
    strcpy(s.calibration_id,c.calibration_id);
}
static void point(float range,float angle,unsigned bearing)
{
    RadarSpatialPoint *q=&s.points[s.count++];memset(q,0,sizeof *q);
    q->range_m=range;q->range_bin=(unsigned)(range/(float)RADAR_RANGE_BIN_METERS+0.5f);
    q->horizontal_angle_deg=angle;q->snr_db=20.0f;q->power_dbfs=-45.0f;q->coherence=.2f;
    q->flags=bearing?RADAR_POINT_BEARING_VALID|RADAR_POINT_LOW_COHERENCE|RADAR_POINT_ANGLE_EDGE:0U;
    q->x_m=q->y_m=q->z_m=NAN;
}
static void frame(uint64_t now)
{
    ++c.frame_id;s.session=c.frame_id;s.timestamp_us=c.timestamp_us=now;memset(&result,0,sizeof result);
    radar_reflection_update(&p,&s,&c,&result);
}
int main(void)
{
    unsigned i,id;uint64_t last;
    assert(sizeof(RadarStableReflection)==64&&sizeof(RadarSceneDiagnostics)==128);
    initial();point(2.68f,NAN,0);
    for(i=0;i<25;++i)frame(1000000U+i*100000U);
    assert(result.reflection_count==1&&result.reflections[0].state==RADAR_REFLECTION_STABLE);
    assert(!(result.reflections[0].flags&RADAR_REFLECTION_U_VALID)&&isnan(result.reflections[0].u));
    assert(fabsf(result.reflections[0].range_median_m-2.68f)<1e-5f);
    id=result.reflections[0].id;last=result.reflections[0].last_observed_us;
    s.count=0;frame(last+100000U);
    assert(result.reflections[0].state==RADAR_REFLECTION_STALE&&result.reflections[0].id==id);
    assert(result.reflections[0].last_observed_us==last&&!(result.reflections[0].flags&RADAR_REFLECTION_MEASURED));
    frame(last+500000U);assert(result.reflection_count==1);
    frame(last+500001U);assert(result.reflection_count==0);

    initial();point(2.68f,12.0f,1);
    for(i=0;i<25;++i)frame(1000000U+i*100000U);
    assert(result.reflections[0].state==RADAR_REFLECTION_STABLE);
    assert(result.reflections[0].flags&RADAR_REFLECTION_U_VALID);
    assert(fabsf(result.reflections[0].u-sinf(12.0f*3.14159265f/180))<1e-5f);
    last=result.reflections[0].last_observed_us;
    s.count=0;point(2.66f,NAN,0);point(2.70f,NAN,0);frame(last+100000U);
    assert(result.reflection_count==1&&result.reflections[0].state==RADAR_REFLECTION_AMBIGUOUS);
    assert(result.reflections[0].last_observed_us==last);

    initial();point(2.68f,0,1);
    for(i=0;i<25;++i)frame(1000000U+i*100000U);
    id=result.reflections[0].id;
    c.frame_flags|=RADAR_FRAME_DISCONTINUITY;frame(3500000U);
    assert(result.reflection_count==0&&result.diagnostics.reject_flags&RADAR_SCENE_REJECT_INTEGRITY);
    c.frame_flags=7U;frame(3600000U);
    assert(result.reflection_count==1&&result.reflections[0].id!=id&&result.reflections[0].state==RADAR_REFLECTION_TENTATIVE);
    c.frame_flags|=RADAR_FRAME_CLIPPED;frame(3700000U);
    assert(result.reflection_count==0&&result.diagnostics.reject_flags&RADAR_SCENE_REJECT_CLIPPED);
    c.frame_flags=3U;frame(3800000U);
    assert(result.reflection_count==0&&result.diagnostics.reject_flags&RADAR_SCENE_REJECT_TIME);
    c.frame_flags=5U;frame(3850000U);
    assert(result.reflection_count==0&&result.diagnostics.reject_flags&RADAR_SCENE_REJECT_INTEGRITY);
    c.frame_flags=7U;frame(3900000U);id=result.reflections[0].id;
    memset(&result,0,sizeof result);radar_reflection_update(&p,&s,&c,&result);
    assert(result.reflection_count==0&&result.diagnostics.reject_flags&RADAR_SCENE_REJECT_TIME);
    frame(4000000U);assert(result.reflections[0].id!=id);id=result.reflections[0].id;
    ++c.installation_epoch;frame(4100000U);assert(result.reflections[0].id!=id);
    id=result.reflections[0].id;++c.capture_epoch;frame(4200000U);assert(result.reflections[0].id!=id);
    s.session=c.frame_id+5U;memset(&result,0,sizeof result);radar_reflection_update(&p,&s,&c,&result);
    assert(result.reflection_count==0&&result.diagnostics.reject_flags&RADAR_SCENE_REJECT_INTEGRITY);

    initial();point(2.5f,0,1);
    for(i=0;i<40;++i){s.points[0].range_m=2.5f+.02f*(float)i;frame(1000000U+i*100000U);}
    assert(result.reflection_count==1&&result.reflections[0].state!=RADAR_REFLECTION_STABLE); /* drift */
    initial();point(2.5f,0,1);
    for(i=0;i<40;++i){s.points[0].range_m=2.5f+.16f*sinf(2.0f*3.1415926536f*(float)i/10.0f);
        frame(1000000U+i*100000U);}
    assert(result.reflection_count==1&&result.reflections[0].state!=RADAR_REFLECTION_STABLE);
    assert(result.reflections[0].range_mad_m>2.0f*(float)RADAR_RANGE_BIN_METERS);
    initial();point(2.68f,0,1);
    for(i=0;i<8;++i)frame(1000000U+i*400000U);
    assert(result.reflection_count==1&&result.reflections[0].state!=RADAR_REFLECTION_STABLE); /* bucket holes */
    initial();for(i=0;i<10;++i)point(1.0f+.6f*(float)i,NAN,0);frame(1000000U);
    assert(result.reflection_count==8&&result.diagnostics.capacity_count==2);
    puts("PASS point-only LOS replay, independent bearing, 2s measured stability, ambiguity, expiry, bad frame/time/context, no fabricated XYZ; reflection=64 diag=128");
    return 0;
}
