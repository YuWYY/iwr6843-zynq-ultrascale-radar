/* HOST-ONLY validator tests. No serial/JTAG or target execution. */
#include "radar_capture_profile.h"
#include <stdio.h>
#include <stdlib.h>

static const char *const commands[]={"flushCfg"};
static const RadarCaptureProfile fixture={
    .schema=1U,.approvals=255U,
    .qualification=RADAR_CAPTURE_QUALIFIED,.source_review_flags=0U,
    .name="HOST_ONLY_PROFILE_QUALIFICATION",
    .profile_id="123456789abcdef0",
    .firmware_sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    .cfg_sha256="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    .review_sha256="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    .version_substring="03.05.00.04",
    .lanes=2U,.lane_mbps=600U,.word_bits=16U,.format_flags=8U,
    .packet_bytes=131072U,.packet_count=1U,.skip_pairs=0U,
    .startup_mode=RADAR_CAPTURE_START_REVIEWED_PREAMBLE,
    .receive_timeout_us=4000000U,.snapshot_timeout_us=500000U,
    .use_delay=0U,.taps={0U,0U,0U},.command_count=1U,.commands=commands,
    .start_command="sensorStart",.stop_command="sensorStop"
};
static unsigned checks;
#define CHECK(expr) do { ++checks; if(!(expr)) { fprintf(stderr,"FAIL line %u: %s\n",(unsigned)__LINE__,#expr);return 1; } } while(0)
#define REJECT(change) do { p=trial; change; CHECK(radar_capture_profile_validate(&p)!=0); } while(0)

int main(void)
{
    RadarCaptureProfile p=fixture,trial;
    unsigned i;
    CHECK(radar_capture_profile_validate(&p)==0);
    for(i=0U;i<8U;++i) {
        p=fixture;p.approvals &= ~(1U<<i);
        CHECK(radar_capture_profile_validate(&p)!=0);
    }
    trial=fixture;
    trial.qualification=RADAR_CAPTURE_FIRST_PACKET_TEST;
    trial.approvals=RADAR_CAPTURE_TRIAL_APPROVAL_MASK;
    trial.source_review_flags=RADAR_CAPTURE_SOURCE_REVIEW_MASK;
    CHECK(radar_capture_profile_validate(&trial)==0);
    CHECK(radar_capture_profile_version_matches(&trial,"SDK 03.05.00.04"));
    CHECK(radar_capture_profile_command(&trial,RADAR_CAPTURE_CMD_START,0U)!=NULL);
    REJECT(p.approvals &= ~1U);
    REJECT(p.approvals &= ~2U);
    REJECT(p.approvals &= ~128U);
    for(i=0U;i<3U;++i) { REJECT(p.source_review_flags &= ~(1U<<i)); }
    REJECT(p.approvals |= 256U);
    REJECT(p.source_review_flags |= 8U);
    REJECT(p.qualification=2U);
    REJECT(p.qualification=RADAR_CAPTURE_QUALIFIED);
    REJECT(p.packet_bytes=65536U);
    REJECT(p.packet_bytes=131080U);
    REJECT(p.packet_count=2U);
    REJECT(p.format_flags=0U);
    REJECT(p.skip_pairs=1U);
    REJECT(p.use_delay=1U);
    REJECT(p.startup_mode=RADAR_CAPTURE_START_IDLE_CLOCK);
    REJECT(p.lanes=4U);
    REJECT(p.lane_mbps=450U);
    REJECT(p.word_bits=14U);
    REJECT(p.receive_timeout_us=4000001U);
    REJECT(p.start_command="sensorStart 0");
    p=trial;p.approvals=255U;
    CHECK(radar_capture_profile_validate(&p)==0);
    CHECK(p.qualification==RADAR_CAPTURE_FIRST_PACKET_TEST);
    printf("PASS %u profile qualification checks; host only, no hardware proof\n",checks);
    return 0;
}

