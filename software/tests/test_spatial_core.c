/* SPDX-License-Identifier: MIT
 * Host-only DSP regression / production-C oracle. Never accesses a board.
 */
#include "../radar_runtime/src/processing/radar_spatial.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static RadarSpatialProcessor spatial;
static RadarRangeProcessor range_processor;
static uint8_t raw[RADAR_RANGE_INPUT_BYTES];
static float re[256],im[256];
static const double pi=3.1415926535897932384626433832795;

static void unit_calibration(RadarArrayCalibration *cal)
{
    unsigned c;memset(cal,0,sizeof(*cal));
    for(c=0;c<12;++c) cal->channel[c].re=1.0f;
    strcpy(cal->array_id,RADAR_SPATIAL_ARRAY_ID);
    strcpy(cal->cfg_sha256,RADAR_RANGE_CFG_SHA256);
    strcpy(cal->calibration_id,"HOST_TEST_CAL");
}
static int complete(void)
{
    unsigned steps=0;int rc;
    do { rc=radar_spatial_step(&spatial);assert(++steps<15000U); } while(rc==RADAR_SPATIAL_BUSY);
    return rc;
}
static void feed_tone(int doppler,float azimuth,float elevation,float amplitude)
{
    static const unsigned tx_order[3]={0U,2U,1U};
    static const unsigned xp[12]={0U,1U,2U,3U,2U,3U,4U,5U,4U,5U,6U,7U};
    unsigned chirp,rx;
    const double q=2.0*pi*RADAR_SPATIAL_PITCH_M*RADAR_SPATIAL_FC_HZ/299792458.0;
    double u=sin((double)azimuth*pi/180.0)*cos((double)elevation*pi/180.0);
    double v=sin((double)elevation*pi/180.0);
    memset(re,0,sizeof(re));memset(im,0,sizeof(im));
    for(chirp=0U;chirp<96U;++chirp) for(rx=0U;rx<4U;++rx) {
        unsigned tx=tx_order[chirp%3U],c=tx*4U+rx;
        double phase=-q*((double)xp[c]*u+(tx==1U?0.0:v))+
                      2.0*pi*(double)doppler*((double)(chirp/3U)+(double)(chirp%3U)/3.0)/32.0;
        re[40]=(float)((double)amplitude*cos(phase)/spatial.range_scale);
        im[40]=(float)((double)amplitude*sin(phase)/spatial.range_scale);
        assert(!radar_spatial_accept_fft(&spatial,chirp,rx,re,im));
    }
}
static void synthetic_tests(void)
{
    const RadarSpatialFrame *f;
    RadarArrayCalibration cal;
    unsigned a,chirp,rx,r;float peak;
    assert(sizeof(spatial)<1100U*1024U);
    assert(radar_spatial_begin(&spatial,1U,0U,NULL)==RADAR_SPATIAL_E_STATE);
    radar_spatial_init(&spatial);
    assert(!radar_spatial_begin(&spatial,1U,1234U,NULL));
    assert(radar_spatial_result(&spatial)==NULL);
    assert(radar_spatial_step(&spatial)==RADAR_SPATIAL_E_INCOMPLETE);
    assert(!radar_spatial_begin(&spatial,1U,1234U,NULL));
    assert(radar_spatial_accept_fft(&spatial,0U,1U,re,im)==RADAR_SPATIAL_E_SEQUENCE);
    radar_spatial_cancel(&spatial);
    assert(!radar_spatial_begin(&spatial,1U,1234U,NULL));
    re[7]=NAN;
    assert(radar_spatial_accept_fft(&spatial,0U,0U,re,im)==RADAR_SPATIAL_E_NUMERIC);
    re[7]=0.0f;
    unit_calibration(&cal);cal.array_id[0]='!';
    assert(radar_spatial_begin(&spatial,1U,0U,&cal)==RADAR_SPATIAL_E_CALIBRATION);
    unit_calibration(&cal);cal.channel[3].re=NAN;
    assert(radar_spatial_begin(&spatial,1U,0U,&cal)==RADAR_SPATIAL_E_CALIBRATION);
    unit_calibration(&cal);cal.measured=1U;strcpy(cal.calibration_id,"UNCALIBRATED");
    assert(radar_spatial_begin(&spatial,1U,0U,&cal)==RADAR_SPATIAL_E_CALIBRATION);
    assert(!radar_spatial_begin(&spatial,7U,1234U,NULL));
    feed_tone(2,20.0f,8.0f,0.5f);
    assert(complete()==RADAR_SPATIAL_DONE);
    f=radar_spatial_result(&spatial);assert(f && f->valid && !f->calibrated);
    assert(f->session==7U && f->timestamp_us==1234U);
    assert(f->count && f->points[0].range_bin==40U && f->points[0].doppler_bin==2);
    assert(f->points[0].azimuth_deg==20.0f && f->points[0].elevation_deg==8.0f);
    assert(f->points[0].flags==7U && f->points[0].coherence>0.999f);
    assert(f->points[0].x_m>0.0f && f->points[0].z_m>0.0f);
    assert(fabsf(f->points[0].power_dbfs+6.0206f)<0.002f);
    assert(isnan(f->heatmap_dbfs[0][60]));
    peak=-160.0f;for(a=0;a<121;++a) if(f->heatmap_dbfs[40][a]>peak) peak=f->heatmap_dbfs[40][a];
    assert(fabsf(peak+6.0206f)<0.03f);
    assert(!radar_spatial_begin(&spatial,8U,5678U,NULL));
    feed_tone(-3,-24.0f,-9.0f,0.25f);assert(complete()==RADAR_SPATIAL_DONE);
    f=radar_spatial_result(&spatial);
    assert(f->points[0].doppler_bin==-3 && f->points[0].azimuth_deg==-24.0f && f->points[0].elevation_deg==-9.0f);
    assert(f->points[0].x_m<0.0f && f->points[0].z_m<0.0f);
    assert(!radar_spatial_begin(&spatial,9U,0U,NULL));
    feed_tone(15,0.0f,0.0f,0.5f);assert(complete()==RADAR_SPATIAL_DONE);
    f=radar_spatial_result(&spatial);
    assert((f->points[0].flags&RADAR_POINT_DOPPLER_EDGE) && !(f->points[0].flags&7U));
    assert(isnan(f->points[0].x_m));
    assert(!radar_spatial_begin(&spatial,10U,0U,NULL));
    feed_tone(0,0.0f,0.0f,0.0f);assert(complete()==RADAR_SPATIAL_DONE);
    f=radar_spatial_result(&spatial);assert(f->count==0U && f->candidate_count==0U && f->valid);
    /* More resolvable strong range peaks than the display storage capacity.
     * Candidate bounds must hold through actual FFT/CFAR/angle processing. */
    assert(!radar_spatial_begin(&spatial,11U,0U,NULL));
    memset(re,0,sizeof(re));memset(im,0,sizeof(im));
    for(r=10U;r<=190U;r+=4U) re[r]=(0.8f-0.005f*(float)((r-10U)/4U))/spatial.range_scale;
    for(chirp=0U;chirp<96U;++chirp) for(rx=0U;rx<4U;++rx)
        assert(!radar_spatial_accept_fft(&spatial,chirp,rx,re,im));
    assert(complete()==RADAR_SPATIAL_DONE);f=radar_spatial_result(&spatial);
    assert(f->candidate_count>32U && f->count==32U && f->truncated_count==f->candidate_count-32U);
    assert(f->points[0].range_bin==10U);
    for(a=1U;a<f->count;++a) assert(f->points[a-1U].power_dbfs>=f->points[a].power_dbfs);
    /* Valid angles do not license an impossible calibrated negative range. */
    unit_calibration(&cal);cal.range_bias_m=3.0f;
    assert(!radar_spatial_begin(&spatial,12U,0U,&cal));
    feed_tone(0,0.0f,0.0f,0.5f);assert(complete()==RADAR_SPATIAL_DONE);
    f=radar_spatial_result(&spatial);
    assert((f->points[0].flags&3U)==3U && !(f->points[0].flags&RADAR_POINT_XYZ_VALID));
    assert(isnan(f->points[0].x_m) && isfinite(f->points[0].horizontal_angle_deg));
    puts("PASS spatial core: signed angles/TDM/power/edge/empty/input ownership/calibration rejection");
}

