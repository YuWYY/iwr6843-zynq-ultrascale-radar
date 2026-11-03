/* Host replay of the production point-only reflection processor.
 * Input is the decoder-validated RSCENE01 image. It is not a complete heatmap
 * sequence, so this program never replays or claims structural-map results.
 */
/* Include search is explicitly bound to --source-root by the host launcher;
 * do not use a relative path that would silently select today's mainline. */
#include "radar_reflection.h"
#include "radar_dsp_types.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static RadarReflectionProcessor processor;
static RadarSpatialFrame spatial;
static RadarSceneContext context;
static RadarSceneFrame result;
static unsigned char header[4096], row[2560];
static uint32_t get32(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8U|(uint32_t)p[2]<<16U|(uint32_t)p[3]<<24U;}
static uint64_t get64(const unsigned char *p)
{return get32(p)|((uint64_t)get32(p+4)<<32U);}
static float getfloat(const unsigned char *p)
{uint32_t bits=get32(p);float f;memcpy(&f,&bits,sizeof f);return f;}
static void number(float value)
{if(isfinite(value))printf("%.9g",(double)value);else fputs("null",stdout);}
static int bad(const char *reason)
{fprintf(stderr,"REPLAY_INPUT_ERROR: %s\n",reason);return 2;}
static uint32_t age(uint64_t now,uint64_t previous)
{uint64_t a;if(!previous||now<previous)return UINT32_MAX;a=(now-previous)/1000U;return a>UINT32_MAX?UINT32_MAX:(uint32_t)a;}
int main(int argc,char **argv)
{
    FILE *in;uint32_t count,i,j,reset,flags;
    if(argc!=2)return bad("usage: replay_reflection_record IMAGE.bin");
    in=fopen(argv[1],"rb");if(!in)return bad(strerror(errno));
    if(fread(header,1,sizeof header,in)!=sizeof header||memcmp(header,"RSCENE01",8)||
       get32(header+8)!=1U||get32(header+12)!=4096U||get32(header+16)!=2560U||
       get32(header+28)!=UINT32_C(0x01020304)){fclose(in);return bad("image header");}
    count=get32(header+52);processor.next_id=get32(header+128);
    if(count>2048U||!processor.next_id){fclose(in);return bad("count or recorded initial ID seed");}
    if(!memchr(header+416,0,65)||!memchr(header+544,0,48)){
        fclose(in);return bad("configuration/calibration string bounds");}
    memcpy(context.cfg_sha256,header+416,65);memcpy(context.calibration_id,header+544,48);
    for(i=0;i<count;++i){
        if(fread(row,1,sizeof row,in)!=sizeof row||memcmp(row,"RSF1",4)||get32(row+4)!=2560U||get32(row+8)!=i+1U){
            fclose(in);return bad("frame identity/length");}
        memset(&spatial,0,sizeof spatial);memset(&result,0,sizeof result);
        context.capture_epoch=get32(row+12);context.frame_id=get32(row+16);
        context.frame_flags=get32(row+28);context.timestamp_us=get64(row+32);
        context.installation_epoch=get32(row+52);
        context.time_valid=!!(context.frame_flags&RADAR_FRAME_TIME_TRUSTED);
        spatial.session=context.frame_id;spatial.timestamp_us=context.timestamp_us;
        spatial.valid=get32(row+56);spatial.calibrated=get32(row+60);
        spatial.candidate_count=get32(row+64);spatial.count=get32(row+68);
        spatial.truncated_count=get32(row+72);spatial.xyz_count=get32(row+76);
        memcpy(spatial.calibration_id,context.calibration_id,sizeof spatial.calibration_id);
        if(spatial.count>32U){fclose(in);return bad("point capacity");}
        for(j=0;j<spatial.count;++j){
            const unsigned char *v=row+128U+j*56U;RadarSpatialPoint *q=&spatial.points[j];
            q->range_bin=get32(v);q->doppler_bin=(int32_t)get32(v+4);
            q->range_m=getfloat(v+8);q->velocity_mps=getfloat(v+12);
            q->azimuth_deg=getfloat(v+16);q->elevation_deg=getfloat(v+20);
            q->horizontal_angle_deg=getfloat(v+24);q->x_m=getfloat(v+28);
            q->y_m=getfloat(v+32);q->z_m=getfloat(v+36);q->snr_db=getfloat(v+40);
            q->coherence=getfloat(v+44);q->power_dbfs=getfloat(v+48);q->flags=get32(v+52);
        }
        flags=get32(row+80);reset=get32(row+84);
        /* The outer scene processor resets BEFORE ordinary reset-frame input,
         * but detects MAP_CHANGE AFTER updating reflections on that frame.
         * Its decision is an observed input here, never recomputed from the
         * three sparse heatmaps or described as independently proven. */
        if((flags&RADAR_SCENE_MAP_RESET)&&reset!=RADAR_SCENE_RESET_MAP_CHANGE)
            radar_reflection_reset(&processor);
        radar_reflection_update(&processor,&spatial,&context,&result);
        if((flags&RADAR_SCENE_MAP_RESET)&&reset==RADAR_SCENE_RESET_MAP_CHANGE){
            radar_reflection_reset(&processor);result.reflection_count=0U;
        }
        printf("{\"record_index\":%u,\"frame_id\":%u,\"timestamp_us\":%" PRIu64 ",\"next_id\":%u,\"reflections\":[",
            i+1U,context.frame_id,context.timestamp_us,processor.next_id);
        for(j=0;j<result.reflection_count;++j){const RadarStableReflection *r=&result.reflections[j];
            if(j)putchar(',');
            printf("{\"id\":%u,\"state\":%u,\"flags\":%u,\"range_m\":",r->id,r->state,r->flags);number(r->range_m);
            fputs(",\"u\":",stdout);number(r->u);fputs(",\"range_mad_m\":",stdout);number(r->range_mad_m);
            fputs(",\"range_median_m\":",stdout);number(r->range_median_m);
            printf(",\"age_ms\":%u,\"last_observed_us\":%" PRIu64 ",\"first_observed_us\":%" PRIu64
                   ",\"support_buckets\":%u,\"sample_count\":%u}",
                age(context.timestamp_us,r->last_observed_us),r->last_observed_us,r->first_observed_us,r->support_buckets,r->sample_count);
        }
        fputs("]}\n",stdout);
    }
    if(ferror(in)||ferror(stdout)){fclose(in);return bad("IO failure");}
    fclose(in);return 0;
}
