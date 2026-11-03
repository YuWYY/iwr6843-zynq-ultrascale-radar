/* SPDX-License-Identifier: MIT
 * Host-only deterministic register/UART/mailbox model.
 * Real uploaded AMD SG driver code executes above these seams. This is NOT
 * a cycle-accurate DMA, AXI, CDC, LVDS, DDR, timing or cache-coherency model.
 */
#define _GNU_SOURCE
#include "model.h"
#include "radar_hw.h"
#include "radar_memory.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "xuartps_hw.h"
#include <sys/mman.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Model model;
static u32 uart_control[2],uart_mode[2],uart_isr[2];
static XUartPsFormat uart_format;
static int mapped;
static const u32 RX_MASK=RADAR_FLAG_RX_READY|RADAR_FLAG_DESER_RESETN|RADAR_FLAG_CLOCK_ALIVE|RADAR_FLAG_FIFO_RESETN;
static XAxiDma_Config dma_config={.Name="axi_dma",.BaseAddr=RADAR_DMA_BASE,.HasS2Mm=1,.S2MmDataWidth=64,.HasSg=1,.S2MmNumChannels=1,.S2MmBurstSize=64,.AddrWidth=32,.SgLengthWidth=23};
static XGpio_Config gpio_config={.Name="gpio",.BaseAddress=RADAR_GPIO_BASE,.IsDual=1,.Width=32,.InterruptPresent=0};
static XUartPs_Config uart_config[2]={{.Name="uart0",.BaseAddress=RADAR_UART0_BASE,.InputClockHz=100000000U},{.Name="uart1",.BaseAddress=RADAR_UART1_BASE,.InputClockHz=100000000U}};

