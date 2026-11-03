/* SPDX-License-Identifier: MIT
 * Communication PROFILE ONLY. Not an approved radar acquisition configuration.
 * Candidate response format: TI xWR68xx SDK CLI "version". The actual flashed
 * firmware/bin/cfg is still unknown. Never treat this header as proof of it.
 */
#ifndef RADAR_PROFILE_H
#define RADAR_PROFILE_H

#define RADAR_PROFILE_NAME             "candidate TI xWR68xx SDK version CLI"
#define RADAR_PROFILE_FIRMWARE_ID      "UNCONFIRMED - preserve real query output"
/* Legacy communication-profile marker only. SW4 qualification is exclusively
 * radar_capture_profile_data.h + its validator; do not force this old bit. */
#define RADAR_PROFILE_RAW16_CONFIRMED   0U
#define RADAR_CLI_BAUD                  115200U
#define RADAR_CLI_QUERY_COMMAND         "version"
#define RADAR_CLI_LINE_END              "\n"  /* one terminator, not two commands */
#define RADAR_CLI_PAYLOAD_PREFIX        "mmWave SDK Version"
#define RADAR_CLI_DONE_LINE             "Done"
#define RADAR_CLI_PROMPT                "mmwDemo:/>"

/* Software budgets, NOT device datasheet guarantees. All in microseconds. */
#define RADAR_CLI_TIMEOUT_US            3000000U
#define RADAR_CLI_TX_TIMEOUT_US         100000U
#define RADAR_CLI_DRAIN_LIMIT_US        500000U
#define RADAR_CLI_DRAIN_QUIET_US        20000U
#define RADAR_CLI_END_QUIET_US          10000U
#define RADAR_CLI_RX_CAPACITY           4096U
#define RADAR_CLI_LINE_CAPACITY         256U
#define RADAR_CLI_TX_CAPACITY           256U
#define RADAR_CLI_RX_BUDGET             64U

/* User invoking reset/reset3 authorizes these nominal values. Verify the
 * actual board/firmware before use; firmware must NOT auto-start RF/LVDS. */
#define RADAR_RESET_HOLD_US             10000U
#define RADAR_RESET_BOOT_US             2000000U
#define RADAR_IO_CONFIRM_TIMEOUT_US     1000U
#define RADAR_SW2_MONITOR_US            1000U
#define RADAR_SW2_CONNECT_SETTLE_US     100000U
#define RADAR_SW2_REPEAT_GAP_US          100000U
#define RADAR_SW2_HEARTBEAT_US          5000000U

#endif
