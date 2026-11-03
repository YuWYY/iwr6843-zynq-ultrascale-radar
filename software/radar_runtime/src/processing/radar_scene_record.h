/* SPDX-License-Identifier: MIT
 * Bounded CPU-only recorder. No capture, DMA, console or hardware ownership.
 * The caller owns the existing6MiB telemetry reservation exclusively while
 * armed. No heap, overwrite, complete-scene replay or physical label claims.
 */
#ifndef RADAR_SCENE_RECORD_H
#define RADAR_SCENE_RECORD_H
#include <stddef.h>
#include <stdint.h>
#include "radar_scene_types.h"
#include "radar_spatial_types.h"
#include "radar_dsp_types.h"
#include "radar_range.h"
#define RADAR_RECORD_CAPACITY 2048U
#define RADAR_RECORD_FRAME_BYTES 2560U
#define RADAR_RECORD_TIMING_BYTES 320U
#define RADAR_RECORD_HEADER_BYTES 4096U
#define RADAR_RECORD_HEAT_BYTES 100352U
#define RADAR_RECORD_HEAT_COUNT 3U
#define RADAR_RECORD_REGION_BYTES 0x600000U
#define RADAR_RECORD_FRAMES_OFFSET 0x0a0000U
#define RADAR_RECORD_META_OFFSET 0x5a0000U
enum RadarRecordState { RADAR_RECORD_EMPTY=0, RADAR_RECORD_ARMED,
    RADAR_RECORD_ACTIVE, RADAR_RECORD_DRAINING, RADAR_RECORD_HASHING,
    RADAR_RECORD_FROZEN };
enum RadarRecordRc { RADAR_RECORD_BUSY=0,RADAR_RECORD_DONE=1,
    RADAR_RECORD_E_STATE=-701,RADAR_RECORD_E_CAPACITY=-702,
    RADAR_RECORD_E_IDENTITY=-703,RADAR_RECORD_E_TIME=-704,
    RADAR_RECORD_E_ARGUMENT=-705 };
typedef struct {
    const char *software_version,*build_id,*profile_id,*cfg_sha256,*array_id;
    const char *calibration_id,*bit_sha256,*xsa_sha256,*firmware_expected_sha256;
    const char *firmware_version_observed,*scene_parameters;
    uint32_t installation_epoch,reflection_id_seed;
} RadarRecordIdentity;
typedef struct {
    uint8_t *region;
    const uint8_t *heat_source;
    uint32_t state,duration_s,count,timing_count,heat_count,capture_epoch;
    uint32_t first_frame,last_frame,last_transport,installation_epoch;
    uint32_t heat_schedule[3],heat_record_index[3],pending_heat,heat_offset,heat_crc;
    uint32_t pending_record,pending_frame,pending_transport,pending_error;
    uint32_t stop_due,freeze_stage,hash_offset,payload_crc,image_crc,image_bytes;
    uint32_t final_presented,final_skipped,final_dp_flags,final_source_state;
    uint32_t incomplete,heat_missed;
    uint64_t first_us,last_us,pending_us;
    int error;
} RadarSceneRecorder;
void radar_scene_record_init(RadarSceneRecorder *,void *region,size_t bytes);
int radar_scene_record_arm(RadarSceneRecorder *,uint32_t seconds,const RadarRecordIdentity *);
int radar_scene_record_start(RadarSceneRecorder *,uint32_t capture_epoch);
int radar_scene_record_begin(RadarSceneRecorder *,const RadarFrameRef *,
    const RadarSpatialFrame *,const RadarSceneFrame *,const RadarRangeFrame *,uint32_t dsp_flags);
/* Copies at most8KiB of a selected real heatmap per call. No producer may
 * mutate the referenced spatial frame until DONE or explicit cancel. */
int radar_scene_record_step(RadarSceneRecorder *);
void radar_scene_record_cancel_pending(RadarSceneRecorder *,int reason);
void radar_scene_record_stop_requested(RadarSceneRecorder *,int reason);
/* Both arguments must be positive observations, never inferred from timeout.
 * Caller has stopped RF/PL/DMA, released all RAW/DSP owners and seen the final
 * published frame's actual DP ACK before hashing immutable source regions. */
int radar_scene_record_freeze(RadarSceneRecorder *,uint32_t timing_count,
    uint32_t presented,uint32_t skipped,uint32_t dp_flags,uint32_t source_state,
    int quiescent,int tail_acknowledged,int terminal_error);
/* Hashes at most4KiB per call; data export is refused until FROZEN. */
int radar_scene_record_hash_step(RadarSceneRecorder *);
int radar_scene_record_read(const RadarSceneRecorder *,uint32_t offset,void *,uint32_t bytes);
int radar_scene_record_clear(RadarSceneRecorder *);
/* Explicit loss of an unfrozen session after hardware ownership is gone.
 * This escape does not certify a final DP ACK or manufacture a valid image. */
int radar_scene_record_discard(RadarSceneRecorder *,int quiescent);
int radar_scene_record_holds(const RadarSceneRecorder *);
const char *radar_scene_record_state_name(uint32_t);
#endif
