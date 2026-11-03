/* SPDX-License-Identifier: MIT */
#include "radar_reflection.h"
#include "radar_dsp_types.h"
#include "radar_range.h"
#include <float.h>
#include <math.h>
#include <string.h>

#define WINDOW_US UINT64_C(2000000)
#define EXPIRE_US UINT64_C(500000)
#define BUCKET_US UINT64_C(250000)
#define RANGE_GATE 0.15f
#define U_GATE 0.12f
#define MATCH_MARGIN 0.25f
#define PI 3.14159265358979323846f
typedef struct { float r,u,power,snr; unsigned uv; } Observation;
_Static_assert(sizeof(RadarStableReflection)==64U,"recorded reflection ABI");
_Static_assert(sizeof(RadarSceneDiagnostics)==128U,"recorded scene diagnostic ABI");

static float median(const float *v,unsigned n)
{
    float a[RADAR_REFLECTION_SAMPLES];unsigned i,j;
    if(!n)return NAN;
    for(i=0;i<n;++i){float x=v[i];j=i;while(j&&a[j-1U]>x){a[j]=a[j-1U];--j;}a[j]=x;}
    return n&1U?a[n/2U]:(a[n/2U-1U]+a[n/2U])*0.5f;
}
static unsigned bits(unsigned x){unsigned n=0;while(x){x&=x-1U;++n;}return n;}
void radar_reflection_reset(RadarReflectionProcessor *p)
{
    unsigned next=p->next_id;memset(p,0,sizeof *p);p->next_id=next?next:1U;
}
static float cost(float r,float u,unsigned uv,const Observation *q)
{
    float dr=fabsf(r-q->r),du=0.0f;
    if(dr>RANGE_GATE)return FLT_MAX;
    if(uv&&q->uv){du=fabsf(u-q->u);if(du>U_GATE)return FLT_MAX;}
    return (dr/RANGE_GATE)*(dr/RANGE_GATE)+(du/U_GATE)*(du/U_GATE);
}
static void measured(RadarReflectionTrack *t,const Observation *q,uint64_t now,unsigned limited)
{
    float ranges[32]={0},deviations[32]={0},early[32]={0},late[32]={0};
    unsigned i,n=0,e=0,l=0,mask=0;
    uint64_t bucket=now/BUCKET_US;
    while(t->count&&now-t->samples[0].time_us>WINDOW_US){--t->count;
        memmove(t->samples,t->samples+1U,t->count*sizeof t->samples[0]);}
    if(t->count==RADAR_REFLECTION_SAMPLES){--t->count;
        memmove(t->samples,t->samples+1U,t->count*sizeof t->samples[0]);limited=1U;}
    t->samples[t->count].time_us=now;t->samples[t->count++].range_m=q->r;
    t->value.flags=RADAR_REFLECTION_MEASURED|(q->uv?RADAR_REFLECTION_U_VALID:0U)|
        (limited?RADAR_REFLECTION_INPUT_LIMITED:0U);
    t->value.last_observed_us=now;t->value.range_m=q->r;t->value.u=q->uv?q->u:NAN;
    t->value.power_dbfs=q->power;t->value.snr_db=q->snr;t->value.reject_flags=0U;
    for(i=0;i<t->count;++i){const RadarReflectionSample *s=&t->samples[i];
        uint64_t b=s->time_us/BUCKET_US;
        ranges[n++]=s->range_m;
        if(now-s->time_us>=WINDOW_US/2U)early[e++]=s->range_m;else late[l++]=s->range_m;
        if(bucket>=b&&bucket-b<8U)mask|=1U<<(unsigned)(bucket-b);
    }
    t->value.range_median_m=median(ranges,n);
    for(i=0;i<n;++i)deviations[i]=fabsf(ranges[i]-t->value.range_median_m);
    t->value.range_mad_m=median(deviations,n);t->value.support_buckets=bits(mask);
    t->value.sample_count=n;t->value.state=RADAR_REFLECTION_TENTATIVE;
    if(now-t->value.first_observed_us>=WINDOW_US&&t->value.support_buckets>=7U&&e&&l&&
       t->value.range_mad_m<=2.0f*(float)RADAR_RANGE_BIN_METERS&&
       fabsf(median(early,e)-median(late,l))<=2.0f*(float)RADAR_RANGE_BIN_METERS)
        t->value.state=RADAR_REFLECTION_STABLE;
}
void radar_reflection_update(RadarReflectionProcessor *p,const RadarSpatialFrame *s,
                             const RadarSceneContext *c,RadarSceneFrame *out)
{
    Observation obs[32];float costs[8][32];unsigned n=0,i,j,k,used[32]={0},blocked[32]={0};
    unsigned best[8],cbest[32],matches[8]={0},cmatches[32]={0};
    float minimum[8],second[8],cmin[32],csecond[32];uint64_t now=c->timestamp_us;
    RadarSceneDiagnostics *d=&out->diagnostics;
    out->reflection_count=0U;d->range_mad_limit_m=2.0f*(float)RADAR_RANGE_BIN_METERS;
    d->stable_count=d->tentative_count=d->stale_count=d->ambiguous_count=0U;
    d->range_only_count=d->rejected_points=d->capacity_count=0U;
    d->association_range_m=RANGE_GATE;d->association_u=U_GATE;
    if(!s->valid||s->error||s->count>32U||s->session!=c->frame_id||s->timestamp_us!=now||
       !c->cfg_sha256[0]||!c->calibration_id[0]||
       strncmp(s->calibration_id,c->calibration_id,sizeof c->calibration_id)||
       !c->time_valid||(c->frame_flags&(RADAR_FRAME_HSI_VALID|RADAR_FRAME_CRC_VALID|RADAR_FRAME_TIME_TRUSTED))!=
       (RADAR_FRAME_HSI_VALID|RADAR_FRAME_CRC_VALID|RADAR_FRAME_TIME_TRUSTED)||
       (c->frame_flags&(RADAR_FRAME_CLIPPED|RADAR_FRAME_DISCONTINUITY|RADAR_FRAME_DIAGNOSTIC_PACKED))){
        radar_reflection_reset(p);
        d->reject_flags|=(c->frame_flags&RADAR_FRAME_CLIPPED)?RADAR_SCENE_REJECT_CLIPPED:
            (!c->time_valid||!(c->frame_flags&RADAR_FRAME_TIME_TRUSTED))?
            RADAR_SCENE_REJECT_TIME:RADAR_SCENE_REJECT_INTEGRITY;
        return;
    }
    if(p->have_context&&(p->context.capture_epoch!=c->capture_epoch||
       p->context.installation_epoch!=c->installation_epoch||
       strncmp(p->context.cfg_sha256,c->cfg_sha256,sizeof c->cfg_sha256)||
       strncmp(p->context.calibration_id,c->calibration_id,sizeof c->calibration_id)))
        radar_reflection_reset(p);
    if(p->have_context&&(c->frame_id<=p->context.frame_id||now<=p->context.timestamp_us)){
        radar_reflection_reset(p);d->reject_flags|=RADAR_SCENE_REJECT_TIME;return;
    }
    if(p->have_context&&now-p->context.timestamp_us>EXPIRE_US)radar_reflection_reset(p);
    p->context=*c;p->have_context=1U;
    if(!p->next_id)p->next_id=1U;
    for(i=0;i<s->count&&i<32U;++i){const RadarSpatialPoint *q=&s->points[i];Observation o;
        if(q->range_bin<=RADAR_RANGE_FIRST_BIN||q->range_bin>=RADAR_RANGE_LAST_BIN||
           !isfinite(q->range_m)||q->range_m<=0.0f||q->range_m>9.0f||
           !isfinite(q->snr_db)||q->snr_db<12.0f||!isfinite(q->power_dbfs)||
           (q->flags&RADAR_POINT_DOPPLER_EDGE)){++d->rejected_points;continue;}
        o.r=q->range_m;o.power=q->power_dbfs;o.snr=q->snr_db;o.u=NAN;o.uv=0U;
        if(((q->flags&RADAR_POINT_BEARING_VALID)||((q->flags&RADAR_POINT_XYZ_VALID)&&
            !(q->flags&(RADAR_POINT_LOW_COHERENCE|RADAR_POINT_ANGLE_EDGE))))&&
           isfinite(q->horizontal_angle_deg)&&fabsf(q->horizontal_angle_deg)<60.0f){
            o.uv=1U;o.u=sinf(q->horizontal_angle_deg*PI/180.0f);}
        /* Deterministic power order for unused-slot admission; ties keep the
         * recorded input order. Association itself never chooses by power. */
        j=n;while(j&&obs[j-1U].power<o.power){obs[j]=obs[j-1U];--j;}obs[j]=o;++n;
    }
    for(j=0;j<n;++j){cbest[j]=8U;cmin[j]=csecond[j]=FLT_MAX;}
    for(i=0;i<8U;++i){RadarReflectionTrack *t=&p->tracks[i];best[i]=32U;minimum[i]=second[i]=FLT_MAX;
        if(t->alive&&(now<=t->value.last_observed_us||now-t->value.last_observed_us>EXPIRE_US))t->alive=0U;
        for(j=0;j<n;++j){float z=t->alive?cost(t->value.range_m,t->value.u,
                !!(t->value.flags&RADAR_REFLECTION_U_VALID),&obs[j]):FLT_MAX;costs[i][j]=z;
            if(z==FLT_MAX)continue;
            ++matches[i];++cmatches[j];
            if(z<minimum[i]){second[i]=minimum[i];minimum[i]=z;best[i]=j;}else if(z<second[i])second[i]=z;
            if(z<cmin[j]){csecond[j]=cmin[j];cmin[j]=z;cbest[j]=i;}else if(z<csecond[j])csecond[j]=z;
        }
    }
    for(i=0;i<8U;++i){RadarReflectionTrack *t=&p->tracks[i];unsigned accept=0,ambiguous=0;
        if(!t->alive)continue;
        j=best[i];
        if(j<n){unsigned unknown=!(t->value.flags&RADAR_REFLECTION_U_VALID)||!obs[j].uv;
            accept=cbest[j]==i&&second[i]-minimum[i]>=MATCH_MARGIN&&csecond[j]-cmin[j]>=MATCH_MARGIN&&
                !(unknown&&(matches[i]>1U||cmatches[j]>1U));
            ambiguous=!accept;
        }
        if(accept){used[j]=1U;measured(t,&obs[j],now,s->truncated_count!=0U);}
        else {t->value.flags&=~RADAR_REFLECTION_MEASURED;
            t->value.state=ambiguous?RADAR_REFLECTION_AMBIGUOUS:RADAR_REFLECTION_STALE;
            t->value.reject_flags=ambiguous?RADAR_SCENE_REJECT_ASSOCIATION:RADAR_SCENE_REJECT_NO_FRESH;
            if(ambiguous)for(j=0;j<n;++j)if(costs[i][j]!=FLT_MAX)blocked[j]=1U;
        }
    }
    for(j=0;j<n;++j)if(!used[j]&&!blocked[j]){
        unsigned conflict=0;
        for(k=0;k<n;++k)if(k!=j&&!used[k]&&cost(obs[j].r,obs[j].u,obs[j].uv,&obs[k])!=FLT_MAX){conflict=1U;break;}
        /* Never start another identity beside an existing compatible owner,
         * including an ambiguous owner that could not consume this frame. */
        for(i=0;i<8U&&!conflict;++i)if(p->tracks[i].alive&&cost(p->tracks[i].value.range_m,
            p->tracks[i].value.u,!!(p->tracks[i].value.flags&RADAR_REFLECTION_U_VALID),&obs[j])!=FLT_MAX)conflict=1U;
        if(conflict){++d->ambiguous_count;d->reject_flags|=RADAR_SCENE_REJECT_ASSOCIATION;continue;}
        for(i=0;i<8U&&p->tracks[i].alive;++i){}
        if(i==8U){++d->capacity_count;d->reject_flags|=RADAR_SCENE_REJECT_CAPACITY;continue;}
        {RadarReflectionTrack *t=&p->tracks[i];memset(t,0,sizeof *t);t->alive=1U;
            t->value.id=p->next_id++;if(!p->next_id)p->next_id=1U;
            t->value.first_observed_us=now;measured(t,&obs[j],now,s->truncated_count!=0U);used[j]=1U;}
    }
    for(i=0;i<8U;++i)if(p->tracks[i].alive){RadarStableReflection *v=&p->tracks[i].value;
        out->reflections[out->reflection_count++]=*v;
        if(v->state==RADAR_REFLECTION_STABLE)++d->stable_count;
        else if(v->state==RADAR_REFLECTION_TENTATIVE)++d->tentative_count;
        else if(v->state==RADAR_REFLECTION_AMBIGUOUS)++d->ambiguous_count;else ++d->stale_count;
        if(!(v->flags&RADAR_REFLECTION_U_VALID))++d->range_only_count;
    }
}
