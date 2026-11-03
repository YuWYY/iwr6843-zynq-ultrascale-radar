/* SPDX-License-Identifier: MIT */
#include "radar_range.h"
#include <math.h>
#include <string.h>

#define RANGE_INIT_MAGIC UINT32_C(0x52464654)
#define PI_F 3.14159265358979323846f
static const unsigned tx_sequence[RADAR_RANGE_TX] = RADAR_CAPTURE_GEOMETRY_TX_SEQUENCE;
_Static_assert(RADAR_RANGE_TX == 3U && RADAR_RANGE_CHIRPS == 96U,
               "This target processor requires the reviewed 3TX single frame");
_Static_assert(RADAR_RANGE_INPUT_BYTES == RADAR_CAPTURE_GEOMETRY_FRAME_BYTES,
               "Generated capture and DSP geometry must agree");

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16);
}
static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}
/* CBUFF memory words [0,1,2,3] arrive in FPGA RAW16 as [0,2,1,3].
 * This same permutation applies to the HSI header and ADC payload. */
static uint16_t memory_word(const uint8_t *raw, unsigned word)
{
    static const unsigned lane_order[4] = {0U, 2U, 1U, 3U};
    unsigned physical = (word & ~3U) + lane_order[word & 3U];
    return le16(raw + physical * 2U);
}
static float signed_code(uint16_t word)
{
    return word < 32768U ? (float)word : (float)((int32_t)word - 65536);
}
static int valid_header(const uint8_t *raw, uint64_t *id)
{
    static const uint64_t ids[] = {
        UINT64_C(0x0cda0adc0cda0adc), UINT64_C(0x09cc0cc909cc0cc9),
        UINT64_C(0x08cc0cc808cc0cc8), UINT64_C(0x0f48084f0f48084f),
        UINT64_C(0x076c0c67076c0c67), UINT64_C(0x0dda0add0dda0add),
        UINT64_C(0x0eda0ade0eda0ade), UINT64_C(0x0fda0adf0fda0adf)
    };
    uint8_t header[64];
    unsigned i;
    uint16_t lsw, msw;
    int found = 0;
    for (i = 0; i < 32U; ++i) {
        uint16_t w = memory_word(raw, i);
        header[2U * i] = (uint8_t)w;
        header[2U * i + 1U] = (uint8_t)(w >> 8);
    }
    *id = le64(header);
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        if (*id == ids[i]) { found = 1; }
    }
    lsw = le16(header + 8); msw = le16(header + 10);
    if (!found || (lsw & 0xf000U) || (msw & 0xf000U) ||
        ((uint32_t)lsw | ((uint32_t)msw << 12)) != 4144U ||
        le32(header + 12) != 0U || le16(header + 16) != 0x0744U ||
        le16(header + 18) != 32U || header[20] != 4U ||
        header[21] != 2U || header[22] != 0U || header[23] != 2U ||
        header[24] != 15U || header[25] != 1U ||
        le16(header + 26) != 1U || le16(header + 28) != 512U) {
        return 0;
    }
    for (i = 30U; i < 52U; ++i) { if (header[i] != 0U) { return 0; } }
    for (i = 52U; i < 64U; ++i) { if (header[i] != 15U) { return 0; } }
    return 1;
}
int radar_range_validate_frame(const void *raw,size_t bytes,uint64_t *hsi_id)
{
    uint64_t first=0U,id;unsigned chirp;
    if(!raw || !hsi_id)return RADAR_RANGE_E_ARGUMENT;
    if(bytes!=RADAR_RANGE_INPUT_BYTES)return RADAR_RANGE_E_LENGTH;
    for(chirp=0U;chirp<RADAR_RANGE_CHIRPS;++chirp) {
        if(!valid_header((const uint8_t *)raw+chirp*RADAR_RANGE_RECORD_BYTES,&id) ||
           (chirp && id!=first))return RADAR_RANGE_E_HSI;
        if(!chirp)first=id;
    }
    *hsi_id=first;return 0;
}
static int fail(RadarRangeProcessor *p, int error)
{
    p->error = error; p->running = 0U; p->raw = NULL; p->frame.valid = 0U;
    return error;
}
void radar_range_init(RadarRangeProcessor *p)
{
    unsigned n;
    float window_sum = 0.0f;
    if (p == NULL) { return; }
    memset(p, 0, sizeof(*p));
    for (n = 0; n < RADAR_RANGE_BINS; ++n) {
        p->window[n] = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)n / 255.0f);
        window_sum += p->window[n];
    }
    for (n = 0; n < RADAR_RANGE_BINS / 2U; ++n) {
        float angle = -2.0f * PI_F * (float)n / (float)RADAR_RANGE_BINS;
        p->twiddle_re[n] = cosf(angle); p->twiddle_im[n] = sinf(angle);
    }
    /* A bin-centered complex exponential of amplitude32768 gives0dBFS.
     * Linear power is averaged BEFORE logarithm, across384 chirp/RX FFTs. */
    p->normalization = 1.0f / ((float)(RADAR_RANGE_CHIRPS * RADAR_RANGE_RX) *
                                  32768.0f * 32768.0f * window_sum * window_sum);
    p->initialized = RANGE_INIT_MAGIC;
}
int radar_range_set_fft_sink(RadarRangeProcessor *p, RadarRangeFftSink sink, void *context)
{
    if (p == NULL) { return RADAR_RANGE_E_ARGUMENT; }
    if (p->initialized != RANGE_INIT_MAGIC || p->running) { return RADAR_RANGE_E_STATE; }
    p->fft_sink = sink; p->fft_sink_context = context;
    return 0;
}
int radar_range_begin(RadarRangeProcessor *p, const void *raw, size_t bytes,
                      uint32_t session, uint64_t timestamp_us)
{
    unsigned chirp;
    uint64_t first_id = 0U;
    if (p == NULL) { return RADAR_RANGE_E_ARGUMENT; }
    if (p->initialized != RANGE_INIT_MAGIC || p->running) {
        return RADAR_RANGE_E_STATE;
    }
    memset(&p->frame, 0, sizeof(p->frame));
    p->raw = NULL; p->error = 0; p->error_chirp = 0U; p->next_chirp = 0U;
    if (raw == NULL) { return fail(p, RADAR_RANGE_E_ARGUMENT); }
    if (bytes != RADAR_RANGE_INPUT_BYTES) { return fail(p, RADAR_RANGE_E_LENGTH); }
    for (chirp = 0; chirp < RADAR_RANGE_CHIRPS; ++chirp) {
        uint64_t id;
        p->error_chirp = chirp;
        if (!valid_header((const uint8_t *)raw + chirp * RADAR_RANGE_RECORD_BYTES, &id)) {
            return fail(p, RADAR_RANGE_E_HSI);
        }
        if (chirp == 0U) { first_id = id; }
        else if (id != first_id) { return fail(p, RADAR_RANGE_E_HSI); }
    }
    memset(p->power_sum, 0, sizeof(p->power_sum));
    memset(p->tx_sample_power, 0, sizeof(p->tx_sample_power));
    p->frame.session = session; p->frame.timestamp_us = timestamp_us;
    p->frame.bin_m = (float)RADAR_RANGE_BIN_METERS;
    p->frame.hsi_records = RADAR_RANGE_CHIRPS; p->frame.hsi_session_id = first_id;
    p->frame.peak_bin = RADAR_RANGE_FIRST_BIN;
    p->frame.peak_dbfs = RADAR_RANGE_FLOOR_DBFS;
    p->error_chirp = 0U; p->raw = raw; p->running = 1U;
    return RADAR_RANGE_BUSY;
}
static void fft(RadarRangeProcessor *p)
{
    unsigned i, j = 0U, length;
    for (i = 1U; i < RADAR_RANGE_BINS; ++i) {
        unsigned bit = RADAR_RANGE_BINS >> 1;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) {
            float tmp = p->re[i]; p->re[i] = p->re[j]; p->re[j] = tmp;
            tmp = p->im[i]; p->im[i] = p->im[j]; p->im[j] = tmp;
        }
    }
    for (length = 2U; length <= RADAR_RANGE_BINS; length <<= 1) {
        unsigned half = length >> 1, stride = RADAR_RANGE_BINS / length;
        for (i = 0U; i < RADAR_RANGE_BINS; i += length) {
            for (j = 0U; j < half; ++j) {
                unsigned lower = i + j, upper = lower + half, t = j * stride;
                float rr = p->twiddle_re[t] * p->re[upper] - p->twiddle_im[t] * p->im[upper];
                float ii = p->twiddle_re[t] * p->im[upper] + p->twiddle_im[t] * p->re[upper];
                p->re[upper] = p->re[lower] - rr; p->im[upper] = p->im[lower] - ii;
                p->re[lower] += rr; p->im[lower] += ii;
            }
        }
    }
}

