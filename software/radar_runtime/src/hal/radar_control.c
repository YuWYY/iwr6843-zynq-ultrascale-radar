/* SPDX-License-Identifier: MIT
 * SW1 GPIO mailbox, grounded in radar_control.v / ABI 0x52430101.
 * No DMA MMIO, radar UART, output enable or destructive memory scan here.
 * ALL CPU poll timeouts assume MMIO itself returns. An unclocked/unconfigured
 * AXI slave can stall the CPU before software can examine a deadline.
 */
#include "radar_control.h"
#include "radar_hw.h"
#include "xil_printf.h"
#include <string.h>

_Static_assert(sizeof(u32) == 4U, "32-bit GPIO words required");
_Static_assert(RADAR_CTRL_ATTACH_QUIET_US < RADAR_CTRL_ATTACH_TIMEOUT_US,
               "Quiet period must fit attach deadline");

static RadarControlReply decode_reply(u32 raw)
{
    RadarControlReply reply;
    reply.raw = raw;
    reply.ack = (u8)((raw >> 31U) & 1U);
    reply.error = (u8)((raw >> 30U) & 1U);
    reply.result = (u8)((raw >> 24U) & RADAR_CTRL_RESULT_MASK);
    reply.index = (u8)((raw >> 16U) & 0xFFU);
    reply.value = (u16)(raw & 0xFFFFU);
    return reply;
}

static int lose_sync(RadarControl *c, int status)
{
    c->pending = 0U;
    c->desynced = 1U;
    if (status == RADAR_CONTROL_E_TIMEOUT) {
        ++c->timeouts;
    } else {
        ++c->protocol_faults;
    }
    return status;
}

int radar_control_init(RadarControl *c)
{
    XGpio_Config *config;
    u32 raw, previous, count;
    u64 start, stable_since, now;
    if (c == NULL) { return RADAR_CONTROL_E_ARGUMENT; }
    memset(c, 0, sizeof(*c));
    config = XGpio_LookupConfig((UINTPTR)RADAR_GPIO_BASE);
    if ((config == NULL) || (config->BaseAddress != RADAR_GPIO_BASE) ||
        (config->IsDual != 1) || (config->Width != 32U) ||
        (config->InterruptPresent != 0)) {
        return RADAR_CONTROL_E_BSP;
    }
    if (radar_hw_counter_hz() == 0U) { return RADAR_CONTROL_E_TIMER; }
    /* Default supplied A53 EL3 map: identity, Device-nGnRnE at PL aperture.
     * AT/PAR examines translation without touching the GPIO slave. */
    if (radar_hw_check_translation((UINTPTR)RADAR_GPIO_BASE, 0x00U, 1) != 0) {
        return RADAR_CONTROL_E_MAPPING;
    }
    /* Supplied XGpio_CfgInitialize only initializes the instance in RAM.
     * Fixed output/input channels need no TRI writes or GPIO self-test. */
    if (XGpio_CfgInitialize(&c->gpio, config, config->BaseAddress) != XST_SUCCESS) {
        return RADAR_CONTROL_E_BSP;
    }
    radar_hw_barrier();
    previous = XGpio_DiscreteRead(&c->gpio, RADAR_GPIO_RESPONSE_CHANNEL);
    start = radar_hw_now_us();
    stable_since = start;
    for (count = 0U; count < RADAR_CTRL_MAX_POLLS; ++count) {
        raw = XGpio_DiscreteRead(&c->gpio, RADAR_GPIO_RESPONSE_CHANNEL);
        now = radar_hw_now_us();
        c->last_observed = raw;
        if ((now - start) >= RADAR_CTRL_ATTACH_TIMEOUT_US) {
            return lose_sync(c, RADAR_CONTROL_E_TIMEOUT);
        }
        if (raw != previous) {
            previous = raw;
            stable_since = now;
        }
        if ((now - stable_since) >= RADAR_CTRL_ATTACH_QUIET_US) {
            /* ACK is the actual RTL's processed toggle. Do not assume zero
             * on warm attach, or rely on output-DATA readback across IPs.
             * No concurrent writer or asynchronous PL reset is permitted. */
            c->initial_response = raw;
            c->last_response = raw;
            c->toggle = (u8)((raw >> 31U) & 1U);
            c->ack_seed = c->toggle;
            c->initialized = 1U;
            return RADAR_CONTROL_OK;
        }
    }
    return lose_sync(c, RADAR_CONTROL_E_TIMER);
}

