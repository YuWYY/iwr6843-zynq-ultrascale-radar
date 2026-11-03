/* SPDX-License-Identifier: MIT */
#include "radar_capture_profile.h"
#include <string.h>
#ifndef RADAR_CAPTURE_PROFILE_HEADER
#define RADAR_CAPTURE_PROFILE_HEADER "radar_capture_profile_data.h"
#endif
#include RADAR_CAPTURE_PROFILE_HEADER
#include "profiles/radar_capture_batch8.h"
#include "profiles/radar_capture_batch16.h"
#include "profiles/radar_capture_stream16.h"
#include "profiles/radar_capture_stream64.h"
#include "profiles/radar_capture_stream0.h"

static int hex_identity(const char *s,size_t n)
{
    size_t i; int nonzero=0;
    if (!s || strlen(s)!=n) { return 0; }
    for (i=0;i<n;++i) {
        if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) { return 0; }
        if (s[i]!='0') { nonzero=1; }
    }
    return nonzero;
}
static int config_line_ok(const char *s)
{
    /* Exact reviewed lines are the authority, with a defensive verb allowlist.
     * Values are not inferred here. Unknown firmware verbs require review. */
    static const char *const verbs[]={
        "flushCfg","dfeDataOutputMode","channelCfg","adcCfg","adcbufCfg",
        "profileCfg","chirpCfg","frameCfg","advFrameCfg","subFrameCfg",
        "lowPower","lvdsStreamCfg","guiMonitor","cfarCfg","peakGrouping",
        "multiObjBeamForming","calibDcRangeSig","clutterRemoval",
        "compRangeBiasAndRxChanPhase","measureRangeBiasAndRxChanPhase",
        "extendedMaxVelocity","bpmCfg","CQRxSatMonitor","CQSigImgMonitor",
        "analogMonitor","aoaFovCfg","cfarFovCfg","calibData"
    };
    size_t i,n;
    if (!s || !(n=strlen(s)) || n>RADAR_CAPTURE_MAX_LINE) { return 0; }
    for (i=0;i<n;++i) {
        if ((unsigned char)s[i]<32U||(unsigned char)s[i]>126U||s[i]==';') { return 0; }
    }
    for (i=0;i<sizeof(verbs)/sizeof(verbs[0]);++i) {
        size_t v=strlen(verbs[i]);
        if (strncmp(s,verbs[i],v)==0 && (s[v]=='\0'||s[v]==' ')) { return 1; }
    }
    return 0;
}
const RadarCaptureProfile *radar_capture_profile_get(void) { return &radar_profile_data; }
const RadarCaptureProfile *radar_capture_profile_batch(u32 frames)
{
    if(frames==1U)return &radar_profile_data;
    if(frames==8U)return &radar_batch8_data;
    if(frames==16U)return &radar_batch16_data;
    return NULL;
}
int radar_capture_profile_supported(const RadarCaptureProfile *p)
{
    const RadarCaptureProfile *expected;
    if(!p || !p->profile_id || !p->cfg_sha256)return 0;
    expected=p->qualification==RADAR_CAPTURE_STREAM_FRAME_TEST?
        radar_capture_profile_stream(p->packet_count):radar_capture_profile_batch(p->packet_count);
    return expected && !strcmp(p->profile_id,expected->profile_id) &&
        !strcmp(p->cfg_sha256,expected->cfg_sha256);
}
const RadarCaptureProfile *radar_capture_profile_stream(u32 frames)
{
    if(frames==16U)return &radar_stream16_data;
    if(frames==64U)return &radar_stream64_data;
    if(frames==0U)return &radar_stream0_data;
    return NULL;
}
int radar_capture_profile_stream_geometry(const RadarCaptureProfile *p,RadarFiniteGeometry *out)
{
    RadarCaptureProfile finite;const char *commands[RADAR_CAPTURE_MAX_COMMANDS];
    u32 i,seen=0U;
    if(!p || !out || p->qualification!=RADAR_CAPTURE_STREAM_FRAME_TEST ||
        !p->commands || !p->command_count || p->command_count>RADAR_CAPTURE_MAX_COMMANDS ||
        (p->packet_count!=0U && p->packet_count!=16U && p->packet_count!=64U))return -1;
    finite=*p;finite.qualification=RADAR_CAPTURE_FINITE_FRAME_TEST;finite.packet_count=16U;
    finite.commands=commands;
    for(i=0U;i<p->command_count;++i) {
        const char *s=p->commands[i];if(!config_line_ok(s))return -1;commands[i]=s;
        if(!strncmp(s,"frameCfg ",9U)) {
            const char *wanted=p->packet_count==0U?"frameCfg 0 2 32 0 100 1 0":
                p->packet_count==16U?"frameCfg 0 2 32 16 100 1 0":"frameCfg 0 2 32 64 100 1 0";
            if(seen++ || strcmp(s,wanted))return -1;
            commands[i]="frameCfg 0 2 32 16 100 1 0";
        }
    }
    if(seen!=1U || radar_capture_profile_finite_geometry(&finite,out) || out->tx_count!=3U)return -1;
    out->source_frames=p->packet_count;return 0;
}
int radar_capture_profile_finite_geometry(const RadarCaptureProfile *p,RadarFiniteGeometry *out)
{
    static const char *const fixed[]={
        "flushCfg", "dfeDataOutputMode 1", "adcCfg 2 1",
        "adcbufCfg -1 0 1 1 1", "profileCfg 0 60 7 3 24 0 0 166 1 256 12500 0 0 158",
        "bpmCfg -1 0 0 0", "lowPower 0 0"
    };
    static const char *const chirps[]={"chirpCfg 0 0 0 0 0 0 0 1",
        "chirpCfg 1 1 0 0 0 0 0 4", "chirpCfg 2 2 0 0 0 0 0 2"};
    u32 i,j,seen=0U,frames=0U,hsi=0U,lvds_seen=0U,bytes;
    u32 tx=0U,frame_tx=0U,chirps_seen=0U;
    if (!p || !out || p->qualification!=RADAR_CAPTURE_FINITE_FRAME_TEST ||
        !p->commands || !p->command_count || p->command_count>RADAR_CAPTURE_MAX_COMMANDS) { return -1; }
    memset(out,0,sizeof(*out));
    for (i=0U;i<p->command_count;++i) {
        const char *s=p->commands[i]; size_t n; int protected_verb=0,matched=0;
        if (!config_line_ok(s)) { return -1; }
        n=strcspn(s," ");
        if ((n==11U && strncmp(s,"advFrameCfg",n)==0) ||
            (n==11U && strncmp(s,"subFrameCfg",n)==0)) { return -1; }
        for (j=0U;j<sizeof(fixed)/sizeof(fixed[0]);++j) {
            if (strcspn(fixed[j]," ")==n && strncmp(s,fixed[j],n)==0) { protected_verb=1; }
            if (strcmp(s,fixed[j])==0) {
                if (seen&(1U<<j)) { return -1; }
                seen|=1U<<j; matched=1;
            }
        }
        if (protected_verb && !matched) { return -1; }
        if (n==10U && strncmp(s,"channelCfg",n)==0) {
            if (tx) { return -1; }
            if (strcmp(s,"channelCfg 15 5 0")==0) { tx=2U; }
            else if (strcmp(s,"channelCfg 15 7 0")==0) { tx=3U; }
            else { return -1; }
        }
        if (n==8U && strncmp(s,"chirpCfg",n)==0) {
            for (j=0U;j<3U;++j) { if (strcmp(s,chirps[j])==0) { break; } }
            if (j==3U || (chirps_seen&(1U<<j))) { return -1; }
            chirps_seen|=1U<<j;
        }
        if (n==8U && strncmp(s,"frameCfg",n)==0) {
            if (frames) { return -1; }
            if (strcmp(s,"frameCfg 0 1 32 1 100 1 0")==0) { frames=1U;frame_tx=2U; }
            else if (strcmp(s,"frameCfg 0 1 32 16 100 1 0")==0) { frames=16U;frame_tx=2U; }
            else if (strcmp(s,"frameCfg 0 2 32 1 100 1 0")==0) { frames=1U;frame_tx=3U; }
            else if (strcmp(s,"frameCfg 0 2 32 8 100 1 0")==0) { frames=8U;frame_tx=3U; }
            else if (strcmp(s,"frameCfg 0 2 32 16 100 1 0")==0) { frames=16U;frame_tx=3U; }
            else { return -1; }
        }
        if (n==13U && strncmp(s,"lvdsStreamCfg",n)==0) {
            if (lvds_seen++) { return -1; }
            if (strcmp(s,"lvdsStreamCfg -1 1 1 0")==0) { hsi=1U; }
            else if (strcmp(s,"lvdsStreamCfg -1 0 1 0")!=0) { return -1; }
        }
    }
    if (seen!=((1U<<(sizeof(fixed)/sizeof(fixed[0])))-1U) || !frames || !lvds_seen ||
        !tx || tx!=frame_tx || chirps_seen!=((1U<<tx)-1U) ||
        strcmp(p->commands[0],"flushCfg")!=0 || (tx==2U && hsi && frames!=1U) ||
        (tx==3U && (!hsi || (frames!=1U && frames!=8U && frames!=16U)))) { return -1; }
    bytes=(4096U+(hsi?64U:0U))*tx*32U;
    if (p->packet_bytes!=bytes || p->packet_count!=frames) { return -1; }
    out->source_frames=frames; out->hsi_header_enabled=hsi;
    out->chirps_per_frame=tx*32U; out->samples_per_rx=256U; out->rx_mask=15U;
    out->tx_count=tx;out->tx_mask=tx==3U?7U:5U;out->loops=32U;
    out->tx_order[0]=0U;out->tx_order[1]=2U;out->tx_order[2]=tx==3U?1U:0U;
    out->adc_bytes_per_chirp=4096U; out->expected_header_bytes=hsi?64U:0U;
    out->wire_bytes_per_frame=bytes;
    return 0;
}
int radar_capture_profile_validate(const RadarCaptureProfile *p)
{
    u32 i; u64 per; RadarFiniteGeometry geometry;
    if (!p || p->schema!=RADAR_CAPTURE_PROFILE_SCHEMA ||
        (p->approvals&~RADAR_CAPTURE_APPROVAL_MASK) ||
        (p->source_review_flags&~RADAR_CAPTURE_SOURCE_REVIEW_MASK)) { return -1; }
    if (p->qualification==RADAR_CAPTURE_QUALIFIED) {
        if (p->approvals!=RADAR_CAPTURE_APPROVAL_MASK) { return -1; }
    } else if (p->qualification==RADAR_CAPTURE_FIRST_PACKET_TEST ||
               p->qualification==RADAR_CAPTURE_FINITE_FRAME_TEST ||
               p->qualification==RADAR_CAPTURE_STREAM_FRAME_TEST) {
        if ((p->approvals&RADAR_CAPTURE_TRIAL_APPROVAL_MASK)!=RADAR_CAPTURE_TRIAL_APPROVAL_MASK ||
            p->source_review_flags!=RADAR_CAPTURE_SOURCE_REVIEW_MASK) { return -1; }
        if ((p->qualification==RADAR_CAPTURE_FIRST_PACKET_TEST &&
             (p->packet_bytes!=131072U || p->packet_count!=1U)) || p->format_flags!=8U ||
            p->skip_pairs!=0U || p->use_delay!=0U ||
            p->startup_mode!=RADAR_CAPTURE_START_REVIEWED_PREAMBLE) { return -5; }
    } else { return -1; }
    if (!p->name || !*p->name || strlen(p->name)>64U ||
        !hex_identity(p->profile_id,16U) || !hex_identity(p->firmware_sha256,64U) ||
        !hex_identity(p->cfg_sha256,64U) || !hex_identity(p->review_sha256,64U) ||
        !p->version_substring || strlen(p->version_substring)<5U || strlen(p->version_substring)>64U) { return -2; }
    if (p->lanes!=2U || p->lane_mbps!=600U || p->word_bits!=16U || p->format_flags>31U) { return -3; }
    if (p->packet_bytes<8U || p->packet_bytes>8388600U || (p->packet_bytes&7U)) { return -4; }
    per=((u64)p->packet_bytes+RADAR_RX_SLICE_BYTES-1U)/RADAR_RX_SLICE_BYTES;
    if(p->qualification==RADAR_CAPTURE_STREAM_FRAME_TEST) {
        if(p->packet_bytes!=399360U || (p->packet_count!=0U && p->packet_count!=16U && p->packet_count!=64U) ||
           per*18U>RADAR_RX_BD_COUNT)return -4;
    } else if(!p->packet_count || (u64)p->packet_bytes*p->packet_count>RADAR_CAPTURE_STORE_BYTES ||
              per*p->packet_count>RADAR_RX_BD_COUNT)return -4;
    if ((p->startup_mode!=RADAR_CAPTURE_START_IDLE_CLOCK && p->startup_mode!=RADAR_CAPTURE_START_REVIEWED_PREAMBLE) ||
        p->receive_timeout_us<100000U || p->receive_timeout_us>4000000U ||
        p->snapshot_timeout_us<1000U || p->snapshot_timeout_us>1000000U || p->use_delay>1U) { return -5; }
    if (p->use_delay && p->startup_mode!=RADAR_CAPTURE_START_IDLE_CLOCK) { return -5; }
    for (i=0;i<3U;++i) { if (p->taps[i]>511U) { return -5; } }
    if (!p->command_count || p->command_count>RADAR_CAPTURE_MAX_COMMANDS || !p->commands ||
        !p->start_command || (strcmp(p->start_command,"sensorStart")!=0 && strcmp(p->start_command,"sensorStart 0")!=0) ||
        !p->stop_command || strcmp(p->stop_command,"sensorStop")!=0) { return -6; }
    for (i=0;i<p->command_count;++i) { if (!config_line_ok(p->commands[i])) { return -6; } }
    /* Applying a complete cfg requires sensorStart with reconfiguration. 0 is
     * intentionally refused despite the parser recognizing its spelling. */
    if (strcmp(p->start_command,"sensorStart")!=0) { return -6; }
    if (p->qualification==RADAR_CAPTURE_FINITE_FRAME_TEST &&
        radar_capture_profile_finite_geometry(p,&geometry)!=0) { return -6; }
    if (p->qualification==RADAR_CAPTURE_STREAM_FRAME_TEST &&
        radar_capture_profile_stream_geometry(p,&geometry)!=0) { return -6; }
    return 0;
}
int radar_capture_profile_version_matches(const RadarCaptureProfile *p,const char *version)
{
    return radar_capture_profile_validate(p)==0 && version && strstr(version,p->version_substring)!=NULL;
}
const char *radar_capture_profile_command(const RadarCaptureProfile *p,RadarCaptureCommand kind,u32 index)
{
    if (radar_capture_profile_validate(p)!=0) { return NULL; }
    switch (kind) {
    case RADAR_CAPTURE_CMD_STOP:return index==0U?p->stop_command:NULL;
    case RADAR_CAPTURE_CMD_START:return index==0U?p->start_command:NULL;
    /* Separate runtime operation, never accepted as a full-profile START.
     * The session owns the successful-config cache and restart admission. */
    case RADAR_CAPTURE_CMD_START_REUSE:return index==0U?"sensorStart 0":NULL;
    case RADAR_CAPTURE_CMD_CONFIG:return index<p->command_count?p->commands[index]:NULL;
    default:return NULL;
    }
}
