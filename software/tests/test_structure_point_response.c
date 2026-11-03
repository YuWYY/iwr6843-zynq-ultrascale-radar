/* Independent direct-DFT single point families, not envelope-table replay. */
#include "../radar_runtime/src/processing/radar_scene.h"
#include "../radar_runtime/src/processing/radar_spatial.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PI 3.14159265358979323846
static RadarSpatialFrame s;
static RadarSceneContext c;
static RadarSceneProcessor p;
static void generate(double tone,double angle,unsigned perturb)
{
    double re[256],im[256],mr=0,mi=0,power[256],max=0;
    unsigned n,k,a,m;
    for(n=0;n<256;++n){re[n]=cos(2*PI*tone*n/256);im[n]=sin(2*PI*tone*n/256);mr+=re[n]/256;mi+=im[n]/256;}
    for(k=0;k<256;++k){double sr=0,si=0;
        for(n=0;n<256;++n){double w=.5-.5*cos(2*PI*n/255),rr=(re[n]-mr)*w,ii=(im[n]-mi)*w;
            double co=cos(2*PI*k*n/256),sn=-sin(2*PI*k*n/256);sr+=rr*co-ii*sn;si+=rr*sn+ii*co;}
        power[k]=sr*sr+si*si;if(power[k]>max)max=power[k];
    }
    for(k=0;k<205;++k)for(a=0;a<121;++a){double sr=0,si=0,u=sin(angle*PI/180),v=sin(((double)a-60)*PI/180);
        double q=2*PI*RADAR_SPATIAL_PITCH_M*RADAR_SPATIAL_FC_HZ/299792458.0;
        for(m=0;m<8;++m){double phase=q*m*(u-v)+(perturb?.25*sin((double)m*1.3):0.0);
            double gain=perturb?1.0+.15*cos((double)m*.8):1.0;sr+=gain*cos(phase);si+=gain*sin(phase);}
        {double pp=power[k]/max*(sr*sr+si*si)/64;s.heatmap_dbfs[k][a]=pp>1e-12?(float)(-35+10*log10(pp)):-160.0f;}
    }
}
static const RadarSceneFrame *frame(unsigned i)
{
    int rc;unsigned steps=0;++c.frame_id;s.session=c.frame_id;s.timestamp_us=c.timestamp_us=1000000U+i*250000U;
    assert(radar_scene_begin(&p,&s,&c)==0);
    do {rc=radar_scene_step(&p);assert(++steps<1000);}while(rc==0);
    assert(rc==1);return radar_scene_result(&p);
}
int main(void)
{
    const double range[]={10.23,45.31,100.49,199.17},angles[]={-31.2,.4,40.3};
    unsigned r,a,j,phase,tests=0;const RadarSceneFrame *f;
    void *work=calloc(1,radar_scene_workspace_bytes());assert(work);
    assert(radar_scene_init(&p,work,radar_scene_workspace_bytes())==1);
    for(r=0;r<4;++r)for(a=0;a<3;++a)for(phase=0;phase<2;++phase){
        radar_scene_reset(&p,0);memset(&s,0,sizeof s);memset(&c,0,sizeof c);
        s.valid=1;c.capture_epoch=c.installation_epoch=c.time_valid=1;c.frame_flags=7;
        strcpy(c.cfg_sha256,"point-response-synthetic");strcpy(c.calibration_id,"UNCALIBRATED");strcpy(s.calibration_id,c.calibration_id);
        generate(range[r],angles[a],phase);
        for(j=0;j<42;++j){f=frame(j);assert(f->wall_count==0);}
        assert(!(f->flags&RADAR_SCENE_MAP_WARMUP));
        assert(f->diagnostics.independent_groups<3U);
        ++tests;
    }
    printf("PASS %u direct-DFT single-point sequences: fractional range/angle, sidelobes, modest gain/phase error, zero structural candidates\n",tests);
    free(work);return 0;
}
