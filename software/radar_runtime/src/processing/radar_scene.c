/* SPDX-License-Identifier: MIT */
#include "radar_scene.h"
#include "radar_range.h"
#include "radar_reflection.h"
#include "radar_reflection_envelope.h"
#include "radar_dsp_types.h"
#include <math.h>
#include <string.h>

#define CELLS (RADAR_SPATIAL_MAP_RANGES*RADAR_SPATIAL_MAP_ANGLES)
#define MAP_BYTES ((CELLS+7U)/8U)
#define BUCKETS 40U
#define BUCKET_US UINT64_C(250000)
#define EXPIRE_US UINT64_C(500000)
#define TRAIL_US UINT64_C(2000000)
#define MAP_US UINT64_C(10000000)
#define FIT_POINTS 256U
#define FIT_TRIALS 96U
#define SURFACE_CANDIDATES 64U
#define PI 3.14159265358979323846f
#define SQ(x) ((x)*(x))
typedef struct {
    RadarSceneTrack out;
    float state[4],cov[4][4];
    float extent_mean,extent_deviation,radial_spread_mean;
    unsigned alive,hits5,total_hits,nonrigid_hits,motion_hits;
    uint64_t state_time_us,last_feature_us;
} Track;
typedef struct {float x,y,extent,snr,radial_spread;unsigned points,strong;} Cluster;
typedef struct {float x,y,support;unsigned cell,used;} Reflection;
typedef struct {float x,y,u,power;unsigned range,angle;} SurfaceAnchor;
typedef struct {
    RadarSceneFrame result;
    RadarSceneContext context;
    const RadarSpatialFrame *source;
    Track tracks[RADAR_SCENE_MAX_TRACKS];
    RadarReflectionProcessor stable_reflections;
    Cluster clusters[RADAR_SPATIAL_MAX_POINTS];
    unsigned cluster_count;
    uint8_t buckets[BUCKETS][MAP_BYTES],support[CELLS];
    uint64_t valid_buckets,clear_buckets,map_start_us,last_time_us,last_bucket;
    float angle_sin[RADAR_SPATIAL_MAP_ANGLES],angle_cos[RADAR_SPATIAL_MAP_ANGLES];
    Reflection reflections[FIT_POINTS];
    unsigned reflection_count,reflection_seen,prior_persistent,new_supported,changed_frames;
    unsigned next_id,have_context,have_time,stage,row,active_bucket,fit_trial,fit_best_n;
    float fit_best_nx,fit_best_ny,fit_best_c;
    uint32_t reset_reason;
    SurfaceAnchor candidates[SURFACE_CANDIDATES],anchors[SURFACE_CANDIDATES];
    unsigned candidate_count,anchor_count,guard_cursor;
} Work;
_Static_assert(sizeof(Work)<=RADAR_SCENE_WORKSPACE_LIMIT,"scene reserved workspace");
_Static_assert(sizeof(RadarSceneFrame)<32768U,"scene immutable snapshot size");

