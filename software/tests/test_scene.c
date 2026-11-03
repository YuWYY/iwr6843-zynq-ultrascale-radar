/* Host-only synthetic scenes. No KEY3 or board interfaces are linked. */
#include "../radar_runtime/src/processing/radar_scene.h"
#include "../radar_runtime/src/processing/radar_range.h"
#include "../radar_runtime/src/processing/radar_dsp_types.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static RadarSceneProcessor processor;
static RadarSpatialFrame source;
static RadarSceneContext context;
static unsigned maximum_steps;
static void initial(void)
{
    radar_scene_reset(&processor,RADAR_SCENE_RESET_START);memset(&source,0,sizeof source);
    memset(&context,0,sizeof context);context.capture_epoch=1;context.time_valid=1;
    context.frame_flags=RADAR_FRAME_HSI_VALID|RADAR_FRAME_CRC_VALID|RADAR_FRAME_TIME_TRUSTED;
    strcpy(context.cfg_sha256,"synthetic-test-only");strcpy(context.calibration_id,"UNCALIBRATED");
    source.valid=1;strcpy(source.calibration_id,context.calibration_id);
}
static void blank(void)
{
    unsigned r,a;source.count=source.xyz_count=0;
    for(r=0;r<RADAR_SPATIAL_MAP_RANGES;++r)for(a=0;a<RADAR_SPATIAL_MAP_ANGLES;++a)source.heatmap_dbfs[r][a]=-100.0f;
}
static void point(float x,float y,float velocity)
{
    RadarSpatialPoint *p=&source.points[source.count++];memset(p,0,sizeof *p);
    p->x_m=x;p->y_m=y;p->z_m=0.2f;p->range_m=sqrtf(x*x+y*y+0.04f);
    p->range_bin=(unsigned)(p->range_m/(float)RADAR_RANGE_BIN_METERS+0.5f);
    p->horizontal_angle_deg=asinf(x/p->range_m)*180.0f/3.1415926536f;p->power_dbfs=-45.0f;
    p->snr_db=20;p->coherence=0.8f;p->velocity_mps=velocity;
    p->flags=RADAR_POINT_AZ_VALID|RADAR_POINT_EL_VALID|RADAR_POINT_XYZ_VALID;++source.xyz_count;
}
static const RadarSceneFrame *frame(uint64_t time)
{
    unsigned steps=0;int rc;const RadarSceneFrame *f;
    ++context.frame_id;source.session=context.frame_id;source.timestamp_us=context.timestamp_us=time;
    assert(radar_scene_begin(&processor,&source,&context)==RADAR_SCENE_BUSY);
    do {rc=radar_scene_step(&processor);assert(++steps<1000U);}while(rc==RADAR_SCENE_BUSY);
    assert(rc==RADAR_SCENE_DONE);if(steps>maximum_steps)maximum_steps=steps;
    f=radar_scene_result(&processor);assert(f&&f->frame_id==context.frame_id);return f;
}
static void wall(float y,unsigned start,unsigned end)
{
    unsigned a;for(a=start;a<=end;++a){float range=y/cosf(((float)a-60.0f)*3.1415926536f/180.0f);
        unsigned r=(unsigned)(range/(float)RADAR_RANGE_BIN_METERS+0.5f);
        if(r<RADAR_SPATIAL_MAP_RANGES)source.heatmap_dbfs[r][a]=-45.0f;}
}
int main(void)
{
    const RadarSceneFrame *f;unsigned i,id;void *workspace=calloc(1,radar_scene_workspace_bytes());
    assert(workspace);assert(radar_scene_init(&processor,workspace,radar_scene_workspace_bytes())==RADAR_SCENE_DONE);
    assert(radar_scene_reflection_next_id(&processor)==1U);
    initial();blank();point(0,3,0);f=frame(1000000);assert(f->track_count==1&&!f->tracks[0].confirmed);
    id=f->tracks[0].id;f=frame(1100000);assert(!f->tracks[0].confirmed);
    f=frame(1200000);assert(f->tracks[0].confirmed&&f->tracks[0].id==id);
    for(i=3;i<25;++i)f=frame(1000000+i*100000);
    assert(f->tracks[0].label==RADAR_SCENE_STATIC_REFLECTOR&&f->tracks[0].trail_count<=21);
    blank();f=frame(3500000);assert(f->tracks[0].predicted&&f->tracks[0].label==RADAR_SCENE_UNKNOWN);
    assert(f->tracks[0].last_measured_us==3400000);
    for(i=36;i<=39;++i)f=frame(i*100000);
    assert(f->track_count==1);f=frame(4000000);assert(f->track_count==0);

    /* Strong isolated hits must recur; low-coherence XYZ and bearing-only
     * points cannot become a track with invented zero elevation. */
    initial();blank();point(0,3,0);source.points[0].coherence=0.4f;
    f=frame(1000000);assert(f->track_count==0&&f->rejected_points==1);
    source.points[0].coherence=0.8f;source.points[0].flags=RADAR_POINT_AZ_VALID;
    source.points[0].horizontal_angle_deg=-20;f=frame(1100000);
    assert(f->track_count==0&&f->bearing_count==1);
    source.points[0].flags=RADAR_POINT_BEARING_VALID|RADAR_POINT_LOW_COHERENCE|RADAR_POINT_ANGLE_EDGE;
    source.points[0].x_m=source.points[0].y_m=source.points[0].z_m=NAN;
    f=frame(1150000);assert(f->track_count==0&&f->bearing_count==1);
    source.points[0].x_m=0;source.points[0].y_m=3;source.points[0].z_m=0.2f;
    source.points[0].flags=RADAR_POINT_XYZ_VALID|RADAR_POINT_AZ_VALID|RADAR_POINT_EL_VALID;
    source.points[0].snr_db=10;f=frame(1200000);assert(f->track_count==0);
    point(0.1f,3,0);f=frame(1300000);assert(f->track_count==1&&f->tracks[0].point_count==2);

    initial();for(i=0;i<30;++i){blank();point(-1.0f+0.04f*(float)i,3,-0.4f);point(-0.8f+0.04f*(float)i,3,0.4f);
        f=frame(1000000+i*100000);}
    assert(f->tracks[0].label==RADAR_SCENE_MOVING_OBJECT);assert(!(f->tracks[0].rule_bits&RADAR_SCENE_RULE_NONRIGID));
    assert(f->tracks[0].label!=RADAR_SCENE_HUMAN_CANDIDATE);
    initial();for(i=0;i<45;++i){float x=-1.0f+0.04f*(float)i,extent=i%2U?0.5f:0.2f;
        blank();point(x-extent*0.5f,3,-0.8f);point(x,3,0);point(x+extent*0.5f,3,0.8f);
        f=frame(1000000+i*100000);}
    assert(f->tracks[0].label==RADAR_SCENE_HUMAN_CANDIDATE);
    assert(f->tracks[0].rule_score>=3U&&f->tracks[0].rule_bits&RADAR_SCENE_RULE_NONRIGID);
    blank();f=frame(5500000);assert(f->tracks[0].predicted&&f->tracks[0].label==RADAR_SCENE_UNKNOWN);
    blank();point(0.8f,3,0);point(1.0f,3,0);f=frame(5600000);
    assert(f->tracks[0].label!=RADAR_SCENE_HUMAN_CANDIDATE); /* evidence conflict */
    id=f->tracks[0].id;++context.installation_epoch;f=frame(4000000);
    assert(f->tracks[0].id!=id&&!f->tracks[0].confirmed&&f->reset_reason==RADAR_SCENE_RESET_INSTALLATION);
    ++context.capture_epoch;f=frame(4100000);assert(!f->tracks[0].confirmed);
    strcpy(context.cfg_sha256,"new-waveform");f=frame(4200000);assert(f->reset_reason==RADAR_SCENE_RESET_CONTEXT);
    strcpy(context.calibration_id,"new-array");strcpy(source.calibration_id,"new-array");
    f=frame(4300000);assert(f->reset_reason==RADAR_SCENE_RESET_CONTEXT);
    context.time_valid=0;f=frame(4400000);assert(f->flags&RADAR_SCENE_TIME_UNTRUSTED);assert(f->track_count==0);
    context.time_valid=1;f=frame(4500000);assert(f->track_count==1&&!f->tracks[0].confirmed);
    assert(radar_scene_begin(&processor,&source,&context)==RADAR_SCENE_E_TIME);
    assert(!radar_scene_result(&processor));

    /* Two moving targets crossing in X but separated in Y retain identities;
     * true unresolved merges are flagged rather than invented detections. */
    initial();{unsigned left=0,right=0;
        for(i=0;i<40;++i){blank();point(-0.8f+0.04f*(float)i,2.8f,0);point(0.8f-0.04f*(float)i,3.4f,0);
            f=frame(1000000+i*100000);assert(f->track_count==2);
            if(i==0){left=f->tracks[0].id;right=f->tracks[1].id;}}
        assert(f->tracks[0].id==left&&f->tracks[1].id==right);
        assert(f->tracks[0].x_m>0.5f&&f->tracks[1].x_m< -0.5f);
    }

    initial();for(i=0;i<8;++i){float x=0.8f-0.1f*(float)i;
        blank();point(-x,3,0);point(x,3,0);f=frame(1000000+i*100000);}
    assert(f->flags&RADAR_SCENE_AMBIGUOUS_ASSOCIATION);
    for(i=0;i<f->track_count;++i)assert(f->tracks[i].label!=RADAR_SCENE_HUMAN_CANDIDATE);

    initial();for(i=0;i<=105;++i){blank();wall(3.0f,35,85);f=frame(1000000+i*100000);}
    assert(!(f->flags&RADAR_SCENE_MAP_WARMUP));assert(f->wall_count>=1);
    assert(f->walls[0].support_fraction>=0.7f&&f->walls[0].residual_m<=0.15f);
    assert(hypotf(f->walls[0].x1_m-f->walls[0].x0_m,f->walls[0].y1_m-f->walls[0].y0_m)>=1.0f);
    blank();f=frame(11700000);assert(f->wall_count==0); /* departed evidence does not coast */
    initial();for(i=0;i<=105;++i){blank();if(i%5U==0)wall(3.0f,35,85);f=frame(1000000+i*100000);}
    assert(f->wall_count==0); /* intermittent below70% per250ms time bins */
    initial();for(i=0;i<=105;++i){blank();wall(3.0f,58,62);f=frame(1000000+i*100000);}
    assert(f->wall_count==0); /* short reflector is not a wall */
    initial();for(i=0;i<=105;++i){blank();wall(3.0f,35,45);wall(3.0f,75,85);f=frame(1000000+i*100000);}
    assert(f->wall_count==0); /* disconnected short runs must not bridge a gap */
    initial();for(i=0;i<=105;++i){blank();point(0,3,0);wall(3.0f,10,110);f=frame(1000000+i*100000);}
    assert(f->reflection_count==1&&f->reflections[0].state==RADAR_REFLECTION_STABLE);
    for(i=106;i<=108;++i){blank();point(0,3,0);f=frame(1000000+i*100000);}
    assert(f->reset_reason==RADAR_SCENE_RESET_MAP_CHANGE&&f->wall_count==0&&f->track_count==0);
    assert(f->reflection_count==0&&f->diagnostics.stable_count==0);
    initial();for(i=0;i<=105;++i){blank();f=frame(1000000+i*100000);}
    assert(f->track_count==0&&f->wall_count==0);
    radar_scene_cancel(&processor);assert(!radar_scene_result(&processor));
    initial();blank();point(0,3,0);f=frame(1000000);
    assert(f->reflection_count==1);
    id=radar_scene_reflection_next_id(&processor);
    assert(id==f->reflections[0].id+1U);
    context.frame_flags|=RADAR_FRAME_CLIPPED;f=frame(1100000);
    assert(f->reflection_count==0&&f->diagnostics.reject_flags&RADAR_SCENE_REJECT_CLIPPED);
    assert(radar_scene_reflection_next_id(&processor)==id);
    context.frame_flags=7U;f=frame(1200000);
    assert(f->reflection_count==1&&f->reflections[0].state==RADAR_REFLECTION_TENTATIVE);
    source.error=-1;assert(radar_scene_begin(&processor,&source,&context)==RADAR_SCENE_E_IDENTITY);
    assert(!radar_scene_result(&processor));
    printf("PASS scene identity,3/5 confirmation,500ms expiry,measured trails,motion,unknown fallback,context reset,wall positive/negative; workspace=%zu result=%zu max_steps=%u\n",radar_scene_workspace_bytes(),sizeof(RadarSceneFrame),maximum_steps);
    free(workspace);return 0;
}
