/* Host-only frozen geometry and production CLI tests using the existing UART
 * behavioral model. No serial/JTAG, source timing or real ADC proof. */
#include "model.h"
#include "radar_memory.h"
#include "radar_crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const commands[]={
    "flushCfg", "dfeDataOutputMode 1", "channelCfg 15 5 0", "adcCfg 2 1",
    "adcbufCfg -1 0 1 1 1", "profileCfg 0 60 7 3 24 0 0 166 1 256 12500 0 0 158",
    "chirpCfg 0 0 0 0 0 0 0 1", "chirpCfg 1 1 0 0 0 0 0 4", "bpmCfg -1 0 0 0",
    "lowPower 0 0", "frameCfg 0 1 32 1 100 1 0", "lvdsStreamCfg -1 1 1 0"
};
static const RadarCaptureProfile fixture={
    .schema=1U,.approvals=0x83U,.qualification=RADAR_CAPTURE_FINITE_FRAME_TEST,.source_review_flags=7U,
    .name="HOST_ONLY_FINITE",.profile_id="123456789abcdef0",
    .firmware_sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    .cfg_sha256="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    .review_sha256="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    .version_substring="03.05.00.04",.lanes=2U,.lane_mbps=600U,.word_bits=16U,.format_flags=8U,
    .packet_bytes=266240U,.packet_count=1U,.skip_pairs=0U,
    .startup_mode=RADAR_CAPTURE_START_REVIEWED_PREAMBLE,.receive_timeout_us=4000000U,.snapshot_timeout_us=500000U,
    .use_delay=0U,.taps={0U,0U,0U},.command_count=12U,.commands=commands,
    .start_command="sensorStart",.stop_command="sensorStop"
};
static unsigned checks;
#define CHECK(e) do { ++checks; if (!(e)) { fprintf(stderr,"FAIL %u: %s\n",(unsigned)__LINE__,#e);exit(1); } } while (0)
static RadarCli cli;
static void begin(const RadarCaptureProfile *p,int kind)
{
    unsigned n;
    model_init(); model.uart_silent=1; radar_cli_init(&cli);
    CHECK(radar_cli_prepare(&cli)==0);
    CHECK((kind<0?radar_cli_start_query(&cli,model.now):
           radar_cli_start_capture(&cli,p,(RadarCaptureCommand)kind,0U,model.now))==0);
    for(n=0;n<1000U && cli.state!=RADAR_CLI_RESPONSE;++n) {
        model_advance(500U);radar_cli_poll(&cli,model.now);
    }
    CHECK(cli.state==RADAR_CLI_RESPONSE);
}
static int response(const char *text)
{
    unsigned n; int result=99;
    model_force_uart(text);
    for(n=0;n<20000U && radar_cli_busy(&cli);++n) { model_advance(500U);radar_cli_poll(&cli,model.now); }
    CHECK(radar_cli_take_result(&cli,&result));
    return result;
}
static void geometry_tests(void)
{
    RadarCaptureProfile p=fixture; RadarFiniteGeometry geo;
    const char *changed[14];unsigned i;
    CHECK(radar_capture_profile_validate(radar_capture_profile_get())==0);
    CHECK(radar_capture_profile_finite_geometry(radar_capture_profile_get(),&geo)==0);
    CHECK(geo.tx_count==3U && geo.chirps_per_frame==96U && geo.wire_bytes_per_frame==399360U);
    for(i=8U;i<=16U;i*=2U) {
        const RadarCaptureProfile *batch=radar_capture_profile_batch(i);
        CHECK(batch && radar_capture_profile_supported(batch));
        CHECK(radar_capture_profile_validate(batch)==0);
        CHECK(radar_capture_profile_finite_geometry(batch,&geo)==0);
        CHECK(geo.source_frames==i && geo.tx_count==3U && geo.wire_bytes_per_frame==399360U);
        CHECK(batch->packet_count==i && strcmp(batch->profile_id,radar_capture_profile_get()->profile_id));
    }
    CHECK(!radar_capture_profile_batch(2U));
    CHECK(radar_capture_profile_validate(&p)==0);
    CHECK(radar_capture_profile_finite_geometry(&p,&geo)==0);
    CHECK(geo.hsi_header_enabled==1U && geo.source_frames==1U && geo.wire_bytes_per_frame==266240U);
    CHECK(geo.chirps_per_frame==64U && geo.samples_per_rx==256U && geo.rx_mask==15U && geo.expected_header_bytes==64U);
    memcpy(changed,commands,sizeof(commands));p.commands=changed;
    changed[11]="lvdsStreamCfg -1 0 1 0";p.packet_bytes=262144U;
    CHECK(radar_capture_profile_validate(&p)==0);
    CHECK(radar_capture_profile_finite_geometry(&p,&geo)==0 && geo.hsi_header_enabled==0U && geo.expected_header_bytes==0U);
    changed[10]="frameCfg 0 1 32 16 100 1 0";p.packet_count=16U;
    CHECK(radar_capture_profile_validate(&p)==0);
    CHECK(radar_capture_profile_finite_geometry(&p,&geo)==0 && geo.source_frames==16U);
    p.packet_count=1U;CHECK(radar_capture_profile_validate(&p)!=0);
    p=fixture;p.commands=changed;
    for(i=0U;i<12U;++i) {
        memcpy(changed,commands,sizeof(commands));changed[i]="guiMonitor -1 0 0 0 0 0 0";
        CHECK(radar_capture_profile_validate(&p)!=0);
        memcpy(changed,commands,sizeof(commands));changed[12]=commands[i];p.command_count=13U;
        CHECK(radar_capture_profile_validate(&p)!=0);p.command_count=12U;
    }
    memcpy(changed,commands,sizeof(commands));changed[4]="adcbufCfg -1 0 0 1 1";
    CHECK(radar_capture_profile_validate(&p)!=0);
    memcpy(changed,commands,sizeof(commands));changed[12]="advFrameCfg 1 0 1 1 0";p.command_count=13U;
    CHECK(radar_capture_profile_validate(&p)!=0);
    p=fixture;p.packet_bytes=131072U;CHECK(radar_capture_profile_validate(&p)!=0);
    p=fixture;p.qualification=RADAR_CAPTURE_FIRST_PACKET_TEST;CHECK(radar_capture_profile_validate(&p)!=0);
    p.packet_bytes=131072U;CHECK(radar_capture_profile_validate(&p)==0);
    p=fixture;p.approvals=0xffU;p.qualification=RADAR_CAPTURE_QUALIFIED;CHECK(radar_capture_profile_validate(&p)==0);
    for(i=0U;i<3U;++i) { p=fixture;p.source_review_flags&=~(1U<<i);CHECK(radar_capture_profile_validate(&p)!=0); }
    for(i=0U;i<8U;++i) { if ((0x83U>>i)&1U) {p=fixture;p.approvals&=~(1U<<i);CHECK(radar_capture_profile_validate(&p)!=0);} }
    p=fixture;memcpy(changed,commands,sizeof(commands));p.commands=changed;p.command_count=13U;
    changed[2]="channelCfg 15 7 0";changed[10]="frameCfg 0 2 32 1 100 1 0";
    changed[12]="chirpCfg 2 2 0 0 0 0 0 2";p.packet_bytes=399360U;
    CHECK(radar_capture_profile_validate(&p)==0);
    CHECK(radar_capture_profile_finite_geometry(&p,&geo)==0);
    CHECK(geo.tx_count==3U && geo.tx_mask==7U && geo.loops==32U && geo.chirps_per_frame==96U);
    CHECK(geo.tx_order[0]==0U && geo.tx_order[1]==2U && geo.tx_order[2]==1U);
    CHECK(geo.wire_bytes_per_frame==399360U);
    p.packet_bytes=266240U;CHECK(radar_capture_profile_validate(&p)!=0);p.packet_bytes=399360U;
    changed[12]="chirpCfg 2 2 0 0 0 0 0 4";CHECK(radar_capture_profile_validate(&p)!=0);
    changed[12]="chirpCfg 2 2 0 0 0 0 0 2";
    changed[11]="lvdsStreamCfg -1 0 1 0";p.packet_bytes=393216U;CHECK(radar_capture_profile_validate(&p)!=0);
    changed[11]="lvdsStreamCfg -1 1 1 0";p.packet_bytes=399360U;
    changed[10]="frameCfg 0 2 32 0 100 1 0";CHECK(radar_capture_profile_validate(&p)!=0);
    changed[10]="frameCfg 0 2 32 1 100 1 0";p.command_count=12U;CHECK(radar_capture_profile_validate(&p)!=0);
    p.command_count=14U;changed[13]=changed[12];CHECK(radar_capture_profile_validate(&p)!=0);
}
static void cli_tests(void)
{
    static const char warning[]="mmWave Stop error ignored [Error code: -1 Subsystem: 0]\r\n";
    static const char accepted[]="sensorStop\r\nmmWave Stop error ignored [Error code: -1 Subsystem: 0]\r\nDone\r\nmmwDemo:/>";
    static const char *const bad[]={
        "mmWave Stop error ignored [Error code: -1 Subsystem: 0] Assert\r\nDone\r\nmmwDemo:/>",
        "mmWave Stop error ignored [Error code: -1 Subsystem: 0]\r\nError -2\r\nDone\r\nmmwDemo:/>",
        "mmWave Stop error ignored [Error code: 2147483648 Subsystem: 0]\r\nDone\r\nmmwDemo:/>",
        "mmWave Stop error ignored [Error code: -2147483649 Subsystem: 0]\r\nDone\r\nmmwDemo:/>",
        "mmWave Stop error ignored [Error code: x Subsystem: 0]\r\nDone\r\nmmwDemo:/>",
        "mmWave Stop error ignored [Error code: -1 Subsystem: 0] trailing\r\nDone\r\nmmwDemo:/>",
        "other error ignored\r\nDone\r\nmmwDemo:/>"
    };
    RadarCaptureProfile p;unsigned i;int kind;
    begin(&fixture,RADAR_CAPTURE_CMD_STOP);CHECK(cli.allow_finite_stop_warning);
    CHECK(response(accepted)==0 && cli.finite_stop_warnings==1U);
    /* Warning alone, missing prompt or missing Done never acknowledges stop. */
    begin(&fixture,RADAR_CAPTURE_CMD_STOP);CHECK(response(warning)==RADAR_CLI_E_TIMEOUT);
    begin(&fixture,RADAR_CAPTURE_CMD_STOP);CHECK(response("mmWave Stop error ignored [Error code: -1 Subsystem: 0]\r\nDone\r\n")==RADAR_CLI_E_TIMEOUT);
    begin(&fixture,RADAR_CAPTURE_CMD_STOP);CHECK(response("mmWave Stop error ignored [Error code: -1 Subsystem: 0]\r\nmmwDemo:/>")==RADAR_CLI_E_TIMEOUT);
    for(i=0U;i<sizeof(bad)/sizeof(bad[0]);++i) {
        begin(&fixture,RADAR_CAPTURE_CMD_STOP);CHECK(response(bad[i])==RADAR_CLI_E_REMOTE);
    }
    /* Same exact line remains an error outside finite sensorStop. */
    for(kind=-1;kind<=2;++kind) { if (kind==RADAR_CAPTURE_CMD_STOP) { continue; }
        begin(&fixture,kind);CHECK(!cli.allow_finite_stop_warning);CHECK(response(accepted)==RADAR_CLI_E_REMOTE);
    }
    p=fixture;p.qualification=RADAR_CAPTURE_FIRST_PACKET_TEST;p.packet_bytes=131072U;
    begin(&p,RADAR_CAPTURE_CMD_STOP);CHECK(response(accepted)==RADAR_CLI_E_REMOTE);
    p=fixture;p.qualification=RADAR_CAPTURE_QUALIFIED;p.approvals=0xffU;
    begin(&p,RADAR_CAPTURE_CMD_STOP);CHECK(response(accepted)==RADAR_CLI_E_REMOTE);
    begin(&fixture,RADAR_CAPTURE_CMD_STOP);CHECK(response(accepted)==0);
    CHECK(radar_cli_start_query(&cli,model.now)==0 && !cli.allow_finite_stop_warning && !cli.finite_stop_warnings);
    radar_cli_new_reset_epoch(&cli);CHECK(!cli.allow_finite_stop_warning);
}
static void export_tests(void)
{
    const char *path=getenv("RADAR_FINITE_EXPORT_LOG");
    FILE *file=path?fopen(path,"wb"):NULL;
    unsigned k;
    if(path) { CHECK(file!=NULL); }
    for(k=0U;k<4U;++k) {
        RadarCaptureProfile p=fixture;
        const char *table[13];
        RadarSession exported={0};RadarConsole console;
        u32 i,limit;u8 *payload;
        memcpy(table,commands,sizeof(commands));p.commands=table;
        if(k==1U || k==2U) {table[11]="lvdsStreamCfg -1 0 1 0";p.packet_bytes=262144U;}
        if(k==2U) {table[10]="frameCfg 0 1 32 16 100 1 0";p.packet_count=16U;}
        if(k==3U) {
            table[2]="channelCfg 15 7 0";table[10]="frameCfg 0 2 32 1 100 1 0";
            table[12]="chirpCfg 2 2 0 0 0 0 0 2";p.command_count=13U;p.packet_bytes=399360U;
        }
        CHECK(radar_capture_profile_validate(&p)==0);
        model_init();radar_console_init(&console);
        exported.profile=&p;exported.console=&console;exported.state=RADAR_SESSION_DONE;
        exported.result.valid=1U;exported.result.session=701U+k;
        exported.result.packet_bytes=p.packet_bytes;exported.result.packet_count=p.packet_count;
        exported.result.bytes=p.packet_bytes*p.packet_count;exported.result.format=p.format_flags;
        exported.result.skip_pairs=p.skip_pairs;exported.result.startup_mode=p.startup_mode;
        payload=(u8 *)radar_memory_capture_base();
        for(i=0U;i<exported.result.bytes;++i) {payload[i]=(u8)(i*37U+k*53U+11U);}
        exported.result.crc32=radar_crc32_update(UINT32_MAX,payload,exported.result.bytes)^UINT32_MAX;
        exported.exporting=1U;
        limit=31U+(exported.result.bytes+127U)/128U+1U;
        for(i=0U;exported.exporting||console.count;++i) {
            CHECK(i<limit);
            radar_export_poll(&exported);model_flush_console(&console);
        }
        CHECK(exported.export_offset==exported.result.bytes && console.dropped==0U);
        CHECK(strstr(model.log,"@R4META qualification FINITE_FRAME_TEST\r\n")!=NULL);
        CHECK(strstr(model.log,"@R4META review_bits 83 07\r\n")!=NULL);
        CHECK(strstr(model.log,k==0U?"@R4META finite_source 1 1 64 256 15\r\n":
                              k==1U?"@R4META finite_source 1 0 64 256 15\r\n":
                              k==2U?"@R4META finite_source 16 0 64 256 15\r\n":
                                    "@R4META finite_source 1 1 96 256 15\r\n")!=NULL);
        if(file) {CHECK(fwrite(model.log,1,model.log_count,file)==model.log_count);}
    }
    if(file) {CHECK(fclose(file)==0);}
}
int main(void)
{
    geometry_tests();cli_tests();export_tests();
    printf("PASS %u finite geometry, bounded sensorStop and production exporter checks; host only\n",checks);
    return 0;
}
