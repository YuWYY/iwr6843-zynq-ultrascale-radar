/* SPDX-License-Identifier: MIT
 * Implements docs/m2_real/CONTROL_ABI.md from the frozen M2 design.
 * RX counters and tap measurements have distinct atomicity/generation domains.
 */
#include "radar_snapshot.h"
#include <string.h>
static int read(RadarControl *c,u8 page,u32 *v) { return radar_control_read32(c,page,v); }
static int finish(RadarSnapshot *s,int result)
{
    s->result=result;s->state=result?RADAR_SNAP_FAILED:RADAR_SNAP_DONE;return result;
}
static int ready(u32 flags,u32 phy)
{
    return (phy&(RADAR_PHY_BASELINE_VALID|RADAR_PHY_RX_READY|RADAR_PHY_REF_READY))==
                (RADAR_PHY_BASELINE_VALID|RADAR_PHY_RX_READY|RADAR_PHY_REF_READY) &&
           !(flags&(RADAR_FLAG_PHY_BUSY|RADAR_FLAG_RECOVERING));
}
void radar_snapshot_begin(RadarSnapshot *s,u64 now,u32 timeout_us)
{
    memset(s,0,sizeof(*s));s->state=RADAR_SNAP_READY_WAIT;s->started_us=now;s->timeout_us=timeout_us;
}
int radar_snapshot_poll(RadarSnapshot *s,RadarControl *c,u64 now)
{
    int rc;
    u32 flags,phy,rg,cg;
    if (!s||!c||!s->timeout_us) { return -1; }
    if (s->state==RADAR_SNAP_DONE) { return 0; }
    if (s->state==RADAR_SNAP_FAILED) { return s->result; }
    if (s->state==RADAR_SNAP_IDLE) { return -1; }
    if (now-s->started_us>=s->timeout_us) { return finish(s,-100); }
    if (s->state==RADAR_SNAP_READY_WAIT) {
        rc=read(c,RADAR_PAGE_FLAGS,&flags);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_PHY_STATUS,&phy);if(rc){return finish(s,rc);}
        if (!ready(flags,phy) || (flags&RADAR_FLAG_SNAPSHOT_PENDING)) { return 1; }
        rc=read(c,RADAR_PAGE_SNAPSHOT_GEN,&s->old_rx_generation);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_TAP_COPY_GEN,&s->old_copy_generation);if(rc){return finish(s,rc);}
        rc=radar_control_exec(c,RADAR_OP_SNAPSHOT,0U,0U,NULL);if(rc){return finish(s,rc);}
        s->state=RADAR_SNAP_COMPLETION_WAIT;return 1;
    }
    if (s->state==RADAR_SNAP_COMPLETION_WAIT) {
        rc=read(c,RADAR_PAGE_FLAGS,&flags);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_SNAPSHOT_GEN,&rg);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_TAP_COPY_GEN,&cg);if(rc){return finish(s,rc);}
        if ((flags&RADAR_FLAG_SNAPSHOT_PENDING)||!(flags&RADAR_FLAG_SNAPSHOT_VALID)||
            rg==s->old_rx_generation||cg==s->old_copy_generation) { return 1; }
        s->data.rx_generation=rg;s->data.tap_copy_generation=cg;s->index=0U;
        s->state=RADAR_SNAP_READ;return 1;
    }
    if (s->state==RADAR_SNAP_READ) {
        static const u8 extra_pages[]={0x0BU,0x0CU,0x0FU,0x10U,0x14U,0x02U,0x11U,0x12U,0x05U,0x06U,0x07U,0x08U};
        u32 value;
        if (s->index<12U) {
            rc=read(c,(u8)(0x20U+s->index),&s->data.words[s->index]);
        } else {
            u32 i=s->index-12U;
            rc=read(c,extra_pages[i],&value);
            if (!rc) {
                switch(i) {
                case 0:s->data.taps=value;break;
                case 1:s->data.tap_generation=value;break;
                case 2:s->data.phy_status=value;break;
                case 3:s->data.phy_generation=value;break;
                case 4:s->data.phy_fault=value;break;
                case 5:s->data.sys_faults=value;break;
                case 6:s->data.baseline=value;break;
                case 7:s->data.baseline_generation=value;break;
                case 8:s->data.first_sys_tick=value;break;
                case 9:s->data.first_sys_tick|=(u64)value<<32U;break;
                case 10:s->data.complete_sys_tick=value;break;
                case 11:s->data.complete_sys_tick|=(u64)value<<32U;break;
                default:return finish(s,-101);
                }
            }
        }
        if (rc) { return finish(s,rc); }
        if (++s->index==24U) { s->state=RADAR_SNAP_VALIDATE; }
        return 1;
    }
    if (s->state==RADAR_SNAP_VALIDATE) {
        u32 baseline_gen,phy_gen,phy_fault,sys_faults;
        rc=read(c,RADAR_PAGE_SNAPSHOT_GEN,&rg);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_TAP_COPY_GEN,&cg);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_FLAGS,&flags);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_PHY_STATUS,&phy);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_BASELINE_GEN,&baseline_gen);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_PHY_GEN,&phy_gen);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_PHY_FAULT,&phy_fault);if(rc){return finish(s,rc);}
        rc=read(c,RADAR_PAGE_SYS_FAULTS,&sys_faults);if(rc){return finish(s,rc);}
        if (rg!=s->data.rx_generation||cg!=s->data.tap_copy_generation||
            baseline_gen!=s->data.baseline_generation||phy_gen!=s->data.phy_generation||
            !ready(flags,phy)||!ready(flags,s->data.phy_status)||
            !(flags&RADAR_FLAG_SNAPSHOT_VALID)||(flags&RADAR_FLAG_SNAPSHOT_PENDING)||
            phy_fault||sys_faults||s->data.phy_fault||s->data.sys_faults) { return finish(s,-102); }
        s->data.flags=flags;s->data.phy_status=phy;return finish(s,0);
    }
    return finish(s,-101);
}
int radar_snapshot_matches(const RadarSnapshotData *d,const RadarCaptureProfile *p,u32 session)
{
    u64 bytes,pairs,beats;
    u32 wanted=RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_FIRST|RADAR_FLAG_SOURCE_COMPLETE|
               RADAR_FLAG_SNAPSHOT_VALID|RADAR_FLAG_DMA_RESETN;
    if (!d||!p||radar_capture_profile_validate(p)) { return -1; }
    bytes=(u64)p->packet_bytes*p->packet_count;
    pairs=((u64)d->words[1]<<32U)|d->words[0];
    beats=((u64)d->words[3]<<32U)|d->words[2];
    if (pairs!=bytes/4U||beats!=bytes/8U||d->words[4]!=p->packet_count||d->words[6]!=session) { return -2; }
    if ((d->words[7]&UINT32_C(0xffff)) || ((d->words[7]>>16U)&7U)!=4U ||
        d->words[9] || d->words[5]>8192U || d->words[11]!=0U ||
        (d->words[10]&((1U<<16U)|(1U<<5U))) ||
        ((d->words[10]>>11U)&31U)!=p->format_flags) { return -3; }
    if ((d->flags&wanted)!=wanted || (d->flags&(RADAR_FLAG_RX_FAULT|RADAR_FLAG_ARM_PENDING|RADAR_FLAG_CMD_PENDING)) ||
        d->sys_faults||d->phy_fault||!ready(d->flags,d->phy_status)||d->complete_sys_tick<d->first_sys_tick) { return -4; }
    if (p->use_delay && d->taps!=(p->taps[0]|(p->taps[1]<<9U)|(p->taps[2]<<18U))) { return -5; }
    /* Zero/wrapped tap generations are legal. Only change on actual new
     * measurement is interpreted; a copy generation is never substituted. */
    return 0;
}
int radar_phy_begin(RadarPhyTransaction *t,RadarControl *c,u8 opcode,u64 now)
{
    int rc;u32 flags;
    if (!t||!c||t->active||(opcode!=RADAR_OP_RECAL&&opcode!=RADAR_OP_DELAY_APPLY)) { return -1; }
    rc=read(c,RADAR_PAGE_FLAGS,&flags);if(rc){return rc;}
    if (flags&(RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_PHY_BUSY|RADAR_FLAG_SNAPSHOT_PENDING|RADAR_FLAG_RECOVERING)) { return -1; }
    rc=read(c,RADAR_PAGE_PHY_GEN,&t->old_generation);if(rc){return rc;}
    rc=radar_control_exec(c,opcode,0U,0U,NULL);if(rc){return rc;}
    t->opcode=opcode;t->started_us=now;t->active=1U;t->result=0U;t->phase=0U;
    return 0;
}
int radar_phy_poll(RadarPhyTransaction *t,RadarControl *c,u64 now)
{
    int rc;u32 flags,gen,phy,fault;
    if (!t||!c||!t->active) { return -1; }
    rc=read(c,RADAR_PAGE_FLAGS,&flags);if(rc){return rc;}
    rc=read(c,RADAR_PAGE_PHY_GEN,&gen);if(rc){return rc;}
    if (!(flags&RADAR_FLAG_PHY_BUSY)&&gen!=t->old_generation) {
        rc=read(c,RADAR_PAGE_PHY_STATUS,&phy);if(rc){return rc;}
        rc=read(c,RADAR_PAGE_PHY_FAULT,&fault);if(rc){return rc;}
        t->result=phy&15U;t->phase=(phy>>4U)&7U;t->active=0U;
        if (t->result!=1U||t->phase!=4U||fault||!(phy&RADAR_PHY_REF_READY)) { return -103; }
        if (t->opcode==RADAR_OP_DELAY_APPLY && !(phy&RADAR_PHY_RX_READY)) { return -103; }
        return 0;
    }
    if (now-t->started_us>=20000U) { t->active=0U;return -104; }
    return 1;
}
