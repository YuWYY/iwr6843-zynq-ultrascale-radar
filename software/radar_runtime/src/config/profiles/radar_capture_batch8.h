/* SPDX-License-Identifier: MIT
 * GENERATED from actual supplied files and explicit evidence review.
 * Expected identities only, not live flash/readback or electrical measurement.
 * Regenerate after ANY firmware/cfg/review change. Do not hand-edit approvals.
 */
#ifndef RADAR_CAPTURE_PROFILE_BATCH8_H
#define RADAR_CAPTURE_PROFILE_BATCH8_H
static const char *const radar_batch8_commands[] = {
    "flushCfg",
    "dfeDataOutputMode 1",
    "channelCfg 15 7 0",
    "adcCfg 2 1",
    "adcbufCfg -1 0 1 1 1",
    "lowPower 0 0",
    "profileCfg 0 60 7 3 24 0 0 166 1 256 12500 0 0 158",
    "chirpCfg 0 0 0 0 0 0 0 1",
    "chirpCfg 1 1 0 0 0 0 0 4",
    "chirpCfg 2 2 0 0 0 0 0 2",
    "frameCfg 0 2 32 8 100 1 0",
    "guiMonitor -1 1 1 1 0 0 1",
    "cfarCfg -1 0 2 8 4 3 0 15.0 0",
    "cfarCfg -1 1 0 4 2 3 1 15.0 0",
    "multiObjBeamForming -1 1 0.5",
    "calibDcRangeSig -1 0 -5 8 256",
    "clutterRemoval -1 0",
    "compRangeBiasAndRxChanPhase 0.0 1 0 1 0 1 0 1 0 1 0 1 0 1 0 1 0 1 0 1 0 1 0 1 0",
    "measureRangeBiasAndRxChanPhase 0 1. 0.2",
    "aoaFovCfg -1 -90 90 -90 90",
    "cfarFovCfg -1 0 0.25 9.0",
    "cfarFovCfg -1 1 -12.9 12.9",
    "extendedMaxVelocity -1 0",
    "CQRxSatMonitor 0 3 4 63 0",
    "CQSigImgMonitor 0 127 4",
    "analogMonitor 0 0",
    "lvdsStreamCfg -1 1 1 0",
    "bpmCfg -1 0 0 0",
    "calibData 0 0 0",
};
static const RadarCaptureProfile radar_batch8_data = {
    .schema=1U, .approvals=0x00000083U,
    .qualification=2U, .source_review_flags=0x00000007U,
    .name="TI35_F20_3TX_HSI_BATCH8", .profile_id="5d312c0a176fdb1d",
    .firmware_sha256="a48f76f25afca829ea9ebf432e0a4a091b6ad7cfa5c8df2783b2944864f7ac0b",
    .cfg_sha256="c364ba144f4c051717e426720be11db5dcfe3053d3619a4f49f1d3cdced2e230",
    .review_sha256="b898e8d4b0ed7ef370b4b1b8d36cb89b091f69c3e24e8887393dc451f3216210",
    .version_substring="03.05.00.04",
    .lanes=2U, .lane_mbps=600U, .word_bits=16U, .format_flags=8U,
    .packet_bytes=399360U, .packet_count=8U, .skip_pairs=0U,
    .startup_mode=2U, .receive_timeout_us=4000000U, .snapshot_timeout_us=500000U,
    .use_delay=0U, .taps={0U,0U,0U},
    .command_count=29U, .commands=radar_batch8_commands,
    .start_command="sensorStart", .stop_command="sensorStop"
};
#endif