int radar_control_command_begin(RadarControl *c, u8 opcode, u8 index,
                                u16 argument, u32 timeout_us)
{
    u32 raw;
    if ((c == NULL) || (c->initialized == 0U) || (opcode > 0x7FU) ||
        (timeout_us == 0U)) { return RADAR_CONTROL_E_ARGUMENT; }
    if (c->desynced != 0U) { return RADAR_CONTROL_E_DESYNC; }
    if (c->pending != 0U) { return RADAR_CONTROL_E_BUSY; }
    if ((c->abi_verified == 0U) && (opcode != RADAR_OP_READ)) {
        return RADAR_CONTROL_E_ABI;
    }
    raw = XGpio_DiscreteRead(&c->gpio, RADAR_GPIO_RESPONSE_CHANNEL);
    c->last_observed = raw;
    /* Responses are RTL registers, changed ONLY by a command or global reset.
     * A spontaneous change means another owner/reset: do not keep writing. */
    if (raw != c->last_response) {
        return lose_sync(c, RADAR_CONTROL_E_DESYNC);
    }
    c->toggle ^= 1U;
    c->pending_index = index;
    c->timeout_us = timeout_us;
    c->polls = 0U;
    c->issued_us = radar_hw_now_us();
    c->last_request = ((u32)c->toggle << 31U) |
                      ((u32)opcode << RADAR_CTRL_OPCODE_SHIFT) |
                      ((u32)index << RADAR_CTRL_INDEX_SHIFT) | (u32)argument;
    c->pending = 1U;
    ++c->submitted;
    radar_hw_barrier();
    XGpio_DiscreteWrite(&c->gpio, RADAR_GPIO_COMMAND_CHANNEL, c->last_request);
    radar_hw_barrier();
    return RADAR_CONTROL_OK;
}

int radar_control_command_poll(RadarControl *c, RadarControlReply *reply)
{
    u32 raw;
    u64 elapsed;
    RadarControlReply r;
    if ((c == NULL) || (reply == NULL) || (c->initialized == 0U)) {
        return RADAR_CONTROL_E_ARGUMENT;
    }
    if (c->desynced != 0U) { return RADAR_CONTROL_E_DESYNC; }
    if (c->pending == 0U) { return RADAR_CONTROL_E_ARGUMENT; }
    raw = XGpio_DiscreteRead(&c->gpio, RADAR_GPIO_RESPONSE_CHANNEL);
    c->last_observed = raw;
    r = decode_reply(raw);
    *reply = r;
    elapsed = radar_hw_now_us() - c->issued_us;
    ++c->polls;
    if (r.ack != c->toggle) {
        if (elapsed >= c->timeout_us) { return lose_sync(c,RADAR_CONTROL_E_TIMEOUT); }
        if (c->polls >= RADAR_CTRL_MAX_POLLS) { return lose_sync(c,RADAR_CONTROL_E_TIMER); }
        return RADAR_CONTROL_PENDING;
    }
    radar_hw_barrier();
    if ((r.index != c->pending_index) ||
        ((r.error != 0U) != (r.result != RADAR_GPIO_OK))) {
        return lose_sync(c, RADAR_CONTROL_E_PROTOCOL);
    }
    c->last_response = raw;
    c->pending = 0U;
    ++c->acknowledged;
    if (elapsed > c->max_ack_us) { c->max_ack_us = (u32)elapsed; }
    if (r.error != 0U) {
        ++c->remote_rejects;
        return RADAR_CONTROL_E_REMOTE;
    }
    if (elapsed>=c->timeout_us || c->polls>=RADAR_CTRL_MAX_POLLS) {
        ++c->late_acks;
        return RADAR_CONTROL_E_LATE; /* accepted; no blind resend */
    }
    return RADAR_CONTROL_OK;
}

int radar_control_exec(RadarControl *c,u8 opcode,u8 index,u16 argument,RadarControlReply *r)
{
    RadarControlReply local;
    int rc=radar_control_command_begin(c,opcode,index,argument,RADAR_CTRL_ACK_TIMEOUT_US);
    if (!r) { r=&local; }
    if (rc==0) { do { rc=radar_control_command_poll(c,r); } while (rc==RADAR_CONTROL_PENDING); }
    return rc;
}
int radar_control_write_config(RadarControl *c,u8 index,u32 value)
{
    u32 got=0U;
    int rc;
    if (index>=8U) { return RADAR_CONTROL_E_ARGUMENT; }
    rc=radar_control_exec(c,RADAR_OP_CFG_LO,index,(u16)value,NULL);
    if (rc==0) { rc=radar_control_exec(c,RADAR_OP_CFG_HI,index,(u16)(value>>16U),NULL); }
    if (rc==0) { rc=radar_control_read32(c,(u8)(RADAR_PAGE_SHADOW_BASE+index),&got); }
    if (rc==0 && got!=value) { rc=RADAR_CONTROL_E_VERIFY; }
    return rc;
}