static void rx_ready(void)
{
    if(model.no_rx_clock){model.pages[1]&=~RX_MASK;model.pages[0x0f]&=~(RADAR_PHY_BASELINE_VALID|RADAR_PHY_RX_READY);}
    else{model.pages[1]|=RX_MASK;model.pages[0x0f]|=RADAR_PHY_BASELINE_VALID|RADAR_PHY_RX_READY;}
}
static u8 pattern(u32 offset){return (u8)((offset*17U+11U)&255U);}
static void raw_complete(void)
{
    u32 addr=model.dma[(XAXIDMA_RX_OFFSET+XAXIDMA_CDESC_OFFSET)/4U];
    u32 tail=model.dma[(XAXIDMA_RX_OFFSET+XAXIDMA_TDESC_OFFSET)/4U];
    u32 packet_bytes=model.pages[0x40]*4U,total=0U,i;
    if(!packet_bytes || !tail || !(model.dma[XAXIDMA_RX_OFFSET/4U]&1U)){return;}
    for(i=0U;i<128U;++i){
        volatile u32 *bd;
        u32 len,buf,status,j;
        assert(addr>=0x200000U&&addr<0x202000U&&(addr&63U)==0U);
        bd=(volatile u32 *)(UINTPTR)addr;
        len=bd[XAXIDMA_BD_CTRL_LEN_OFFSET/4U]&0x7fffffU;buf=bd[XAXIDMA_BD_BUFA_OFFSET/4U];
        assert(len&&len<=65536U&&buf>=0x400000U&&(u64)buf+len<=0xc00000U);
        for(j=0U;j<len;++j){((u8 *)(UINTPTR)buf)[j]=pattern(total+j);}
        status=XAXIDMA_BD_STS_COMPLETE_MASK|len;
        if(total%packet_bytes==0U){status|=XAXIDMA_BD_STS_RXSOF_MASK;}
        total+=len;
        if(total%packet_bytes==0U){status|=XAXIDMA_BD_STS_RXEOF_MASK;}
        if(model.corrupt_bd&&i==0U){status|=XAXIDMA_BD_STS_INT_ERR_MASK;}
        if(model.short_bd&&i==0U){status-=8U;}
        bd[XAXIDMA_BD_STS_OFFSET/4U]=status;
        model.dma[(XAXIDMA_RX_OFFSET+XAXIDMA_CDESC_OFFSET)/4U]=addr;
        if(addr==tail){break;}addr=bd[0];
    }
    model.produced_bytes=total;
    model.dma[(XAXIDMA_RX_OFFSET+XAXIDMA_SR_OFFSET)/4U]|=XAXIDMA_IDLE_MASK;
    model.pages[1]|=RADAR_FLAG_FIRST|RADAR_FLAG_SOURCE_COMPLETE;
    model.pages[5]=(u32)(model.now*100U);model.pages[6]=0U;
    model.pages[7]=model.pages[5]+1000U;model.pages[8]=0U;
    model.pages[0x20]=total/4U;model.pages[0x21]=0U;
    model.pages[0x22]=total/8U;model.pages[0x23]=0U;
    model.pages[0x24]=total/packet_bytes;model.pages[0x25]=64U;
    model.pages[0x26]=model.pages[0x43];model.pages[0x27]=4U<<16U;
    model.pages[0x28]=total/4U;model.pages[0x29]=0U;
    model.pages[0x2a]=((model.pages[0x44]&31U)<<11U)|(1U<<6U)|(1U<<3U)|(1U<<2U);
    model.pages[0x2b]=0U;
}
static void service(void)
{
    if(model.reset_at && !model.reset_stuck && model.now>=model.reset_at){
        model.reset_at=0U;memset(model.dma,0,sizeof(model.dma));
        model.dma[XAXIDMA_RX_OFFSET/4U]=0x10002U;
        model.dma[(XAXIDMA_RX_OFFSET+4U)/4U]=0x10009U;
        model.halt_at=0U;
    }
    if(model.halt_at && !model.halt_stuck && model.now>=model.halt_at){model.halt_at=0U;model.dma[(XAXIDMA_RX_OFFSET+4U)/4U]|=XAXIDMA_HALTED_MASK;}
    if(model.mailbox_at && !model.mailbox_stuck && model.now>=model.mailbox_at){model.mailbox_at=0U;model.reply=model.pending_reply;}
    if(model.phy_at && !model.phy_stuck && model.now>=model.phy_at){
        model.phy_at=0U;model.pages[1]&=~RADAR_FLAG_PHY_BUSY;
        model.pages[0x0f]=1U|(4U<<4U)|RADAR_PHY_REF_READY|RADAR_PHY_ARM_ADMISSIBLE;
        ++model.pages[0x10];model.pages[0x14]=0U;++model.pages[0x12];
        rx_ready();
    }
    if(model.arm_at && !model.arm_stuck && !model.no_rx_clock && model.now>=model.arm_at){
        model.arm_at=0U;model.pages[1]&=~(RADAR_FLAG_ARM_PENDING|RADAR_FLAG_CMD_PENDING);
        model.pages[1]|=RADAR_FLAG_RX_IDLE;
        rx_ready();
    }
    if(model.stream_at && model.now>=model.stream_at && !model.no_rx_clock && !model.arm_at){
        model.stream_at=0U;
        if(model.source_running&&!model.no_payload){raw_complete();}
    }
    if(model.snapshot_at && !model.snapshot_stuck && !model.no_rx_clock && model.now>=model.snapshot_at){
        model.snapshot_at=0U;model.pages[1]&=~RADAR_FLAG_SNAPSHOT_PENDING;
        model.pages[1]|=RADAR_FLAG_SNAPSHOT_VALID;++model.pages[9];++model.pages[0x13];
        if(model.wrong_snapshot_session){++model.pages[0x26];}
        if(model.wrong_snapshot_count){++model.pages[0x24];}
    }
}
void model_init(void)
{
    if(!mapped){
        void *p=mmap((void *)0x200000U,0x1200000U,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        if(p==MAP_FAILED){perror("mmap model DDR at 0x200000");exit(2);}mapped=1;
    }
    memset((void *)0x200000U,0,0x1200000U);memset(&model,0,sizeof(model));
    memset(uart_control,0,sizeof(uart_control));memset(uart_isr,0,sizeof(uart_isr));memset(uart_mode,0,sizeof(uart_mode));
    model.now=100U;model.halt_latency=50U;model.reset_latency=50U;model.mailbox_latency=2U;
    model.phy_latency=200U;model.snapshot_latency=200U;model.arm_latency=100U;
    model.dma[XAXIDMA_RX_OFFSET/4U]=0x10002U;model.dma[(XAXIDMA_RX_OFFSET+4U)/4U]=0x10009U;
    model.pages[0]=RADAR_EXPECTED_ABI;
    model.pages[1]=RADAR_FLAG_PG|RADAR_FLAG_PG_QUALIFIED|RADAR_FLAG_NERR_N|RADAR_FLAG_DMA_RESETN|RX_MASK|RADAR_FLAG_RX_IDLE;
    model.pages[0x0d]=100000000U;
    model.pages[0x0f]=1U|(4U<<4U)|RADAR_PHY_REF_READY|RADAR_PHY_ARM_ADMISSIBLE|RADAR_PHY_BASELINE_VALID|RADAR_PHY_RX_READY;
    model.pages[0x10]=1U;model.pages[0x11]=32U|(32U<<9U)|(32U<<18U);model.pages[0xb]=model.pages[0x11];
    model.pages[0x12]=1U;model.pages[0xc]=1U;
    model.pages[0x40]=32768U;model.pages[0x41]=1U;model.pages[0x44]=0U;
    uart_format=(XUartPsFormat){.BaudRate=115200U,.DataBits=XUARTPS_FORMAT_8_BITS,.Parity=XUARTPS_FORMAT_NO_PARITY,.StopBits=XUARTPS_FORMAT_1_STOP_BIT};
}
void model_advance(u64 us){model.now+=us;service();}
void model_set_page(u8 p,u32 value){model.pages[p]=value;}
u64 radar_hw_now_us(void){++model.now;service();return model.now;}
u32 radar_hw_counter_hz(void){return 1000000U;}
u64 radar_hw_counter(void){return radar_hw_now_us();}
void radar_hw_barrier(void){__sync_synchronize();}
int radar_hw_check_translation(UINTPTR a,u8 attr,int wr){(void)a;(void)attr;(void)wr;return model.mapping_fail?-1:0;}
UINTPTR radar_memory_bd_base(void){return 0x200000U;}
UINTPTR radar_memory_rx_base(void){return 0x400000U;}
UINTPTR radar_memory_capture_base(void){return 0xc00000U;}
void Xil_DCacheFlushRange(UINTPTR a,u32 n){(void)a;(void)n;++model.cache_flushes;}
void Xil_DCacheInvalidateRange(UINTPTR a,u32 n){(void)a;(void)n;++model.cache_invalidates;}
int xil_printf(const char *format,...){(void)format;return 0;}
XAxiDma_Config *XAxiDma_LookupConfig(UINTPTR a){return a==RADAR_DMA_BASE?&dma_config:NULL;}
XGpio_Config *XGpio_LookupConfig(UINTPTR a){return a==RADAR_GPIO_BASE?&gpio_config:NULL;}
int XGpio_CfgInitialize(XGpio *g,XGpio_Config *cfg,UINTPTR a){memset(g,0,sizeof(*g));g->BaseAddress=a;g->IsReady=XIL_COMPONENT_IS_READY;g->IsDual=cfg->IsDual;return 0;}
u32 XGpio_DiscreteRead(XGpio *g,unsigned channel){(void)g;assert(channel==2);++model.now;service();return model.reply;}
static void apply_gpio(u8 op,u8 index,u16 arg)
{
    switch(op){
    case RADAR_OP_IO_SET:
        model.pages[0xe]=arg;model.pages[1]&=~(RADAR_SW1_OUTPUT_MASK|1U);
        if(arg&1U){model.pages[1]|=1U;}
        if(arg==3U){model.pages[1]|=RADAR_FLAG_UART_OE|RADAR_FLAG_SYNC_OE;}
        if(arg==5U){model.pages[1]|=RADAR_FLAG_NRST_LOW;model.source_running=0;++model.producer_resets;model.stream_at=0U;}
        break;
    case RADAR_OP_CFG_LO:model.pages[0x40U+index]=(model.pages[0x40U+index]&0xffff0000U)|arg;break;
    case RADAR_OP_CFG_HI:model.pages[0x40U+index]=(model.pages[0x40U+index]&0xffffU)|((u32)arg<<16U);break;
    case RADAR_OP_RECOVER:
        assert(arg==0xc1eaU);assert(!model.source_running);
        assert(!(model.dma[XAXIDMA_RX_OFFSET/4U]&(XAXIDMA_CR_RUNSTOP_MASK|XAXIDMA_CR_RESET_MASK)));
        assert(model.dma[(XAXIDMA_RX_OFFSET+4U)/4U]&XAXIDMA_HALTED_MASK);
        model.pages[1]&=~(RADAR_SW1_ACTIVITY_MASK|RADAR_FLAG_FIRST|RADAR_FLAG_SOURCE_COMPLETE|RADAR_FLAG_RX_FAULT|RADAR_FLAG_SNAPSHOT_VALID);
        model.pages[2]=0U;model.arm_at=0U;model.snapshot_at=0U;memset(&model.pages[0x20],0,12U*sizeof(u32));break;
    case RADAR_OP_RECAL:case RADAR_OP_DELAY_APPLY:
        model.pages[1]|=RADAR_FLAG_PHY_BUSY;model.pages[0x0f]=(1U<<4U)|RADAR_PHY_REF_READY;
        model.phy_at=model.now+model.phy_latency;
        if(op==RADAR_OP_DELAY_APPLY){model.pages[0xb]=(model.pages[0x45]&511U)|((model.pages[0x46]&511U)<<9U)|((model.pages[0x47]&511U)<<18U);++model.pages[0xc];}
        break;
    case RADAR_OP_ARM:
        model.pages[1]|=RADAR_FLAG_SESSION_OPEN|RADAR_FLAG_ARM_PENDING|RADAR_FLAG_CMD_PENDING;
        model.pages[1]&=~(RADAR_FLAG_FIRST|RADAR_FLAG_SOURCE_COMPLETE);model.pages[3]=model.pages[0x43];
        model.arm_at=model.now+model.arm_latency;break;
    case RADAR_OP_SNAPSHOT:
        model.pages[1]|=RADAR_FLAG_SNAPSHOT_PENDING;model.pages[1]&=~RADAR_FLAG_SNAPSHOT_VALID;
        model.snapshot_at=model.now+model.snapshot_latency;break;
    case RADAR_OP_ABORT:
        model.arm_at=0U;model.phy_at=0U;model.pages[1]&=~(RADAR_FLAG_ARM_PENDING|RADAR_FLAG_CMD_PENDING|RADAR_FLAG_PHY_BUSY);
        if(model.pages[1]&RADAR_FLAG_SESSION_OPEN){model.pages[2]|=1U<<8U;}break;
    default:break;
    }
}
void XGpio_DiscreteWrite(XGpio *g,unsigned channel,u32 raw)
{
    u8 op=(u8)((raw>>24U)&127U),index=(u8)(raw>>16U);u16 arg=(u16)raw,value=0U;u32 reject=0U;
    (void)g;assert(channel==1U);++model.gpio_writes;++model.ops[op];
    if(op==RADAR_OP_READ){
        if(model.read_change_after && index==model.read_change_page &&
           ++model.read_change_count==model.read_change_after){++model.pages[index];}
        if(arg>1U){reject=RADAR_GPIO_BAD_VALUE;value=arg;}
        else if(model.read_reject_code && index==model.read_reject_page){reject=model.read_reject_code;}
        else if(index==255U){reject=RADAR_GPIO_NOT_READY;}
        else{u32 v=model.pages[index];value=(u16)(arg?v>>16U:v);}
    }
    else{apply_gpio(op,index,arg);value=arg;}
    model.pending_reply=(raw&0x80000000U)|((u32)index<<16U)|value;
    if(reject){model.pending_reply|=0x40000000U|(reject<<24U);}
    model.mailbox_at=model.now+model.mailbox_latency;
}
static void log_character(char ch)
{
    assert(model.log_count+1U<MODEL_LOG_BYTES);model.log[model.log_count++]=ch;model.log[model.log_count]='\0';
}
u32 Xil_In32(UINTPTR addr)
{
    ++model.now;service();
    if(addr>=RADAR_DMA_BASE && addr<RADAR_DMA_BASE+256U){return model.dma[(addr-RADAR_DMA_BASE)/4U];}
    if((addr>=RADAR_UART0_BASE&&addr<RADAR_UART0_BASE+256U)||(addr>=RADAR_UART1_BASE&&addr<RADAR_UART1_BASE+256U)){
        unsigned u=addr>=RADAR_UART1_BASE;u32 off=(u32)(addr-(u?RADAR_UART1_BASE:RADAR_UART0_BASE));
        if(off==XUARTPS_SR_OFFSET){return XUARTPS_SR_TXEMPTY|((u && model.uart_rx_head<model.uart_rx_tail)?0U:XUARTPS_SR_RXEMPTY);}
        if(off==XUARTPS_CR_OFFSET){return uart_control[u];}
        if(off==XUARTPS_MR_OFFSET){return uart_mode[u];}
        if(off==XUARTPS_ISR_OFFSET){return uart_isr[u]|(u&&model.uart_error?XUARTPS_IXR_FRAMING:0U);}
        if(off==XUARTPS_FIFO_OFFSET && u && model.uart_rx_head<model.uart_rx_tail){return (u8)model.uart_rx[model.uart_rx_head++];}
        return 0U;
    }
    fprintf(stderr,"unmodelled MMIO read %llx\n",(unsigned long long)addr);abort();
}
void Xil_Out32(UINTPTR addr,u32 value)
{
    ++model.now;service();
    if(addr>=RADAR_DMA_BASE && addr<RADAR_DMA_BASE+256U){
        u32 off=(u32)(addr-RADAR_DMA_BASE),old=model.dma[off/4U];
        model.dma[off/4U]=value;
        if(off==XAXIDMA_RX_OFFSET){
            if(value&XAXIDMA_CR_RESET_MASK){++model.dma_reset_writes;model.reset_at=model.now+model.reset_latency;}
            else if(value&XAXIDMA_CR_RUNSTOP_MASK){model.dma[(off+4U)/4U]&=~(XAXIDMA_HALTED_MASK|XAXIDMA_IDLE_MASK);}
            else if(old&XAXIDMA_CR_RUNSTOP_MASK){++model.dma_halt_writes;model.halt_at=model.now+model.halt_latency;}
        }
        if(off==XAXIDMA_RX_OFFSET+XAXIDMA_SR_OFFSET){model.dma[off/4U]=old&~(value&XAXIDMA_IRQ_ALL_MASK);}
        return;
    }
    if((addr>=RADAR_UART0_BASE&&addr<RADAR_UART0_BASE+256U)||(addr>=RADAR_UART1_BASE&&addr<RADAR_UART1_BASE+256U)){
        unsigned u=addr>=RADAR_UART1_BASE;u32 off=(u32)(addr-(u?RADAR_UART1_BASE:RADAR_UART0_BASE));
        if(off==XUARTPS_CR_OFFSET){uart_control[u]=value;}
        if(off==XUARTPS_MR_OFFSET){uart_mode[u]=value;}
        if(off==XUARTPS_ISR_OFFSET){uart_isr[u]&=~value;}
        if(off==XUARTPS_FIFO_OFFSET&&!u){log_character((char)value);}
        return;
    }
    fprintf(stderr,"unmodelled MMIO write %llx\n",(unsigned long long)addr);abort();
}
void model_force_uart(const char *response)
{
    size_t n=strlen(response);assert(n<sizeof(model.uart_rx));
    memcpy(model.uart_rx,response,n);model.uart_rx_head=0U;model.uart_rx_tail=(u32)n;
}
static void command_complete(void)
{
    char response[1024];const char *cmd=model.uart_tx;
    model.uart_tx[model.uart_tx_count]='\0';
    snprintf(model.last_command,sizeof(model.last_command),"%.255s",cmd);
    if(model.command_transcript_count<512U){snprintf(model.command_transcript[model.command_transcript_count++],256,"%.255s",cmd);}
    if(!strcmp(cmd,"sensorStart") || !strcmp(cmd,"sensorStart 0")){
        ++model.starts;
        /* Even an error/timeout response cannot prove the remote command had
         * no effect. Model it as potentially started. */
        model.source_running=1;model.no_rx_clock=0;rx_ready();model.stream_at=model.now+500U;
    }else if(!strcmp(cmd,"sensorStop")){++model.stops;if(!model.uart_stop_fail){model.source_running=0;model.stream_at=0U;}}
    else if(strcmp(cmd,"version")){++model.config_commands;}
    if(model.uart_silent){model.uart_tx_count=0U;return;}
    if(model.uart_echo_only){snprintf(response,sizeof(response),"%.239s\r\nmmwDemo:/>",cmd);}
    else if(!strcmp(cmd,"version")){
        snprintf(response,sizeof(response),"version\r\nmmWave SDK Version: %s\r\nDone\r\nmmwDemo:/>",model.uart_bad_version?"OTHER":"03.06.02.00");
    }else if((!strcmp(cmd,"sensorStart") || !strcmp(cmd,"sensorStart 0")) && model.uart_start_ignored){
        snprintf(response,sizeof(response),"%.239s\r\nIgnored: Sensor is already started\r\nDone\r\nmmwDemo:/>",cmd);
    }else if(((!strcmp(cmd,"sensorStart") || !strcmp(cmd,"sensorStart 0"))&&model.uart_start_fail)||(!strcmp(cmd,"sensorStop")&&model.uart_stop_fail)){
        snprintf(response,sizeof(response),"%.239s\r\nError -1\r\nmmwDemo:/>",cmd);
    }else{snprintf(response,sizeof(response),"%.239s\r\nDone\r\nmmwDemo:/>",cmd);}
    model_force_uart(response);model.uart_tx_count=0U;
}
XUartPs_Config *XUartPs_LookupConfig(u32 a){return a==RADAR_UART0_BASE?&uart_config[0]:a==RADAR_UART1_BASE?&uart_config[1]:NULL;}
s32 XUartPs_CfgInitialize(XUartPs *u,XUartPs_Config *c,u32 a){memset(u,0,sizeof(*u));u->Config=*c;u->Config.BaseAddress=a;u->IsReady=XIL_COMPONENT_IS_READY;return 0;}
void XUartPs_SetInterruptMask(XUartPs *u,u32 m){(void)u;(void)m;}
void XUartPs_SetOptions(XUartPs *u,u16 m){(void)u;(void)m;}
void XUartPs_SetOperMode(XUartPs *u,u8 m){(void)u;uart_mode[1]=m;}
u8 XUartPs_GetOperMode(XUartPs *u){(void)u;return (u8)uart_mode[1];}
s32 XUartPs_SetDataFormat(XUartPs *u,XUartPsFormat *f){(void)u;uart_format=*f;return 0;}
void XUartPs_GetDataFormat(XUartPs *u,XUartPsFormat *f){(void)u;*f=uart_format;}
void XUartPs_SetFifoThreshold(XUartPs *u,u8 m){(void)u;(void)m;}
void XUartPs_SetRecvTimeout(XUartPs *u,u8 m){(void)u;(void)m;}
u32 XUartPs_Send(XUartPs *u,u8 *buf,u32 size)
{
    u32 i;(void)u;
    if(size>32U){size=32U;}
    for(i=0U;i<size;++i){
        if(buf[i]=='\n'||buf[i]=='\r'){command_complete();}
        else{assert(model.uart_tx_count+1U<sizeof(model.uart_tx));model.uart_tx[model.uart_tx_count++]=(char)buf[i];}
    }
    return size;
}
u32 XUartPs_Recv(XUartPs *u,u8 *buf,u32 size)
{
    u32 available=model.uart_rx_tail-model.uart_rx_head;(void)u;
    if(size>available){size=available;}
    memcpy(buf,model.uart_rx+model.uart_rx_head,size);model.uart_rx_head+=size;return size;
}
void model_flush_console(RadarConsole *c)
{
    char command[80];u32 bound=0U;
    while(c->count){assert(++bound<10000U);(void)radar_console_poll(c,command,sizeof(command));}
}
void model_complete_descriptors(RadarDma *d,u32 packets)
{
    XAxiDma_Bd *bd=d->ring->HwHead;u32 i,n=packets*d->fragments_per_packet,total=0U;
    assert(n<=(u32)d->ring->HwCnt);
    for(i=0U;i<n;++i){
        u32 slot=(u32)(((UINTPTR)bd-d->bd_base)/d->ring->Separation),len=d->slot[slot].capacity;
        u32 status=XAXIDMA_BD_STS_COMPLETE_MASK|len,j;
        if(d->slot[slot].first){status|=XAXIDMA_BD_STS_RXSOF_MASK;}
        if(d->slot[slot].last){status|=XAXIDMA_BD_STS_RXEOF_MASK;}
        for(j=0U;j<len;++j){((u8 *)(d->rx_base+(UINTPTR)slot*65536U))[j]=pattern(total+j);}
        total+=len;*(volatile u32 *)((UINTPTR)bd+XAXIDMA_BD_STS_OFFSET)=status;
        bd=(XAxiDma_Bd *)(UINTPTR)XAxiDma_BdRingNext(d->ring,bd);
    }
}
void model_destroy_dma(RadarDma *d)
{
    /* TEST fixture teardown only, after process-model hardware is discarded.
     * Never used by target code or represented as a valid DMA recovery. */
    if(d->ring_created && d->ring && d->ring->CyclicBd){free(d->ring->CyclicBd);d->ring->CyclicBd=NULL;}
}
