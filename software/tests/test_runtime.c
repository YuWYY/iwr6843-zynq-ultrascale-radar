/* SPDX-License-Identifier: MIT
 * Tests run the production app modules and the unmodified uploaded AMD SG
 * driver above model.c. No test fixture/profile is installed in target src.
 */
#include "model.h"
#include "radar_crc32.h"
#include "radar_memory.h"
#include "radar_hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(c) do{if(!(c)){fprintf(stderr,"FAIL %s:%d: %s (session=%s dma=%s now=%llu)\nLOG:\n%s\n",__FILE__,__LINE__,#c,radar_session_state_name(session.state),radar_dma_state_name(sw3.dma.state),(unsigned long long)model.now,model.log);exit(1);}}while(0)
static RadarControl control;
static RadarConsole console;
static RadarSw2 sw2;
static RadarSw3 sw3;
static RadarSession session;
static u32 tests_passed;
static const char *const cfg[]={"flushCfg","dfeDataOutputMode 1","channelCfg 15 3 0","adcCfg 2 1","adcbufCfg -1 0 0 1 1","profileCfg 0 60 7 3 57 0 0 70 1 256 5000 0 0 30","chirpCfg 0 0 0 0 0 0 0 1","frameCfg 0 0 128 0 100 1 0","lvdsStreamCfg -1 0 1 0"};
static const RadarCaptureProfile test_profile={
    .schema=1,.approvals=255,.name="HOST_MODEL_ONLY_NOT_FOR_RADAR",.profile_id="123456789abcdef0",
    .firmware_sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    .cfg_sha256="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    .review_sha256="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    .version_substring="03.06.02.00",.lanes=2,.lane_mbps=600,.word_bits=16,.format_flags=8,
    .packet_bytes=131072,.packet_count=1,.startup_mode=1,.receive_timeout_us=2000000,
    .snapshot_timeout_us=100000,.command_count=sizeof(cfg)/sizeof(cfg[0]),.commands=cfg,
    .start_command="sensorStart",.stop_command="sensorStop"
};
static void boot(void)
{
    model_destroy_dma(&sw3.dma);memset(&sw3,0,sizeof(sw3));model_init();
    REQUIRE(radar_control_run_sw1(&control)==0);
    radar_console_init(&console);radar_sw2_init(&sw2,&control,&console);
    radar_sw3_init(&sw3,&sw2,&control,&console);radar_session_init(&session,&sw3,&sw2,&control,&console);
    model_flush_console(&console);
}
static void tick(void)
{
    char cmd[80];
    model_advance(500U);
    if(session.owns){radar_session_poll(&session);}
    else{radar_sw3_poll(&sw3);radar_sw2_poll(&sw2);radar_session_poll(&session);}
    (void)radar_console_poll(&console,cmd,sizeof(cmd));
}
static void wait_sw3(void)
{
    u32 n=0;while(sw3.job!=SW3_IDLE){REQUIRE(++n<100000U);tick();}model_flush_console(&console);
}
static void wait_sw2(void)
{
    u32 n=0;while(sw2.job!=SW2_JOB_NONE||sw2.state==SW2_CONNECT_WAIT){REQUIRE(++n<100000U);tick();}model_flush_console(&console);
    REQUIRE(sw2.state==SW2_ONLINE);
}
static void wait_session(void)
{
    u32 n=0;while(session.owns&&session.state!=RADAR_SESSION_FAILED){REQUIRE(++n<100000U);tick();}model_flush_console(&console);
}
static void wait_phase(RadarSessionState phase)
{
    u32 n=0;while(session.state!=phase&&session.state!=RADAR_SESSION_FAILED){REQUIRE(++n<100000U);tick();}
    REQUIRE(session.state==phase);
}
static void qualify_io_profile(void)
{
    radar_sw2_command(&sw2,"confirm io");radar_sw2_command(&sw2,"connect");wait_sw2();
    radar_sw2_command(&sw2,"check10");wait_sw2();REQUIRE(sw2.check10_passes==1);
    radar_sw2_command(&sw2,"reset3");wait_sw2();REQUIRE(sw2.reset3_passes==1&&sw2.cli.version_valid);
    session.profile=&test_profile;
    REQUIRE(radar_session_command(&session,"capture confirm 123456789abcdef0")==1);
    REQUIRE(session.confirmed);
    model_flush_console(&console);
}
static void qualify(void)
{
    boot();model.halt_stuck=1;
    REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);wait_sw3();
    REQUIRE(sw3.checks_passed==0 && sw3.direct_reset_passes==1 &&
            !sw3.control_qualified && radar_sw3_capture_ready(&sw3));
    REQUIRE(model.dma_halt_writes==0);model.halt_stuck=0;
    qualify_io_profile();
}
static void dma_running(u32 bytes,u32 packets)
{
    boot();REQUIRE(radar_dma_open(&sw3.dma)==0);REQUIRE(radar_dma_prepare(&sw3.dma,bytes,packets,42U,1000000U)==0);REQUIRE(radar_dma_start(&sw3.dma)==0);
}
static void stop_dma(void)
{
    REQUIRE(radar_dma_stop_begin(&sw3.dma)>=0);model_advance(500U);REQUIRE(radar_dma_poll(&sw3.dma,model.now)==0);REQUIRE(sw3.dma.state==RADAR_DMA_HALTED);
}
static void reset_dma(void)
{
    REQUIRE(radar_dma_reset_begin(&sw3.dma)>=0);model_advance(500U);REQUIRE(radar_dma_poll(&sw3.dma,model.now)==0);REQUIRE(sw3.dma.state==RADAR_DMA_READY);
}
static void pass(const char *name){++tests_passed;printf("PASS %02u %s\n",tests_passed,name);}
static void local_command(const char *cmd)
{
    if (!radar_session_command(&session,cmd) && !radar_sw3_command(&sw3,cmd)) {
        radar_sw2_command(&sw2,cmd);
    }
    model_flush_console(&console);
}
static void clear_log(void) { model.log_count=0U;model.log[0]='\0'; }
static void test_readonly_diagnostics(void)
{
    u32 i,writes,reads,ops[128],pages[256],dma[64];
    const u8 optional_pages[]={RADAR_PAGE_BASELINE,0x0BU,0x0CU};
    boot();clear_log();local_command("status");
    REQUIRE(sw2.cli.queries==0U && strstr(model.log,"last=NOT_RUN") && !strstr(model.log,"last=PASS"));
    REQUIRE(RADAR_CLI_OK==0 && strcmp(radar_cli_result_name(RADAR_CLI_OK),"PASS")==0);
    radar_sw2_command(&sw2,"confirm io");radar_sw2_command(&sw2,"connect");wait_sw2();
    radar_sw2_command(&sw2,"query");wait_sw2();clear_log();local_command("status");
    REQUIRE(sw2.cli.queries==1U && sw2.cli.passed==1U && strstr(model.log,"last=PASS"));
    pass("UART last result is NOT_RUN before any query, real PASS afterward; enum ABI unchanged");

    boot();model.pages[RADAR_PAGE_SYS_FAULTS]=0x20U;model.pages[RADAR_PAGE_PHY_FAULT]=0x40U;
    memcpy(ops,model.ops,sizeof(ops));memcpy(pages,model.pages,sizeof(pages));memcpy(dma,model.dma,sizeof(dma));
    writes=model.gpio_writes;reads=model.ops[RADAR_OP_READ];clear_log();local_command("phy status");
    REQUIRE(strstr(model.log,"read_ok=1 consistent=1 observation=STABLE_TWO_PASSES passes=2 atomic=0"));
    REQUIRE(strstr(model.log,"sys_faults=00000020") && strstr(model.log,"phy_fault=00000040"));
    REQUIRE(strstr(model.log,"retained_taps=") && strstr(model.log,"No SNAPSHOT means no new tap measurement"));
    REQUIRE(model.ops[RADAR_OP_READ]>reads && model.gpio_writes-writes==model.ops[RADAR_OP_READ]-reads);
    for(i=0U;i<128U;++i){if(i!=RADAR_OP_READ){REQUIRE(model.ops[i]==ops[i]);}}
    REQUIRE(!memcmp(pages,model.pages,sizeof(pages)) && !memcmp(dma,model.dma,sizeof(dma)));
    REQUIRE(model.command_transcript_count==0U && model.dma_reset_writes==0U && model.dma_halt_writes==0U);
    REQUIRE(!session.owns && sw2.state==SW2_SAFE && sw3.job==SW3_IDLE && !sw3.reset_qualified);
    pass("live PHY diagnostics only READ pages, preserve faults/IO/DMA, label non-atomic retained tap copy");

    boot();clear_log();writes=model.gpio_writes;sw3.job=SW3_WAIT_RUN;
    local_command("phy status");REQUIRE(model.gpio_writes==writes && !strstr(model.log,"[PHY PAGE]"));
    sw3.job=SW3_IDLE;sw2.job=SW2_JOB_QUERY;clear_log();local_command("phy status");
    REQUIRE(model.gpio_writes==writes && strstr(model.log,"[REFUSE]"));
    sw2.job=SW2_JOB_NONE;sw2.cli.state=RADAR_CLI_RESPONSE;clear_log();local_command("phy status");
    REQUIRE(model.gpio_writes==writes && strstr(model.log,"[REFUSE]"));
    sw2.cli.state=RADAR_CLI_IDLE;sw2.state=SW2_CAPTURE;clear_log();
    REQUIRE(radar_sw3_command(&sw3,"phy status")==1);model_flush_console(&console);
    REQUIRE(model.gpio_writes==writes && strstr(model.log,"[REFUSE]"));
    session.owns=1U;clear_log();local_command("phy status");
    REQUIRE(model.gpio_writes==writes && strstr(model.log,"[BUSY]"));
    pass("PHY command rejects SW3/SW2/UART/SW4 ownership before all mailbox writes, including direct SW3 entry");

    boot();writes=model.gpio_writes;control.pending=1U;clear_log();local_command("phy status");
    REQUIRE(model.gpio_writes==writes && strstr(model.log,"no READ issued"));
    control.pending=0U;control.desynced=1U;clear_log();local_command("phy status");
    REQUIRE(model.gpio_writes==writes && strstr(model.log,"no READ issued"));
    control.desynced=0U;control.abi_verified=0U;clear_log();local_command("phy status");
    REQUIRE(model.gpio_writes==writes && strstr(model.log,"no READ issued"));
    pass("PHY command cannot touch a pending/desynchronized/unverified GPIO mailbox");

    for(i=0U;i<sizeof(optional_pages)/sizeof(optional_pages[0]);++i){
        boot();model.read_reject_page=optional_pages[i];model.read_reject_code=RADAR_GPIO_NOT_READY;
        clear_log();local_command("phy status");
        REQUIRE(strstr(model.log,"read_ok=1 consistent=1") && strstr(model.log,"=N/A (NOT_READY)"));
        REQUIRE(!control.desynced && !strstr(model.log,"read_ok=0"));
    }
    pass("unavailable baseline or retained tap pages are N/A, never fabricated zero measurements");

    boot();model.read_change_page=RADAR_PAGE_PHY_GEN;model.read_change_after=4U;
    clear_log();local_command("phy status");
    REQUIRE(strstr(model.log,"read_ok=1 consistent=0 observation=CHANGED") && !strstr(model.log,"consistent=1"));
    REQUIRE(model.read_change_count==6U);
    pass("generation changes between two bounded live passes are reported CHANGED, not consistent");

    boot();model.read_reject_page=RADAR_PAGE_PHY_FAULT;model.read_reject_code=RADAR_GPIO_BAD_VALUE;
    clear_log();local_command("phy status");
    REQUIRE(strstr(model.log,"read_ok=0 consistent=0") && strstr(model.log,"failed_page=14"));
    REQUIRE(!strstr(model.log,"[PHY PAGE]") && !strstr(model.log,"read_ok=1"));
    boot();model.read_reject_page=RADAR_PAGE_PHY_STATUS;model.read_reject_code=RADAR_GPIO_NOT_READY;
    clear_log();local_command("phy status");
    REQUIRE(strstr(model.log,"read_ok=0") && !strstr(model.log,"[PHY PAGE]"));
    boot();model.mailbox_stuck=1;writes=model.gpio_writes;clear_log();local_command("phy status");
    REQUIRE(control.desynced && model.gpio_writes==writes+1U && strstr(model.log,"read_ok=0"));
    REQUIRE(!strstr(model.log,"[PHY PAGE]") && !strstr(model.log,"read_ok=1"));
    pass("remote errors and missing ACK stop PHY diagnostics without publishing partial or valid data");

    boot();model.pages[RADAR_PAGE_FLAGS]|=RADAR_FLAG_SESSION_OPEN;
    reads=model.ops[RADAR_OP_READ];clear_log();local_command("phy status");
    REQUIRE(strstr(model.log,"PL_ACTIVE") && !strstr(model.log,"[PHY PAGE]"));
    REQUIRE(model.ops[RADAR_OP_READ]==reads+3U && model.ops[RADAR_OP_ABORT]==0U && model.ops[RADAR_OP_RECOVER]==0U);
    pass("observed PL activity stops further diagnostic reads without abort/reset/clear");
}
static void test_profile_guard(void)
{
    RadarCaptureProfile p=test_profile,unqualified=test_profile;u32 b,n;
    unqualified.approvals=0U;
    boot();session.profile=&unqualified;REQUIRE(radar_capture_profile_validate(session.profile)!=0);
    REQUIRE(radar_session_start(&session)==RADAR_SESSION_E_PROFILE);REQUIRE(model.starts==0&&model.ops[RADAR_OP_ARM]==0);
    REQUIRE(radar_capture_profile_validate(&p)==0);p.format_flags=32;REQUIRE(radar_capture_profile_validate(&p)!=0);
    p=test_profile;p.packet_bytes=131073;REQUIRE(radar_capture_profile_validate(&p)!=0);
    p=test_profile;p.packet_count=65;REQUIRE(radar_capture_profile_validate(&p)!=0);
    p=test_profile;p.use_delay=1;p.startup_mode=2;REQUIRE(radar_capture_profile_validate(&p)!=0);
    p=test_profile;p.start_command="sensorStart 0";REQUIRE(radar_capture_profile_validate(&p)!=0);
    REQUIRE(radar_dma_plan(131072,1,&b,&n)==0&&b==2&&n==1);
    REQUIRE(radar_dma_plan(131072,0,&b,&n)==0&&b==128&&n==64);
    REQUIRE(radar_dma_plan(8388600,1,&b,&n)==0&&b==128);
    REQUIRE(radar_dma_plan(8388608,1,&b,&n)<0);
    pass("default profile denies ARM/RF; finite geometry and configuration gate");
}
static void test_dma_controls(void)
{
    u32 resets,epoch;RadarDmaRegisters r;
    boot();REQUIRE(radar_sw3_command(&sw3,"dma check10")==1);wait_sw3();
    REQUIRE(sw3.checks_passed==10&&sw3.control_qualified&&sw3.checks_failed==0);
    REQUIRE(sw3.dma.ring->FreeCnt==128&&sw3.dma.ring->HwCnt==0&&sw3.dma.completed_packets==0);
    REQUIRE(model.starts==0&&model.ops[RADAR_OP_ARM]==0);pass("10 real-driver RUN/HALT/RESET no-input control cycles and canaries");
    REQUIRE(radar_sw3_command(&sw3,"dma check reset")==1);wait_sw3();
    REQUIRE(sw3.direct_reset_passes==1&&!sw3.control_qualified&&sw3.dma.halt_result==RADAR_DMA_PHASE_NOT_RUN);
    REQUIRE(radar_sw3_capture_ready(&sw3) && sw3.qualification_kind==RADAR_QUAL_RESET_CONTROL);
    pass("direct RUN-to-RESET qualifies entry independently, never fabricates no-input HALT_PASS");
    dma_running(131072,1);model.halt_stuck=1;resets=model.dma_reset_writes;
    REQUIRE(radar_dma_stop_begin(&sw3.dma)>=0);model_advance(12000U);
    REQUIRE(radar_dma_poll(&sw3.dma,model.now)==RADAR_DMA_E_HALT_TIMEOUT);
    REQUIRE(sw3.dma.state==RADAR_DMA_HALT_TIMEOUT&&model.dma_reset_writes==resets);
    REQUIRE(radar_dma_registers(&sw3.dma,&r)==0&&r.bd[0].valid&&r.bd[1].valid&&r.bd[0].control==65536U&&r.bd[0].buffer_lo==0x400000U);
    REQUIRE(radar_dma_reset_begin(&sw3.dma)<0&&model.dma_reset_writes==resets);
    epoch=sw3.dma.epoch;REQUIRE(radar_dma_reset_authorized(&sw3.dma)>=0);model_advance(500U);
    REQUIRE(radar_dma_poll(&sw3.dma,model.now)==0&&sw3.dma.state==RADAR_DMA_READY&&sw3.dma.epoch==epoch+1U);
    REQUIRE(sw3.dma.halt_result==RADAR_DMA_PHASE_TIMEOUT&&sw3.dma.reset_result==RADAR_DMA_PHASE_PASS);
    REQUIRE(model.dma_reset_writes==resets+1U&&sw3.dma.ring->FreeCnt==128);
    pass("HALT timeout isolates, actual BD diagnostics, one explicit reset preserves HALT_FAIL");
    dma_running(131072,1);REQUIRE(radar_dma_stop_begin(&sw3.dma)>=0);model_advance(12000U);
    REQUIRE(radar_dma_poll(&sw3.dma,model.now)==0&&sw3.dma.state==RADAR_DMA_HALTED&&sw3.dma.halt_result==RADAR_DMA_PHASE_LATE);
    REQUIRE(radar_dma_reset_begin(&sw3.dma)>=0);model_advance(12000U);
    REQUIRE(radar_dma_poll(&sw3.dma,model.now)==0&&sw3.dma.state==RADAR_DMA_READY&&sw3.dma.reset_result==RADAR_DMA_PHASE_LATE);
    REQUIRE(sw3.dma.last_error==RADAR_DMA_E_DEADLINE);pass("late poll observes physical stop/reset without falsely locking or passing latency");
    dma_running(131072,1);model.reset_stuck=1;REQUIRE(radar_dma_reset_authorized(&sw3.dma)>=0);resets=model.dma_reset_writes;model_advance(12000U);
    REQUIRE(radar_dma_poll(&sw3.dma,model.now)==RADAR_DMA_E_RESET_TIMEOUT&&sw3.dma.state==RADAR_DMA_LOCKED);
    REQUIRE(radar_dma_reset_authorized(&sw3.dma)<0&&radar_dma_reset_begin(&sw3.dma)<0&&radar_dma_stop_begin(&sw3.dma)<0);
    REQUIRE(model.dma_reset_writes==resets&&sw3.dma.ring->HwCnt==2);
    pass("RESET timeout is final locked; no retry or ring-memory reuse");
}
static void test_sg_ownership(void)
{
    RadarDmaPacket packet,stale;RadarDmaFragment f;u32 epoch;u32 *bd;
    dma_running(131080U,2U);model_complete_descriptors(&sw3.dma,2U);
    REQUIRE(radar_dma_poll(&sw3.dma,model.now)==2&&sw3.dma.completed_bytes==262160U);
    REQUIRE(radar_dma_acquire(&sw3.dma,&packet)==0&&packet.slot_count==3U&&packet.bytes==131080U);
    REQUIRE(radar_dma_fragment(&sw3.dma,&packet,2,&f)==0&&f.bytes==8U);
    REQUIRE(radar_dma_stop_begin(&sw3.dma)<0&&radar_dma_reset_authorized(&sw3.dma)<0);
    stale=packet;stale.epoch++;REQUIRE(radar_dma_fragment(&sw3.dma,&stale,0,&f)<0&&radar_dma_release(&sw3.dma,&stale)<0);
    REQUIRE(radar_dma_release(&sw3.dma,&packet)==0&&radar_dma_release(&sw3.dma,&packet)<0);
    REQUIRE(radar_dma_acquire(&sw3.dma,&packet)==0&&packet.sequence==2U);REQUIRE(radar_dma_release(&sw3.dma,&packet)==0);
    epoch=sw3.dma.epoch;stop_dma();reset_dma();REQUIRE(sw3.dma.epoch==epoch+1U&&radar_dma_fragment(&sw3.dma,&packet,0,&f)<0);
    pass("multi-BD nonuniform tail, exact length, FIFO-order lease, stale/double tokens, reset ownership");
    dma_running(131072,1);bd=(u32 *)(UINTPTR)sw3.dma.bd_base;bd[XAXIDMA_BD_STS_OFFSET/4U]=XAXIDMA_BD_STS_COMPLETE_MASK|XAXIDMA_BD_STS_RXSOF_MASK|65536U;
    REQUIRE(radar_dma_poll(&sw3.dma,model.now)==0&&sw3.dma.partial_prefix==1&&sw3.dma.packet_queued==0);
    model_advance(1100000U);REQUIRE(radar_dma_poll(&sw3.dma,model.now)==RADAR_DMA_E_PAYLOAD_TIMEOUT&&sw3.dma.packet_queued==0);
    pass("partial packet without EOF is not released; payload timeout distinct from HALT timeout");
    dma_running(131072,1);model_complete_descriptors(&sw3.dma,1);bd=(u32 *)(UINTPTR)sw3.dma.bd_base;bd[XAXIDMA_BD_STS_OFFSET/4U]-=8U;
    REQUIRE(radar_dma_poll(&sw3.dma,model.now)==RADAR_DMA_E_LENGTH&&sw3.dma.packet_queued==0);
    pass("short hardware actual length rejected before any consumer exposure");
    dma_running(131072,1);model_complete_descriptors(&sw3.dma,1);bd=(u32 *)(UINTPTR)sw3.dma.bd_base;bd[XAXIDMA_BD_STS_OFFSET/4U]&=~XAXIDMA_BD_STS_RXSOF_MASK;
    REQUIRE(radar_dma_poll(&sw3.dma,model.now)<0&&sw3.dma.packet_queued==0);
    pass("missing RXSOF rejects complete-looking descriptors");
    dma_running(131072,1);bd=(u32 *)(UINTPTR)sw3.dma.bd_base;bd[XAXIDMA_BD_STS_OFFSET/4U]=XAXIDMA_BD_STS_INT_ERR_MASK;
    REQUIRE(radar_dma_poll(&sw3.dma,model.now)==RADAR_DMA_E_HARDWARE&&sw3.dma.failure.bd_status==XAXIDMA_BD_STS_INT_ERR_MASK);
    pass("BD error detected even without COMPLETE or EOF");
}
static void test_mailbox(void)
{
    RadarControlReply reply;int rc;u32 ops;
    boot();ops=model.ops[RADAR_OP_IO_SET];
    REQUIRE(radar_control_command_begin(&control,RADAR_OP_IO_SET,0,3,1000)==0);
    model_advance(1200U);rc=radar_control_command_poll(&control,&reply);
    REQUIRE(rc==RADAR_CONTROL_E_LATE&&!control.desynced&&!control.pending&&control.late_acks==1);
    REQUIRE(model.ops[RADAR_OP_IO_SET]==ops+1U&&model.pages[0xe]==3U);
    pass("late matching GPIO ACK is accepted-as-late, no duplicate command or false desync");
    boot();model.mailbox_stuck=1;REQUIRE(radar_control_command_begin(&control,RADAR_OP_READ,0,0,1000)==0);model_advance(1200);
    REQUIRE(radar_control_command_poll(&control,&reply)==RADAR_CONTROL_E_TIMEOUT&&control.desynced);
    ops=model.gpio_writes;REQUIRE(radar_control_exec(&control,RADAR_OP_SAFE,0,0,NULL)<0&&model.gpio_writes==ops);
    pass("missing GPIO ACK latches desync and blocks blind cleanup writes");
}
static void test_cli(void)
{
    RadarCli c;int rc=1;u32 n;RadarCaptureProfile bad=test_profile;const char *evil[]={"sensorStart; sensorStop"};
    boot();radar_cli_init(&c);REQUIRE(radar_cli_prepare(&c)==0);REQUIRE(radar_cli_start_query(&c,model.now)==0);
    for(n=0;n<20000U&&radar_cli_busy(&c);++n){model_advance(500);radar_cli_poll(&c,model.now);}
    REQUIRE(radar_cli_take_result(&c,&rc)&&rc==0&&c.version_valid&&radar_capture_profile_version_matches(&test_profile,c.last_version));
    REQUIRE(radar_cli_start_capture(&c,&test_profile,RADAR_CAPTURE_CMD_CONFIG,0,model.now)==0);
    for(n=0;n<20000U&&radar_cli_busy(&c);++n){model_advance(500);radar_cli_poll(&c,model.now);}
    REQUIRE(radar_cli_take_result(&c,&rc)&&rc==0&&c.version_valid);
    bad.commands=evil;bad.command_count=1;REQUIRE(radar_cli_start_capture(&c,&bad,RADAR_CAPTURE_CMD_CONFIG,0,model.now)==RADAR_CLI_E_PROFILE);
    radar_cli_new_reset_epoch(&c);REQUIRE(!c.version_valid);
    pass("strict SDK version and generic approved Done/prompt; no raw UART passthrough; reset epoch invalidates identity");
    model.uart_echo_only=1;REQUIRE(radar_cli_start_query(&c,model.now)==0);
    for(n=0;n<20000U&&radar_cli_busy(&c);++n){model_advance(500);radar_cli_poll(&c,model.now);}
    REQUIRE(radar_cli_take_result(&c,&rc)&&rc<0&&c.needs_reset&&!c.version_valid);
    REQUIRE(radar_cli_start_query(&c,model.now)==RADAR_CLI_E_RESET_REQUIRED);
    pass("echo/prompt without version is not success; ambiguous timeout requires remote reset");
}
static void test_session_happy(void)
{
    u32 i,ops;char *logfile=getenv("RADAR_TEST_EXPORT_LOG");RadarCaptureProfile p=test_profile;
    qualify();REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_ARM_IDLE);
    ops=model.gpio_writes;radar_sw2_poll(&sw2);REQUIRE(model.gpio_writes==ops&&sw2.state==SW2_CAPTURE);
    REQUIRE(radar_session_command(&session,"dma reset")==1);REQUIRE(sw3.dma.state==RADAR_DMA_RUNNING);
    wait_session();REQUIRE(session.state==RADAR_SESSION_DONE&&session.result.valid&&!session.owns&&sw2.state==SW2_SAFE);
    REQUIRE(session.result.copy_begin_us>=session.result.complete_us);
    REQUIRE(session.result.copy_complete_us>=session.result.copy_begin_us);
    REQUIRE(session.result.ready_us>=session.result.copy_complete_us&&session.result.ready_us>session.result.complete_us);
    REQUIRE(model.starts==1&&model.stops==2&&sw3.dma.ring->FreeCnt==128&&session.result.bytes==131072);
    for(i=0U;i<session.result.bytes;++i){REQUIRE(((u8 *)(UINTPTR)radar_memory_capture_base())[i]==(u8)((i*17U+11U)&255U));}
    REQUIRE((radar_crc32_update(UINT32_MAX,(void *)(UINTPTR)radar_memory_capture_base(),session.result.bytes)^UINT32_MAX)==session.result.crc32);
    REQUIRE(session.result.dma_epoch+1U==sw3.dma.epoch);
    REQUIRE(radar_session_start(&session)==RADAR_SESSION_E_OWNER);
    pass("full finite session: real control+CLI+SG driver+fresh snapshot+copy+halt/reset/recover; exclusive ownership");
    model.log_count=0;model.log[0]='\0';REQUIRE(radar_session_command(&session,"capture export")==1);
    for(i=0;session.exporting||console.count;++i){REQUIRE(i<200000U);tick();}
    REQUIRE(strstr(model.log,"@R4BEGIN 1 131072 ")&&strstr(model.log,"@R4END 1 131072 "));
    if(logfile){FILE *f=fopen(logfile,"wb");REQUIRE(f!=NULL);REQUIRE(fwrite(model.log,1,model.log_count,f)==model.log_count);fclose(f);}
    pass("production bounded ASCII exporter outputs immutable store with metadata/CRC after teardown");
    REQUIRE(radar_session_command(&session,"capture clear")==1&&!session.result.valid);
    REQUIRE(!radar_sw3_capture_ready(&sw3));
    REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);wait_sw3();
    radar_sw2_command(&sw2,"connect");wait_sw2();radar_sw2_command(&sw2,"query");wait_sw2();
    p.packet_count=3;session.profile=&p;REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.result.valid&&session.result.session==2&&session.result.bytes==393216&&session.result.snapshot.words[4]==3);
    pass("second session increments identity, exact three packets, separate retained data and repeat-safe cleanup");
}
static void test_display_config_gap(void)
{
    u32 polls=0U,gaps=0U;
    qualify();REQUIRE(!radar_session_display_idle(&session));
    REQUIRE(radar_session_start(&session)==0);
    while(session.owns && session.state!=RADAR_SESSION_FAILED) {
        REQUIRE(++polls<100000U);tick();
        if(radar_session_display_idle(&session)) {
            RadarCliState saved=sw2.cli.state;
            REQUIRE(session.state==RADAR_SESSION_CONFIG && !model.source_running && model.starts==0U);
            REQUIRE(model.config_commands==session.config_index && session.config_index==++gaps);
            REQUIRE(!strcmp(model.last_command,cfg[gaps-1U]));
            control.pending=1U;REQUIRE(!radar_session_display_idle(&session));control.pending=0U;
            sw2.cli.state=RADAR_CLI_RESPONSE;REQUIRE(!radar_session_display_idle(&session));sw2.cli.state=saved;
            session.source=RADAR_SOURCE_RUNNING;REQUIRE(!radar_session_display_idle(&session));session.source=RADAR_SOURCE_STOPPED;
            /* A deliberately conservative 20ms render stall while no UART
             * reply is outstanding must not skip or duplicate a config. */
            model_advance(20000U);
        }
    }
    REQUIRE(gaps==test_profile.command_count-1U && model.config_commands==test_profile.command_count);
    REQUIRE(session.state==RADAR_SESSION_DONE && session.result.valid && model.starts==1U && model.stops==2U);
    REQUIRE(!radar_session_display_idle(&session) && !sw2.cli.total_uart_errors);
    pass("render gaps only between acknowledged config commands; 20ms pauses preserve exact command order and capture lifecycle");
}
static void rearm_reuse_test(void)
{
    REQUIRE(!session.owns && session.state==RADAR_SESSION_DONE);
    REQUIRE(radar_session_command(&session,"capture clear")==1);
    REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);wait_sw3();
    radar_sw2_command(&sw2,"connect");wait_sw2();
    radar_sw2_command(&sw2,"query");wait_sw2();
}
static u32 command_count_since(u32 first,const char *text)
{
    u32 i,n=0U;
    for(i=first;i<model.command_transcript_count;++i)
        if(!strcmp(model.command_transcript[i],text))++n;
    return n;
}
static void seed_reuse_test(void)
{
    qualify();local_command("capture reuse on");
    REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.state==RADAR_SESSION_DONE && session.result.valid);
    REQUIRE(model.config_commands==test_profile.command_count);
    REQUIRE(command_count_since(0U,"sensorStart")==1U && !command_count_since(0U,"sensorStart 0"));
}
static void test_config_reuse(void)
{
    u32 first,commands,starts,epoch;
    RadarCaptureProfile changed=test_profile;
    seed_reuse_test();rearm_reuse_test();first=model.command_transcript_count;
    commands=model.config_commands;epoch=sw2.cli.reset_epoch;
    REQUIRE(radar_session_start(&session)==0);
    local_command("capture reuse off"); /* A running owner cannot change policy. */
    wait_session();REQUIRE(session.state==RADAR_SESSION_DONE && session.result.valid);
    REQUIRE(model.config_commands==commands && sw2.cli.reset_epoch==epoch);
    REQUIRE(command_count_since(first,"sensorStop")==2U && command_count_since(first,"sensorStart 0")==1U);
    REQUIRE(!command_count_since(first,"sensorStart") && session.result.bytes==test_profile.packet_bytes);
    pass("opt-in reuse first configures fully, then keeps STOP/PHY/SG/ARM/CRC/teardown with exactly one sensorStart 0");

    rearm_reuse_test();local_command("capture reuse off");first=model.command_transcript_count;
    REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.result.valid && model.config_commands==commands+test_profile.command_count);
    REQUIRE(command_count_since(first,"sensorStart")==1U && !command_count_since(first,"sensorStart 0"));
    rearm_reuse_test();local_command("capture reuse on");first=model.command_transcript_count;
    REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.result.valid && command_count_since(first,"sensorStart")==1U);
    pass("reuse off and re-enable both require complete config; disabled policy never seeds reuse");

    rearm_reuse_test();epoch=sw2.cli.reset_epoch;
    radar_sw2_command(&sw2,"reset");wait_sw2();REQUIRE(sw2.cli.reset_epoch>epoch);
    first=model.command_transcript_count;commands=model.config_commands;
    REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.result.valid && model.config_commands==commands+test_profile.command_count);
    REQUIRE(command_count_since(first,"sensorStart")==1U && !command_count_since(first,"sensorStart 0"));
    pass("real modeled NRST epoch invalidates configuration cache even with same profile and valid version");

    rearm_reuse_test();changed.cfg_sha256="dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    session.profile=&changed;first=model.command_transcript_count;commands=model.config_commands;
    REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.result.valid && model.config_commands==commands+changed.command_count);
    REQUIRE(command_count_since(first,"sensorStart")==1U && !command_count_since(first,"sensorStart 0"));
    pass("changed compiled profile/config identity cannot reuse prior radar configuration");

    seed_reuse_test();rearm_reuse_test();first=model.command_transcript_count;commands=model.config_commands;
    model.uart_start_fail=1;REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED && !session.result.valid && sw2.cli.needs_reset && !session.reuse_valid);
    REQUIRE(model.config_commands==commands && command_count_since(first,"sensorStart 0")==1U);
    starts=model.starts;for(u32 i=0U;i<50U;++i)tick();
    REQUIRE(model.starts==starts && model.config_commands==commands && !command_count_since(first,"sensorStart"));
    pass("sensorStart 0 remote failure remains quarantined with no full-config fallback or RF retry");

    seed_reuse_test();rearm_reuse_test();first=model.command_transcript_count;
    model.uart_start_ignored=1;REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED && !session.result.valid && sw2.cli.needs_reset && !session.reuse_valid);
    REQUIRE(command_count_since(first,"sensorStart 0")==1U);
    pass("TI already-started Ignored plus Done cannot masquerade as a fresh reuse acquisition");

    seed_reuse_test();rearm_reuse_test();first=model.command_transcript_count;
    REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_RECEIVE);
    radar_session_cancel(&session);REQUIRE(session.state==RADAR_SESSION_FAILED && !session.result.valid && !session.reuse_valid);
    starts=model.starts;for(u32 i=0U;i<50U;++i)tick();
    REQUIRE(model.starts==starts && command_count_since(first,"sensorStart 0")==1U);
    pass("cancel during reused acquisition retains failure and performs no automatic source restart");
}
static void test_first_packet_evidence(void)
{
    RadarCaptureProfile p=test_profile;
    const char *logfile=getenv("RADAR_TEST_TRIAL_EXPORT_LOG");
    u32 i;
    qualify();p.qualification=RADAR_CAPTURE_FIRST_PACKET_TEST;p.source_review_flags=7U;
    p.approvals=0x83U;p.startup_mode=RADAR_CAPTURE_START_REVIEWED_PREAMBLE;
    session.profile=&p;session.confirmed=0U;model.no_rx_clock=1;
    model.log_count=0U;model.log[0]='\0';
    REQUIRE(radar_session_command(&session,"capture confirm 123456789abcdef0")==1);
    model_flush_console(&console);
    REQUIRE(session.confirmed && strstr(model.log,"[TEST CONFIRMED]") && !strstr(model.log,"[USER ATTESTED]"));
    REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.result.valid && session.result.bytes==131072U && model.starts==1U);
    REQUIRE(session.result.payload_halt_result==RADAR_DMA_PHASE_PASS && session.result.teardown_reset_result==RADAR_DMA_PHASE_PASS);
    model.log_count=0U;model.log[0]='\0';
    REQUIRE(radar_session_command(&session,"capture export")==1);
    for(i=0;session.exporting||console.count;++i){REQUIRE(i<200000U);tick();}
    REQUIRE(strstr(model.log,"@R4META qualification FIRST_PACKET_TEST") &&
        strstr(model.log,"@R4META review_bits 83 07") &&
        strstr(model.log,"@R4META validation_scope INPUT_TIMING_AND_WIRE_FORMAT_PENDING") &&
        strstr(model.log,"@R4END 1 131072 "));
    if(logfile){FILE *f=fopen(logfile,"wb");REQUIRE(f!=NULL);REQUIRE(fwrite(model.log,1,model.log_count,f)==model.log_count);fclose(f);}
    pass("first-packet experiment preserves teardown and exports its pending timing/wire evidence scope");
}
static void test_session_preamble_delay(void)
{
    RadarCaptureProfile p=test_profile;
    qualify();p.startup_mode=2;session.profile=&p;model.no_rx_clock=1;
    REQUIRE(radar_session_start(&session)==0);wait_session();REQUIRE(session.result.valid&&model.starts==1&&session.result.startup_mode==2);
    pass("reviewed-preamble mode queues ARM without RX clock; waits real qualification/snapshot, no fixed sleep proof");
    qualify();p=test_profile;p.use_delay=1;p.taps[0]=40;p.taps[1]=41;p.taps[2]=42;session.profile=&p;
    REQUIRE(radar_session_start(&session)==0);wait_session();REQUIRE(session.result.valid&&model.ops[RADAR_OP_DELAY_APPLY]==1&&session.result.snapshot.taps==(40U|(41U<<9U)|(42U<<18U)));
    pass("absolute delay apply above measured baseline, terminal generation, actual tap readback");
    qualify();p=test_profile;p.use_delay=1;p.taps[0]=0;p.taps[1]=31;p.taps[2]=32;session.profile=&p;
    REQUIRE(radar_session_start(&session)==0);wait_session();REQUIRE(session.state==RADAR_SESSION_FAILED&&model.ops[RADAR_OP_DELAY_APPLY]==0&&model.starts==0);
    pass("below-baseline absolute delay rejected before write/apply/start");
}
static void test_session_failures(void)
{
    u32 resets,recovers,n;RadarCaptureProfile p=test_profile;
    qualify();model.no_payload=1;REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED&&!session.result.valid&&session.source==RADAR_SOURCE_UNKNOWN&&sw2.expected_io==0);
    resets=model.dma_reset_writes;recovers=model.ops[RADAR_OP_RECOVER];
    for(n=0;n<100;++n){tick();}REQUIRE(model.dma_reset_writes==resets&&model.ops[RADAR_OP_RECOVER]==recovers&&session.owns);
    REQUIRE(radar_session_recover(&session)==0);wait_session();
    REQUIRE(session.state==RADAR_SESSION_IDLE&&!session.owns&&!session.result.valid&&sw2.state==SW2_SAFE);
    REQUIRE(model.producer_resets>=4&&model.dma_reset_writes==resets+1&&model.ops[RADAR_OP_RECOVER]==recovers+1);
    REQUIRE(!sw2.cli.version_valid&&!session.confirmed&&!sw3.control_qualified);
    pass("no payload fails/quarantines; high-Z is not stop; explicit NRST/one reset/RECOVER with requalification");
    qualify();model.wrong_snapshot_session=1;REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED&&!session.result.valid&&session.failed_phase==RADAR_SESSION_SNAPSHOT);
    pass("wrong fresh snapshot session rejected despite valid-looking DMA completion");
    qualify();model.snapshot_stuck=1;REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED&&session.failed_phase==RADAR_SESSION_SNAPSHOT&&!session.result.valid);
    pass("snapshot ACK without new generation never certifies data");
    qualify();model.phy_stuck=1;REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED&&session.failed_phase==RADAR_SESSION_RECAL&&model.starts==0);
    pass("RECAL ACK alone is not PHY completion; timeout blocks start");
    qualify();model.uart_start_fail=1;REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED&&session.source==RADAR_SOURCE_UNKNOWN&&!session.result.valid&&model.source_running);
    pass("sensorStart remote error leaves source UNKNOWN, does not claim stopped RF");
    qualify();p.receive_timeout_us=100000;session.profile=&p;model.no_rx_clock=1;
    REQUIRE(radar_session_start(&session)==0);wait_session();REQUIRE(session.state==RADAR_SESSION_FAILED&&model.starts==0);
    pass("IDLE_CLOCK mode without real pre-start RX clock blocks sensorStart");
    qualify();REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_START);
    radar_session_cancel(&session);REQUIRE(session.state==RADAR_SESSION_FAILED&&!session.result.valid&&session.source==RADAR_SOURCE_UNKNOWN);
    model.reset_stuck=1;for(n=0;n<50;++n){tick();}REQUIRE(radar_session_recover(&session)==0);wait_session();
    REQUIRE(sw3.dma.state==RADAR_DMA_LOCKED&&session.state==RADAR_SESSION_FAILED&&session.source==RADAR_SOURCE_UNKNOWN);
    resets=model.dma_reset_writes;REQUIRE(radar_session_recover(&session)<0&&model.dma_reset_writes==resets);
    pass("canceled START plus recovery RESET timeout stays final-locked, no automated second reset");
    qualify();REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_START);
    model.pages[1]&=~RADAR_FLAG_PG;for(n=0;n<10&&session.state!=RADAR_SESSION_FAILED;++n){tick();}
    REQUIRE(session.state==RADAR_SESSION_FAILED&&!session.result.valid);pass("power loss fails active session and invalidates result");
}
static void test_boundary_and_handoff(void)
{
    RadarCaptureProfile p=test_profile;u32 i;
    qualify();p.packet_count=64U;session.profile=&p;
    REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.result.valid && session.result.bytes==8388608U &&
        session.result.snapshot.words[4]==64U && sw3.dma.ring->FreeCnt==128);
    for(i=0;i<session.result.bytes;i+=4093U){
        REQUIRE(((u8 *)(UINTPTR)radar_memory_capture_base())[i]==(u8)((i*17U+11U)&255U));
    }
    REQUIRE((radar_crc32_update(UINT32_MAX,(void *)(UINTPTR)radar_memory_capture_base(),session.result.bytes)^UINT32_MAX)==session.result.crc32);
    pass("maximum finite 8MiB / 128 BD / 64 packet result; bounded copying and exact CRC (not throughput proof)");
    qualify();REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_RECOVER_AFTER);
    sw2.io_verified=0U;wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED && session.owns && !session.result.valid &&
        session.captures_passed==0U && sw2.state==SW2_CAPTURE);
    pass("final SW4-to-SW2 handoff readback failure cannot become PASS or surrender ownership");
    qualify();radar_sw2_command(&sw2,"safe");
    REQUIRE(sw2.state==SW2_SAFE);model.halt_stuck=1;
    REQUIRE(radar_sw3_command(&sw3,"dma check")==1);wait_sw3();
    REQUIRE(sw3.direct_reset_passes==1 && sw3.checks_failed==1 && !sw3.control_qualified && !radar_sw3_capture_ready(&sw3));
    pass("historical SW3 PASS cannot authorize a capture after a newer failed control test");
}
static void test_sw32_board_regression(void)
{
    u32 resets,epoch,n;
    boot();model.halt_stuck=1;
    REQUIRE(radar_sw3_command(&sw3,"dma check")==1);wait_sw3();
    REQUIRE(sw3.dma.state==RADAR_DMA_HALT_TIMEOUT && sw3.checks_failed==1 && !sw3.reset_qualified);
    resets=model.dma_reset_writes;
    model_advance(353721839U); /* replicate operator waiting at quarantine */
    REQUIRE(radar_sw3_command(&sw3,"dma reset")==1);wait_sw3();
    REQUIRE(sw3.dma.state==RADAR_DMA_READY && sw3.dma.reset_result==RADAR_DMA_PHASE_PASS &&
        model.dma_reset_writes==resets+1U && sw3.dma.max_poll_gap_us<10000U &&
        !radar_sw3_capture_ready(&sw3) && sw3.direct_reset_passes==0U);
    epoch=sw3.dma.epoch;
    REQUIRE(radar_sw3_command(&sw3,"dma check reset")==1);wait_sw3();
    REQUIRE(radar_sw3_capture_ready(&sw3) && !sw3.control_qualified &&
        sw3.dma.epoch==epoch+1U && sw3.direct_reset_passes==1U && sw3.checks_failed==1U &&
        sw3.dma.halt_result==RADAR_DMA_PHASE_NOT_RUN && sw3.dma.halt_elapsed_us==0U &&
        sw3.dma.max_poll_gap_us<10000U && sw3.dma.reset_elapsed_us<10000U);
    pass("board log sequence: HALT timeout, 353s wait, explicit reset, direct test; no stale time or fake qualification");
    model.halt_stuck=0;qualify_io_profile();
    REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(session.result.valid && sw3.checks_failed==1U && sw3.checks_passed==0U &&
        session.result.admission_kind==RADAR_QUAL_RESET_CONTROL);
    pass("historical no-input HALT failure does not block fresh reset-qualified real-payload session");

    boot();model.halt_stuck=1;
    REQUIRE(radar_sw3_command(&sw3,"dma qualify10")==1);
    for(n=0;sw3.job!=SW3_IDLE;++n) {
        REQUIRE(n<100000U);REQUIRE(!sw3.reset_qualified);tick();
    }
    REQUIRE(sw3.direct_reset_passes==10U && sw3.batch_done==10U && radar_sw3_capture_ready(&sw3));
    REQUIRE(sw3.qualification_epoch==sw3.dma.epoch && sw3.qualification_reset_count==sw3.dma.reset_count);
    REQUIRE(model.dma_halt_writes==0U && model.starts==0U && model.ops[RADAR_OP_ARM]==0U);
    pass("qualify10 issues exactly one permission at final epoch, no intermediate pass licenses capture");

    boot();REQUIRE(radar_sw3_command(&sw3,"dma check10 reset")==1);
    for(n=0;sw3.batch_done<2U;++n){REQUIRE(n<10000U);tick();}
    REQUIRE(!sw3.reset_qualified);model.reset_stuck=1;wait_sw3();
    REQUIRE(sw3.direct_reset_passes==2U && sw3.checks_failed==1U && !radar_sw3_capture_ready(&sw3));
    REQUIRE(sw3.dma.state==RADAR_DMA_LOCKED);
    pass("batch failure after earlier successful reset rounds never grants admission");
}
static void test_qualification_expiration(void)
{
    u32 resets,epoch;
    boot();sw3.direct_reset_passes=100U;sw3.checks_passed=100U;sw3.control_qualified=1U;
    REQUIRE(!radar_sw3_capture_ready(&sw3));
    pass("historical counters alone cannot create a permission");
    boot();REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);wait_sw3();
    REQUIRE(radar_sw3_capture_ready(&sw3));epoch=sw3.qualification_epoch;
    reset_dma();
    REQUIRE(sw3.dma.epoch==epoch+1U && !radar_sw3_capture_ready(&sw3) &&
        sw3.qualification_reason==RADAR_QUAL_STALE_EPOCH);
    pass("out-of-band real reset and ring epoch change invalidate an old permission");
    boot();REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);wait_sw3();
    REQUIRE(radar_sw3_command(&sw3,"dma reset")==1);wait_sw3();
    REQUIRE(!radar_sw3_capture_ready(&sw3) && sw3.qualification_reason==RADAR_QUAL_EXPLICIT_RESET);
    pass("manual reset clears permission and is never rebranded as qualification");
    boot();REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);wait_sw3();
    REQUIRE(radar_dma_prepare(&sw3.dma,131072U,1U,55U,1000000U)==0);
    REQUIRE(!radar_sw3_capture_ready(&sw3) && sw3.qualification_reason==RADAR_QUAL_DMA_NOT_READY);
    pass("staged descriptors invalidate READY/all-free permission, even within same epoch");
    boot();REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);wait_sw3();
    model.dma[(XAXIDMA_RX_OFFSET+XAXIDMA_SR_OFFSET)/4U]|=XAXIDMA_ERR_INTERNAL_MASK;
    REQUIRE(!radar_sw3_capture_ready(&sw3) && !sw3.reset_qualified);
    pass("live hardware fault overrides apparently READY software state");
    boot();REQUIRE(radar_sw3_command(&sw3,"dma qualify10")==1);
    while(sw3.batch_done<1U){tick();}resets=model.dma_reset_writes;
    REQUIRE(radar_sw3_command(&sw3,"safe")==0);
    REQUIRE(!radar_sw3_capture_ready(&sw3) && sw3.qualification_reason==RADAR_QUAL_CANCELED);
    REQUIRE(model.dma_reset_writes==resets);
    pass("canceling a qualification batch invalidates partial progress without automatic reset");
}
static void test_preflight_and_consumption(void)
{
    u32 op[128],writes,resets;RadarQualificationKind kind;u32 epoch;
    qualify();memcpy(op,model.ops,sizeof(op));writes=model.command_transcript_count;resets=model.dma_reset_writes;
    kind=sw3.qualification_kind;epoch=sw3.qualification_epoch;
    REQUIRE(radar_session_preflight(&session)==0U);
    REQUIRE(radar_session_command(&session,"capture preflight")==1);
    REQUIRE(radar_sw3_capture_ready(&sw3) && sw3.qualification_epoch==epoch && sw3.qualification_kind==kind);
    REQUIRE(model.command_transcript_count==writes && model.dma_reset_writes==resets);
    for(u32 i=0U;i<128U;++i){if(i!=RADAR_OP_READ){REQUIRE(model.ops[i]==op[i]);}}
    pass("ready preflight is read-only, does not TX, ARM, reset or consume epoch permission");
    session.confirmed=0U;
    REQUIRE(radar_session_start(&session)==RADAR_SESSION_E_PROFILE && radar_sw3_capture_ready(&sw3));
    session.confirmed=1U;sw2.check10_passes=0U;
    REQUIRE(radar_session_preflight(&session)&RADAR_PREFLIGHT_SW2_TESTS);
    REQUIRE(radar_session_start(&session)==RADAR_SESSION_E_PREREQUISITE && radar_sw3_capture_ready(&sw3));
    sw2.check10_passes=1U;sw2.cli.version_valid=0U;
    REQUIRE(radar_session_preflight(&session)&RADAR_PREFLIGHT_VERSION);
    REQUIRE(model.command_transcript_count==writes);
    pass("missing profile attestation, SW2 batch, or current version blocks entry without consuming valid DMA evidence");
    qualify();epoch=sw3.dma.epoch;
    REQUIRE(radar_session_start(&session)==0 && !sw3.reset_qualified &&
        sw3.qualification_reason==RADAR_QUAL_CONSUMED && session.admission_epoch==epoch &&
        session.admission_kind==RADAR_QUAL_RESET_CONTROL);
    REQUIRE(radar_session_start(&session)==RADAR_SESSION_E_OWNER);
    wait_session();REQUIRE(session.result.valid && session.result.payload_halt_result==RADAR_DMA_PHASE_PASS &&
        session.result.teardown_reset_result==RADAR_DMA_PHASE_PASS && session.normal_halt_passes==1U);
    REQUIRE(!radar_sw3_capture_ready(&sw3));
    REQUIRE(radar_session_command(&session,"capture clear")==1);
    radar_sw2_command(&sw2,"connect");wait_sw2();radar_sw2_command(&sw2,"query");wait_sw2();
    writes=model.command_transcript_count;
    REQUIRE(radar_session_start(&session)==RADAR_SESSION_E_PREREQUISITE && model.command_transcript_count==writes);
    pass("entry consumes once, full normal HALT/RESET yields result, clear/reconnect cannot reuse old permission");
}
static void test_sw41_strict_teardown(void)
{
    u32 resets;
    qualify();REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_STOP_AFTER);
    REQUIRE(sw3.dma.completed_packets==1U && sw3.dma.ring->HwCnt==0);
    model.halt_stuck=1;resets=model.dma_reset_writes;wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED && session.failed_phase==RADAR_SESSION_HALT &&
        !session.result.valid && sw3.dma.state==RADAR_DMA_HALT_TIMEOUT &&
        model.dma_reset_writes==resets && !radar_sw3_capture_ready(&sw3));
    REQUIRE(radar_session_command(&session,"capture export")==1 && !session.exporting);
    REQUIRE(radar_session_recover(&session)==0);wait_session();
    REQUIRE(session.state==RADAR_SESSION_IDLE && !session.result.valid &&
        session.captures_passed==0 && !radar_sw3_capture_ready(&sw3) && model.dma_reset_writes==resets+1U);
    pass("full data followed by HALT timeout stays invalid; authorized RESET recovery cannot promote capture");
    qualify();REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_COPY);
    model.reset_stuck=1;wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED && session.failed_phase==RADAR_SESSION_RESET &&
        session.copy_bytes==131072U && !session.result.valid && sw3.dma.state==RADAR_DMA_LOCKED);
    resets=model.dma_reset_writes;
    REQUIRE(radar_session_recover(&session)<0 && model.dma_reset_writes==resets);
    pass("CPU copy/CRC alone cannot pass teardown RESET; timeout is final lock without reuse");
    qualify();REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_HALT);
    model_advance(12000U);radar_session_poll(&session);
    REQUIRE(session.state==RADAR_SESSION_FAILED && !session.result.valid &&
        sw3.dma.halt_result==RADAR_DMA_PHASE_LATE);
    pass("late normal HALT is physically quiet but fails SW4 result latency criterion");
}
static void test_memory_guard_and_missing_eof(void)
{
    u32 resets,first,second;volatile u32 *bd;
    boot();REQUIRE(radar_dma_open(&sw3.dma)==0);
    *(volatile u32 *)sw3.dma.rx_base=UINT32_C(0x12345678);
    resets=model.dma_reset_writes;
    model.dma[XAXIDMA_RX_OFFSET/4U]|=XAXIDMA_CR_RUNSTOP_MASK;
    model.dma[(XAXIDMA_RX_OFFSET+XAXIDMA_SR_OFFSET)/4U]&=~XAXIDMA_HALTED_MASK;
    REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);
    REQUIRE(sw3.job==SW3_IDLE && !sw3.reset_qualified &&
        *(volatile u32 *)sw3.dma.rx_base==UINT32_C(0x12345678) && model.dma_reset_writes==resets);
    REQUIRE(radar_dma_prepare(&sw3.dma,131072,1,1,1000000)==RADAR_DMA_E_STATE);
    pass("physical quiescence checked before canary writes/staging, not just READY enum");
    boot();REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);wait_sw3();
    bd=(volatile u32 *)sw3.dma.bd_base;bd[XAXIDMA_BD_NDESC_OFFSET/4U]=0U;
    REQUIRE(!radar_sw3_capture_ready(&sw3) && !sw3.reset_qualified && sw3.dma.state==RADAR_DMA_FAULT);
    pass("corrupted all-free ring rejects admission before any radar command");
    qualify();model.no_payload=1;REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_RECEIVE);
    model_complete_descriptors(&sw3.dma,1);
    bd=(volatile u32 *)sw3.dma.bd_base;first=bd[XAXIDMA_BD_NDESC_OFFSET/4U];
    bd=(volatile u32 *)(UINTPTR)first;bd[XAXIDMA_BD_STS_OFFSET/4U]&=~XAXIDMA_BD_STS_RXEOF_MASK;
    model.pages[1]|=RADAR_FLAG_SOURCE_COMPLETE;second=model.ops[RADAR_OP_SNAPSHOT];
    wait_session();
    REQUIRE(session.state==RADAR_SESSION_FAILED && !session.result.valid && session.copy_bytes==0U &&
        sw3.dma.packet_queued==0U && model.ops[RADAR_OP_SNAPSHOT]==second);
    pass("SOURCE_COMPLETE with all BDs COMPLETE but missing EOF cannot produce a valid SW4 packet");
}
static void test_raw_lease_prefetch(void)
{
    RadarCaptureResult prior;RadarCaptureProfile p=test_profile;u32 i,resets;
    for(p.packet_count=8U;p.packet_count<=16U;p.packet_count*=2U) {
        qualify();p.packet_bytes=399360U;session.profile=&p;
        REQUIRE(radar_session_start(&session)==0);wait_session();
        REQUIRE(session.result.valid&&model.pages[0x40]==99840U&&model.pages[0x41]==p.packet_count);
        REQUIRE(radar_session_lease_result(&session,&prior)==0&&session.raw_store_owner==prior.session);
        REQUIRE(prior.valid&&!session.result.valid&&session.state==RADAR_SESSION_IDLE);
        REQUIRE(radar_session_lease_result(&session,&prior)==RADAR_SESSION_E_OWNER);
        REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);wait_sw3();
        radar_sw2_command(&sw2,"connect");wait_sw2();radar_sw2_command(&sw2,"query");wait_sw2();
        REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_COPY);
        REQUIRE(session.source==RADAR_SOURCE_STOPPED&&sw3.dma.state==RADAR_DMA_HALTED);
        *(u8 *)(UINTPTR)radar_memory_rx_base()=0xa6U;resets=model.dma_reset_writes;
        for(i=0U;i<100U;++i)tick();
        REQUIRE(session.state==RADAR_SESSION_COPY&&session.copy_bytes==0U&&session.copy_wait_polls>=100U);
        REQUIRE(!session.result.valid&&sw3.dma.packet_queued==p.packet_count&&model.dma_reset_writes==resets);
        REQUIRE(*(u8 *)(UINTPTR)radar_memory_capture_base()==11U);
        REQUIRE(radar_session_release_raw(&session,prior.session+1U)==RADAR_SESSION_E_OWNER);
        REQUIRE(radar_session_release_raw(&session,prior.session)==0);wait_session();
        REQUIRE(session.result.valid&&*(u8 *)(UINTPTR)radar_memory_capture_base()==0xa6U);
        for(i=0U;i<p.packet_count;++i)REQUIRE(session.result.frame_crc32[i]==
            (radar_crc32_update(UINT32_MAX,(const u8 *)(UINTPTR)radar_memory_capture_base()+i*p.packet_bytes,p.packet_bytes)^UINT32_MAX));
    }
    pass("production8/16 finite RX prefetch waits HALTED for immutable RAW lease; per-frame CRC and packet pairs/limit exact");
    qualify();REQUIRE(radar_session_start(&session)==0);wait_session();
    REQUIRE(radar_session_lease_result(&session,&prior)==0);
    REQUIRE(radar_sw3_command(&sw3,"dma qualify")==1);wait_sw3();
    radar_sw2_command(&sw2,"connect");wait_sw2();radar_sw2_command(&sw2,"query");wait_sw2();
    REQUIRE(radar_session_start(&session)==0);wait_phase(RADAR_SESSION_COPY);resets=model.dma_reset_writes;
    model_advance(30000000U);radar_session_poll(&session);
    REQUIRE(session.state==RADAR_SESSION_FAILED&&session.failed_phase==RADAR_SESSION_COPY&&session.raw_store_owner==prior.session);
    REQUIRE(session.copy_bytes==0U&&!session.result.valid&&model.dma_reset_writes==resets);
    pass("stalled RAW consumer produces bounded COPY ownership failure; preserves oldRAW and newRX without blind reset");
}
int main(void)
{
    test_readonly_diagnostics();
    test_memory_guard_and_missing_eof();test_sw32_board_regression();test_qualification_expiration();test_preflight_and_consumption();test_sw41_strict_teardown();
    test_profile_guard();test_dma_controls();test_sg_ownership();test_mailbox();test_cli();
    test_session_happy();test_display_config_gap();test_config_reuse();test_first_packet_evidence();test_session_preamble_delay();test_session_failures();test_boundary_and_handoff();
    test_raw_lease_prefetch();
    REQUIRE((radar_crc32_update(UINT32_MAX,"123456789",9U)^UINT32_MAX)==0xcbf43926U);pass("CRC32 ISO-HDLC golden vector");
    model_destroy_dma(&sw3.dma);printf("ALL %u HOST REGRESSION GROUPS PASSED (model evidence, not board evidence)\n",tests_passed);return 0;
}
