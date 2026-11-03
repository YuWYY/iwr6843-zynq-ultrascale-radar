/* SPDX-License-Identifier: MIT
 * Synthetic phase/order/scale tests plus a raw-file oracle entry point.
 * These are host calculations, not a physical ranging calibration.
 */
#include "../radar_runtime/src/processing/radar_range.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t raw[RADAR_RANGE_INPUT_BYTES];
static uint8_t memory[RADAR_RANGE_INPUT_BYTES];
static RadarRangeProcessor processor;
static void set16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
static void headers(void)
{
    unsigned c;
    memset(memory, 0, sizeof(memory));
    for (c = 0U; c < RADAR_RANGE_CHIRPS; ++c) {
        uint8_t *h = memory + c * RADAR_RANGE_RECORD_BYTES;
        static const uint8_t ident[8] = {0xdc,0x0a,0xda,0x0c,0xdc,0x0a,0xda,0x0c};
        memcpy(h, ident, 8);
        set16(h + 8, 48); set16(h + 10, 1); /* 0x1030=4144 */
        set16(h + 16, 0x744); set16(h + 18, 32);
        h[20]=4; h[21]=2; h[22]=0; h[23]=2; h[24]=15; h[25]=1;
        set16(h + 26, 1); set16(h + 28, 512);
        memset(h + 52, 15, 12);
    }
}
static void encode_raw(void)
{
    size_t b;
    for (b = 0; b < sizeof(raw); b += 8U) {
        raw[b]=memory[b]; raw[b+1]=memory[b+1];
        raw[b+2]=memory[b+4]; raw[b+3]=memory[b+5];
        raw[b+4]=memory[b+2]; raw[b+5]=memory[b+3];
        raw[b+6]=memory[b+6]; raw[b+7]=memory[b+7];
    }
}
static void tone(int bin, float amplitude, int offsets, int vary)
{
    unsigned c, rx, n;
    headers();
    for (c = 0; c < RADAR_RANGE_CHIRPS; ++c) {
        for (rx = 0; rx < RADAR_RANGE_RX; ++rx) {
            float a = amplitude * (vary ? (float)(rx + 1U) / 4.0f : 1.0f);
            for (n = 0; n < RADAR_RANGE_BINS; ++n) {
                double phase = 6.2831853071795864769 * (double)bin * n / 256.0 +
                               0.27 * rx + 0.19 * c;
                int i = (int)lround(a * cos(phase)) + (offsets ? 1700 : 0);
                int q = (int)lround(a * sin(phase)) - (offsets ? 2300 : 0);
                uint8_t *p = memory + c * RADAR_RANGE_RECORD_BYTES + 64U + rx * 1024U + n * 4U;
                set16(p, (uint16_t)q); set16(p + 2, (uint16_t)i);
            }
        }
    }
    encode_raw();
}
static const RadarRangeFrame *process(void)
{
    unsigned i;
    assert(radar_range_begin(&processor, raw, sizeof(raw), 123U, UINT64_C(987654321)) == RADAR_RANGE_BUSY);
    assert(radar_range_result(&processor) == NULL);
    for (i = 0; i < RADAR_RANGE_CHIRPS; ++i) {
        int rc = radar_range_step(&processor);
        assert(rc == (i + 1U == RADAR_RANGE_CHIRPS ? RADAR_RANGE_DONE : RADAR_RANGE_BUSY));
        assert(processor.next_chirp == i + 1U);
        assert(processor.frame.fft_count == (i + 1U) * RADAR_RANGE_RX);
    }
    assert(radar_range_step(&processor) == RADAR_RANGE_DONE);
    assert(radar_range_result(&processor) != NULL);
    assert(processor.frame.hsi_records == 96U && processor.frame.fft_count == 384U);
    for (i=0U;i<3U;++i) {
        assert(processor.frame.tx_records[i] == 32U);
        assert(isfinite(processor.frame.tx_power_dbfs[i]));
    }
    assert(processor.frame.session == 123U && processor.frame.timestamp_us == UINT64_C(987654321));
    assert(processor.frame.hsi_session_id == UINT64_C(0x0cda0adc0cda0adc));
    assert(fabsf(processor.frame.bin_m - 0.04409127594f) < 0.0000001f);
    for (i = 0U; i < RADAR_RANGE_BINS; ++i) { assert(isfinite(processor.frame.power_dbfs[i])); }
    return &processor.frame;
}
static void synthetic_tests(void)
{
    const RadarRangeFrame *f;
    unsigned i;
    assert(sizeof(RadarRangeProcessor) < 100U * 1024U);
    memset(&processor, 0, sizeof(processor));
    assert(radar_range_begin(&processor, raw, sizeof(raw), 1, 1) == RADAR_RANGE_E_STATE);
    radar_range_init(&processor);
    assert(radar_range_begin(&processor, NULL, sizeof(raw), 1, 1) == RADAR_RANGE_E_ARGUMENT);
    assert(radar_range_begin(&processor, raw, sizeof(raw)-1, 1, 1) == RADAR_RANGE_E_LENGTH);
    tone(24, 16384.0f, 0, 0); f=process();
    assert(f->peak_bin == 24U && fabsf(f->peak_dbfs + 6.0206f) < 0.003f);
    assert(f->power_dbfs[24] - f->power_dbfs[232] > 85.0f);
    assert(f->clipped_components == 0U);
    for (i=0U;i<3U;++i) { assert(fabsf(f->tx_power_dbfs[i] + 6.0206f) < 0.003f); }
    /* Each physical TX has a different amplitude and each RX a different
     * scale. This catches confusing TDM slot1(TX2) with physical TX1. */
    headers();
    for (i=0U;i<RADAR_RANGE_CHIRPS;++i) {
        static const unsigned order[3]={0U,2U,1U};
        unsigned rx,n,tx=order[i%3U];
        float amplitude=4000.0f*(float)(tx+1U);
        for(rx=0U;rx<4U;++rx) for(n=0U;n<256U;++n) {
            double phase=6.2831853071795864769*24.0*n/256.0+0.31*rx+0.17*i;
            float a=amplitude*(float)(rx+1U)/4.0f;
            uint8_t *dst=memory+i*RADAR_RANGE_RECORD_BYTES+64U+rx*1024U+n*4U;
            set16(dst,(uint16_t)(int16_t)lround(a*sin(phase)));
            set16(dst+2,(uint16_t)(int16_t)lround(a*cos(phase)));
        }
    }
    encode_raw();f=process();
    for(i=0U;i<3U;++i) {
        float a=4000.0f*(float)(i+1U)/32768.0f;
        assert(fabsf(f->tx_power_dbfs[i]-10.0f*log10f(a*a*30.0f/64.0f))<0.005f);
    }
    assert(fabsf(f->peak_dbfs-10.0f*log10f((4000.0f/32768.0f)*(4000.0f/32768.0f)*
                                           (14.0f/3.0f)*(30.0f/64.0f)))<0.005f);
    tone(-24, 16384.0f, 0, 0); f=process();
    assert(fabsf(f->power_dbfs[232] + 6.0206f) < 0.003f);
    assert(f->power_dbfs[232] - f->power_dbfs[24] > 85.0f);
    tone(24, 10000.0f, 1, 1); f=process();
    assert(f->peak_bin == 24U);
    /* Mean of amplitudes squared for1/4,2/4,3/4,4/4 is30/64. */
    assert(fabsf(f->peak_dbfs - 10.0f*log10f((10000.0f/32768.0f)*(10000.0f/32768.0f)*30.0f/64.0f)) < 0.005f);
    tone(0, 0.0f, 1, 0); f=process();
    for (i=0; i<RADAR_RANGE_BINS; ++i) { assert(f->power_dbfs[i] == RADAR_RANGE_FLOOR_DBFS); }
    headers();
    for (i=0; i<RADAR_RANGE_CHIRPS; ++i) {
        unsigned j;
        for (j=64U; j<RADAR_RANGE_RECORD_BYTES; j+=4U) {
            set16(memory+i*RADAR_RANGE_RECORD_BYTES+j, 32767);
            set16(memory+i*RADAR_RANGE_RECORD_BYTES+j+2U, 32768);
        }
    }
    encode_raw(); f=process();
    assert(f->clipped_components == RADAR_RANGE_CHIRPS*RADAR_RANGE_RX*RADAR_RANGE_BINS*2U);
    assert(f->power_dbfs[0] == RADAR_RANGE_FLOOR_DBFS);
    /* Every fixed protocol field is checked; corruption in the last record
     * is rejected before any FFT. It is not skipped/replaced with old data. */
    {
        static const unsigned offsets[]={0,8,9,10,11,12,16,18,20,21,22,23,24,25,26,28,30,32,34,36,38,40,42,44,51,52,63};
        for (i=0U; i<sizeof(offsets)/sizeof(offsets[0]); ++i) {
            headers(); memory[95U*RADAR_RANGE_RECORD_BYTES+offsets[i]] ^= 0x80U; encode_raw();
            assert(radar_range_begin(&processor, raw, sizeof(raw), 1, 1) == RADAR_RANGE_E_HSI);
            assert(processor.error_chirp == 95U && processor.frame.fft_count == 0U);
            assert(radar_range_result(&processor) == NULL);
        }
    }
    headers();
    {
        static const uint8_t alternate_id[8]={0xc9,0x0c,0xcc,0x09,0xc9,0x0c,0xcc,0x09};
        memcpy(memory+RADAR_RANGE_RECORD_BYTES, alternate_id, 8); encode_raw();
        assert(radar_range_begin(&processor, raw, sizeof(raw), 1, 1) == RADAR_RANGE_E_HSI);
        assert(processor.error_chirp == 1U);
    }
    tone(24, 1000.0f, 0, 0);
    assert(radar_range_begin(&processor, raw, sizeof(raw), 1, 1)==0);
    assert(radar_range_begin(&processor, raw, sizeof(raw), 2, 2)==RADAR_RANGE_E_STATE);
    radar_range_cancel(&processor); assert(radar_range_result(&processor)==NULL);
    assert(radar_range_step(&processor)==RADAR_RANGE_E_STATE);
    assert(radar_range_begin(&processor, raw, sizeof(raw), 3, 3)==0);
    radar_range_cancel(&processor);
    puts("PASS range DSP: strict headers, forward complex FFT sign/bin, dBFS scale, power average, DC, clipping, bounded steps and cancellation");
}
static int file_mode(const char *path, const char *output)
{
    FILE *fp=fopen(path,"rb"), *out;
    size_t n;
    int rc;
    unsigned bin;
    if (fp==NULL) { perror(path); return 2; }
    n=fread(raw,1,sizeof(raw),fp);
    if (n!=sizeof(raw) || fgetc(fp)!=EOF) { fclose(fp); fputs("wrong raw file size\n",stderr); return 2; }
    fclose(fp); radar_range_init(&processor);
    rc=radar_range_begin(&processor,raw,n,1U,0U);
    while(rc==RADAR_RANGE_BUSY) { rc=radar_range_step(&processor); }
    if(rc!=RADAR_RANGE_DONE) { fprintf(stderr,"DSP_ERROR %s chirp=%u\n",radar_range_error_name(rc),processor.error_chirp); return 2; }
    out=fopen(output,"w"); if(out==NULL) { perror(output); return 2; }
    fputs("bin,range_m,power_dbfs\n",out);
    for(bin=0;bin<RADAR_RANGE_BINS;++bin) {
        fprintf(out,"%u,%.10f,%.8f\n",bin,(double)bin*RADAR_RANGE_BIN_METERS,(double)processor.frame.power_dbfs[bin]);
    }
    fclose(out);
    printf("PASS raw HSI range: records=%u fft_count=%u peak_bin=%u peak_dbfs=%.6f clipped_components=%u\n",
        processor.frame.hsi_records,processor.frame.fft_count,processor.frame.peak_bin,
        (double)processor.frame.peak_dbfs,processor.frame.clipped_components);
    for (bin=0U;bin<3U;++bin) {
        printf("TX_STATS tx=%u records=%u power_dbfs=%.8f\n",bin,
            processor.frame.tx_records[bin],(double)processor.frame.tx_power_dbfs[bin]);
    }
    return 0;
}
int main(int argc, char **argv)
{
    if(argc==3) { return file_mode(argv[1],argv[2]); }
    if(argc!=1) { fputs("usage: test_range [raw.bin output.csv]\n",stderr); return 2; }
    synthetic_tests(); return 0;
}