int radar_range_pack_step(RadarRangeProcessor *p,int32_t *iq,size_t words)
{
    unsigned rx,n,chirp,tx;
    if(!p || !iq || words!=196608U)return RADAR_RANGE_E_ARGUMENT;
    if(p->initialized!=RANGE_INIT_MAGIC || p->running!=1U || p->next_chirp>=96U)
        return RADAR_RANGE_E_STATE;
    chirp=p->next_chirp;tx=tx_sequence[chirp%3U];
    for(rx=0;rx<4U;++rx)for(n=0;n<256U;++n) {
        const uint8_t *raw=p->raw+chirp*RADAR_RANGE_RECORD_BYTES;
        unsigned base=32U+rx*512U+2U*n;
        uint16_t iw=memory_word(raw,base+1U);
        uint16_t qw=memory_word(raw,base);
        int32_t i=iw<32768U?(int32_t)iw:(int32_t)iw-65536;
        int32_t q=qw<32768U?(int32_t)qw:(int32_t)qw-65536;
        size_t k=((size_t)chirp*4U*256U+rx*256U+n)*2U;
        iq[k]=i;iq[k+1U]=q;
        if(iw==0x8000U || iw==0x7fffU)++p->frame.clipped_components;
        if(qw==0x8000U || qw==0x7fffU)++p->frame.clipped_components;
        p->tx_sample_power[tx]+=(uint64_t)((int64_t)i*i+(int64_t)q*q);
    }
    ++p->frame.tx_records[tx];
    if(++p->next_chirp==96U) { p->running=2U;return RADAR_RANGE_DONE; }
    return RADAR_RANGE_BUSY;
}

