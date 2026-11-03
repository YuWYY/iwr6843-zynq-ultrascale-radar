/* Production STREAM + upstream AMD SG driver on deterministic HOST model.
 * This is not RTL, DDR/cache timing or physical acquisition evidence. */
#include "model.h"
#include "radar_stream.h"
#include "radar_range.h"
#include "radar_crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static RadarControl control;
static RadarConsole console;
static RadarSw2 sw2;
static RadarSw3 sw3;
static RadarSession session;
static RadarStream stream;
static u8 frame[RADAR_STREAM_FRAME_BYTES];
static u32 produced,meta_next,meta_epoch,meta_open,limit,received,wrong_meta;
static u32 reset_base,halt_base,meta_end_calls;
static u64 next_frame;
static unsigned checks;
#define CHECK(e) do{++checks;if(!(e)){fprintf(stderr,"FAIL %d %s state=%d error=%d/%d dma=%d produced=%u verified=%u\n%s\n",__LINE__,#e,session.state,stream.error,stream.native_error,sw3.dma.state,produced,stream.frames_verified,model.log);exit(1);}}while(0)
static void put16(u8 *p,u32 v){p[0]=(u8)v;p[1]=(u8)(v>>8);}
static void make_frame(void)
{
    u32 c,i;u8 h[64];static const unsigned order[4]={0,2,1,3};
    memset(frame,0,sizeof(frame));memset(h,0,sizeof(h));
    h[0]=0xdc;h[1]=0x0a;h[2]=0xda;h[3]=0x0c;memcpy(h+4,h,4);
    put16(h+8,4144U&4095U);put16(h+10,4144U>>12);put16(h+16,0x744);put16(h+18,32);
    h[20]=4;h[21]=2;h[23]=2;h[24]=15;h[25]=1;put16(h+26,1);put16(h+28,512);memset(h+52,15,12);
    for(c=0;c<96;++c){u8 *r=frame+c*4160U;for(i=0;i<32;++i)memcpy(r+2*((i&~3U)+order[i&3U]),h+2*i,2);
        for(i=64;i<4160;++i)r[i]=(u8)((i*17U+c)&255U);}
}
static int validate(void *ctx,const void *raw,u32 bytes,u64 *id)
{(void)ctx;return radar_range_validate_frame(raw,bytes,id);}
static int meta_begin(void *ctx,u32 epoch)
{(void)ctx;if(meta_open)return -1;meta_open=1;meta_epoch=epoch;meta_next=1;return 0;}
static int meta_pop(void *ctx,RadarTransportTime *t,u64 *first,u32 *error)
{
    (void)ctx;if(meta_next>produced)return 0;memset(t,0,sizeof(*t));
    t->sequence=meta_next+(wrong_meta?1U:0U);t->bytes=RADAR_STREAM_FRAME_BYTES;
    t->hz=100000000;t->first_tick=(u64)meta_next*10000000U;t->last_tick=t->first_tick+100000U;t->valid=1;
    *first=(u64)meta_next*100000U;*error=10U;++meta_next;return 1;
}
static int meta_end(void *ctx,u32 epoch)
{
    (void)ctx;CHECK(epoch==meta_epoch);CHECK(!model.source_running);
    CHECK(sw3.dma.state==RADAR_DMA_READY && sw3.dma.reset_confirmed);
    CHECK(sw3.dma.epoch==session.admission_epoch+1U);
    CHECK(radar_dma_is_quiescent(&sw3.dma));CHECK(sw3.dma.ring->FreeCnt==128);
    ++meta_end_calls;meta_open=0;return 0;
}
static void idle_tick(void)
{char cmd[80];model_advance(100);radar_sw3_poll(&sw3);radar_sw2_poll(&sw2);(void)radar_console_poll(&console,cmd,sizeof(cmd));}
static void wait_sw2(void)
{u32 n=0;while(sw2.job!=SW2_JOB_NONE || sw2.state==SW2_CONNECT_WAIT){CHECK(++n<100000);idle_tick();}}
static void boot(u32 count)
{
    RadarStreamIo io={NULL,validate,meta_begin,meta_pop,meta_end};u32 n=0;
    model_destroy_dma(&sw3.dma);memset(&sw3,0,sizeof(sw3));model_init();
    CHECK(!radar_control_run_sw1(&control));radar_console_init(&console);radar_sw2_init(&sw2,&control,&console);
    radar_sw3_init(&sw3,&sw2,&control,&console);radar_session_init(&session,&sw3,&sw2,&control,&console);
    CHECK(radar_sw3_command(&sw3,"dma qualify")==1);while(sw3.job!=SW3_IDLE){CHECK(++n<100000);idle_tick();}
    CHECK(radar_sw3_capture_ready(&sw3));radar_sw2_command(&sw2,"confirm io");radar_sw2_command(&sw2,"connect");wait_sw2();
    radar_sw2_command(&sw2,"check10");wait_sw2();radar_sw2_command(&sw2,"reset3");wait_sw2();
    /* The shared host UART fixture reports SDK3.6. Set this test's reviewed
     * version seam to3.5 after the unchanged parser completed real exchanges. */
    strcpy(sw2.cli.last_version,"mmWave SDK Version: 03.05.00.04");
    session.profile=radar_capture_profile_stream(count);session.confirmed=1;
    produced=0;received=0;meta_next=0;meta_open=0;wrong_meta=0;limit=count;next_frame=0;model.no_payload=1;
    reset_base=model.dma_reset_writes;halt_base=model.dma_halt_writes;meta_end_calls=0;
    radar_stream_init(&stream,&session,&io);CHECK(!radar_stream_start(&stream,count));
}
static void summary(void)
{
    u64 bytes=(u64)produced*RADAR_STREAM_FRAME_BYTES;
    model.pages[1]|=RADAR_FLAG_FIRST;model.pages[5]=100;model.pages[7]=1000;
    model.pages[0x20]=(u32)(bytes/4);model.pages[0x21]=(u32)((bytes/4)>>32);
    model.pages[0x22]=(u32)(bytes/8);model.pages[0x23]=(u32)((bytes/8)>>32);
    model.pages[0x24]=produced;model.pages[0x25]=64;model.pages[0x26]=session.active_session;
    model.pages[0x27]=4U<<16;model.pages[0x2a]=8U<<11;
}
static void produce(void)
{
    RadarDma *d=&sw3.dma;XAxiDma_Bd *bd=d->ring->HwHead;u32 i,offset=0;
    model_complete_descriptors(d,1);
    for(i=0;i<7;++i){u32 slot=(u32)(((UINTPTR)bd-d->bd_base)/d->ring->Separation),n=d->slot[slot].capacity;
        memcpy((void *)(d->rx_base+(UINTPTR)slot*65536U),frame+offset,n);offset+=n;
        bd=(XAxiDma_Bd *)(UINTPTR)XAxiDma_BdRingNext(d->ring,bd);}
    CHECK(offset==RADAR_STREAM_FRAME_BYTES);++produced;summary();
    if(limit && produced==limit)model.pages[1]|=RADAR_FLAG_SOURCE_COMPLETE;
}
static void tick(int consume)
{
    RadarStreamFrame f;model_advance(100);
    if(session.state==RADAR_SESSION_RECEIVE && !(model.pages[1]&RADAR_FLAG_SOURCE_COMPLETE)){
        if(model.ops[RADAR_OP_STOP])model.pages[1]|=RADAR_FLAG_SOURCE_COMPLETE;
        else if(!next_frame || model.now>=next_frame){produce();next_frame=model.now+100000;}
    }
    radar_stream_poll(&stream);
    if(consume && !radar_stream_acquire(&stream,&f)) {
        CHECK(f.transport_sequence==received+1 && f.capture_epoch==session.active_session);
        CHECK(f.crc32==(radar_crc32_update(UINT32_MAX,(const void *)f.raw,f.bytes)^UINT32_MAX));
        CHECK(f.hsi_id==UINT64_C(0x0cda0adc0cda0adc));CHECK(!radar_stream_release(&stream,&f));
        CHECK(radar_stream_release(&stream,&f)<0);++received;
    }
}
static void run(u32 count,int consume)
{
    u32 n=0;boot(count);
    /* Reproduce the board's continuous preposted-ring stop: RS clear never
     * reports Halted here, while native software reset remains functional.
     * This injected outcome is not a claim that PG021 requires a next TLAST. */
    if(!count)model.halt_stuck=1;
    while(stream.active){CHECK(++n<200000);tick(consume);if(!count && received>=64 && !stream.stop_requested)CHECK(!radar_stream_request_stop(&stream,0));}
}
static void reach(u32 frames)
{u32 n=0;while(received<frames){CHECK(stream.active && ++n<100000);tick(1);}}
static void finish_stop(void)
{u32 n=0;while(stream.active){CHECK(++n<100000);tick(1);}}
static void stop_faults(void)
{
    u32 n,kind,reset_epoch;RadarStreamFrame held;
    /* Native reset is issued only once. Failed reset never releases epoch,
     * descriptor storage or source ownership, even if normal HALT is stuck. */
    boot(0);reach(4);model.halt_stuck=1;model.reset_stuck=1;reset_epoch=sw3.dma.epoch;
    CHECK(!radar_stream_request_stop(&stream,0));finish_stop();
    CHECK(stream.error==RADAR_SESSION_E_DMA && stream.native_error==RADAR_DMA_E_RESET_TIMEOUT);
    CHECK(sw3.dma.state==RADAR_DMA_LOCKED && !sw3.dma.reset_confirmed && sw3.dma.epoch==reset_epoch);
    CHECK(meta_open && !meta_end_calls && session.owns && sw3.dma.ring->HwCnt>0);
    CHECK(model.dma_reset_writes==reset_base+1U && model.dma_halt_writes==halt_base);
    for(n=0;n<100;++n)tick(0);
    CHECK(model.dma_reset_writes==reset_base+1U && sw3.dma.state==RADAR_DMA_LOCKED);
    /* Failed sensorStop is never positive source isolation. */
    boot(0);reach(4);model.uart_stop_fail=1;CHECK(!radar_stream_request_stop(&stream,0));finish_stop();
    CHECK(stream.error==RADAR_SESSION_E_UART && meta_open && session.owns);
    CHECK(model.dma_reset_writes==reset_base && !meta_end_calls);
    /* Directly inject post-drain evidence failures, before the reset decision:
     * partial BD, unconsumed metadata, incomplete release, lost boundary flag,
     * mismatched metadata total. None may license a software reset. */
    for(kind=0;kind<5;++kind) {
        boot(0);reach(4);CHECK(!radar_stream_request_stop(&stream,0));n=0;
        while(stream.active && session.state!=RADAR_SESSION_COPY){CHECK(++n<100000);tick(1);}
        CHECK(stream.active && !model.source_running);
        if(kind==0)XAxiDma_BdWrite(sw3.dma.ring->HwHead,XAXIDMA_BD_STS_OFFSET,
            XAXIDMA_BD_STS_COMPLETE_MASK|XAXIDMA_BD_STS_RXSOF_MASK|65536U);
        else if(kind==1)stream.metadata_sequence[0]=0xdeadU;
        else if(kind==2)--sw3.dma.released_packets;
        else if(kind==3)model.pages[1]&=~RADAR_FLAG_SOURCE_COMPLETE;
        else ++stream.metadata_received;
        finish_stop();CHECK(stream.error && meta_open && session.owns);
        CHECK(model.dma_reset_writes==reset_base && !meta_end_calls);
    }
    /* CPU-owned verified RAW is independent of the RX descriptor memory.
     * Its lease survives reset and can be consumed after the source is idle. */
    boot(0);reach(1);n=0;
    while(!stream.raw.ready){CHECK(++n<100000);tick(0);}
    CHECK(!radar_stream_acquire(&stream,&held));CHECK(stream.raw.processing==1);
    CHECK(!radar_stream_request_stop(&stream,0));finish_stop();
    CHECK(!stream.error && stream.raw.processing==1 && meta_end_calls==1);
    CHECK(!radar_stream_release(&stream,&held));CHECK(!stream.raw.used);
    /* An early user stop of a finite producer also retains preposted BDs. */
    boot(64);reach(4);model.halt_stuck=1;CHECK(!radar_stream_request_stop(&stream,0));finish_stop();
    CHECK(!stream.error && received==4 && stream.stop_by_reset && stream.stop_unused_bds);
    CHECK(model.dma_reset_writes==reset_base+1U && model.dma_halt_writes==halt_base && meta_end_calls==1);
}
static void slots_test(void)
{
    RadarRawSlots p;RadarStreamFrame *w,old,f[16];u32 i;
    radar_raw_slots_init(&p,0xc00000U);
    for(i=0;i<16;++i){CHECK(!radar_raw_slots_reserve(&p,1,i+1,4,100+i,&w));CHECK(!radar_raw_slots_publish(&p,w));}
    CHECK(radar_raw_slots_reserve(&p,1,17,4,120,&w)==1);
    for(i=0;i<16;++i)CHECK(!radar_raw_slots_acquire(&p,&f[i]));
    old=f[0];CHECK(radar_raw_slots_reset(&p)<0);CHECK(!radar_raw_slots_release(&p,&f[0]));
    CHECK(!radar_raw_slots_reserve(&p,1,17,4,200,&w));CHECK(w->lease!=old.lease);
    CHECK(radar_raw_slots_release(&p,&old)<0);CHECK(!radar_raw_slots_discard_write(&p,w));
    for(i=1;i<16;++i)CHECK(!radar_raw_slots_release(&p,&f[i]));
    CHECK(!radar_raw_slots_reset(&p));
}
int main(void)
{
    u32 n,b;RadarFiniteGeometry g;make_frame();slots_test();
    CHECK(!radar_capture_profile_validate(radar_capture_profile_stream(0)));
    CHECK(!radar_capture_profile_validate(radar_capture_profile_stream(64)));
    CHECK(!radar_capture_profile_stream_geometry(radar_capture_profile_stream(64),&g) && g.source_frames==64);
    CHECK(radar_capture_profile_finite_geometry(radar_capture_profile_stream(16),&g)<0);
    CHECK(radar_dma_plan(RADAR_STREAM_FRAME_BYTES,64,&b,&n)<0);
    run(16,1);CHECK(!stream.error && received==16 && session.state==RADAR_SESSION_IDLE && !meta_open);CHECK(model.starts==1);
    CHECK(!stream.stop_by_reset && !stream.stop_unused_bds && meta_end_calls==1);
    run(64,1);CHECK(!stream.error && received==64 && sw3.dma.state==RADAR_DMA_READY);CHECK(model.starts==1);
    CHECK(!stream.stop_by_reset && !stream.stop_unused_bds && meta_end_calls==1);
    run(0,1);CHECK(!stream.error && received==64 && model.ops[RADAR_OP_STOP]==1);CHECK(model.starts==1);
    CHECK(stream.stop_by_reset && stream.stop_unused_bds==126U && meta_end_calls==1);
    CHECK(model.dma_reset_writes==reset_base+1U && model.dma_halt_writes==halt_base);
    run(0,0);CHECK(stream.error==RADAR_STREAM_E_STALE || stream.error==RADAR_STREAM_E_OVERLOAD);
    CHECK(!session.owns && session.source==RADAR_SOURCE_STOPPED && model.starts==1);
    boot(16);wrong_meta=1;n=0;while(stream.active){CHECK(++n<100000);tick(0);}CHECK(stream.error==RADAR_STREAM_E_METADATA);
    CHECK(session.owns && session.state==RADAR_SESSION_FAILED && received==0);
    boot(16);frame[0]^=1;n=0;while(stream.active){CHECK(++n<100000);tick(0);}CHECK(stream.error==RADAR_STREAM_E_HSI);frame[0]^=1;
    stop_faults();
    model_destroy_dma(&sw3.dma);printf("PASS STREAM producer16/64/continuous64; preposted-ring RS-HALT-stuck regression; qualified single software-reset stop; reset LOCKED ownership; missing ACK/partial/metadata/release/boundary rejection; retained RAW lease; early finite stop; overload and HSI/CRC checks; %u assertions. HOST MODEL ONLY\n",checks);return 0;
}
