/* SPDX-License-Identifier: Apache-2.0
 * F103ZE SPI1 master DMA stream. PA4 CS, PA5 SCK, PA6 MISO, PA7 MOSI.
 * g_run/g_div/g_pattern may be changed through SWD while the program runs.
 * 72MHz from HSE8, safe 64MHz HSI fallback; default SPI=18MHz (BR=1). */
#include <stdint.h>
#define REG(a) (*(volatile uint32_t *)(a))
#define RCC_CR REG(0x40021000)
#define RCC_CFGR REG(0x40021004)
#define GPIO_CRL REG(0x40010800)
#define GPIO_BSRR REG(0x40010810)
#define SPI_CR1 REG(0x40013000)
#define SPI_CR2 REG(0x40013004)
#define SPI_SR REG(0x40013008)
#define DMA_ISR REG(0x40020000)
#define DMA_IFCR REG(0x40020004)
#define DMA_CCR REG(0x40020030)
#define DMA_COUNT REG(0x40020034)
#define DMA_PERIPH REG(0x40020038)
#define DMA_MEMORY REG(0x4002003c)
volatile uint32_t g_run = 0, g_div = 1, g_pattern = 0;
volatile uint32_t g_mode = 0, g_lsb = 0;
volatile uint32_t g_core_hz, g_spi_hz, g_sequence, g_refill_late, g_blocks;
/* Integrity word for simultaneous JScope/SPI CDC regression. Low bits count
 * completed DMA halves; the high word stays fixed for sampling validation. */
volatile struct { uint32_t u_hi; } g_pack = { 0x10000000U };
static uint8_t data[4096] __attribute__((aligned(4)));
static const uint8_t hello[] = "hello world!\r\n";
static void clock_init(void)
{
    REG(0x40022000) = 0x12; /* flash prefetch, two wait states */
    RCC_CR |= 1U;
    RCC_CFGR &= ~3U; while (RCC_CFGR & 12U) {}
    RCC_CR &= ~(1U << 24); while (RCC_CR & (1U << 25)) {}
    RCC_CR |= 1U << 16;
    uint32_t timeout = 1000000U;
    while (!(RCC_CR & (1U << 17)) && --timeout) {}
    uint32_t pll = timeout ? ((1U << 16) | (7U << 18)) : (14U << 18);
    g_core_hz = timeout ? 72000000U : 64000000U;
    RCC_CFGR = pll | (4U << 8); /* APB1 half speed, APB2 full speed */
    RCC_CR |= 1U << 24; while (!(RCC_CR & (1U << 25))) {}
    RCC_CFGR |= 2U; while ((RCC_CFGR & 12U) != 8U) {}
}
static void fill(uint32_t offset)
{
    for (uint32_t p = offset; p < offset + sizeof(data)/2; p += 64) {
        uint32_t sequence = g_sequence++;
        data[p] = 'S'; data[p+1] = 'P'; data[p+2] = 'I'; data[p+3] = 'C';
        for (uint32_t i = 0; i < 4; i++) { data[p+4+i] = sequence >> (8*i); }
        for (uint32_t i = 8; i < 64; i++) { data[p+i] = (sequence + 17*i) ^ 0x5a; }
    }
}
static void stop(void)
{
    DMA_CCR = 0; SPI_CR2 = 0;
    uint32_t timeout = 100000U; while ((SPI_SR & 128U) && --timeout) {}
    GPIO_BSRR = 1U << 4; SPI_CR1 = 0; DMA_IFCR = 15U << 8;
}
int main(void)
{
    clock_init();
    REG(0x40021018) |= (1U<<0) | (1U<<2) | (1U<<12);
    REG(0x40021014) |= 1U; /* DMA1 */
    GPIO_BSRR = 1U << 4;
    GPIO_CRL = (GPIO_CRL & 0x0000ffffU) | 0xb4b30000U;
    uint32_t active = 0, div = ~0U, pattern = ~0U, mode = ~0U, lsb = ~0U;
    for (;;) {
        if (g_run != active || g_div != div || g_pattern != pattern || g_mode != mode || g_lsb != lsb) {
            stop(); active = g_run; div = g_div & 7U; pattern = g_pattern; mode = g_mode & 3U; lsb = g_lsb & 1U;
            if (!active) { continue; }
            g_sequence = g_refill_late = g_blocks = 0;
            g_pack.u_hi = 0x10000000U;
            if (pattern) { fill(0); fill(sizeof(data)/2); }
            else for (uint32_t i = 0; i < sizeof(data); i++) { data[i] = hello[i % (sizeof(hello)-1)]; }
            /* For hello mode make the ring an exact multiple of the string. */
            uint32_t size = pattern ? sizeof(data) : sizeof(data)/(sizeof(hello)-1)*(sizeof(hello)-1);
            DMA_PERIPH = 0x4001300cU; DMA_MEMORY = (uint32_t)data; DMA_COUNT = size;
            SPI_CR1 = (1U<<15) | (1U<<14) | (1U<<9) | (1U<<8) | (lsb<<7) | (div<<3) | (1U<<2) | ((mode & 2U)>>1) | ((mode & 1U)<<1);
            SPI_CR2 = 1U<<1;
            g_spi_hz = g_core_hz / (2U << div);
            /* Establish idle SCK before asserting CS (CPOL=1 must not look
             * like a data clock edge to an already-selected slave). */
            SPI_CR1 |= 1U << 6; GPIO_BSRR = 1U << (4+16);
            DMA_CCR = (2U<<12) | (1U<<7) | (1U<<5) | (1U<<4) | 1U;
        }
        if (active && pattern) {
            uint32_t flags = DMA_ISR & (6U << 8);
            if (flags == (6U<<8)) { g_refill_late++; }
            if (flags & (4U<<8)) { DMA_IFCR = 4U<<8; fill(0); g_blocks++; }
            if (flags & (2U<<8)) { DMA_IFCR = 2U<<8; fill(sizeof(data)/2); g_blocks++; }
            g_pack.u_hi = 0x10000000U | (g_blocks & 0xffffU);
        }
    }
}
