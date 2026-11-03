/* SPDX-License-Identifier: MIT
 * Fixed SDK 3.5.0.4 HSI/RAW16 range processing. No hardware access or heap.
 * The caller must bind the fixed cfg hash and a valid retained capture result.
 */
#ifndef RADAR_RANGE_H
#define RADAR_RANGE_H
#include <stddef.h>
#include <stdint.h>
#include "../config/radar_capture_geometry.h"

#define RADAR_RANGE_BINS RADAR_CAPTURE_GEOMETRY_SAMPLES
#define RADAR_RANGE_CHIRPS RADAR_CAPTURE_GEOMETRY_CHIRPS
#define RADAR_RANGE_RX RADAR_CAPTURE_GEOMETRY_RX
#define RADAR_RANGE_TX RADAR_CAPTURE_GEOMETRY_TX
#define RADAR_RANGE_RECORD_BYTES RADAR_CAPTURE_GEOMETRY_RECORD_BYTES
#define RADAR_RANGE_INPUT_BYTES (RADAR_RANGE_CHIRPS * RADAR_RANGE_RECORD_BYTES)
#define RADAR_RANGE_FIRST_BIN 6U
#define RADAR_RANGE_LAST_BIN 204U
#define RADAR_RANGE_CFG_SHA256 RADAR_CAPTURE_GEOMETRY_CFG_SHA256
#define RADAR_RANGE_BIN_METERS (299792458.0 * 12500000.0 / (2.0 * 166000000000000.0 * 256.0))
#define RADAR_RANGE_FLOOR_DBFS (-160.0f)

typedef struct {
    uint32_t session;
    uint64_t timestamp_us;
    float power_dbfs[RADAR_RANGE_BINS];
    float bin_m;
    uint32_t peak_bin;
    float peak_dbfs;
    uint32_t clipped_components;
    uint32_t hsi_records;
    uint64_t hsi_session_id;
    uint32_t fft_count;
    /* Indexed by physical TX number, not TDM slot. Raw ADC mean(I^2+Q^2),
     * including DC, relative to32768^2; not a range-bin or RF power. */
    uint32_t tx_records[3];
    float tx_power_dbfs[3];
    uint32_t valid;
    /* Unknown per-frame time must not be plotted as measured acquisition time.
     * Zero preserves the legacy trusted-time API for direct/reference callers. */
    uint32_t time_untrusted;
} RadarRangeFrame;

enum RadarRangeRc {
    RADAR_RANGE_BUSY = 0, RADAR_RANGE_DONE = 1,
    RADAR_RANGE_E_ARGUMENT = -1, RADAR_RANGE_E_LENGTH = -2,
    RADAR_RANGE_E_HSI = -3, RADAR_RANGE_E_STATE = -4,
    RADAR_RANGE_E_NUMERIC = -5, RADAR_RANGE_E_SINK = -6
};

typedef int (*RadarRangeFftSink)(void *context, uint32_t chirp, uint32_t rx,
                               const float *re, const float *im);

typedef struct {
    RadarRangeFrame frame;
    const uint8_t *raw;
    float window[RADAR_RANGE_BINS];
    float twiddle_re[RADAR_RANGE_BINS / 2U];
    float twiddle_im[RADAR_RANGE_BINS / 2U];
    float re[RADAR_RANGE_BINS], im[RADAR_RANGE_BINS];
    float power_sum[RADAR_RANGE_BINS];
    float normalization;
    uint64_t tx_sample_power[3];
    uint32_t next_chirp, initialized, running;
    uint32_t error_chirp;
    int error;
    RadarRangeFftSink fft_sink;
    void *fft_sink_context;
} RadarRangeProcessor;

/* init computes fixed Hann/twiddle tables once; call at cold start. */
void radar_range_init(RadarRangeProcessor *p);
/* Strict producer-side certification; does not claim ADC timing or run FFT. */
int radar_range_validate_frame(const void *raw,size_t bytes,uint64_t *hsi_id);
/* Optional read-only FFT consumer; set only while idle. NULL disconnects.
 * A nonzero sink result aborts the frame; the original power arithmetic is
 * unchanged when a sink succeeds. */
int radar_range_set_fft_sink(RadarRangeProcessor *p, RadarRangeFftSink sink,
                             void *context);
/* begin validates all 96 headers before processing. It does not copy raw.
 * Raw data must stay immutable until step returns DONE or cancel is called.
 * timestamp_us is caller-provided measured acquisition time, not cfg period.
 * Returns BUSY(0) on accepted work; a negative code leaves no valid result. */
int radar_range_begin(RadarRangeProcessor *p, const void *raw, size_t bytes,
                      uint32_t session, uint64_t timestamp_us);
/* At most one chirp (4 x 256-point FFTs) per call; 96 calls finish a frame.
 * Average linear power across RX and chirps, then convert to dBFS. No phase
 * combining, CFAR, target identification, range-bias or antenna calibration. */
int radar_range_step(RadarRangeProcessor *p);
/* PL path: validate headers with begin, then pack exactly one chirp per call
 * into raw signed16 I/Q sign-extended to two32-bit words. Never runs an FFT.
 * import_power is permitted only after all96 chirps and a verified PL DONE. */
int radar_range_pack_step(RadarRangeProcessor *p,int32_t *iq,size_t words);
int radar_range_import_power(RadarRangeProcessor *p,const uint64_t power[256]);
void radar_range_cancel(RadarRangeProcessor *p);
const RadarRangeFrame *radar_range_result(const RadarRangeProcessor *p);
const char *radar_range_error_name(int error);
#endif