static void json_float(FILE *f,float value)
{
    if(isfinite(value)) fprintf(f,"%.9g",(double)value);else fputs("null",f);
}
static void json_result(FILE *out,const RadarSpatialFrame *f)
{
    unsigned i,r,a;
    fprintf(out,"{\n\"session\":%u,\"timestamp_us\":%llu,\"valid\":%u,\"calibrated\":%u,\"error\":%d,\n",
            f->session,(unsigned long long)f->timestamp_us,f->valid,f->calibrated,f->error);
    fprintf(out,"\"calibration_id\":\"%s\",\"candidate_count\":%u,\"count\":%u,\"truncated_count\":%u,\"xyz_count\":%u,\n\"points\":[\n",
            f->calibration_id,f->candidate_count,f->count,f->truncated_count,f->xyz_count);
    for(i=0;i<f->count;++i) {
        const RadarSpatialPoint *p=&f->points[i];
        fprintf(out,"%s{\"range_bin\":%u,\"doppler_bin\":%d,\"flags\":%u",i?",\n":"",p->range_bin,p->doppler_bin,p->flags);
#define J(name) do { fputs(",\"" #name "\":",out);json_float(out,p->name); } while(0)
        J(range_m);J(velocity_mps);J(azimuth_deg);J(elevation_deg);J(horizontal_angle_deg);
        J(x_m);J(y_m);J(z_m);J(snr_db);J(coherence);J(power_dbfs);
#undef J
        fputs("}",out);
    }
    fputs("\n],\n\"heatmap_dbfs\":[\n",out);
    for(r=0;r<RADAR_SPATIAL_MAP_RANGES;++r) {
        if(r) fputs(",\n",out);
        fputs("[",out);
        for(a=0;a<RADAR_SPATIAL_MAP_ANGLES;++a) { if(a)fputs(",",out);json_float(out,f->heatmap_dbfs[r][a]); }
        fputs("]",out);
    }
    fputs("\n]}\n",out);
}
static int load_calibration(const char *path,RadarArrayCalibration *cal)
{
    unsigned c,measured;char id[48];float bias;FILE *f=fopen(path,"r");
    if(!f)return -1;
    unit_calibration(cal);
    if(fscanf(f,"%47s %f %u",id,&bias,&measured)!=3) { fclose(f);return -1; }
    strcpy(cal->calibration_id,id);cal->range_bias_m=bias;cal->measured=measured;
    for(c=0;c<12;++c) if(fscanf(f,"%f %f",&cal->channel[c].re,&cal->channel[c].im)!=2) { fclose(f);return -1; }
    fclose(f);return 0;
}
static int raw_oracle(int argc,char **argv)
{
    const char *input=NULL,*output=NULL,*calfile=NULL,*prefix=NULL;
    RadarArrayCalibration cal;RadarRangeFrame baseline;
    FILE *file;size_t bytes;unsigned n;int rc,i;
    for(i=1;i<argc;++i) {
        if(i+1>=argc) return 2;
        if(!strcmp(argv[i],"--raw")) input=argv[++i];
        else if(!strcmp(argv[i],"--json")) output=argv[++i];
        else if(!strcmp(argv[i],"--calibration")) calfile=argv[++i];
        else if(!strcmp(argv[i],"--arrays-prefix")) prefix=argv[++i];
        else return 2;
    }
    if(!input || !output)return 2;
    if(calfile && load_calibration(calfile,&cal))return 2;
    file=fopen(input,"rb");if(!file)return 2;
    bytes=fread(raw,1,sizeof(raw),file);rc=fgetc(file);fclose(file);
    if(bytes!=sizeof(raw) || rc!=EOF)return 3;
    radar_range_init(&range_processor);
    if(radar_range_begin(&range_processor,raw,bytes,1U,0U)) return 4;
    for(n=0;n<96;++n) if(radar_range_step(&range_processor)<0)return 4;
    baseline=*radar_range_result(&range_processor);
    radar_spatial_init(&spatial);
    if(radar_spatial_begin(&spatial,1U,0U,calfile?&cal:NULL))return 5;
    if(radar_range_set_fft_sink(&range_processor,radar_spatial_accept_fft,&spatial))return 5;
    if(radar_range_begin(&range_processor,raw,bytes,1U,0U))return 5;
    for(n=0;n<96;++n) if(radar_range_step(&range_processor)<0)return 5;
    if(memcmp(&baseline,radar_range_result(&range_processor),sizeof(baseline))) {
        fputs("FAIL overview range output changed with sink\n",stderr);return 6;
    }
    rc=complete();if(rc!=RADAR_SPATIAL_DONE) { fprintf(stderr,"DSP %s\n",radar_spatial_error_name(rc));return 7; }
    file=fopen(output,"w");if(!file)return 8;
    json_result(file,radar_spatial_result(&spatial));if(fclose(file))return 8;
    if(prefix) {
        char name[1024];
        if(snprintf(name,sizeof(name),"%s_cube_cf32.bin",prefix)>=(int)sizeof(name))return 8;
        file=fopen(name,"wb");if(!file)return 8;
        if(fwrite(spatial.cube,1,sizeof(spatial.cube),file)!=sizeof(spatial.cube))return 8;
        if(fclose(file))return 8;
        if(snprintf(name,sizeof(name),"%s_rd_f32.bin",prefix)>=(int)sizeof(name))return 8;
        file=fopen(name,"wb");if(!file)return 8;
        if(fwrite(spatial.rd_power,1,sizeof(spatial.rd_power),file)!=sizeof(spatial.rd_power))return 8;
        if(fclose(file))return 8;
    }
    printf("PASS production spatial oracle: candidates=%u xyz=%u range_output_bit_identical=1 processor_bytes=%zu\n",
           spatial.frame.count,spatial.frame.xyz_count,sizeof(spatial));return 0;
}
int main(int argc,char **argv)
{
    if(argc>1)return raw_oracle(argc,argv);
    synthetic_tests();return 0;
}
