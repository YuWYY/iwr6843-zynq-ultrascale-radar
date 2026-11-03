/* SPDX-License-Identifier: MIT */
#ifndef RADAR_DSP_DIAG_H
#define RADAR_DSP_DIAG_H
#include "radar_dsp.h"
#include "radar_console.h"

enum {
    RADAR_DIAG_IDLE=0, RADAR_DIAG_PREPARE, RADAR_DIAG_LOAD_READY,
    RADAR_DIAG_VERIFY, RADAR_DIAG_OUTPUT_PREPARE, RADAR_DIAG_RUNNING,
    RADAR_DIAG_CHECK, RADAR_DIAG_FROZEN, RADAR_DIAG_FAILED,
    RADAR_DIAG_RECOVERING, RADAR_DIAG_STOPPED
};
enum { RADAR_DIAG_ZERO=1, RADAR_DIAG_TONE=2, RADAR_DIAG_HOST_REPLAY=3 };
enum { RADAR_DIAG_REGIONS=6, RADAR_DIAG_E_CRC=-451,
       RADAR_DIAG_E_FORMAT=-452, RADAR_DIAG_E_CANARY=-453,
       RADAR_DIAG_E_RESULT=-454 };

/* Physically visible, 64-byte mailbox. It is a host observation/ownership
 * contract, not a command channel. Host tools resolve its ELF symbol address.
 * u32: magic,ABI,state(1=load,2=frozen,0=other),token,input,bytes,CRC,job,
 * error,source,driver_state,reserved[5]. Mutated only by this module. */
extern volatile uint32_t radar_dsp_diag_mailbox[16];

typedef struct {
    RadarDspDriver driver; /* Separate source-identity ledger; never scan.dsp. */
    RadarConsole *console;
    uint32_t state,source,token,offset,region,crc,expected_crc,input_crc;
    uint32_t region_crc[RADAR_DIAG_REGIONS];
    uint64_t submitted_us,done_us;
    int error,report_pending,expected_crc_valid;
} RadarDspDiag;

void radar_dsp_diag_init(RadarDspDiag *,RadarConsole *,const RadarDspIo *);
/* may_acquire is supplied by the application: no RF/session/scan/CLI/DMA
 * owner, no retained RAW lease or pending publication, hardware idle. */
int radar_dsp_diag_command(RadarDspDiag *,const char *,int may_acquire);
void radar_dsp_diag_poll(RadarDspDiag *);
int radar_dsp_diag_owns(const RadarDspDiag *);
/* Called by immediate SAFE routing; never releases a still-busy driver. */
void radar_dsp_diag_cancel(RadarDspDiag *);
#endif
