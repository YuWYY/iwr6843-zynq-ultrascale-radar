/* SPDX-License-Identifier: MIT
 * F20 KEY3 / PS_BUT: MIO43, active low, external 2.2k pull-up to 1.8 V.
 * Use the BSP low-level GPIO API: CfgInitialize masks interrupts on every bank,
 * which is unnecessary for this pin-only polling input. No self-test is run.
 */
#include "radar_keys.h"
#include "radar_display.h"
#include "radar_hw.h"
#include "xgpiops_hw.h"
#include "xparameters.h"
#include "xil_io.h"
#include <string.h>

#define KEY_MIO43_MUX ((UINTPTR)0xFF1800ACU)
#define KEY_MUX_MASK 0x000000FEU
#define KEY_BANK 1U
#define KEY_MASK (1U << 17U) /* ZynqMP GPIO bank 1 covers MIO26..51. */
#define KEY_POLL_US UINT64_C(5000)
#define KEY_DEBOUNCE_US UINT64_C(30000)
#define KEY_LONG_US UINT64_C(1000000)

static struct {
    u64 last_poll,changed_us,pressed_us;
    u8 candidate,stable,pressed,long_sent;
} key;

void radar_keys_init(void)
{
    const UINTPTR base=XPAR_XGPIOPS_0_BASEADDR;
    const u32 direction=KEY_BANK*XGPIOPS_REG_MASK_OFFSET+XGPIOPS_DIRM_OFFSET;
    const u32 enable=KEY_BANK*XGPIOPS_REG_MASK_OFFSET+XGPIOPS_OUTEN_OFFSET;
    /* Preserve every other GPIO/MIO bit and the existing electrical settings. */
    XGpioPs_WriteReg(base,enable,XGpioPs_ReadReg(base,enable)&~KEY_MASK);
    XGpioPs_WriteReg(base,direction,XGpioPs_ReadReg(base,direction)&~KEY_MASK);
    Xil_Out32(KEY_MIO43_MUX,Xil_In32(KEY_MIO43_MUX)&~KEY_MUX_MASK);
    memset(&key,0,sizeof key);
    key.candidate=key.stable=1U;
    key.last_poll=key.changed_us=radar_hw_now_us();
}

void radar_keys_poll(void)
{
    u64 now=radar_hw_now_us();
    u8 level;
    if(now-key.last_poll<KEY_POLL_US)return;
    key.last_poll=now;
    level=(XGpioPs_ReadReg(XPAR_XGPIOPS_0_BASEADDR,
              KEY_BANK*XGPIOPS_DATA_BANK_OFFSET+XGPIOPS_DATA_RO_OFFSET)&KEY_MASK)?1U:0U;
    if(level!=key.candidate){key.candidate=level;key.changed_us=now;}
    if(level!=key.stable && now-key.changed_us>=KEY_DEBOUNCE_US){
        key.stable=level;
        if(!level){key.pressed=1U;key.long_sent=0U;key.pressed_us=now;}
        else if(key.pressed){
            if(!key.long_sent)
                radar_display_request_page((RadarViewPage)
                    ((radar_display_get_requested_page()+1U)%RADAR_VIEW_COUNT));
            key.pressed=0U;
        }
    }
    if(key.pressed && !key.stable && !key.long_sent && now-key.pressed_us>=KEY_LONG_US){
        radar_display_request_page(RADAR_VIEW_OVERVIEW);
        key.long_sent=1U;
    }
}
