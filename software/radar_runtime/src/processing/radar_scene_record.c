/* SPDX-License-Identifier: MIT */
#include "radar_scene_record.h"
#include "radar_crc32.h"
#include <string.h>
#include <math.h>

_Static_assert(sizeof(float)==4U,"record IEEE binary32 contract");
_Static_assert(sizeof(RadarSpatialPoint)==56U,"record point contract");
_Static_assert(sizeof(RadarSceneDiagnostics)==128U,"record diagnostics contract");
_Static_assert(RADAR_RECORD_CAPACITY*RADAR_RECORD_TIMING_BYTES==RADAR_RECORD_FRAMES_OFFSET,"timing bound");
_Static_assert(RADAR_RECORD_FRAMES_OFFSET+RADAR_RECORD_CAPACITY*RADAR_RECORD_FRAME_BYTES==RADAR_RECORD_META_OFFSET,"frame bound");
_Static_assert(RADAR_RECORD_META_OFFSET+RADAR_RECORD_HEADER_BYTES+3U*RADAR_RECORD_HEAT_BYTES<=RADAR_RECORD_REGION_BYTES,"heat bound");

static void u32(uint8_t *p,uint32_t v)
{p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);p[2]=(uint8_t)(v>>16U);p[3]=(uint8_t)(v>>24U);}
static void u64(uint8_t *p,uint64_t v){u32(p,(uint32_t)v);u32(p+4,(uint32_t)(v>>32U));}
static void f32(uint8_t *p,float v){uint32_t w;memcpy(&w,&v,4);u32(p,w);}
static uint32_t age_ms(uint64_t now,uint64_t previous)
{uint64_t d;if(!previous || now<previous)return UINT32_MAX;d=(now-previous)/1000U;return d>UINT32_MAX?UINT32_MAX:(uint32_t)d;}
static uint8_t *header(RadarSceneRecorder *r){return r->region+RADAR_RECORD_META_OFFSET;}
static uint8_t *frame(RadarSceneRecorder *r,uint32_t i)
{return r->region+RADAR_RECORD_FRAMES_OFFSET+i*RADAR_RECORD_FRAME_BYTES;}
static uint8_t *heat(RadarSceneRecorder *r,uint32_t i)
{return header(r)+RADAR_RECORD_HEADER_BYTES+i*RADAR_RECORD_HEAT_BYTES;}
static int text_field(uint8_t *p,size_t n,const char *text)
{size_t z;if(!text)return -1;z=strlen(text);if(z>=n)return -1;memcpy(p,text,z);return 0;}
static int bad(RadarSceneRecorder *r,int error)
{if(!r->error)r->error=error;r->stop_due=1U;r->incomplete=1U;return error;}
void radar_scene_record_init(RadarSceneRecorder *r,void *memory,size_t bytes)
{memset(r,0,sizeof *r);if(memory && bytes>=RADAR_RECORD_REGION_BYTES)r->region=memory;}
int radar_scene_record_holds(const RadarSceneRecorder *r){return r && r->state!=RADAR_RECORD_EMPTY;}
int radar_scene_record_arm(RadarSceneRecorder *r,uint32_t seconds,const RadarRecordIdentity *id)
{
    uint8_t *h;uint8_t *base;
    if(!r || !r->region || !id || (seconds!=60U && seconds!=180U))return RADAR_RECORD_E_ARGUMENT;
    if(r->state!=RADAR_RECORD_EMPTY)return RADAR_RECORD_E_STATE;
    base=r->region;memset(r,0,sizeof *r);r->region=base;h=header(r);memset(h,0,RADAR_RECORD_HEADER_BYTES);
    memcpy(h,"RSCENE01",8);u32(h+8,1U);u32(h+12,RADAR_RECORD_HEADER_BYTES);
    u32(h+16,RADAR_RECORD_FRAME_BYTES);u32(h+20,RADAR_RECORD_TIMING_BYTES);
    u32(h+24,RADAR_RECORD_HEAT_BYTES);u32(h+28,UINT32_C(0x01020304));
    u32(h+32,RADAR_SPATIAL_MAP_RANGES);u32(h+36,RADAR_SPATIAL_MAP_ANGLES);u32(h+40,seconds);
    u32(h+44,id->installation_epoch);
    u32(h+128,id->reflection_id_seed);
    if(text_field(h+256,64,id->software_version) || text_field(h+320,64,id->build_id) ||
       text_field(h+384,32,id->profile_id) || text_field(h+416,80,id->cfg_sha256) ||
       text_field(h+496,48,id->array_id) || text_field(h+544,64,id->calibration_id) ||
       text_field(h+608,80,id->bit_sha256) || text_field(h+688,80,id->xsa_sha256) ||
       text_field(h+768,80,id->firmware_expected_sha256) || text_field(h+848,256,id->firmware_version_observed) ||
       text_field(h+1104,1808,id->scene_parameters))return RADAR_RECORD_E_IDENTITY;
    text_field(h+2912,256,"UNCALIBRATED_IF_UNIT_COEFFICIENTS; DIAGNOSTIC_ONLY; THREE_HEATMAP_SAMPLES_NOT_COMPLETE_SCENE_REPLAY; HOST_BINDS_SEALED_ELF_SHA256");
    r->duration_s=seconds;r->installation_epoch=id->installation_epoch;
    r->heat_schedule[0]=10U;r->heat_schedule[1]=seconds==60U?30U:90U;r->heat_schedule[2]=seconds-5U;
    r->state=RADAR_RECORD_ARMED;return 0;
}
int radar_scene_record_start(RadarSceneRecorder *r,uint32_t epoch)
{if(!r || r->state!=RADAR_RECORD_ARMED || !epoch)return RADAR_RECORD_E_STATE;r->capture_epoch=epoch;r->state=RADAR_RECORD_ACTIVE;return 0;}
static void point(uint8_t *p,const RadarSpatialPoint *v)
{
    u32(p,v->range_bin);u32(p+4,(uint32_t)v->doppler_bin);
    f32(p+8,v->range_m);f32(p+12,v->velocity_mps);f32(p+16,v->azimuth_deg);f32(p+20,v->elevation_deg);
    f32(p+24,v->horizontal_angle_deg);f32(p+28,v->x_m);f32(p+32,v->y_m);f32(p+36,v->z_m);
    f32(p+40,v->snr_db);f32(p+44,v->coherence);f32(p+48,v->power_dbfs);u32(p+52,v->flags);
}
static void diagnostics(uint8_t *p,const RadarSceneDiagnostics *d)
{
    /* Existing fixed diagnostic contract:24 integers followed by8 floats.
     * Copy each word through the explicit little-endian encoder. */
    unsigned i;uint32_t word;for(i=0;i<32U;++i){memcpy(&word,(const uint8_t *)d+i*4U,4);u32(p+i*4U,word);}
}
static int commit(RadarSceneRecorder *r)
{
    uint8_t *p=frame(r,r->count);uint32_t crc;
    crc=radar_crc32_update(UINT32_MAX,p,124U);crc=radar_crc32_update(crc,p+128U,RADAR_RECORD_FRAME_BYTES-128U);
    u32(p+124,crc^UINT32_MAX);
    if(!r->count){r->first_us=r->pending_us;r->first_frame=r->pending_frame;}
    r->last_us=r->pending_us;r->last_frame=r->pending_frame;r->last_transport=r->pending_transport;
    ++r->count;r->pending_record=0U;
    if(r->last_us-r->first_us>=(uint64_t)r->duration_s*1000000U)r->stop_due=1U;
    else if(r->count>=RADAR_RECORD_CAPACITY-64U)bad(r,RADAR_RECORD_E_CAPACITY);
    return RADAR_RECORD_DONE;
}
int radar_scene_record_begin(RadarSceneRecorder *r,const RadarFrameRef *f,const RadarSpatialFrame *s,
    const RadarSceneFrame *scene,const RadarRangeFrame *range,uint32_t dsp_flags)
{
    uint8_t *p;unsigned i;uint64_t elapsed;
    if(!r || !f || !s || !scene || !range)return RADAR_RECORD_E_ARGUMENT;
    if(r->state!=RADAR_RECORD_ACTIVE && r->state!=RADAR_RECORD_DRAINING)return RADAR_RECORD_E_STATE;
    if(r->pending_record)return RADAR_RECORD_E_STATE;
    if(r->count>=RADAR_RECORD_CAPACITY)return bad(r,RADAR_RECORD_E_CAPACITY);
    if((f->flags&(RADAR_FRAME_HSI_VALID|RADAR_FRAME_CRC_VALID))!=(RADAR_FRAME_HSI_VALID|RADAR_FRAME_CRC_VALID) ||
       f->capture_epoch!=r->capture_epoch || f->frame_id!=s->session || f->frame_id!=scene->frame_id ||
       f->frame_id!=scene->session || f->frame_id!=range->session ||
       f->timestamp_us!=s->timestamp_us || f->timestamp_us!=scene->timestamp_us || f->timestamp_us!=range->timestamp_us ||
       f->capture_epoch!=scene->capture_epoch || scene->installation_epoch!=r->installation_epoch ||
       strcmp(f->cfg_sha256,(char *)header(r)+416) || strcmp(scene->calibration_id,(char *)header(r)+544) ||
       (r->count && (f->frame_id!=r->last_frame+1U || f->transport_sequence!=r->last_transport+1U)) ||
       s->count>32U || scene->track_count>8U || scene->reflection_count>8U || scene->wall_count>4U)
        return bad(r,RADAR_RECORD_E_IDENTITY);
    if(!(f->flags&RADAR_FRAME_TIME_TRUSTED) || !f->timestamp_us || f->time_error_us>1000U ||
       (r->count && (f->timestamp_us<=r->last_us || f->timestamp_us-r->last_us>500000U)))return bad(r,RADAR_RECORD_E_TIME);
    p=frame(r,r->count);memset(p,0,RADAR_RECORD_FRAME_BYTES);memcpy(p,"RSF1",4);u32(p+4,RADAR_RECORD_FRAME_BYTES);
    u32(p+8,r->count+1U);u32(p+12,f->capture_epoch);u32(p+16,f->frame_id);u32(p+20,f->transport_sequence);
    u32(p+24,f->crc32);u32(p+28,f->flags);u64(p+32,f->timestamp_us);u64(p+40,f->observed_us);
    u32(p+48,f->time_error_us);u32(p+52,r->installation_epoch);u32(p+56,s->valid);u32(p+60,s->calibrated);
    u32(p+64,s->candidate_count);u32(p+68,s->count);u32(p+72,s->truncated_count);u32(p+76,s->xyz_count);
    u32(p+80,scene->flags);u32(p+84,scene->reset_reason);u32(p+88,scene->track_count);u32(p+92,scene->reflection_count);
    u32(p+96,scene->wall_count);u32(p+100,dsp_flags);u32(p+104,scene->valid);u32(p+108,scene->rule_version);
    u64(p+112,range->hsi_session_id);u32(p+120,range->clipped_components);
    for(i=0;i<s->count;++i)point(p+128U+i*56U,&s->points[i]);
    for(i=0;i<scene->track_count;++i){const RadarSceneTrack *t=&scene->tracks[i];uint8_t *v=p+1920U+i*24U;
        u32(v,t->id);u32(v+4,(t->label&255U)|(t->confirmed?256U:0U)|(t->measured?512U:0U)|(t->predicted?1024U:0U));
        f32(v+8,t->x_m);f32(v+12,t->y_m);u32(v+16,age_ms(f->timestamp_us,t->last_measured_us));f32(v+20,t->motion_speed_mps);}
    for(i=0;i<scene->reflection_count;++i){const RadarStableReflection *t=&scene->reflections[i];uint8_t *v=p+2112U+i*24U;
        u32(v,t->id);u32(v+4,(t->state&255U)|(t->flags<<8U));f32(v+8,t->range_m);f32(v+12,t->u);
        u32(v+16,age_ms(f->timestamp_us,t->last_observed_us));f32(v+20,t->range_mad_m);}
    for(i=0;i<scene->wall_count;++i){const RadarSceneWall *w=&scene->walls[i];uint8_t *v=p+2304U+i*32U;
        f32(v,w->x0_m);f32(v+4,w->y0_m);f32(v+8,w->x1_m);f32(v+12,w->y1_m);f32(v+16,w->residual_m);
        f32(v+20,w->support_fraction);u32(v+24,w->cells);u32(v+28,age_ms(f->timestamp_us,w->latest_observed_us));}
    diagnostics(p+2432U,&scene->diagnostics);
    r->pending_record=1U;r->pending_us=f->timestamp_us;r->pending_frame=f->frame_id;r->pending_transport=f->transport_sequence;
    elapsed=r->count?f->timestamp_us-r->first_us:0U;
    i=r->heat_count+r->heat_missed;
    while(i<3U && elapsed>(uint64_t)r->heat_schedule[i]*1000000U+500000U){++r->heat_missed;++i;}
    if(i<3U && elapsed>=(uint64_t)r->heat_schedule[i]*1000000U) {
        uint8_t *h=heat(r,r->heat_count);const uint32_t data_bytes=RADAR_SPATIAL_MAP_RANGES*RADAR_SPATIAL_MAP_ANGLES*4U;
        memset(h,0,256U);memset(h+256U+data_bytes,0,RADAR_RECORD_HEAT_BYTES-256U-data_bytes);
        memcpy(h,"RSH1",4);u32(h+4,r->heat_count+1U);u32(h+8,r->count+1U);u32(h+12,f->frame_id);
        u64(h+16,f->timestamp_us);u32(h+24,RADAR_SPATIAL_MAP_RANGES);u32(h+28,RADAR_SPATIAL_MAP_ANGLES);
        u32(h+32,r->heat_schedule[i]);u32(h+36,f->crc32);u32(h+40,f->capture_epoch);
        r->heat_record_index[r->heat_count]=r->count;r->heat_source=(const uint8_t *)s->heatmap_dbfs;
        r->heat_offset=0U;r->heat_crc=UINT32_MAX;r->pending_heat=1U;return RADAR_RECORD_BUSY;
    }
    return commit(r);
}
int radar_scene_record_step(RadarSceneRecorder *r)
{
    uint32_t take,total=RADAR_SPATIAL_MAP_RANGES*RADAR_SPATIAL_MAP_ANGLES*4U;uint8_t *h;
    if(!r || !r->pending_record || !r->pending_heat)return RADAR_RECORD_E_STATE;
    h=heat(r,r->heat_count);take=total-r->heat_offset;if(take>8192U)take=8192U;
    memcpy(h+256U+r->heat_offset,r->heat_source+r->heat_offset,take);
    r->heat_crc=radar_crc32_update(r->heat_crc,h+256U+r->heat_offset,take);r->heat_offset+=take;
    if(r->heat_offset!=total)return RADAR_RECORD_BUSY;
    u32(h+44,r->heat_crc^UINT32_MAX);u32(h+252,radar_crc32_update(UINT32_MAX,h,252U)^UINT32_MAX);
    ++r->heat_count;r->pending_heat=0U;r->heat_source=NULL;return commit(r);
}
void radar_scene_record_cancel_pending(RadarSceneRecorder *r,int reason)
{if(!r || (r->state!=RADAR_RECORD_ACTIVE && r->state!=RADAR_RECORD_DRAINING))return;
 r->pending_record=r->pending_heat=0U;r->heat_source=NULL;if(reason)bad(r,reason);}
