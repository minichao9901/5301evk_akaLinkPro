/* H743 SPI1 master, PA4 CS / PA5 SCK / PA7 MOSI. Test fixture only.
 * HSI64 -> PLL1 400MHz CPU / 200MHz HCLK; PLL2P drives SPI123.
 * I-cache on, D-cache off: DMA and SWD see the same AXI buffer.
 * Register definitions checked against ST stm32h743xx.h / RM0433.
 */
#include <stdint.h>
#define R(a) (*(volatile uint32_t *)(a))
#define RCC 0x58024400u
#define SPI 0x40013000u
#define DMA 0x40020000u
#define GPIO 0x58020000u
volatile uint32_t g_run, g_mhz = 10, g_spi_hz, g_core_hz, g_clock_err;
volatile uint32_t g_refill_late, g_dma_errors, g_blocks, g_sequence;
volatile uint32_t g_spi_sr;
volatile uint32_t g_dma_flags, g_debug[10];
static uint32_t buffer[8192] __attribute__((aligned(32)));
static int wait(uint32_t a, uint32_t mask, uint32_t value) {
  for (uint32_t i=0;i<4000000;i++) if ((R(a)&mask)==value) return 1;
  g_clock_err++; return 0;
}
static void clock_init(void) {
  if (R(0x5802480c)&4) R(0x5802480c)&=~4u;
  R(0x58024818)=(R(0x58024818)&~(3u<<14))|(3u<<14);
  if (!wait(0x58024818,1u<<13,1u<<13)) return;
  R(0x52002000)=(R(0x52002000)&~0x3fu)|4u|(3u<<4);
  R(RCC)|=1u; R(RCC)&=~(3u<<3);
  if (!wait(RCC,4,4)) return;
  R(RCC+0x28)=(8u<<4)|(8u<<12);
  R(RCC+0x30)=99u|(1u<<9)|(3u<<16)|(3u<<24);
  R(RCC+0x34)=0;
  R(RCC+0x2c)=(1u<<16)|(2u<<2);
  R(RCC)|=1u<<24;
  if (!wait(RCC,1u<<25,1u<<25)) return;
  R(RCC+0x18)=8u|(4u<<4);
  R(RCC+0x1c)=(4u<<4)|(4u<<8);
  R(RCC+0x20)=4u<<4;
  R(RCC+0x10)=(R(RCC+0x10)&~7u)|3u;
  if (!wait(RCC+0x10,7u<<3,3u<<3)) return;
  g_core_hz=400000000;
  R(0xe000ef50)=0; __asm__ volatile("dsb; isb");
  R(0xe000ed14)|=1u<<17; __asm__ volatile("dsb; isb");
}
static int spi_clock(uint32_t mhz) {
  /* Integer MHz through 132; 133 selects 400/3 = 133.333MHz. */
  uint32_t p=2,n,rem=0;
  if (mhz==133) {p=3;n=100;}
  else { while (mhz*p*2<192 && p<128) p*=2; n=mhz*p/4;rem=(mhz*p)%4; }
  if (!n || n>100 || p>128) {g_clock_err++;return 0;}
  R(RCC)&=~(1u<<26); if (!wait(RCC,1u<<27,0)) return 0;
  R(RCC+0x38)=(n-1)|((p-1)<<9)|(1u<<16)|(1u<<24);
  R(RCC+0x3c)=(rem*2048u)<<3;
  R(RCC+0x2c)=(R(RCC+0x2c)&~(15u<<4))|(rem?1u<<4:0)|(2u<<6)|(1u<<19);
  R(RCC)|=1u<<26; if (!wait(RCC,1u<<27,1u<<27)) return 0;
  R(RCC+0x50)=(R(RCC+0x50)&~(7u<<12))|(1u<<12);
  g_spi_hz=((8000000u*n+2000000u*rem)/p)/2;
  return 1;
}
static void fill(uint32_t half) {
  uint32_t *dst=buffer+half*4096;
  for (uint32_t f=0;f<256;f++) {
    uint32_t seq=g_sequence++;
    *dst++=0x43495053u; *dst++=seq;
    for (uint32_t i=8;i<64;i+=4) {
      uint32_t w=0;
      for (uint32_t j=0;j<4;j++) w|=(((seq+17*(i+j))^0x5a)&255)<<(j*8);
      *dst++=w;
    }
  }
}
static void stop(void) {
  R(GPIO+0x18)=1u<<4; /* Release CS before clock/FIFO stop. */
  R(SPI)=0; R(DMA+0x10)&=~1u;
  wait(DMA+0x10,1,0); R(DMA+8)=0x3d;
}
static int start(void) {
  stop(); if (!spi_clock(g_mhz)) return 0;
  g_sequence=g_blocks=g_refill_late=g_dma_errors=0;
  fill(0);fill(1);
  R(SPI)=1u<<12; /* SSI high before MASTER: avoid hardware mode fault. */
  R(SPI+4)=0; /* TSIZE=0: endless master transaction. */
  R(SPI+8)=7u|(3u<<5); /* 8-bit frames, 4-byte packed TX DMA. */
  R(SPI+12)=(1u<<17)|(1u<<22)|(1u<<26)|(1u<<31);
  R(SPI+0x18)=0xffffffffu;
  R(0x40020800)=38; /* DMAMUX1 channel0 -> SPI1_TX */
  R(DMA+0x10)=(1u<<6)|(1u<<8)|(1u<<10)|(2u<<11)|(2u<<13)|(3u<<16);
  R(DMA+0x14)=8192; R(DMA+0x18)=SPI+0x20; R(DMA+0x1c)=(uint32_t)buffer;
  R(DMA+0x24)=(1u<<2)|3u; /* FIFO full threshold, packed word transfers. */
  R(SPI)=(1u<<12)|1u; /* Idle clock established before CS. */
  R(DMA+0x10)|=1u;
  R(SPI+8)|=1u<<15; /* DMA requests only after SPI is enabled. */
  R(GPIO+0x18)=1u<<20;
  R(SPI)|=1u<<9;
  return 1;
}
int main(void) {
  R(0xe000e010)=0;
  clock_init();
  R(RCC+0xe0)|=1; R(RCC+0xd8)|=1; R(RCC+0xf0)|=1u<<12;
  R(GPIO+0x18)=1u<<4;
  R(GPIO)=(R(GPIO)&~(0xffu<<8))|(1u<<8)|(2u<<10)|(2u<<14);
  R(GPIO+8)|=0xffu<<8;
  R(GPIO+0x20)=(R(GPIO+0x20)&~(0xfffu<<20))|(5u<<20)|(5u<<28);
  uint32_t running=0;
  for (;;) {
    if (!g_run && running) {stop();running=0;}
    if (g_run && !running) {if (!start()) g_run=0;else running=1;}
    if (running) {
      uint32_t flags=R(DMA);
      if ((flags&0x30)==0x30) g_refill_late++;
      if (flags&0xd) {g_dma_errors++;g_dma_flags=flags; R(DMA+8)=flags&0xd;
        g_debug[0]=R(RCC+0xf0);g_debug[1]=R(RCC+0xd8);
        g_debug[2]=R(SPI);g_debug[3]=R(SPI+8);g_debug[4]=R(SPI+12);g_debug[5]=R(SPI+0x14);
        g_debug[6]=R(DMA+0x10);g_debug[7]=R(DMA+0x14);g_debug[8]=R(DMA+0x1c);g_debug[9]=R(0x40020800);
      }
      if (flags&0x10) {R(DMA+8)=0x10; fill(0);g_blocks++;}
      if (flags&0x20) {R(DMA+8)=0x20; fill(1);g_blocks++;}
      g_spi_sr=R(SPI+0x14);
    }
  }
}
