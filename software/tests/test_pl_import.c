/* SPDX-License-Identifier: MIT */
#define main old_range_test_entry
#include "test_range.c"
#undef main
#include "radar_spatial.h"
static int32_t packed[196608];
static RadarRangeFrame reference;
static uint64_t sums[256],rd[32];
static int32_t row[768];
static RadarSpatialProcessor spatial;
int main(void)
{
    unsigned c,rx,n;RadarDspCandidateHeader h={0};RadarDspCandidate candidates[32]={{0}};
    radar_range_init(&processor);tone(24,10000.0f,1,1);reference=*process();
    for(n=0;n<256U;++n)sums[n]=(uint64_t)llround((double)processor.power_sum[n]*0.25);
    assert(radar_range_begin(&processor,raw,sizeof raw,123U,987654321U)==0);
    for(c=0;c<96U;++c)assert(radar_range_pack_step(&processor,packed,196608U)==(c==95U?1:0));
    assert(!processor.frame.fft_count&&!processor.frame.valid);
    for(c=0;c<96U;++c)for(rx=0;rx<4U;++rx)for(n=0;n<256U;++n) {
        unsigned k=((c*4U+rx)*256U+n)*2U;
        const uint8_t *p=memory+c*4160U+64U+rx*1024U+n*4U;
        int32_t q=(int16_t)((unsigned)p[0]|((unsigned)p[1]<<8));
        int32_t i=(int16_t)((unsigned)p[2]|((unsigned)p[3]<<8));
        assert(packed[k]==i&&packed[k+1U]==q);
    }
    assert(radar_range_import_power(&processor,sums)==RADAR_RANGE_DONE);
    assert(processor.frame.peak_bin==reference.peak_bin);
    for(n=0;n<256U;++n)if(reference.power_dbfs[n]>-80.0f)assert(fabsf(processor.frame.power_dbfs[n]-reference.power_dbfs[n])<0.001f);
    for(n=0;n<3U;++n)assert(processor.frame.tx_records[n]==32U&&fabsf(processor.frame.tx_power_dbfs[n]-reference.tx_power_dbfs[n])<0.0001f);
    assert(radar_range_import_power(&processor,sums)==RADAR_RANGE_E_STATE);
    radar_spatial_init(&spatial);assert(radar_spatial_begin(&spatial,1U,100U,NULL)==0);
    assert(radar_spatial_import_pl_begin(&spatial,&h,candidates,PL_BASELINE)==0);
    memset(row,0,sizeof row);memset(rd,0,sizeof rd);row[(8U*32U+1U)*2U]=4194304;
    for(n=0;n<256U;++n)assert(radar_spatial_import_pl_row(&spatial,n,row,rd)==(n==255U?1:0));
    {
        const double a=-6.2831853071795864769/96.0,scale=(256.0/127.5)*32.0;
        assert(fabs(spatial.cube[0][8][1].re-scale*cos(a))<0.00002);
        assert(fabs(spatial.cube[0][8][1].im-scale*sin(a))<0.00002);
    }
    assert(radar_spatial_import_pl_row(&spatial,0,row,rd)==RADAR_SPATIAL_E_SEQUENCE);
    while(radar_spatial_step(&spatial)==0){}
    assert(radar_spatial_result(&spatial)!=NULL);
    assert(radar_spatial_begin(&spatial,2,200,NULL)==0);
    h.count=h.total=2;candidates[0].range=20;candidates[0].doppler=0;candidates[0].power=100;candidates[0].noise_sum=20;candidates[0].noise_count=10;
    candidates[1]=candidates[0];candidates[1].range=30;candidates[1].power=200;
    assert(radar_spatial_import_pl_begin(&spatial,&h,candidates,PL_BASELINE)==RADAR_SPATIAL_E_SEQUENCE);
    assert(radar_spatial_begin(&spatial,3,300,NULL)==0);
    assert(radar_spatial_import_pl_begin(&spatial,&h,candidates,PL_ENHANCED)==0);
    assert(spatial.frame.points[0].range_bin==20&&spatial.frame.points[1].range_bin==30);
    puts("PASS all-channel raw pack, range normalization, one-time TDM/cube import and enhanced ranking");return 0;
}