static unsigned popcount64(uint64_t v){unsigned n=0;while(v){v&=v-1U;++n;}return n;}
static unsigned popcount4(unsigned v){return (v&1U)+((v>>1U)&1U)+((v>>2U)&1U)+((v>>3U)&1U);}
static int has_bit(const uint8_t *b,unsigned k){return (b[k>>3U]>>(k&7U))&1U;}
static void set_bit(uint8_t *b,unsigned k){b[k>>3U]|=(uint8_t)(1U<<(k&7U));}
static void unset_bit(uint8_t *b,unsigned k){b[k>>3U]&=(uint8_t)~(1U<<(k&7U));}
static float limit(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}
static Work *work(RadarSceneProcessor *p){return p?(Work *)p->workspace:NULL;}
size_t radar_scene_workspace_bytes(void){return sizeof(Work);}
uint32_t radar_scene_reflection_next_id(const RadarSceneProcessor *p)
{
    const Work *w=p?(const Work *)p->workspace:NULL;
    return w&&w->stable_reflections.next_id?w->stable_reflections.next_id:1U;
}
const char *radar_scene_parameter_identity(void)
{
    return "REFLECTION_SURFACE_V1;LOS8;window_us=2000000;bucket_us=250000;support=7/8;"
        "expire_us=500000;mad_bins=2;halfwindow_drift_bins=2;range_gate=.15;u_gate=.12;"
        "mutual_margin=.25;snr_db=12;map_us=10000000;map_buckets=28/40;"
        "power_floor_db=-75;radial_contrast_db=4;radial_offset_bins=3;"
        "same_range_bins=2;guard_db=6;guard_floor_db=-24;groups=3;cells=6;"
        "span_m=1;residual_m=.15;gap_m=.35;occupancy=.7;candidates=64;"
        "envelope=" RADAR_REFLECTION_ENVELOPE_SHA256;
}
const char *radar_scene_label_name(uint32_t label)
{
    switch(label){case RADAR_SCENE_MOVING_OBJECT:return "MOVING OBJECT";
    case RADAR_SCENE_STATIC_REFLECTOR:return "STATIC REFLECTOR";
    case RADAR_SCENE_HUMAN_CANDIDATE:return "HUMAN?";default:return "UNKNOWN";}
}
int radar_scene_init(RadarSceneProcessor *p,void *workspace,size_t bytes)
{
    Work *w;unsigned a;
    if(!p||!workspace||bytes<sizeof(Work)||(uintptr_t)workspace%8U)return RADAR_SCENE_E_ARGUMENT;
    p->workspace=workspace;p->workspace_bytes=bytes;w=work(p);memset(w,0,sizeof *w);
    w->next_id=1U;
    for(a=0;a<RADAR_SPATIAL_MAP_ANGLES;++a){float rad=((float)a-60.0f)*PI/180.0f;
        w->angle_sin[a]=sinf(rad);w->angle_cos[a]=cosf(rad);}
    return RADAR_SCENE_DONE;
}
void radar_scene_reset(RadarSceneProcessor *p,uint32_t reason)
{
    Work *w=work(p);unsigned next;
    if(!w)return;
    next=w->next_id;memset(w->tracks,0,sizeof w->tracks);memset(w->buckets,0,sizeof w->buckets);
    memset(w->support,0,sizeof w->support);memset(&w->result,0,sizeof w->result);
    w->source=NULL;w->stage=0;w->have_context=0;w->have_time=0;w->valid_buckets=0;
    w->clear_buckets=0;w->map_start_us=0;w->last_time_us=0;w->last_bucket=0;
    w->changed_frames=0;w->next_id=next?next:1U;w->reset_reason=reason;
    radar_reflection_reset(&w->stable_reflections);
}
static int different(const RadarSceneContext *a,const RadarSceneContext *b)
{
    return a->capture_epoch!=b->capture_epoch||a->installation_epoch!=b->installation_epoch||
        strncmp(a->cfg_sha256,b->cfg_sha256,sizeof a->cfg_sha256)!=0||
        strncmp(a->calibration_id,b->calibration_id,sizeof a->calibration_id)!=0;
}
int radar_scene_begin(RadarSceneProcessor *p,const RadarSpatialFrame *s,const RadarSceneContext *c)
{
    Work *w=work(p);uint64_t absolute,advance,k;unsigned reason;
    if(!w||!s||!c)return RADAR_SCENE_E_ARGUMENT;
    if(w->stage)return RADAR_SCENE_E_STATE;
    if(!s->valid||s->error||s->count>RADAR_SPATIAL_MAX_POINTS||s->session!=c->frame_id||
       !c->cfg_sha256[0]||!c->calibration_id[0]||s->timestamp_us!=c->timestamp_us||
       strncmp(s->calibration_id,c->calibration_id,sizeof s->calibration_id)!=0){
        radar_scene_reset(p,RADAR_SCENE_RESET_CONTEXT);
        w->result.valid=0U;
        return RADAR_SCENE_E_IDENTITY;
    }
    if(w->have_context&&different(&w->context,c)){
        reason=w->context.installation_epoch!=c->installation_epoch?
            RADAR_SCENE_RESET_INSTALLATION:RADAR_SCENE_RESET_CONTEXT;radar_scene_reset(p,reason);
    }
    if(w->have_context&&(c->frame_id<=w->context.frame_id||
       (w->have_time&&c->time_valid&&c->timestamp_us<=w->last_time_us))){
        radar_scene_reset(p,RADAR_SCENE_RESET_TIME);return RADAR_SCENE_E_TIME;
    }
    /* A long observation gap cannot preserve category evidence or claim a
     * continuously observed map. New data can immediately create new tracks. */
    if(!c->time_valid||!(c->frame_flags&RADAR_FRAME_TIME_TRUSTED)||
       (w->have_time&&c->timestamp_us-w->last_time_us>EXPIRE_US))
        radar_scene_reset(p,RADAR_SCENE_RESET_TIME);
    w->context=*c;w->have_context=1U;w->source=s;
    memset(&w->result,0,sizeof w->result);w->result.valid=1U;
    w->result.session=s->session;w->result.frame_id=c->frame_id;
    w->result.capture_epoch=c->capture_epoch;w->result.installation_epoch=c->installation_epoch;
    w->result.timestamp_us=c->timestamp_us;w->result.calibrated=s->calibrated;
    memcpy(w->result.calibration_id,s->calibration_id,sizeof s->calibration_id);
    w->result.raw_points=s->count;w->result.input_truncated_count=s->truncated_count;
    w->result.rule_version=1U;w->result.reset_reason=w->reset_reason;
    w->result.diagnostics.version=1U;
    w->result.diagnostics.required_buckets=28U;
    if(w->reset_reason)w->result.flags|=RADAR_SCENE_MAP_RESET;
    if(s->truncated_count)w->result.flags|=RADAR_SCENE_INPUT_TRUNCATED;
    if(!c->time_valid||!(c->frame_flags&RADAR_FRAME_TIME_TRUSTED)){
        w->result.flags|=RADAR_SCENE_TIME_UNTRUSTED;
        w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_TIME;
        w->stage=5U;return RADAR_SCENE_BUSY;}
    if((c->frame_flags&(RADAR_FRAME_HSI_VALID|RADAR_FRAME_CRC_VALID))!=
       (RADAR_FRAME_HSI_VALID|RADAR_FRAME_CRC_VALID)||
       (c->frame_flags&(RADAR_FRAME_CLIPPED|RADAR_FRAME_DISCONTINUITY|RADAR_FRAME_DIAGNOSTIC_PACKED))){
        RadarSceneFrame saved=w->result;radar_scene_reset(p,RADAR_SCENE_RESET_CONTEXT);
        w->result=saved;w->result.diagnostics.reject_flags|=(c->frame_flags&RADAR_FRAME_CLIPPED)?
            RADAR_SCENE_REJECT_CLIPPED:RADAR_SCENE_REJECT_INTEGRITY;
        w->stage=5U;return RADAR_SCENE_BUSY;}
    absolute=c->timestamp_us/BUCKET_US;w->clear_buckets=0;
    if(!w->have_time){w->map_start_us=c->timestamp_us;w->last_bucket=absolute;}
    advance=absolute-w->last_bucket;
    for(k=1U;k<=advance&&k<=BUCKETS;++k)w->clear_buckets|=UINT64_C(1)<<((w->last_bucket+k)%BUCKETS);
    w->valid_buckets&=~w->clear_buckets;w->active_bucket=(unsigned)(absolute%BUCKETS);
    w->valid_buckets|=UINT64_C(1)<<w->active_bucket;
    w->last_bucket=absolute;w->last_time_us=c->timestamp_us;w->have_time=1U;
    w->result.map_observed_buckets=popcount64(w->valid_buckets);w->result.map_required_buckets=28U;
    w->result.diagnostics.valid_buckets=w->result.map_observed_buckets;
    w->result.diagnostics.map_elapsed_ms=(uint32_t)((c->timestamp_us-w->map_start_us)/1000U);
    if(c->timestamp_us-w->map_start_us<MAP_US||w->result.map_observed_buckets<28U)
        {w->result.flags|=RADAR_SCENE_MAP_WARMUP;
         w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_WARMUP;}
    w->stage=1U;w->row=0;w->reflection_count=0;w->reflection_seen=0;
    w->prior_persistent=0;w->new_supported=0;w->fit_trial=0;w->fit_best_n=0;
    w->candidate_count=0;w->anchor_count=0;w->guard_cursor=0;
    return RADAR_SCENE_BUSY;
}
static int reliable(const RadarSpatialPoint *q)
{
    return (q->flags&RADAR_POINT_XYZ_VALID)&&!(q->flags&(RADAR_POINT_LOW_COHERENCE|
        RADAR_POINT_ANGLE_EDGE|RADAR_POINT_DOPPLER_EDGE))&&isfinite(q->x_m)&&isfinite(q->y_m)&&
        isfinite(q->z_m)&&isfinite(q->snr_db)&&isfinite(q->coherence)&&
        q->coherence>=0.5f&&q->y_m>=0.0f&&SQ(q->x_m)+SQ(q->y_m)+SQ(q->z_m)<=81.01f;
}
static void cluster_points(Work *w)
{
    const RadarSpatialFrame *s=w->source;unsigned n=s->count<RADAR_SPATIAL_MAX_POINTS?s->count:RADAR_SPATIAL_MAX_POINTS;
    unsigned usable[32],assigned[32]={0},i,j,count=0;
    w->cluster_count=0;
    for(i=0;i<n;++i){const RadarSpatialPoint *q=&s->points[i];
        if(reliable(q)){usable[count++]=i;continue;}
        ++w->result.rejected_points;
        if(((q->flags&RADAR_POINT_BEARING_VALID)||((q->flags&RADAR_POINT_AZ_VALID)&&
           !(q->flags&(RADAR_POINT_LOW_COHERENCE|RADAR_POINT_ANGLE_EDGE|RADAR_POINT_DOPPLER_EDGE))))&&
           isfinite(q->range_m)&&q->range_m>0.0f&&q->range_m<=9.0f&&
           isfinite(q->horizontal_angle_deg)&&fabsf(q->horizontal_angle_deg)<=60.0f&&
           w->result.bearing_count<RADAR_SCENE_MAX_BEARINGS){
            RadarSceneBearing *b=&w->result.bearings[w->result.bearing_count++];
            b->range_m=q->range_m;b->horizontal_angle_deg=q->horizontal_angle_deg;
            b->snr_db=q->snr_db;b->flags=q->flags;
        }
    }
    if(w->result.rejected_points)w->result.flags|=RADAR_SCENE_QUALITY_REJECTED;
    /* DBSCAN minPts=2 including self: every connected pair is a core pair;
     * transitive expansion is exact for this chosen minimum. */
    for(i=0;i<count;++i)if(!assigned[i]){
        unsigned queue[32],head=0,tail=1;float sx=0,sy=0,sv=0,sv2=0,snr=0,extent=0;
        const RadarSpatialPoint *q=&s->points[usable[i]];Cluster *c;
        queue[0]=i;assigned[i]=1U;
        while(head<tail){unsigned a=queue[head++];const RadarSpatialPoint *p=&s->points[usable[a]];
            for(j=0;j<count;++j)if(!assigned[j]){const RadarSpatialPoint *b=&s->points[usable[j]];
                if(SQ(p->x_m-b->x_m)+SQ(p->y_m-b->y_m)<=0.35f*0.35f){assigned[j]=1U;queue[tail++]=j;}}
        }
        if(tail<2U&&(q->snr_db<15.0f||q->coherence<0.65f))continue;
        for(j=0;j<tail;++j){const RadarSpatialPoint *a=&s->points[usable[queue[j]]];unsigned k;
            sx+=a->x_m;sy+=a->y_m;snr+=a->snr_db;
            if(isfinite(a->velocity_mps)){sv+=a->velocity_mps;sv2+=SQ(a->velocity_mps);}
            for(k=0;k<j;++k){const RadarSpatialPoint *b=&s->points[usable[queue[k]]];
                float d=sqrtf(SQ(a->x_m-b->x_m)+SQ(a->y_m-b->y_m));if(d>extent)extent=d;}
        }
        c=&w->clusters[w->cluster_count++];c->x=sx/(float)tail;c->y=sy/(float)tail;
        c->snr=snr/(float)tail;c->extent=extent;c->points=tail;c->strong=tail==1U;
        c->radial_spread=sqrtf(fmaxf(0.0f,sv2/(float)tail-SQ(sv/(float)tail)));
    }
}
static void predict(Track *t,uint64_t now)
{
    float dt=(float)((double)(now-t->state_time_us)*1e-6),f[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    float a[4][4]={{0}},b[4][4]={{0}},q=0.8f;unsigned i,j,k;
    f[0][2]=dt;f[1][3]=dt;t->state[0]+=dt*t->state[2];t->state[1]+=dt*t->state[3];
    for(i=0;i<4U;++i)for(j=0;j<4U;++j)for(k=0;k<4U;++k)a[i][j]+=f[i][k]*t->cov[k][j];
    for(i=0;i<4U;++i)for(j=0;j<4U;++j)for(k=0;k<4U;++k)b[i][j]+=a[i][k]*f[j][k];
    for(i=0;i<2U;++i){b[i][i]+=q*dt*dt*dt*dt*0.25f;b[i][i+2U]+=q*dt*dt*dt*0.5f;
        b[i+2U][i]+=q*dt*dt*dt*0.5f;b[i+2U][i+2U]+=q*dt*dt;}
    memcpy(t->cov,b,sizeof b);t->state_time_us=now;
}
static float association_cost(const Track *t,const Cluster *c)
{
    float dx=c->x-t->state[0],dy=c->y-t->state[1],a=t->cov[0][0]+0.0144f;
    float b=t->cov[0][1],d=t->cov[1][1]+0.0144f,det=a*d-b*b,mahal;
    if(det<=0.0f||SQ(dx)+SQ(dy)>SQ(0.85f))return 1e9f;
    mahal=(d*dx*dx-2.0f*b*dx*dy+a*dy*dy)/det;
    return mahal<=9.21f?mahal:1e9f;
}
static void update(Track *t,const Cluster *c,uint64_t now)
{
    float a=t->cov[0][0]+0.0144f,b=t->cov[0][1],d=t->cov[1][1]+0.0144f,det=a*d-b*b;
    float gain[4][2],old[4][4],dx=c->x-t->state[0],dy=c->y-t->state[1];unsigned i,j;
    memcpy(old,t->cov,sizeof old);
    for(i=0;i<4U;++i){gain[i][0]=(old[i][0]*d-old[i][1]*b)/det;
        gain[i][1]=(-old[i][0]*b+old[i][1]*a)/det;
        t->state[i]+=gain[i][0]*dx+gain[i][1]*dy;}
    for(i=0;i<4U;++i)for(j=0;j<4U;++j)t->cov[i][j]=old[i][j]-gain[i][0]*old[0][j]-gain[i][1]*old[1][j];
    t->hits5|=1U;++t->total_hits;t->out.last_measured_us=now;t->out.measured=1U;
    t->out.point_count=c->points;t->out.extent_m=c->extent;t->out.snr_db=c->snr;
    if(popcount64(t->hits5)>=3U)t->out.confirmed=1U;
    /* Real measurements only. A missed observation invalidates semantic
     * recurrence; extrapolation never contributes a trail or rule hit. */
    if(t->total_hits==1U){t->extent_mean=c->extent;t->extent_deviation=0;}
    else {float dev=fabsf(c->extent-t->extent_mean);
        t->extent_mean+=0.2f*(c->extent-t->extent_mean);
        t->extent_deviation+=0.2f*(dev-t->extent_deviation);}
    t->radial_spread_mean+=0.2f*(c->radial_spread-t->radial_spread_mean);
    t->last_feature_us=now;
    while(t->out.trail_count&&now-t->out.trail[0].timestamp_us>TRAIL_US){
        --t->out.trail_count;memmove(t->out.trail,t->out.trail+1,t->out.trail_count*sizeof t->out.trail[0]);}
    if(t->out.trail_count==RADAR_SCENE_TRAIL_POINTS){--t->out.trail_count;
        memmove(t->out.trail,t->out.trail+1,t->out.trail_count*sizeof t->out.trail[0]);}
    {RadarSceneTrailPoint *p=&t->out.trail[t->out.trail_count++];p->x_m=c->x;p->y_m=c->y;p->timestamp_us=now;}
}
static void classify(Track *t,uint64_t now)
{
    unsigned bits=0;float speed=sqrtf(SQ(t->state[2])+SQ(t->state[3]));
    t->out.label=RADAR_SCENE_UNKNOWN;t->out.rule_bits=0;t->out.rule_score=0;
    t->out.shape_variation_m=t->extent_deviation;t->out.radial_spread_mps=t->radial_spread_mean;
    t->out.motion_speed_mps=speed;
    if(!t->out.measured){t->motion_hits=0;t->nonrigid_hits=0;return;}
    if(t->out.extent_m>=0.2f&&t->out.extent_m<=1.5f)bits|=RADAR_SCENE_RULE_EXTENT;
    /* Speed comes from timestamped XY measurements and the CV filter, never
     * from a fractional interpretation of the coarse Doppler bin width. */
    if(speed>=0.1f&&speed<=2.0f)++t->motion_hits;else t->motion_hits=0;
    if(t->motion_hits>=3U)bits|=RADAR_SCENE_RULE_MOTION;
    if(t->out.confirmed&&now-t->out.first_measured_us>=TRAIL_US&&t->total_hits>=10U)
        bits|=RADAR_SCENE_RULE_PERSISTENCE;
    if(t->out.point_count>=3U&&t->extent_deviation>=0.04f&&t->radial_spread_mean>=0.2f)
        ++t->nonrigid_hits;else t->nonrigid_hits=0;
    if(t->nonrigid_hits>=3U)bits|=RADAR_SCENE_RULE_NONRIGID;
    t->out.rule_bits=bits;t->out.rule_score=popcount4(bits);
    if(!t->out.confirmed)return;
    if(bits&RADAR_SCENE_RULE_MOTION)t->out.label=RADAR_SCENE_MOVING_OBJECT;
    else if((bits&RADAR_SCENE_RULE_PERSISTENCE)&&speed<0.1f)t->out.label=RADAR_SCENE_STATIC_REFLECTOR;
    if((bits&(RADAR_SCENE_RULE_MOTION|RADAR_SCENE_RULE_NONRIGID|RADAR_SCENE_RULE_PERSISTENCE))==
       (RADAR_SCENE_RULE_MOTION|RADAR_SCENE_RULE_NONRIGID|RADAR_SCENE_RULE_PERSISTENCE)&&popcount4(bits)>=3U)
        t->out.label=RADAR_SCENE_HUMAN_CANDIDATE;
}
static void track_frame(Work *w)
{
    unsigned i,j,n=0,indices[8],assigned[32]={0},matched[8]={0};
    float costs[8][32];uint64_t now=w->context.timestamp_us;
    cluster_points(w);
    for(i=0;i<RADAR_SCENE_MAX_TRACKS;++i){Track *t=&w->tracks[i];if(!t->alive)continue;
        if(now-t->out.last_measured_us>EXPIRE_US){memset(t,0,sizeof *t);continue;}
        t->hits5=(t->hits5<<1U)&31U;t->out.measured=0;predict(t,now);indices[n++]=i;
    }
    for(i=0;i<n;++i)for(j=0;j<w->cluster_count;++j)costs[i][j]=association_cost(&w->tracks[indices[i]],&w->clusters[j]);
    /* Global minimum assignment is deterministic. Ambiguous two-track cells
     * deliberately do not renew human evidence (no forced class identity). */
    for(i=0;i<n;++i){float best=1e9f;unsigned ti=8U,ci=32U,a,b;
        for(a=0;a<n;++a)if(!matched[a])for(b=0;b<w->cluster_count;++b)
            if(!assigned[b]&&costs[a][b]<best){best=costs[a][b];ti=a;ci=b;}
        if(ti==8U)break;
        matched[ti]=1U;assigned[ci]=1U;update(&w->tracks[indices[ti]],&w->clusters[ci],now);
        for(a=0;a<n;++a)if(a!=ti&&costs[a][ci]<best+0.5f){
            w->tracks[indices[ti]].nonrigid_hits=0;w->tracks[indices[ti]].extent_deviation=0;
            w->result.flags|=RADAR_SCENE_AMBIGUOUS_ASSOCIATION;
        }
    }
    for(j=0;j<w->cluster_count;++j)if(!assigned[j]){
        Track *t=NULL;
        for(i=0;i<8U;++i)if(!w->tracks[i].alive){t=&w->tracks[i];break;}
        if(!t){++w->result.omitted_tracks;w->result.flags|=RADAR_SCENE_CAPACITY;continue;}
        memset(t,0,sizeof *t);t->alive=1U;t->out.id=w->next_id++;
        if(!w->next_id)w->next_id=1U;
        t->state[0]=w->clusters[j].x;t->state[1]=w->clusters[j].y;
        t->cov[0][0]=t->cov[1][1]=0.0144f;t->cov[2][2]=t->cov[3][3]=0.8f;
        t->state_time_us=now;t->out.first_measured_us=now;update(t,&w->clusters[j],now);
    }
    for(i=0;i<8U;++i){Track *t=&w->tracks[i];if(!t->alive)continue;
        t->out.predicted=!t->out.measured;t->out.x_m=t->state[0];t->out.y_m=t->state[1];
        t->out.vx_mps=t->state[2];t->out.vy_mps=t->state[3];
        t->out.position_variance_m2=fmaxf(t->cov[0][0],t->cov[1][1]);
        while(t->out.trail_count&&now-t->out.trail[0].timestamp_us>TRAIL_US){--t->out.trail_count;
            memmove(t->out.trail,t->out.trail+1,t->out.trail_count*sizeof t->out.trail[0]);}
        classify(t,now);w->result.tracks[w->result.track_count++]=t->out;
        if(t->out.confirmed)++w->result.confirmed_count;
    }
}
static int excluded_motion(const Work *w,float x,float y)
{
    unsigned i;for(i=0;i<8U;++i){const Track *t=&w->tracks[i];float rad;
        if(!t->alive||!t->out.confirmed||SQ(t->state[2])+SQ(t->state[3])<0.01f)continue;
        rad=0.5f+t->out.extent_m*0.5f;
        if(SQ(x-t->state[0])+SQ(y-t->state[1])<rad*rad)return 1;
    }return 0;
}
static int reflection_evidence(const Work *w,unsigned r,unsigned a)
{
    const RadarSpatialFrame *s=w->source;float db=s->heatmap_dbfs[r][a],before,after;
    if(r<RADAR_RANGE_FIRST_BIN||r+3U>=RADAR_SPATIAL_MAP_RANGES||!isfinite(db)||db< -75.0f)return 0;
    before=s->heatmap_dbfs[r-3U][a];after=s->heatmap_dbfs[r+3U][a];
    if(!isfinite(before)||!isfinite(after))return 0;
    /* Fixed evidence floor plus radial contrast; uses the full map, never
     * sparse CFAR points or a per-frame brightness stretch. */
    return db>=before+4.0f&&db>=after+4.0f;
}
static void map_row(Work *w,unsigned r)
{
    unsigned a,b,observed=w->result.map_observed_buckets;
    unsigned best_angle=0U,have=0U;float best_power=-INFINITY;
    /* Expired bucket bits are independent between cells. Clear only the
     * selected buckets for this complete row before applying fresh evidence;
     * this preserves every cell's old/support state and processing order. */
    {uint64_t pending=w->clear_buckets;
        while(pending){b=(unsigned)__builtin_ctzll(pending);pending&=pending-1U;
            for(a=0;a<RADAR_SPATIAL_MAP_ANGLES;++a){unsigned k=r*RADAR_SPATIAL_MAP_ANGLES+a;
                if(has_bit(w->buckets[b],k)){
                    unset_bit(w->buckets[b],k);if(w->support[k])--w->support[k];}
            }
        }
    }
    for(a=0;a<RADAR_SPATIAL_MAP_ANGLES;++a){unsigned k=r*RADAR_SPATIAL_MAP_ANGLES+a,old;
        float range=(float)r*(float)RADAR_RANGE_BIN_METERS,x=range*w->angle_sin[a],y=range*w->angle_cos[a];
        int fresh;
        old=w->support[k];fresh=reflection_evidence(w,r,a)&&!excluded_motion(w,x,y);
        if(fresh)++w->result.diagnostics.radial_candidates;
        if(old*10U>=observed*7U&&old>=20U){++w->prior_persistent;if(fresh)++w->new_supported;}
        if(fresh&&!has_bit(w->buckets[w->active_bucket],k)){
            set_bit(w->buckets[w->active_bucket],k);++w->support[k];}
        /* Current evidence is also required: a departed wall candidate cannot
         * coast for the ten-second learning window without observations. */
        if(!(w->result.flags&RADAR_SCENE_MAP_WARMUP)&&fresh&&w->support[k]*10U>=observed*7U){
            unsigned pos=w->reflection_seen++;Reflection *p;
            ++w->result.diagnostics.persistent_cells;
            if(!have||w->source->heatmap_dbfs[r][a]>best_power){
                have=1U;best_power=w->source->heatmap_dbfs[r][a];best_angle=a;}
            if(pos>=FIT_POINTS){unsigned hash=(pos*2654435761U)^k;
                pos=hash%(w->reflection_seen);if(pos>=FIT_POINTS)continue;}
            else ++w->reflection_count;
            p=&w->reflections[pos];p->x=x;p->y=y;p->support=(float)w->support[k]/(float)observed;
            p->cell=k;p->used=0U;
        }
    }
    /* One strongest anchor per radial row: the 1-degree picture columns never
     * create independent support. Keep a bounded deterministic power order. */
    if(have){SurfaceAnchor q;unsigned i=w->candidate_count;
        float range=(float)r*(float)RADAR_RANGE_BIN_METERS;
        q.range=r;q.angle=best_angle;q.u=w->angle_sin[best_angle];
        q.x=range*q.u;q.y=range*w->angle_cos[best_angle];q.power=powf(10.0f,best_power*0.1f);
        if(i==SURFACE_CANDIDATES){++w->result.diagnostics.candidate_overflow;
            w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_CAPACITY;
            if(q.power<=w->candidates[i-1U].power)return;
            --i;}
        else ++w->candidate_count;
        while(i&&w->candidates[i-1U].power<q.power){w->candidates[i]=w->candidates[i-1U];--i;}
        w->candidates[i]=q;
    }
}
static void guard_one(Work *w)
{
    const SurfaceAnchor *q=&w->candidates[w->guard_cursor++];unsigned i;
    for(i=0;i<w->anchor_count;++i){const SurfaceAnchor *p=&w->anchors[i];
        unsigned dr=q->range>p->range?q->range-p->range:p->range-q->range;
        unsigned du=(unsigned)(fabsf(q->u-p->u)*RADAR_REFLECTION_U_LOOKUP_SCALE);
        float guard,ratio;
        if(dr<=2U){++w->result.diagnostics.point_like_groups;return;}
        if(du>2048U)du=2048U;
        guard=fmaxf(RADAR_REFLECTION_GUARD_FLOOR,RADAR_REFLECTION_GUARD_FACTOR*
            radar_reflection_range_envelope[dr]*radar_reflection_u_envelope[du]);
        ratio=q->power/(p->power*guard);
        if(ratio<=1.0f){++w->result.diagnostics.point_like_groups;
            w->result.diagnostics.best_single_point_ratio=fmaxf(w->result.diagnostics.best_single_point_ratio,ratio);
            return;}
    }
    w->anchors[w->anchor_count++]=*q;
    w->result.diagnostics.independent_groups=w->anchor_count;
}
static void fit_trial(Work *w)
{
    unsigned n=w->reflection_count,trial=w->fit_trial++,a,b,i,count=0;
    float dx,dy,len,nx,ny,c;
    ++w->result.diagnostics.fit_trials;
    if(n<6U)return;
    a=(trial*37U+11U)%n;b=(trial*71U+n/2U+3U)%n;
    if(a==b||w->reflections[a].used||w->reflections[b].used)return;
    dx=w->reflections[b].x-w->reflections[a].x;dy=w->reflections[b].y-w->reflections[a].y;
    len=sqrtf(dx*dx+dy*dy);if(len<0.8f)return;
    nx=-dy/len;ny=dx/len;c=nx*w->reflections[a].x+ny*w->reflections[a].y;
    for(i=0;i<n;++i)if(!w->reflections[i].used&&fabsf(nx*w->reflections[i].x+ny*w->reflections[i].y-c)<=0.15f)++count;
    if(count>w->fit_best_n){w->fit_best_n=count;w->fit_best_nx=nx;w->fit_best_ny=ny;w->fit_best_c=c;}
}
static int fit_finish(Work *w)
{
    unsigned i,n=0;float sx=0,sy=0,xx=0,xy=0,yy=0,nx=w->fit_best_nx,ny=w->fit_best_ny,c=w->fit_best_c;
    float angle,tx,ty,lo=1e9f,hi=-1e9f,error=0,support=0,along_sorted[FIT_POINTS];
    if(w->fit_best_n<6U){w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_SUPPORT;return 0;}
    for(i=0;i<w->reflection_count;++i){Reflection *p=&w->reflections[i];
        if(!p->used&&fabsf(nx*p->x+ny*p->y-c)<=0.15f){sx+=p->x;sy+=p->y;++n;}}
    sx/=(float)n;sy/=(float)n;
    for(i=0;i<w->reflection_count;++i){Reflection *p=&w->reflections[i];
        if(!p->used&&fabsf(nx*p->x+ny*p->y-c)<=0.15f){xx+=SQ(p->x-sx);xy+=(p->x-sx)*(p->y-sy);yy+=SQ(p->y-sy);}}
    angle=0.5f*atan2f(2.0f*xy,xx-yy);tx=cosf(angle);ty=sinf(angle);nx=-ty;ny=tx;c=nx*sx+ny*sy;n=0;
    for(i=0;i<w->reflection_count;++i){Reflection *p=&w->reflections[i];float e=nx*p->x+ny*p->y-c;
        if(!p->used&&fabsf(e)<=0.15f){float along=tx*p->x+ty*p->y;
            unsigned j=n;while(j&&along_sorted[j-1U]>along){along_sorted[j]=along_sorted[j-1U];--j;}
            along_sorted[j]=along;
            lo=fminf(lo,along);hi=fmaxf(hi,along);error+=e*e;support+=p->support;++n;}}
    w->result.diagnostics.best_support_cells=n;
    w->result.diagnostics.best_span_m=hi-lo;
    w->result.diagnostics.best_residual_m=n?sqrtf(error/(float)n):INFINITY;
    if(n<6U){w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_SUPPORT;return 0;}
    if(hi-lo<1.0f){w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_SPAN;return 0;}
    if(sqrtf(error/(float)n)>0.15f){w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_RESIDUAL;return 0;}
    /* A dashed segment must not bridge a large unobserved opening. Keep the
     * longest contiguous measured support run; other runs remain available to
     * later fits instead of connecting separated walls across free space. */
    {unsigned first=0,best_first=0,best_last=0,j;
        for(j=1;j<=n;++j)if(j==n||along_sorted[j]-along_sorted[j-1U]>0.35f){
            if(along_sorted[j-1U]-along_sorted[first]>along_sorted[best_last]-along_sorted[best_first]){
                best_first=first;best_last=j-1U;}first=j;}
        lo=along_sorted[best_first];hi=along_sorted[best_last];
        if(hi-lo<1.0f){w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_GAP;return 0;}
        n=0;error=0;support=0;
        for(j=0;j<w->reflection_count;++j){Reflection *p=&w->reflections[j];float e=nx*p->x+ny*p->y-c,along=tx*p->x+ty*p->y;
            if(!p->used&&fabsf(e)<=0.15f&&along>=lo&&along<=hi){++n;error+=e*e;support+=p->support;}}
        if(n<6U){w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_SUPPORT;return 0;}
    }
    /* Reject a line spanning an unobserved gap: occupancy in 0.25m intervals
     * must be >=70%; this does not fill missing portions or complete rooms. */
    {uint64_t occupied=0;unsigned bins=(unsigned)((hi-lo)/0.25f)+1U;
        if(bins>64U)return 0;
        for(i=0;i<w->reflection_count;++i){Reflection *p=&w->reflections[i];
            float along=tx*p->x+ty*p->y;
            if(!p->used&&fabsf(nx*p->x+ny*p->y-c)<=0.15f&&along>=lo&&along<=hi){
                unsigned b=(unsigned)limit((along-lo)/0.25f,0.0f,(float)(bins-1U));occupied|=UINT64_C(1)<<b;}}
        w->result.diagnostics.best_occupancy=(float)popcount64(occupied)/(float)bins;
        if(popcount64(occupied)*10U<bins*7U){w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_OCCUPANCY;return 0;}
    }
    /* Independent anchor centers, not inflated support-pixel counts, must lie
     * on this actual observed segment. No extrapolation across unknown space. */
    {unsigned groups=0U;
        for(i=0;i<w->anchor_count;++i){const SurfaceAnchor *a=&w->anchors[i];float along=tx*a->x+ty*a->y;
            if(fabsf(nx*a->x+ny*a->y-c)<=0.15f&&along>=lo&&along<=hi)++groups;}
        w->result.diagnostics.best_support_groups=groups;
        if(groups<3U){w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_POINT_RESPONSE;return 0;}}
    w->result.diagnostics.best_span_m=hi-lo;
    w->result.diagnostics.best_temporal_support=support/(float)n;
    w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_PROJECTION_ONLY;
    {RadarSceneWall *wall=&w->result.walls[w->result.wall_count++];
        wall->x0_m=nx*c+tx*lo;wall->y0_m=ny*c+ty*lo;wall->x1_m=nx*c+tx*hi;wall->y1_m=ny*c+ty*hi;
        wall->residual_m=sqrtf(error/(float)n);wall->support_fraction=support/(float)n;
        wall->cells=n;wall->latest_observed_us=w->context.timestamp_us;}
    for(i=0;i<w->reflection_count;++i){Reflection *p=&w->reflections[i];
        float along=tx*p->x+ty*p->y;
        if(fabsf(nx*p->x+ny*p->y-c)<=0.15f&&along>=lo&&along<=hi)p->used=1U;}
    return 1;
}
int radar_scene_step(RadarSceneProcessor *p)
{
    Work *w=work(p);if(!w)return RADAR_SCENE_E_ARGUMENT;
    switch(w->stage){
    case 1U:
        track_frame(w);
        radar_reflection_update(&w->stable_reflections,w->source,&w->context,&w->result);
        w->stage=2U;return RADAR_SCENE_BUSY;
    case 2U:
        map_row(w,w->row++);
        if(w->row==RADAR_SPATIAL_MAP_RANGES){
            /* Broad loss of an established reflection map is only a heuristic
             * for installation changes; not a motion sensor guarantee. */
            if(w->prior_persistent>=64U&&w->new_supported*10U<w->prior_persistent*3U)++w->changed_frames;
            else w->changed_frames=0;
            if(w->changed_frames>=3U){
                RadarSceneFrame saved=w->result;radar_scene_reset(p,RADAR_SCENE_RESET_MAP_CHANGE);
                w->result=saved;w->result.track_count=0;w->result.confirmed_count=0;
                w->result.reflection_count=0;
                memset(w->result.reflections,0,sizeof w->result.reflections);
                w->result.diagnostics.stable_count=0U;w->result.diagnostics.tentative_count=0U;
                w->result.diagnostics.stale_count=0U;w->result.diagnostics.ambiguous_count=0U;
                w->result.diagnostics.range_only_count=0U;
                w->result.flags|=RADAR_SCENE_MAP_RESET|RADAR_SCENE_MAP_WARMUP;
                w->result.reset_reason=RADAR_SCENE_RESET_MAP_CHANGE;return RADAR_SCENE_DONE;
            }
            if(!w->result.diagnostics.radial_candidates)w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_NO_FRESH;
            w->stage=(w->result.flags&RADAR_SCENE_MAP_WARMUP)||w->reflection_count<6U||
                w->result.diagnostics.candidate_overflow?5U:6U;
            if(w->reflection_count<6U)w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_SUPPORT;
        }return RADAR_SCENE_BUSY;
    case 6U:
        if(w->guard_cursor<w->candidate_count){guard_one(w);return RADAR_SCENE_BUSY;}
        if(w->anchor_count<3U){w->result.diagnostics.reject_flags|=RADAR_SCENE_REJECT_POINT_RESPONSE;w->stage=5U;}
        else w->stage=3U;
        return RADAR_SCENE_BUSY;
    case 3U:fit_trial(w);if(w->fit_trial==FIT_TRIALS)w->stage=4U;return RADAR_SCENE_BUSY;
    case 4U:
        if(fit_finish(w)&&w->result.wall_count<RADAR_SCENE_MAX_WALLS){w->fit_trial=0;w->fit_best_n=0;w->stage=3U;}
        else w->stage=5U;
        return RADAR_SCENE_BUSY;
    case 5U:w->stage=0;w->source=NULL;w->reset_reason=0;return RADAR_SCENE_DONE;
    default:return RADAR_SCENE_E_STATE;
    }
}
void radar_scene_cancel(RadarSceneProcessor *p){radar_scene_reset(p,RADAR_SCENE_RESET_CANCEL);}
const RadarSceneFrame *radar_scene_result(const RadarSceneProcessor *p)
{
    const Work *w=p?(const Work *)p->workspace:NULL;return w&&!w->stage&&w->result.valid?&w->result:NULL;
}
