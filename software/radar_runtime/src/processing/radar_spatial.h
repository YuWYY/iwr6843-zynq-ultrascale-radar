/* SPDX-License-Identifier: MIT
 * Cooperative, fixed-geometry spatial preview. Pure CPU work; no hardware,
 * heap or hidden frame ownership. Allocate the processor statically, not on
 * the 16 KiB application stack. All results use the nominal ISK array unless
 * a matching measured calibration is explicitly supplied.
 */
#ifndef RADAR_SPATIAL_H
#define RADAR_SPATIAL_H
#include "radar_spatial_types.h"
#include "radar_range.h"
#include "radar_dsp_types.h"

#define RADAR_SPATIAL_FC_HZ 62197840000.0
#define RADAR_SPATIAL_PITCH_M 0.0025
#define RADAR_SPATIAL_CHIRP_SECONDS 0.000031
#define RADAR_SPATIAL_VELOCITY_BIN_MPS \
    (299792458.0 / (2.0 * RADAR_SPATIAL_FC_HZ * 32.0 * 3.0 * RADAR_SPATIAL_CHIRP_SECONDS))
#define RADAR_SPATIAL_ELEVATIONS 31U

enum RadarSpatialRc {
    RADAR_SPATIAL_BUSY=0, RADAR_SPATIAL_DONE=1,
    RADAR_SPATIAL_E_ARGUMENT=-1, RADAR_SPATIAL_E_STATE=-2,
    RADAR_SPATIAL_E_CALIBRATION=-3, RADAR_SPATIAL_E_SEQUENCE=-4,
    RADAR_SPATIAL_E_NUMERIC=-5, RADAR_SPATIAL_E_INCOMPLETE=-6,
    RADAR_SPATIAL_E_TEMPORAL_INPUT=-7
};

/* Caller-owned reserved DDR; never add this coefficient cache to BSS. */
typedef struct {
    RadarComplex angle[RADAR_SPATIAL_MAP_ANGLES][RADAR_SPATIAL_ELEVATIONS][12];
    double heat[RADAR_SPATIAL_MAP_ANGLES][7][2];
} RadarSpatialWeights;
typedef struct {
    /* Float-complex range FFT / (32768 * sum(Hann256)), then in-place
     * Hann32 Doppler FFT. Channels are physical TX-major, never slot-major. */
    RadarComplex cube[256][RADAR_SPATIAL_CHANNELS][RADAR_SPATIAL_LOOPS];
    float rd_power[256][RADAR_SPATIAL_LOOPS];
    RadarSpatialFrame frame;
    RadarArrayCalibration calibration;
    RadarComplex combined[12][32];
    const RadarSpatialWeights *weights;
    float slow_window[32], slow_sum, slow_energy;
    RadarComplex slow_twiddle[16], tdm[3][32];
    RadarComplex horizontal_root[RADAR_SPATIAL_MAP_ANGLES];
    RadarComplex steering_x[RADAR_SPATIAL_ELEVATIONS][RADAR_SPATIAL_MAP_ANGLES];
    RadarComplex steering_z[RADAR_SPATIAL_ELEVATIONS];
    float range_scale, rd_scale, heat_scale;
    float candidate_power[RADAR_SPATIAL_MAX_POINTS];
    uint32_t initialized, stage, received_ffts, cursor, point_index, angle_index;
    float best_angle_power, angle_energy;
    int32_t best_azimuth, best_elevation;
    int error;
    uint32_t pl_import;
} RadarSpatialProcessor;

void radar_spatial_init(RadarSpatialProcessor *p);
int radar_spatial_cache_weights(RadarSpatialProcessor *,RadarSpatialWeights *);
/* NULL cal selects explicit unit coefficients / UNCALIBRATED. begin must
 * precede the first sink call. A new begin is refused until DONE or cancel. */
int radar_spatial_begin(RadarSpatialProcessor *p, uint32_t session,
                        uint64_t timestamp_us, const RadarArrayCalibration *cal);
/* Same arithmetic, explicit acquisition-config identity for stream profiles. */
int radar_spatial_begin_config(RadarSpatialProcessor *,uint32_t,uint64_t,
                                const RadarArrayCalibration *,const char *cfg_sha256);
/* RadarRangeFftSink-compatible: requires exactly chirp0/rx0 ... chirp95/rx3.
 * The 256-element input vectors are read only and copied during this call. */
int radar_spatial_accept_fft(void *context, uint32_t chirp, uint32_t rx,
                             const float *re, const float *im);
/* Call only after all384 accepted FFTs and the caller's CRC check. One step
 * performs one channel's32-point FFT, one range's heat/CFAR, or one azimuth
 * line of a candidate's2D search. No partial result is returned as valid. */
int radar_spatial_step(RadarSpatialProcessor *p);
/* Import the uncalibrated PL Doppler cube once. Per call copies one range
 * row and applies the existing TDM/calibration exactly once. Heatmap/angles
 * still use PS; supplied PL candidates bypass the PS Doppler FFT and CFAR. */
int radar_spatial_import_pl_begin(RadarSpatialProcessor *p,
    const RadarDspCandidateHeader *header,const RadarDspCandidate *candidates,RadarDspBackend backend);
int radar_spatial_import_pl_row(RadarSpatialProcessor *p,uint32_t range,
    const int32_t *complex_words,const uint64_t *rd_power);
void radar_spatial_cancel(RadarSpatialProcessor *p);
const RadarSpatialFrame *radar_spatial_result(const RadarSpatialProcessor *p);
const char *radar_spatial_error_name(int error);
#endif
