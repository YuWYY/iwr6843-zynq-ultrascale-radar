/* SPDX-License-Identifier: MIT
 * Host-only bounded recorder proof. Synthetic fixtures are never board input. */
#include "radar_scene_record.h"
#include "radar_crc32.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *allocation,*memory;
static RadarSceneRecorder rec;
static RadarFrameRef ref;
static RadarSpatialFrame spatial;
static RadarSceneFrame scene;
static RadarRangeFrame range;
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);p[2]=(uint8_t)(x>>16);p[3]=(uint8_t)(x>>24);}
static void put64(uint8_t *p,uint64_t x){put32(p,(uint32_t)x);put32(p+4,(uint32_t)(x>>32));}
static uint32_t crc(const void *p,size_t n){return radar_crc32_update(UINT32_MAX,p,n)^UINT32_MAX;}
static const RadarRecordIdentity id={"HOST_SYNTHETIC","test-build","test-profile","test-cfg","ISK","UNCALIBRATED",
    "not-board-bit","not-board-xsa","expected-not-observed-fw","HOST_ONLY","synthetic-parameters",3,1};
static void begin_test(unsigned seconds)
{
    memset(memory,0x55,RADAR_RECORD_REGION_BYTES);memset(allocation,0xa5,16);memset(memory+RADAR_RECORD_REGION_BYTES,0xa5,16);
    radar_scene_record_init(&rec,memory,RADAR_RECORD_REGION_BYTES);
    assert(!radar_scene_record_arm(&rec,seconds,&id));assert(!radar_scene_record_start(&rec,7));
    memset(&ref,0,sizeof ref);memset(&spatial,0,sizeof spatial);memset(&scene,0,sizeof scene);memset(&range,0,sizeof range);
    ref.capture_epoch=7;ref.bytes=399360;ref.flags=RADAR_FRAME_HSI_VALID|RADAR_FRAME_CRC_VALID|RADAR_FRAME_TIME_TRUSTED;
    strcpy(ref.cfg_sha256,id.cfg_sha256);ref.time_error_us=35;scene.capture_epoch=7;scene.installation_epoch=3;
    strcpy(scene.calibration_id,id.calibration_id);spatial.valid=scene.valid=1;
    spatial.count=1;spatial.points[0].range_m=2.25f;spatial.points[0].horizontal_angle_deg=-8.0f;
    spatial.points[0].flags=1;spatial.points[0].power_dbfs=-50;spatial.points[0].snr_db=18;
    spatial.heatmap_dbfs[5][7]=-34.25f;scene.diagnostics.version=1;scene.diagnostics.processing_us=123;
}
static void frame_identity(unsigned n)
{
    ref.frame_id=spatial.session=scene.frame_id=scene.session=range.session=100+n;
    ref.transport_sequence=n+1;ref.timestamp_us=spatial.timestamp_us=scene.timestamp_us=range.timestamp_us=1000000ULL+n*100000ULL;
    ref.observed_us=ref.timestamp_us+200;ref.crc32=0x12340000+n;
}
static void add_frame(unsigned n)
{
    int rc;unsigned slices=0;frame_identity(n);
    rc=radar_scene_record_begin(&rec,&ref,&spatial,&scene,&range,0);
    while(rc==RADAR_RECORD_BUSY){uint32_t before=rec.heat_offset;rc=radar_scene_record_step(&rec);assert(rec.heat_offset-before<=8192U);assert(++slices<14);}
    assert(rc==RADAR_RECORD_DONE && rec.count==n+1);
    {uint8_t *t=memory+n*RADAR_RECORD_TIMING_BYTES;memset(t,0,RADAR_RECORD_TIMING_BYTES);
        put64(t,n+1);put32(t+8,ref.frame_id);put32(t+12,ref.capture_epoch);put32(t+24,ref.crc32);
        put32(t+28,ref.flags);put32(t+32,96);put32(t+44,n+1);put32(t+64,ref.transport_sequence);
        put32(t+224,n+1);put32(t+228,ref.frame_id);put32(t+232,n+1);put64(t+272,ref.timestamp_us+50000);put64(t+280,ref.timestamp_us);}
}
static void seal(void)
{
    unsigned i=0;uint8_t v;
    assert(radar_scene_record_read(&rec,0,&v,1)<0);
    assert(radar_scene_record_freeze(&rec,rec.count,rec.count,0,8,0,0,1,0)<0);
    assert(radar_scene_record_freeze(&rec,rec.count,rec.count,0,8,0,1,0,0)<0);
    assert(!radar_scene_record_freeze(&rec,rec.count,rec.count,0,8,0,1,1,0));
    while(radar_scene_record_hash_step(&rec)==RADAR_RECORD_BUSY)assert(++i<4000);
    assert(rec.state==RADAR_RECORD_FROZEN);
}
static void verify(unsigned seconds)
{
    uint8_t *image=malloc(rec.image_bytes);unsigned i;const uint8_t *h;uint32_t sum;
    assert(image && !radar_scene_record_read(&rec,0,image,rec.image_bytes));
    assert(crc(image,rec.image_bytes)==rec.image_crc);assert(crc(image,4092)==get32(image+4092));
    assert(crc(image+4096,rec.image_bytes-4096)==get32(image+120));
    assert(get32(image+40)==seconds && get32(image+52)==seconds*10+1 && !get32(image+88));
    for(i=0;i<rec.count;++i){h=image+4096+i*2560;sum=radar_crc32_update(UINT32_MAX,h,124);
        sum=radar_crc32_update(sum,h+128,2432)^UINT32_MAX;assert(sum==get32(h+124));
        assert(get32(h+8)==i+1 && get32(h+16)==100+i && get32(h+20)==i+1);}
    for(i=0;i<3;++i){h=image+4096+rec.count*(2560+320)+i*100352;
        assert(crc(h,252)==get32(h+252));assert(crc(h+256,99220)==get32(h+44));
        assert(get32(h+32)==rec.heat_schedule[i]);assert(get32(h+8)==rec.heat_schedule[i]*10+1);}
    for(i=0;i<16;++i)assert(allocation[i]==0xa5 && memory[RADAR_RECORD_REGION_BYTES+i]==0xa5);
    {RadarSceneRecorder before=rec;radar_scene_record_cancel_pending(&rec,-99);
        radar_scene_record_stop_requested(&rec,-98);assert(!memcmp(&before,&rec,sizeof rec));
        assert(radar_scene_record_discard(&rec,1)<0);}
    if(seconds==60 && getenv("RADAR_RECORD_FIXTURE")){FILE *f=fopen(getenv("RADAR_RECORD_FIXTURE"),"wb");assert(f);
        assert(fwrite(image,1,rec.image_bytes,f)==rec.image_bytes);assert(!fclose(f));}
    free(image);
}
int main(void)
{
    unsigned i;allocation=malloc(RADAR_RECORD_REGION_BYTES+32);assert(allocation);memory=allocation+16;
    begin_test(60);for(i=0;i<=600;++i)add_frame(i);assert(rec.stop_due && rec.heat_count==3 && !rec.incomplete);seal();verify(60);
    assert(!radar_scene_record_clear(&rec));assert(!radar_scene_record_holds(&rec));
    begin_test(180);for(i=0;i<=1800;++i)add_frame(i);assert(rec.stop_due && rec.heat_count==3);seal();verify(180);
    puts("PASS 60/180s bounded real-schema synthetic fixtures; 601/1801 records; three scheduled heatmaps; little-endian frame/header/payload/image CRC; canaries intact");
    begin_test(60);frame_identity(0);spatial.timestamp_us++;
    assert(radar_scene_record_begin(&rec,&ref,&spatial,&scene,&range,0)==RADAR_RECORD_E_IDENTITY && !rec.count);
    begin_test(60);frame_identity(0);range.session++;
    assert(radar_scene_record_begin(&rec,&ref,&spatial,&scene,&range,0)==RADAR_RECORD_E_IDENTITY);
    begin_test(60);frame_identity(0);ref.flags&=~RADAR_FRAME_CRC_VALID;
    assert(radar_scene_record_begin(&rec,&ref,&spatial,&scene,&range,0)==RADAR_RECORD_E_IDENTITY);
    begin_test(60);add_frame(0);frame_identity(1);ref.time_error_us=1001;
    assert(radar_scene_record_begin(&rec,&ref,&spatial,&scene,&range,0)==RADAR_RECORD_E_TIME);
    begin_test(60);for(i=0;i<100;++i)add_frame(i);frame_identity(100);
    assert(radar_scene_record_begin(&rec,&ref,&spatial,&scene,&range,0)==RADAR_RECORD_BUSY);
    assert(radar_scene_record_step(&rec)==RADAR_RECORD_BUSY);radar_scene_record_cancel_pending(&rec,-99);
    assert(rec.count==100 && !rec.heat_count && !rec.pending_record && rec.incomplete);
    assert(radar_scene_record_discard(&rec,0)<0 && !radar_scene_record_discard(&rec,1));
    begin_test(180);for(i=0;i<RADAR_RECORD_CAPACITY;++i)add_frame(i);frame_identity(i);
    assert(radar_scene_record_begin(&rec,&ref,&spatial,&scene,&range,0)==RADAR_RECORD_E_CAPACITY);
    assert(rec.count==2048);for(i=0;i<16;++i)assert(memory[RADAR_RECORD_REGION_BYTES+i]==0xa5);
    begin_test(180);
    for(i=0;i<1984;++i){int rc;frame_identity(i);
        ref.timestamp_us=spatial.timestamp_us=scene.timestamp_us=range.timestamp_us=1000000ULL+i*1000ULL;
        rc=radar_scene_record_begin(&rec,&ref,&spatial,&scene,&range,0);
        while(rc==RADAR_RECORD_BUSY)rc=radar_scene_record_step(&rec);
        assert(rc==RADAR_RECORD_DONE);}
    assert(rec.error==RADAR_RECORD_E_CAPACITY && rec.incomplete && rec.stop_due && rec.count==1984);
    puts("PASS unexpectedly fast source reserves64 tail records and marks early-capacity stop incomplete with explicit CAPACITY reason");
    puts("PASS mismatched frame/time/CRC identity rejected; uncertain time rejected; partial sampled heat excluded; explicit quiet discard; full capacity refuses overwrite; FROZEN immutable");
    free(allocation);return 0;
}