int radar_range_import_power(RadarRangeProcessor *p,const uint64_t power[256])
{
    unsigned n,tx;
    const double scale=(256.0/127.5)*(256.0/127.5)/(17592186044416.0*384.0);
    if(!p || !power)return RADAR_RANGE_E_ARGUMENT;
    if(p->initialized!=RANGE_INIT_MAGIC || p->running!=2U || p->next_chirp!=96U)
        return RADAR_RANGE_E_STATE;
    for(n=0;n<256U;++n) {
        float linear=(float)((double)power[n]*scale);
        if(!isfinite(linear) || linear<0.0f)return fail(p,RADAR_RANGE_E_NUMERIC);
        p->frame.power_dbfs[n]=linear>1.0e-16f?10.0f*log10f(linear):RADAR_RANGE_FLOOR_DBFS;
        if(n>=RADAR_RANGE_FIRST_BIN && n<=RADAR_RANGE_LAST_BIN && p->frame.power_dbfs[n]>p->frame.peak_dbfs) {
            p->frame.peak_bin=n;p->frame.peak_dbfs=p->frame.power_dbfs[n];
        }
    }
    for(tx=0;tx<3U;++tx) {
        double denominator=(double)p->frame.tx_records[tx]*4.0*256.0*32768.0*32768.0;
        double linear=(double)p->tx_sample_power[tx]/denominator;
        if(p->frame.tx_records[tx]!=32U || !isfinite(linear))return fail(p,RADAR_RANGE_E_NUMERIC);
        p->frame.tx_power_dbfs[tx]=linear>1.0e-16?10.0f*log10f((float)linear):RADAR_RANGE_FLOOR_DBFS;
    }
    p->frame.fft_count=384U;p->frame.valid=1U;p->running=0U;p->raw=NULL;
    return RADAR_RANGE_DONE;
}
int radar_range_step(RadarRangeProcessor *p)
{
    unsigned rx, n, tx;
    const uint8_t *record;
    if (p == NULL) { return RADAR_RANGE_E_ARGUMENT; }
    if (p->initialized != RANGE_INIT_MAGIC) { return RADAR_RANGE_E_STATE; }
    if (!p->running) { return p->frame.valid ? RADAR_RANGE_DONE : (p->error ? p->error : RADAR_RANGE_E_STATE); }
    if (p->raw == NULL || p->next_chirp >= RADAR_RANGE_CHIRPS) { return fail(p, RADAR_RANGE_E_STATE); }
    record = p->raw + p->next_chirp * RADAR_RANGE_RECORD_BYTES;
    tx = tx_sequence[p->next_chirp % RADAR_RANGE_TX];
    for (rx = 0U; rx < RADAR_RANGE_RX; ++rx) {
        float mean_i = 0.0f, mean_q = 0.0f;
        for (n = 0U; n < RADAR_RANGE_BINS; ++n) {
            unsigned word = 32U + rx * 512U + 2U * n;
            uint16_t q = memory_word(record, word), i = memory_word(record, word + 1U);
            p->re[n] = signed_code(i); p->im[n] = signed_code(q);
            {
                int64_t si = i < 32768U ? (int32_t)i : (int32_t)i - 65536;
                int64_t sq = q < 32768U ? (int32_t)q : (int32_t)q - 65536;
                p->tx_sample_power[tx] += (uint64_t)(si * si + sq * sq);
            }
            mean_i += p->re[n]; mean_q += p->im[n];
            if (i == 0x8000U || i == 0x7fffU) { ++p->frame.clipped_components; }
            if (q == 0x8000U || q == 0x7fffU) { ++p->frame.clipped_components; }
        }
        mean_i /= (float)RADAR_RANGE_BINS; mean_q /= (float)RADAR_RANGE_BINS;
        for (n = 0U; n < RADAR_RANGE_BINS; ++n) {
            p->re[n] = (p->re[n] - mean_i) * p->window[n];
            p->im[n] = (p->im[n] - mean_q) * p->window[n];
        }
        fft(p);
        if (p->fft_sink && p->fft_sink(p->fft_sink_context, p->next_chirp,
                                     rx, p->re, p->im) != 0) {
            return fail(p, RADAR_RANGE_E_SINK);
        }
        for (n = 0U; n < RADAR_RANGE_BINS; ++n) {
            p->power_sum[n] += p->re[n] * p->re[n] + p->im[n] * p->im[n];
        }
        ++p->frame.fft_count;
    }
    ++p->frame.tx_records[tx];
    ++p->next_chirp;
    if (p->next_chirp != RADAR_RANGE_CHIRPS) { return RADAR_RANGE_BUSY; }
    for (n = 0U; n < RADAR_RANGE_BINS; ++n) {
        float power = p->power_sum[n] * p->normalization;
        if (!isfinite(power) || power < 0.0f) { return fail(p, RADAR_RANGE_E_NUMERIC); }
        p->frame.power_dbfs[n] = power > 1.0e-16f ? 10.0f * log10f(power) : RADAR_RANGE_FLOOR_DBFS;
        if (n >= RADAR_RANGE_FIRST_BIN && n <= RADAR_RANGE_LAST_BIN &&
            p->frame.power_dbfs[n] > p->frame.peak_dbfs) {
            p->frame.peak_bin = n; p->frame.peak_dbfs = p->frame.power_dbfs[n];
        }
    }
    for (tx = 0U; tx < 3U; ++tx) {
        double denominator = (double)p->frame.tx_records[tx] * RADAR_RANGE_RX *
                             RADAR_RANGE_BINS * 32768.0 * 32768.0;
        double power = denominator > 0.0 ? (double)p->tx_sample_power[tx] / denominator : 0.0;
        p->frame.tx_power_dbfs[tx] = power > 1.0e-16 ? 10.0f * log10f((float)power) : RADAR_RANGE_FLOOR_DBFS;
        if (p->frame.tx_records[tx] != RADAR_CAPTURE_GEOMETRY_LOOPS ||
            !isfinite(p->frame.tx_power_dbfs[tx])) { return fail(p, RADAR_RANGE_E_NUMERIC); }
    }
    p->frame.valid = 1U; p->running = 0U; p->raw = NULL;
    return RADAR_RANGE_DONE;
}
void radar_range_cancel(RadarRangeProcessor *p)
{
    if (p == NULL) { return; }
    p->raw = NULL; p->running = 0U; p->frame.valid = 0U; p->error = 0;
}
const RadarRangeFrame *radar_range_result(const RadarRangeProcessor *p)
{
    return p != NULL && p->initialized == RANGE_INIT_MAGIC && p->frame.valid ? &p->frame : NULL;
}
const char *radar_range_error_name(int error)
{
    switch (error) {
    case RADAR_RANGE_BUSY: return "BUSY";
    case RADAR_RANGE_DONE: return "DONE";
    case RADAR_RANGE_E_ARGUMENT: return "INVALID_ARGUMENT";
    case RADAR_RANGE_E_LENGTH: return "FINITE_HSI_LENGTH_MISMATCH";
    case RADAR_RANGE_E_HSI: return "STRICT_HSI_HEADER_MISMATCH";
    case RADAR_RANGE_E_STATE: return "PROCESSOR_STATE";
    case RADAR_RANGE_E_NUMERIC: return "NONFINITE_POWER";
    case RADAR_RANGE_E_SINK: return "FFT_CONSUMER_REJECTED";
    default: return "UNKNOWN";
    }
}