static int read_transaction(RadarControl *c, u8 page, u16 argument,
                            RadarControlReply *reply)
{
    int rc = radar_control_command_begin(c, RADAR_OP_READ, page, argument,
                                        RADAR_CTRL_ACK_TIMEOUT_US);
    if (rc != RADAR_CONTROL_OK) { return rc; }
    do { rc = radar_control_command_poll(c, reply); }
    while (rc == RADAR_CONTROL_PENDING);
    return rc;
}

int radar_control_read16(RadarControl *c, u8 page, u8 high_half, u16 *value)
{
    RadarControlReply r;
    int rc;
    if ((value == NULL) || (high_half > 1U)) { return RADAR_CONTROL_E_ARGUMENT; }
    rc = read_transaction(c, page, (u16)high_half, &r);
    if (rc == RADAR_CONTROL_OK) { *value = r.value; }
    return rc;
}

int radar_control_read32(RadarControl *c, u8 page, u32 *value)
{
    unsigned attempt;
    u16 high_before, low, high_after;
    int rc;
    if (value == NULL) { return RADAR_CONTROL_E_ARGUMENT; }
    for (attempt = 0U; attempt < RADAR_CTRL_STABLE_RETRIES; ++attempt) {
        rc = radar_control_read16(c, page, 1U, &high_before);
        if (rc != RADAR_CONTROL_OK) { return rc; }
        rc = radar_control_read16(c, page, 0U, &low);
        if (rc != RADAR_CONTROL_OK) { return rc; }
        rc = radar_control_read16(c, page, 1U, &high_after);
        if (rc != RADAR_CONTROL_OK) { return rc; }
        if (high_before == high_after) {
            *value = ((u32)high_after << 16U) | (u32)low;
            return RADAR_CONTROL_OK;
        }
    }
    return RADAR_CONTROL_E_UNSTABLE;
}

int radar_control_check_abi(RadarControl *c, u32 *actual)
{
    int rc;
    if ((c == NULL) || (actual == NULL)) { return RADAR_CONTROL_E_ARGUMENT; }
    c->abi_verified = 0U;
    rc = radar_control_read32(c, RADAR_PAGE_ABI, actual);
    if (rc != RADAR_CONTROL_OK) { return rc; }
    if (*actual != RADAR_EXPECTED_ABI) { return RADAR_CONTROL_E_ABI; }
    c->abi_verified = 1U;
    return RADAR_CONTROL_OK;
}

int radar_control_read_status(RadarControl *c, RadarControlStatusView *v)
{
    unsigned i;
    int rc;
    u32 *destinations[7];
    static const u8 pages[7] = {
        RADAR_PAGE_FLAGS, RADAR_PAGE_SYS_FAULTS, RADAR_PAGE_IO_REQUEST,
        RADAR_PAGE_SYS_HZ, RADAR_PAGE_PHY_STATUS, RADAR_PAGE_PHY_GEN,
        RADAR_PAGE_PHY_FAULT
    };
    if ((c == NULL) || (v == NULL) || (c->abi_verified == 0U)) {
        return RADAR_CONTROL_E_ARGUMENT;
    }
    destinations[0] = &v->flags;
    destinations[1] = &v->sys_faults;
    destinations[2] = &v->io_request;
    destinations[3] = &v->sys_hz;
    destinations[4] = &v->phy_status;
    destinations[5] = &v->phy_generation;
    destinations[6] = &v->phy_fault;
    for (i = 0U; i < 7U; ++i) {
        rc = radar_control_read32(c, pages[i], destinations[i]);
        if (rc != RADAR_CONTROL_OK) { return rc; }
    }
    for (i = 0U; i < 8U; ++i) {
        rc = radar_control_read32(c, (u8)(RADAR_PAGE_SHADOW_BASE + i), &v->shadow[i]);
        if (rc != RADAR_CONTROL_OK) { return rc; }
    }
    /* These are individually sampled SYS pages, NOT an atomic RX snapshot. */
    return RADAR_CONTROL_OK;
}

