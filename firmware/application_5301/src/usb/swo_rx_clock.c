/* SWO receiver clock lease: HID queues mutations; main loop executes them.
 * All PLL / XIP clock transitions and bounded waits execute from ILM.
 */
#include "swo_rx_clock.h"
#include "cdc_interface.h"
#include "hpm_clock_drv.h"
#include "hpm_pllctlv2_drv.h"
#include "hpm_mchtmr_drv.h"
#include "hpm_interrupt.h"
#include <string.h>
#define NODE GET_CLK_NODE_FROM_NAME(BOARD_CDC_UART_CLK_NAME)
#define LEASE_TICKS (5000ULL*24000ULL)
#define FAST __attribute__((section(".fast"),noinline))
swo_rx_work_t swo_rx_work;
static struct {
  volatile uint32_t mode,baud;
  volatile uint64_t heartbeat;
  uint32_t active,changed,pllChanged,seq;
  int32_t rc;
  uint32_t actual,uartHz,osr,div,mux,sysdiv,blocker;
  uint32_t oldClock[36],nextClock[36],oldMfi,oldMfn,pllHz,oldBaud;
} s;
static uint32_t source_freq(uint32_t mux){
  if(!mux)return 24000000U;
  uint32_t pll=mux<=3?0:1,clk=mux<=3?mux-1:mux-4;
  uint32_t mfd=HPM_PLLCTLV2->PLL[pll].MFD&0x3fffffffU;
  if(!mfd)return 0;
  uint64_t f=24000000ULL*(HPM_PLLCTLV2->PLL[pll].MFI&127U)+24000000ULL*(HPM_PLLCTLV2->PLL[pll].MFN&0x3fffffffU)/mfd;
  return (uint32_t)(f*5U/(5U+(HPM_PLLCTLV2->PLL[pll].DIV[clk]&63U)));
}
static int node_active(unsigned n){
  static const clock_name_t names[36]={
    [0]=clock_mchtmr0,[9]=clock_gptmr0,[10]=clock_gptmr1,
    [13]=clock_i2c0,[14]=clock_i2c1,[15]=clock_i2c2,[16]=clock_i2c3,
    [17]=clock_spi0,[18]=clock_spi1,[19]=clock_spi2,[20]=clock_spi3,
    [21]=clock_uart0,[22]=clock_uart1,[23]=clock_uart2,[24]=clock_uart3,[25]=clock_uart4,
    [29]=clock_xpi0,[34]=clock_ref0,[35]=clock_ref1};
  if((HPM_SYSCTL->RESOURCE[65+n]&3U)==1U)return 1;
  if(n==29)return 1; /* Always protect XIP, including ROM-inherited configuration. */
  if(n>=30&&n<=33){for(unsigned g=0;g<4;g++)if(clock_check_in_group(clock_adc0,g))return 1;}
  if(names[n])for(unsigned g=0;g<4;g++)if(clock_check_in_group(names[n],g))return 1;
  return 0;
}
static FAST int set_node(unsigned n,uint32_t v){
  HPM_SYSCTL->CLOCK[n]=(HPM_SYSCTL->CLOCK[n]&~0x100007ffU)|(v&0x100007ffU);
  for(unsigned j=0;j<1000000;j++)if(!(HPM_SYSCTL->CLOCK[n]&0x40000000U))return ((HPM_SYSCTL->CLOCK[n]^v)&0x100007ffU)?-1:0;
  return -1;
}
static FAST int set_pll(uint32_t mfi,uint32_t mfn){
  HPM_PLLCTLV2->PLL[1].MFN=mfn;
  HPM_PLLCTLV2->PLL[1].MFI=mfi;
  for(unsigned j=0;j<1000000;j++){uint32_t v=HPM_PLLCTLV2->PLL[1].MFI;if(!(v&0x80000000U)&&(!(v&0x10000000U)||(v&0x20000000U)))return 0; /* A powered-off PLL is stable, per SDK. The UART node enables it later. */}
  return -1;
}
static FAST int apply(void){
  uint32_t irq=disable_global_irq(CSR_MSTATUS_MIE_MASK);int rc=0;
  for(unsigned n=0;n<36;n++)if(n!=NODE&&s.nextClock[n]!=s.oldClock[n]){s.changed=1;if(set_node(n,s.nextClock[n])){rc=-5;break;}}
  if(!rc&&s.pllHz){s.pllChanged=1;s.changed=1;rc=set_pll((s.oldMfi&~127U)|(s.pllHz/24000000U),(s.pllHz%24000000U)*10U)?-6:0;}
  if(!rc){s.changed=1;rc=set_node(NODE,s.nextClock[NODE])?-5:0;}
  restore_global_irq(irq);return rc;
}
static FAST int rollback(void){
  uint32_t irq=disable_global_irq(CSR_MSTATUS_MIE_MASK);int rc=0;
  if(s.pllChanged&&set_pll(s.oldMfi,s.oldMfn))rc=-7;
  /* Consumers stay on the stable PLL0 while restoring PLL1. */
  if(!rc)for(unsigned n=0;n<36;n++)if(s.nextClock[n]!=s.oldClock[n]||n==NODE)if(set_node(n,s.oldClock[n]))rc=-7;
  restore_global_irq(irq);return rc;
}
static int migrations(void){
  if(SYSCTL_CLOCK_CPU_MUX_GET(HPM_SYSCTL->CLOCK_CPU[0])>=4){s.blocker=36;return -4;}
  /* Only crystal-referenced, non-spread PLL1 with the SDK denominator. */
  if((HPM_PLLCTLV2->PLL[1].MFD&0x3fffffffU)!=240000000U||
      (HPM_PLLCTLV2->PLL[1].DIV[0]&63U)||
      (HPM_PLLCTLV2->PLL[1].CONFIG&(PLLCTLV2_PLL_CONFIG_REFSEL_MASK|PLLCTLV2_PLL_CONFIG_SPREAD_MASK)))return -4;
  for(unsigned n=0;n<36;n++){
    uint32_t v=s.oldClock[n],src=SYSCTL_CLOCK_MUX_GET(v),d=1U+SYSCTL_CLOCK_DIV_GET(v);
    if(n==NODE||src<4)continue;
    uint32_t f=source_freq(src)/d;int found=0;
    for(unsigned mux=0;mux<4&&!found;mux++){uint32_t root=source_freq(mux);
      for(unsigned div=1;div<=256;div++)if(root/div==f){s.nextClock[n]=(v&~0x7ffU)|(mux<<8)|(div-1);found=1;break;}}
    if(!found&&n==29){
      /* XIP may run slower during the lease, never faster than its ROM setup.
       * Move it to a stable PLL0 before touching PLL1; all transitions are ILM.
       */
      uint32_t closest=0;
      for(unsigned mux=0;mux<4;mux++){uint32_t root=source_freq(mux);
        for(unsigned div=1;div<=256;div++){uint32_t rate=root/div;
          if(rate<=f&&rate>closest){closest=rate;s.nextClock[n]=(v&~0x7ffU)|(mux<<8)|(div-1);found=1;}}}
    }
    if(!found&&node_active(n)){s.blocker=n;return -4;}
    /* Inactive consumers remain untouched, and are restored before handoff. */
  }
  return 0;
}
static int plan(void){
  if(!s.baud||s.baud>30000000U||s.mode>2)return -1;
  uint64_t best=~0ULL;uint32_t current=s.oldClock[NODE];
  uint32_t sources[8];for(unsigned m=0;m<8;m++)sources[m]=source_freq(m);
  for(unsigned m=0;m<8;m++)for(unsigned d=1;d<=256;d++){
    if(!s.mode&&(m!=SYSCTL_CLOCK_MUX_GET(current)||d!=1U+SYSCTL_CLOCK_DIV_GET(current)))continue;
    uint32_t f=sources[m]/d;if(!f||f>240000000U||f<s.baud*8U)continue;
    for(unsigned osr=8;osr<=30;osr+=2){uint64_t den=(uint64_t)s.baud*osr;uint32_t div=((uint64_t)f+den/2)/den;
      if(!div||div>65535)continue;
      uint32_t rate=f/(div*osr);uint64_t err=rate>s.baud?rate-s.baud:s.baud-rate;
      if(err<best){best=err;s.actual=rate;s.uartHz=f;s.osr=osr;s.div=div;s.mux=m;s.sysdiv=d;}
    }
  }
  if(s.mode==2&&best){
    for(unsigned osr=30;osr>=8;osr-=2){uint64_t f=(uint64_t)s.baud*osr;if(f>240000000U)continue;
      for(unsigned d=4;d<=16;d+=2){uint64_t pll=f*d;if(pll<400000000U||pll>1000000000U)continue;
        s.uartHz=f;s.osr=osr;s.div=1;s.mux=4;s.sysdiv=d;s.actual=s.baud;s.pllHz=pll;
        int rc=migrations();if(rc)return rc;best=0;goto chosen;}
    }
  }
chosen:
  if(best==~0ULL||best*1000000ULL>(uint64_t)s.baud*5000ULL)return -3;
  s.nextClock[NODE]=(current&~0x7ffU)|(s.mux<<8)|(s.sysdiv-1);return 0;
}
static void release(void){
  int dirty=s.changed||s.active;
  if(s.changed){int rc=rollback();if(rc){s.rc=rc;return;}}
  s.changed=s.pllChanged=0;s.active=0;swo_rx_work.token=0;s.pllHz=0;
  if(dirty)uartx_swo_reconfigure(s.oldBaud?s.oldBaud:115200U);s.rc=0;
}
void swo_rx_poll(void){
  if(!swo_rx_needs_service())return; /* No IRQ or timer overhead outside a SWO lease. */
  uint32_t irq=disable_global_irq(CSR_MSTATUS_MIE_MASK),p=swo_rx_work.pending;swo_rx_work.pending=0;restore_global_irq(irq);
  if(p==1){
    for(unsigned n=0;n<36;n++)s.nextClock[n]=s.oldClock[n]=HPM_SYSCTL->CLOCK[n];
    uint32_t diag[8];uartx_get_diag(diag);s.oldBaud=diag[2];
    s.oldMfi=HPM_PLLCTLV2->PLL[1].MFI;s.oldMfn=HPM_PLLCTLV2->PLL[1].MFN;s.pllHz=0;s.blocker=0xffffffffU;
    int error=uartx_get_cdc_source()!=CDC_SOURCE_UART?-2:plan();
    if(!error){error=apply();if(!error){
      s.active=1;uartx_swo_reconfigure(s.baud);uartx_get_diag(diag);
      if(diag[6]||diag[3]!=s.actual||diag[4]!=s.osr)error=-8;
    }}
    if(error&&s.changed){release();if(s.rc<0)error=s.rc;}
    if(error&&!s.changed)swo_rx_work.token=0;
    s.rc=error; /* Publish success only after UART readback is complete. */
  }else if(p==2)release();
  /* Heartbeat updates run in the USB ISR. Snapshot both timestamps atomically:
   * an ISR between reading 'now' and 'heartbeat' could otherwise wrap unsigned
   * subtraction and expire a freshly renewed lease immediately.
   */
  irq=disable_global_irq(CSR_MSTATUS_MIE_MASK);
  uint64_t now=mchtmr_get_count(HPM_MCHTMR),last=s.heartbeat;
  int expired=swo_rx_work.token&&now>=last&&(now-last)>LEASE_TICKS;
  restore_global_irq(irq);
  if(expired)release();
}
int swo_rx_active(void){return s.active;}
uint32_t swo_rx_baud(void){return s.active?s.baud:0;}
void swo_rx_uart_dividers(uint32_t *div,uint32_t *osr){*div=s.div;*osr=s.osr;}
void swo_rx_usb_reset(void){if(swo_rx_work.token)swo_rx_work.pending=2;}
void swo_rx_command(const uint8_t *req,uint8_t *res){
  uint32_t arg=0,mode=0;uint8_t act=req[3];int32_t rc=s.rc;
  if(req[1]>=6)memcpy(&arg,req+4,4);
  if(req[1]>=10)memcpy(&mode,req+8,4);
  if(act==1){
    if(req[1]<10)rc=-1;
    else if(swo_rx_work.token||swo_rx_work.pending)rc=-2;
    else {s.baud=arg;s.mode=mode;swo_rx_work.token=++s.seq;if(!swo_rx_work.token)swo_rx_work.token=++s.seq;s.heartbeat=mchtmr_get_count(HPM_MCHTMR);swo_rx_work.pending=1;s.rc=1;rc=1;}
  }else if(act==2){if(swo_rx_work.token&&arg!=swo_rx_work.token)rc=-2;else if(swo_rx_work.token){swo_rx_work.pending=2;s.rc=1;rc=1;}else{s.rc=0;rc=0;}}
  else if(act==3){if(!swo_rx_work.token||arg!=swo_rx_work.token)rc=-2;else s.heartbeat=mchtmr_get_count(HPM_MCHTMR);}
  else if(act>6)rc=-1;
  uint32_t w[14]={1,(uint32_t)rc,swo_rx_work.token,s.active?s.mode+1:0,s.baud,s.active?s.actual:0,
    clock_get_frequency(BOARD_CDC_UART_CLK_NAME),s.sysdiv,s.mux,s.osr,s.div,
    pllctlv2_get_pll_freq_in_hz(HPM_PLLCTLV2,pllctlv2_pll1),swo_rx_work.token?(uint32_t)(5000U-((mchtmr_get_count(HPM_MCHTMR)-s.heartbeat)/24000U>5000U?5000U:(mchtmr_get_count(HPM_MCHTMR)-s.heartbeat)/24000U)):0,s.blocker};
  if(act==4){w[1]=arg<3?0:(uint32_t)-1;for(unsigned n=0;n<12;n++)w[n+2]=arg<3?HPM_SYSCTL->CLOCK[arg*12+n]:0;}
  if(act==5){w[1]=0;for(unsigned n=0;n<8;n++)w[n+2]=source_freq(n);w[10]=HPM_SYSCTL->CLOCK_CPU[0];w[11]=HPM_PLLCTLV2->PLL[1].MFI;w[12]=HPM_PLLCTLV2->PLL[1].MFN;w[13]=HPM_PLLCTLV2->PLL[1].MFD;}
  if(act==6){w[1]=0;uartx_get_rx_diag(w+2);}
  res[1]=58;res[2]=0x19;res[3]=act;memcpy(res+4,w,sizeof(w));
}