void radar_scene_record_stop_requested(RadarSceneRecorder *r,int reason)
{if(!r || r->state!=RADAR_RECORD_ACTIVE)return;r->state=RADAR_RECORD_DRAINING;if(reason)bad(r,reason);}
/* Immutable virtual image: manifest, committed frames, actual timing rows,
 * and sampled complete heatmaps. No uninitialized reservation is exported. */
static const uint8_t *segment(const RadarSceneRecorder *r,uint32_t offset,uint32_t *available)
{
    uint32_t n;if(offset<RADAR_RECORD_HEADER_BYTES){*available=RADAR_RECORD_HEADER_BYTES-offset;return r->region+RADAR_RECORD_META_OFFSET+offset;}
    offset-=RADAR_RECORD_HEADER_BYTES;n=r->count*RADAR_RECORD_FRAME_BYTES;
    if(offset<n){*available=n-offset;return r->region+RADAR_RECORD_FRAMES_OFFSET+offset;}
    offset-=n;n=r->timing_count*RADAR_RECORD_TIMING_BYTES;
    if(offset<n){*available=n-offset;return r->region+offset;}
    offset-=n;n=r->heat_count*RADAR_RECORD_HEAT_BYTES;
    if(offset<n){*available=n-offset;return r->region+RADAR_RECORD_META_OFFSET+RADAR_RECORD_HEADER_BYTES+offset;}
    *available=0U;return NULL;
}
int radar_scene_record_freeze(RadarSceneRecorder *r,uint32_t timing_count,uint32_t presented,
    uint32_t skipped,uint32_t dp_flags,uint32_t source_state,int quiet,int ack,int error)
{
    uint8_t *h;
    if(!r || (r->state!=RADAR_RECORD_ACTIVE && r->state!=RADAR_RECORD_DRAINING) ||
       r->pending_record || !quiet || !ack)return RADAR_RECORD_E_STATE;
    if(timing_count>RADAR_RECORD_CAPACITY || timing_count<r->count)return bad(r,RADAR_RECORD_E_CAPACITY);
    r->timing_count=timing_count;r->final_presented=presented;r->final_skipped=skipped;
    r->final_dp_flags=dp_flags;r->final_source_state=source_state;
    if(error)bad(r,error);
    if(!r->count || r->last_us-r->first_us<(uint64_t)r->duration_s*1000000U)r->incomplete=1U;
    if(r->heat_count!=3U)r->incomplete=1U;
    r->image_bytes=RADAR_RECORD_HEADER_BYTES+r->count*RADAR_RECORD_FRAME_BYTES+
        timing_count*RADAR_RECORD_TIMING_BYTES+r->heat_count*RADAR_RECORD_HEAT_BYTES;
    h=header(r);u32(h+48,r->capture_epoch);u32(h+52,r->count);u32(h+56,timing_count);u32(h+60,r->heat_count);
    u64(h+64,r->first_us);u64(h+72,r->last_us);u32(h+80,r->image_bytes);u32(h+84,(uint32_t)r->error);
    u32(h+88,r->incomplete);u32(h+92,presented);u32(h+96,skipped);u32(h+100,dp_flags);u32(h+104,source_state);
    u32(h+108,r->first_frame);u32(h+112,r->last_frame);u32(h+116,r->heat_missed);
    r->payload_crc=UINT32_MAX;r->hash_offset=RADAR_RECORD_HEADER_BYTES;r->freeze_stage=0U;
    r->state=RADAR_RECORD_HASHING;return 0;
}
int radar_scene_record_hash_step(RadarSceneRecorder *r)
{
    uint32_t take;const uint8_t *p;uint8_t *h;
    if(!r || r->state!=RADAR_RECORD_HASHING)return RADAR_RECORD_E_STATE;
    if(r->hash_offset<r->image_bytes){p=segment(r,r->hash_offset,&take);if(!p)return bad(r,RADAR_RECORD_E_ARGUMENT);
        if(take>4096U)take=4096U;
        r->payload_crc=radar_crc32_update(r->payload_crc,p,take);r->hash_offset+=take;return RADAR_RECORD_BUSY;}
    if(!r->freeze_stage){h=header(r);u32(h+120,r->payload_crc^UINT32_MAX);
        u32(h+4092,radar_crc32_update(UINT32_MAX,h,4092U)^UINT32_MAX);
        r->freeze_stage=1U;r->hash_offset=0U;r->payload_crc=UINT32_MAX;return RADAR_RECORD_BUSY;}
    r->image_crc=r->payload_crc^UINT32_MAX;r->state=RADAR_RECORD_FROZEN;return RADAR_RECORD_DONE;
}
int radar_scene_record_read(const RadarSceneRecorder *r,uint32_t offset,void *output,uint32_t bytes)
{
    uint8_t *out=output;uint32_t n;const uint8_t *p;
    if(!r || r->state!=RADAR_RECORD_FROZEN || !output || offset>r->image_bytes || bytes>r->image_bytes-offset)return RADAR_RECORD_E_STATE;
    while(bytes){p=segment(r,offset,&n);if(!p)return RADAR_RECORD_E_ARGUMENT;if(n>bytes)n=bytes;
        memcpy(out,p,n);out+=n;offset+=n;bytes-=n;}return 0;
}
int radar_scene_record_clear(RadarSceneRecorder *r)
{uint8_t *memory;if(!r || (r->state!=RADAR_RECORD_ARMED && r->state!=RADAR_RECORD_FROZEN))return RADAR_RECORD_E_STATE;
 memory=r->region;memset(r,0,sizeof *r);r->region=memory;return 0;}
int radar_scene_record_discard(RadarSceneRecorder *r,int quiet)
{uint8_t *memory;if(!r || !quiet || r->pending_record ||
 (r->state!=RADAR_RECORD_ACTIVE && r->state!=RADAR_RECORD_DRAINING))return RADAR_RECORD_E_STATE;
 memory=r->region;memset(r,0,sizeof *r);r->region=memory;return 0;}
const char *radar_scene_record_state_name(uint32_t s)
{static const char *const names[]={"EMPTY","ARMED","ACTIVE","DRAINING","HASHING","FROZEN"};return s<6U?names[s]:"INVALID";}
