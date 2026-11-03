/* SPDX-License-Identifier: MIT
 * One bounded ASCII record per service step. Export only the retained CPU
 * copy after successful source/DMA teardown; UART0 never holds a DMA lease.
 * Receiver must verify ordered offsets, BEGIN/END and the whole-data CRC32.
 */
#include "radar_session.h"
#include "radar_memory.h"
#include <stdio.h>
#define EXPORT_CHUNK 128U

void radar_export_poll(RadarSession *s)
{
    char b[448];
    const RadarCaptureResult *r=&s->result;
    const RadarCaptureProfile *p=s->profile;
    RadarFiniteGeometry g={0};
    int n=-1,rc;
    u32 take=0U,i;
    static const char hex[]="0123456789abcdef";
    if (!s->exporting) { return; }
    if (s->owns || !r->valid || s->state!=RADAR_SESSION_DONE) { s->exporting=0U;return; }
    /* Leave space for safety/status diagnostics. An output rejection does not
     * advance the stream and is reported by the console's drop counter. */
    if (radar_console_free(s->console)<1024U) { return; }
    switch (s->export_index) {
    case 0:n=snprintf(b,sizeof(b),"@R4BEGIN %u %u %08x",(unsigned)r->session,(unsigned)r->bytes,(unsigned)r->crc32);break;
    case 1:n=snprintf(b,sizeof(b),"@R4META software %s",RADAR_SW_VERSION);break;
    case 2:n=snprintf(b,sizeof(b),"@R4META profile_id %s",p->profile_id);break;
    case 3:n=snprintf(b,sizeof(b),"@R4META firmware_sha256_expected %s",p->firmware_sha256);break;
    case 4:n=snprintf(b,sizeof(b),"@R4META cfg_sha256_expected %s",p->cfg_sha256);break;
    case 5:n=snprintf(b,sizeof(b),"@R4META review_sha256 %s",p->review_sha256);break;
    case 6:n=snprintf(b,sizeof(b),"@R4META xsa_sha256_expected %s",RADAR_EXPECTED_XSA_SHA256);break;
    case 7:n=snprintf(b,sizeof(b),"@R4META bit_sha256_expected %s",RADAR_EXPECTED_BIT_SHA256);break;
    case 8:n=snprintf(b,sizeof(b),"@R4META transport RAW16_TWO_LANE_LITTLE_ENDIAN_NOT_ADC_FRAME_PROOF");break;
    case 9:n=snprintf(b,sizeof(b),"@R4META geometry %u %u %u %u %u",(unsigned)r->packet_bytes,(unsigned)r->packet_count,(unsigned)r->format,(unsigned)r->skip_pairs,(unsigned)r->startup_mode);break;
    case 10:n=snprintf(b,sizeof(b),"@R4META cpu_times_us %llu %llu %llu",(unsigned long long)r->begin_us,(unsigned long long)r->armed_us,(unsigned long long)r->complete_us);break;
    case 11:n=snprintf(b,sizeof(b),"@R4META sys_observed_ticks %llu %llu",(unsigned long long)r->snapshot.first_sys_tick,(unsigned long long)r->snapshot.complete_sys_tick);break;
    case 12:n=snprintf(b,sizeof(b),"@R4META snapshot_generations %u %u %u %u %u",(unsigned)r->snapshot.rx_generation,(unsigned)r->snapshot.tap_copy_generation,(unsigned)r->snapshot.tap_generation,(unsigned)r->snapshot.phy_generation,(unsigned)r->snapshot.baseline_generation);break;
    case 13:n=snprintf(b,sizeof(b),"@R4META snapshot_status %08x %08x %08x %08x %08x %08x",(unsigned)r->snapshot.flags,(unsigned)r->snapshot.phy_status,(unsigned)r->snapshot.sys_faults,(unsigned)r->snapshot.phy_fault,(unsigned)r->snapshot.taps,(unsigned)r->snapshot.baseline);break;
    case 14:n=snprintf(b,sizeof(b),"@R4META dma_before_reset %08x %08x %08x %08x %u",(unsigned)r->final_dma.control,(unsigned)r->final_dma.status,(unsigned)r->final_dma.current,(unsigned)r->final_dma.tail,(unsigned)r->dma_epoch);break;
    case 27:n=snprintf(b,sizeof(b),"@R4META qualification %s",p->qualification==RADAR_CAPTURE_FIRST_PACKET_TEST?"FIRST_PACKET_TEST":p->qualification==RADAR_CAPTURE_FINITE_FRAME_TEST?"FINITE_FRAME_TEST":"QUALIFIED");break;
    case 28:n=snprintf(b,sizeof(b),"@R4META review_bits %02x %02x",(unsigned)p->approvals,(unsigned)p->source_review_flags);break;
    case 29:n=snprintf(b,sizeof(b),"@R4META validation_scope %s",p->qualification!=RADAR_CAPTURE_QUALIFIED?"INPUT_TIMING_AND_WIRE_FORMAT_PENDING":"PROFILE_REVIEWED_NOT_LIVE_MEASUREMENT");break;
    case 30:
        if(p->qualification==RADAR_CAPTURE_FINITE_FRAME_TEST && radar_capture_profile_finite_geometry(p,&g)) { s->exporting=0U;return; }
        n=snprintf(b,sizeof(b),"@R4META finite_source %u %u %u %u %u",(unsigned)g.source_frames,(unsigned)g.hsi_header_enabled,(unsigned)g.chirps_per_frame,(unsigned)g.samples_per_rx,(unsigned)g.rx_mask);break;
    default:
        if (s->export_index<27U) {
            i=s->export_index-15U;
            n=snprintf(b,sizeof(b),"@R4META rx_word_%02u %08x",(unsigned)i,(unsigned)r->snapshot.words[i]);
        } else if (s->export_offset<r->bytes) {
            const u8 *data=(const u8 *)(radar_memory_capture_base()+s->export_offset);
            take=r->bytes-s->export_offset;if(take>EXPORT_CHUNK){take=EXPORT_CHUNK;}
            n=snprintf(b,sizeof(b),"@R4DATA %u %u ",(unsigned)r->session,(unsigned)s->export_offset);
            if(n<0 || (size_t)n+2U*take>=sizeof(b)){s->exporting=0U;return;}
            for(i=0U;i<take;++i){b[n++]=hex[data[i]>>4U];b[n++]=hex[data[i]&15U];}
            b[n]='\0';
        } else {
            n=snprintf(b,sizeof(b),"@R4END %u %u %08x",(unsigned)r->session,(unsigned)r->bytes,(unsigned)r->crc32);
        }
        break;
    }
    if(n<0 || (size_t)n>=sizeof(b)){s->exporting=0U;return;}
    rc=radar_console_printf(s->console,"%s\r\n",b);
    if(rc){return;}
    if(s->export_index<31U){++s->export_index;}
    else if(take){s->export_offset+=take;}
    else{s->exporting=0U;}
}