void radar_control_print_status(const RadarControlStatusView *v)
{
    unsigned i;
    if (v == NULL) { return; }
    xil_printf("[PL] flags=0x%08x sys_faults=0x%08x io_request=0x%08x\r\n",
               v->flags, v->sys_faults, v->io_request);
    xil_printf("[PL] pg=%u qualified=%u nerr_n=%u clock_alive=%u rx_ready=%u\r\n",
               (v->flags >> 1U) & 1U, (v->flags >> 2U) & 1U,
               (v->flags >> 3U) & 1U, (v->flags >> 7U) & 1U,
               (v->flags >> 5U) & 1U);
    xil_printf("[PL] fifo_resetn=%u dma_stream_resetn=%u session_open=%u\r\n",
               (v->flags >> 8U) & 1U, (v->flags >> 20U) & 1U,
               (v->flags >> 9U) & 1U);
    xil_printf("[PL] phy=0x%08x terminal_gen=%u sticky_phy_fault=%u nominal_sys_hz=%u\r\n",
               v->phy_status, v->phy_generation, v->phy_fault, v->sys_hz);
    xil_printf("[PL] ref_ready=%u baseline_valid=%u arm_admissible=%u\r\n",
               (v->phy_status >> 8U) & 1U, (v->phy_status >> 7U) & 1U,
               (v->phy_status >> 10U) & 1U);
    for (i = 0U; i < 8U; ++i) {
        xil_printf("[CFG] shadow[%u]=0x%08x (read only)\r\n", i, v->shadow[i]);
    }
    if (((v->flags & RADAR_FLAG_CLOCK_ALIVE) == 0U) ||
        ((v->flags & RADAR_FLAG_RX_READY) == 0U)) {
        xil_printf("[INFO] RX clock/readiness may be absent in SW1; no SNAPSHOT or ARM requested\r\n");
    }
    if ((v->sys_faults != 0U) || (v->phy_fault != 0U)) {
        xil_printf("[WARN] PL faults are reported, NOT cleared; GPIO PASS is not PHY/capture PASS\r\n");
    }
}

static int quiet_view(const RadarControlStatusView *v)
{
    return (v->io_request == 0U) &&
           ((v->flags & (RADAR_SW1_ACTIVITY_MASK | RADAR_SW1_OUTPUT_MASK)) == 0U);
}

static int expected_read_reject(RadarControl *c, u8 page, u16 argument,
                                u8 expected_result, u16 expected_value)
{
    RadarControlReply r;
    int rc = read_transaction(c, page, argument, &r);
    if (rc != RADAR_CONTROL_E_REMOTE) {
        return (rc == RADAR_CONTROL_OK) ? RADAR_CONTROL_E_VERIFY : rc;
    }
    if ((r.result != expected_result) || (r.value != expected_value)) {
        return RADAR_CONTROL_E_VERIFY;
    }
    return RADAR_CONTROL_OK;
}

