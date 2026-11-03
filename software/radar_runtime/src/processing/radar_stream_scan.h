/* SPDX-License-Identifier: MIT */
#ifndef RADAR_STREAM_SCAN_H
#define RADAR_STREAM_SCAN_H
#include "radar_scan.h"
#include "radar_stream.h"
#include "radar_time_bridge.h"
#include "radar_scene.h"
#include "radar_scene_record.h"
#define RADAR_STREAM_LOG_CAPACITY 8192U
typedef struct {
    RadarTimingRecord timing;
    uint64_t mapped_us,cpu_us;
    uint32_t uncertainty_us,raw_slot,raw_lease,task_slot,task_lease;
} RadarStreamRecord;
typedef struct {
    RadarStreamFrame raw;
    RadarFrameRef frame;
    RadarDspJob job;
    RadarDspResult result;
    RadarRangeProcessor range;
    RadarSceneFrame scene_result; /* Private until publish; DP sees old snapshot. */
    uint64_t scene_cpu_us;
    uint32_t state,lease,crc,record,import_row;
} RadarStreamTask;
typedef struct {
    RadarScan *scan;
    RadarStream capture;
    RadarTimeBridge time;
    RadarStreamTask task[2];
    RadarSceneProcessor scene;
    RadarSceneFrame scene_frame;
    RadarArrayCalibration calibration;
    RadarStreamRecord *log;
    RadarSceneRecorder recorder;
    uint32_t log_capacity,record_exporting,record_export_offset,record_export_limit;
    uint32_t log_count,log_overwritten,last_ack,next_lease;
    uint32_t mode,frame_limit,running,stop_requested,installation_epoch,have_calibration;
    uint32_t completed,presented,coalesced,last_summary_completed,errors;
    uint32_t crc_errors,hsi_errors,dma_errors,metadata_errors,dp_fault_bits,dp_active,latest_ack_frame;
    uint64_t begin_us,last_summary_us,last_ack_us,max_gap_us,cpu_total_us;
    uint32_t export_index,export_remaining,exporting;
    uint32_t stop_view_pending;
    uint32_t scene_budget_overruns;
    char export_line[1536]; /* Reserved DDR, not the16KiB application stack. */
    int engine_slot,post_slot,error;
} RadarStreamScan;
/* Caller allocates control, coefficient and log workspaces in reserved DDR. */
int radar_stream_scan_init(RadarStreamScan *,RadarScan *,RadarStreamRecord *,
                           void *scene_workspace,size_t,RadarSpatialWeights *);
void radar_stream_scan_poll(RadarStreamScan *);
void radar_stream_scan_ack(RadarStreamScan *,const RadarTimingAck *);
int radar_stream_scan_command(RadarStreamScan *,const char *);
int radar_stream_scan_set_calibration(RadarStreamScan *,const RadarArrayCalibration *,uint32_t installation_epoch);
int radar_stream_scan_evidence_held(const RadarStreamScan *);
#endif
