/* SPDX-License-Identifier: MIT */
#ifndef RADAR_SNAPSHOT_H
#define RADAR_SNAPSHOT_H
#include "radar_control.h"
#include "radar_capture_profile.h"
typedef enum { RADAR_SNAP_IDLE=0,RADAR_SNAP_READY_WAIT,RADAR_SNAP_COMPLETION_WAIT,
               RADAR_SNAP_READ,RADAR_SNAP_VALIDATE,RADAR_SNAP_DONE,RADAR_SNAP_FAILED } RadarSnapshotState;
typedef struct {
    u32 words[12];
    u32 rx_generation,tap_copy_generation,tap_generation,taps;
    u32 flags,phy_status,phy_generation,phy_fault,sys_faults,baseline,baseline_generation;
    u64 first_sys_tick,complete_sys_tick;
} RadarSnapshotData;
typedef struct {
    RadarSnapshotState state;
    RadarSnapshotData data;
    u32 old_rx_generation,old_copy_generation,index,timeout_us;
    u64 started_us;
    int result;
} RadarSnapshot;
void radar_snapshot_begin(RadarSnapshot *s,u64 now,u32 timeout_us);
/* Returns 0 success, 1 pending, negative failure. One frozen page per service
 * step; no stale snapshot accepted and no ACK confused with PHY completion. */
int radar_snapshot_poll(RadarSnapshot *s,RadarControl *c,u64 now);
int radar_snapshot_matches(const RadarSnapshotData *d,const RadarCaptureProfile *p,u32 session);

typedef struct { u32 old_generation,result,phase;u64 started_us;u8 active,opcode; } RadarPhyTransaction;
int radar_phy_begin(RadarPhyTransaction *t,RadarControl *c,u8 opcode,u64 now);
int radar_phy_poll(RadarPhyTransaction *t,RadarControl *c,u64 now);
#endif