int radar_control_run_sw1(RadarControl *c)
{
    RadarControlStatusView before, after;
    u32 abi = 0U, i;
    int rc;
    xil_printf("\r\n---------- SW1 GPIO / REAL PL ABI ----------\r\n");
    xil_printf("[STEP] First PL MMIO at A0010000; current M2 bit and PS PL0 must be running\r\n");
    rc = radar_control_init(c);
    if (rc != RADAR_CONTROL_OK) { return rc; }
    xil_printf("[PASS] GPIO driver + Device mapping; ACK seed=%u initial_response=0x%08x\r\n",
               (u32)c->ack_seed, c->initial_response);
    rc = radar_control_check_abi(c, &abi);
    xil_printf("[PL] ABI read=0x%08x expected=0x%08x\r\n", abi, RADAR_EXPECTED_ABI);
    if (rc != RADAR_CONTROL_OK) { return rc; }
    xil_printf("[PASS] Actual PL ABI=0x52430101 (interface check, NOT bitstream hash)\r\n");
    rc = radar_control_read_status(c, &before);
    if (rc != RADAR_CONTROL_OK) { return rc; }
    radar_control_print_status(&before);
    if (!quiet_view(&before)) {
        xil_printf("[FAIL] Existing IO/session/transaction is active. No SAFE/reset/RECOVER was sent\r\n");
        return RADAR_CONTROL_E_ACTIVE;
    }
    if (before.sys_hz != UINT32_C(100000000)) {
        xil_printf("[FAIL] PL nominal clock parameter differs from current hardware contract\r\n");
        return RADAR_CONTROL_E_VERIFY;
    }
    for (i = 0U; i < RADAR_SW1_ABI_REPETITIONS; ++i) {
        rc = radar_control_check_abi(c, &abi);
        if (rc != RADAR_CONTROL_OK) { return rc; }
    }
    xil_printf("[PASS] ABI repeated reads: %u/%u; ACK toggles and echoed indices checked\r\n",
               RADAR_SW1_ABI_REPETITIONS, RADAR_SW1_ABI_REPETITIONS);
    /* Exact current RTL behavior: invalid READ argument -> BAD_VALUE(4),
     * unknown READ page -> NOT_READY(5), NOT BAD_INDEX(3). Neither writes cfg. */
    rc = expected_read_reject(c, RADAR_PAGE_ABI, 2U, RADAR_GPIO_BAD_VALUE, 2U);
    if (rc != RADAR_CONTROL_OK) { return rc; }
    xil_printf("[PASS] Negative READ argument=2: expected GPIO BAD_VALUE(4)\r\n");
    rc = expected_read_reject(c, 0xFFU, 0U, RADAR_GPIO_NOT_READY, 0U);
    if (rc != RADAR_CONTROL_OK) { return rc; }
    xil_printf("[PASS] Negative READ page=FF: expected GPIO NOT_READY(5)\r\n");
    rc = radar_control_check_abi(c, &abi);
    if (rc != RADAR_CONTROL_OK) { return rc; }
    xil_printf("[PASS] Valid ABI read after expected rejections\r\n");
    rc = radar_control_read_status(c, &after);
    if (rc != RADAR_CONTROL_OK) { return rc; }
    if (!quiet_view(&after) || (after.io_request != before.io_request)) {
        xil_printf("[FAIL] IO/session changed during read-only SW1\r\n");
        return RADAR_CONTROL_E_ACTIVE;
    }
    for (i = 0U; i < 8U; ++i) {
        if (after.shadow[i] != before.shadow[i]) {
            xil_printf("[FAIL] shadow[%u] changed during read-only test\r\n", i);
            return RADAR_CONTROL_E_VERIFY;
        }
    }
    xil_printf("[PASS] IO request and eight shadow registers unchanged\r\n");
    xil_printf("[INFO] Invalid READs are expected tests; no actual timeout is deliberately forced\r\n");
    return RADAR_CONTROL_OK;
}

void radar_control_print_diagnostics(const RadarControl *c)
{
    if (c == NULL) { return; }
    xil_printf("[MAILBOX] submitted=%u acked=%u remote_rejects=%u timeouts=%u protocol_faults=%u\r\n",
               c->submitted, c->acknowledged, c->remote_rejects,
               c->timeouts, c->protocol_faults);
    xil_printf("[MAILBOX] last_write=0x%08x last_observed=0x%08x max_ack_us=%u desynced=%u\r\n",
               c->last_request, c->last_observed, c->max_ack_us, (u32)c->desynced);
}

const char *radar_control_status_name(int status)
{
    switch (status) {
    case RADAR_CONTROL_OK: return "OK";
    case RADAR_CONTROL_PENDING: return "PENDING";
    case RADAR_CONTROL_E_ARGUMENT: return "BAD_SOFTWARE_ARGUMENT";
    case RADAR_CONTROL_E_BSP: return "BSP_GPIO_MISMATCH";
    case RADAR_CONTROL_E_TIMER: return "SOFTWARE_TIMER_STALLED";
    case RADAR_CONTROL_E_TIMEOUT: return "ACK_TIMEOUT_NO_AUTO_RETRY";
    case RADAR_CONTROL_E_DESYNC: return "MAILBOX_DESYNCHRONIZED";
    case RADAR_CONTROL_E_PROTOCOL: return "ACK_RESPONSE_PROTOCOL_ERROR";
    case RADAR_CONTROL_E_REMOTE: return "GPIO_REJECTED_REQUEST";
    case RADAR_CONTROL_E_ABI: return "PL_ABI_MISMATCH_OR_UNVERIFIED";
    case RADAR_CONTROL_E_ACTIVE: return "TARGET_NOT_QUIESCENT";
    case RADAR_CONTROL_E_UNSTABLE: return "PAGE_CHANGED_DURING_READ";
    case RADAR_CONTROL_E_VERIFY: return "SW1_RESULT_MISMATCH";
    case RADAR_CONTROL_E_MAPPING: return "GPIO_NOT_DEFAULT_DEVICE_MAPPING";
    case RADAR_CONTROL_E_BUSY: return "MAILBOX_ALREADY_HAS_OWNER";
    case RADAR_CONTROL_E_LATE: return "ACK_ACCEPTED_LATE_NO_RESEND";
    default: return "UNKNOWN_SOFTWARE_RESULT";
    }
}
