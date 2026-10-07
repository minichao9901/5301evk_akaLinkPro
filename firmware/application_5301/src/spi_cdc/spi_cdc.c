/* SPDX-License-Identifier: Apache-2.0 */
/* SPI2 slave -> existing CDC ACM. HID only queues; main loop owns hardware.
 * No extra sample buffer: the SPI master's 16KB OUT ring is circular RX DMA.
 * DMA IRQ only counts laps/errors. CDC uses its existing 32KB USB-safe ring. */
#include <string.h>
#include "board.h"
#include "spi_cdc.h"
#include "spi_cdc_ring.h"
#include "spi_bridge.h"
#include "usb_composite.h"
#include "cdc_interface.h"
service_gate_t spi_cdc_gate;
#define s_running (spi_cdc_gate.flag[0])
#define s_start_req (spi_cdc_gate.flag[1])
#define s_stop_req (spi_cdc_gate.flag[2])
#define s_reset_req (spi_cdc_gate.flag[3])
#define s_fault (spi_cdc_gate.flag[4])
#define s_configuring (spi_cdc_gate.flag[5])
static volatile int32_t s_result;
static volatile uint8_t s_mode, s_lsb;
static uint32_t s_generation, s_received, s_forwarded, s_dropped, s_overflow, s_errors;
static uint32_t s_read, s_size;

#if defined(BOARD_HAS_SPI_BRIDGE) && BOARD_HAS_SPI_BRIDGE
#include "hpm_interrupt.h"
#include "hpm_spi_drv.h"
#include "hpm_dma_mgr.h"
#include "hpm_dmav2_drv.h"
#include "hpm_dmamux_src.h"
#include "hpm_clock_drv.h"
#include "hpm_gpio_drv.h"
#include "pinmux.h"
static dma_resource_t s_dma;
static uint8_t *s_receive;
static volatile uint32_t s_laps;
static uint8_t s_claimed;

static void completed(DMA_Type *base, uint32_t channel, void *data)
{
    (void)base; (void)channel; (void)data; s_laps++;
}
static void failed(DMA_Type *base, uint32_t channel, void *data)
{
    (void)base; (void)channel; (void)data; s_fault = 1U;
}
static uint32_t written(void)
{
    if (!s_dma.base) { return s_received; }
    uint32_t lock = disable_global_irq(CSR_MSTATUS_MIE_MASK);
    uint32_t mask = 1UL << s_dma.channel, before, after, destination;
    /* A wrap can happen even with CPU IRQs masked. Retry if TC changes while
     * reading DSTADDR; never clear status here (the DMA manager owns it). */
    do {
        before = s_dma.base->INTTCSTS & mask;
        destination = s_dma.base->CHCTRL[s_dma.channel].DSTADDR;
        after = s_dma.base->INTTCSTS & mask;
    } while (before != after);
    uint32_t position = destination - (uint32_t)s_receive;
    if (position > s_size) { position = 0U; s_fault = 1U; }
    uint32_t total = spi_cdc_position(s_laps, after ? 1U : 0U, position, s_size, s_received);
    restore_global_irq(lock);
    return total;
}
static void stop(void)
{
    if (s_claimed) { spi_disable_rx_dma(HPM_SPI2); }
    if (s_dma.base) {
        dma_mgr_disable_channel(&s_dma);
        if (s_running) { s_received = written(); }
        dma_mgr_release_resource(&s_dma);
        memset(&s_dma, 0, sizeof(s_dma));
    }
    if (s_claimed) {
        s_dropped += s_received - s_read;
        s_read = s_received;
        /* Retire DMA before releasing the shared storage or SPI pins. */
        spi_bridge_slave_release(); s_claimed = 0U;
        for (uint32_t pad = IOC_PAD_PB10; pad <= IOC_PAD_PB13; pad++) {
            HPM_IOC->PAD[pad].FUNC_CTL = 0U;
            gpio_set_pin_input(HPM_GPIO0, GPIO_GET_PORT_INDEX(pad), GPIO_GET_PIN_INDEX(pad));
        }
    }
    if (uartx_get_cdc_source() == CDC_SOURCE_SPI) { uartx_set_cdc_source(CDC_SOURCE_UART); }
    s_running = 0U;
}
static int32_t start(void)
{
    if (s_running || uartx_get_cdc_source() != CDC_SOURCE_UART) { return -2; }
    uint32_t lock = disable_global_irq(CSR_MSTATUS_MIE_MASK);
    uint8_t claimed = spi_bridge_slave_claim(&s_receive, &s_size);
    restore_global_irq(lock);
    if (!claimed) { return -2; }
    s_claimed = 1U;
    clock_add_to_group(clock_spi2, 0);
    init_spi2_bridge_pins(0U, 1U); /* same pins, hardware CS input in slave mode */
    spi_format_config_t format = {0};
    spi_slave_get_default_format_config(&format);
    format.master_config.addr_len_in_bytes = 1U;
    format.common_config.data_len_in_bits = 8U;
    format.common_config.lsb = s_lsb != 0U;
    format.common_config.cpol = (s_mode & 2U) ? spi_sclk_high_idle : spi_sclk_low_idle;
    format.common_config.cpha = (s_mode & 1U) ? spi_sclk_sampling_even_clk_edges : spi_sclk_sampling_odd_clk_edges;
    spi_format_init(HPM_SPI2, &format);
    spi_control_config_t control = {0};
    spi_slave_get_default_control_config(&control);
    control.slave_config.slave_data_only = true;
    control.common_config.trans_mode = spi_trans_write_read_together;
    control.common_config.data_phase_fmt = spi_single_io_mode;
    if (spi_control_init(HPM_SPI2, &control, 1U, 1U) != status_success) { stop(); return -4; }
    if (dma_mgr_request_resource(&s_dma) != status_success) { stop(); return -3; }
    dma_mgr_chn_conf_t config;
    dma_mgr_get_default_chn_config(&config);
    config.src_width = config.dst_width = DMA_MGR_TRANSFER_WIDTH_BYTE;
    config.src_mode = DMA_MGR_HANDSHAKE_MODE_HANDSHAKE;
    config.dst_mode = DMA_MGR_HANDSHAKE_MODE_NORMAL;
    config.src_addr_ctrl = DMA_MGR_ADDRESS_CONTROL_FIXED;
    config.dst_addr_ctrl = DMA_MGR_ADDRESS_CONTROL_INCREMENT;
    config.src_addr = (uint32_t)&HPM_SPI2->DATA;
    config.dst_addr = (uint32_t)s_receive;
    config.size_in_byte = s_size;
    config.en_dmamux = true; config.dmamux_src = HPM_DMA_SRC_SPI2_RX;
    config.en_infiniteloop = true;
    config.priority = DMA_MGR_CHANNEL_PRIORITY_HIGH;
    config.interrupt_mask = DMA_MGR_INTERRUPT_MASK_ALL;
    if (dma_mgr_setup_channel(&s_dma, &config) != status_success) { stop(); return -3; }
    dma_mgr_install_chn_tc_callback(&s_dma, completed, NULL);
    dma_mgr_install_chn_error_callback(&s_dma, failed, NULL);
    dma_mgr_enable_chn_irq(&s_dma, DMA_MGR_INTERRUPT_MASK_TC | DMA_MGR_INTERRUPT_MASK_ERROR);
    dma_mgr_enable_dma_irq_with_priority(&s_dma, 1U);
    s_laps = s_read = s_received = s_forwarded = s_dropped = s_overflow = s_errors = 0U;
    s_fault = 0U;
    uartx_set_cdc_source(CDC_SOURCE_SPI);
    chry_dap_usb2uart_set_enabled(1U);
    s_running = 1U;
    dma_mgr_enable_channel(&s_dma);
    spi_enable_rx_dma(HPM_SPI2);
    return 0;
}
static void forward(void)
{
    uint32_t budget = 4096U;
    /* Snapshot once: chasing continuously arriving DMA bytes turns the byte
     * budget into a ~1.8ms wait at 18MHz and starves the next scope/DAP poll.
     * New arrivals remain in DMA storage for the next main-loop turn. */
    s_received = written();
    s_dropped += spi_cdc_trim(s_received, &s_read, s_size);
    while (budget) {
        uint32_t lock = disable_global_irq(CSR_MSTATUS_MIE_MASK);
        uint32_t n = spi_cdc_chunk(s_received, s_read, s_size, chry_ringbuffer_get_free(&g_uartrx), budget);
        if (n) { chry_ringbuffer_write(&g_uartrx, s_receive + s_read % s_size, n); }
        restore_global_irq(lock);
        if (!n) { break; }
        s_read += n; s_forwarded += n; budget -= n;
    }
    uint32_t status = spi_get_interrupt_status(HPM_SPI2);
    if (status & spi_rx_fifo_overflow_int) {
        spi_clear_interrupt_status(HPM_SPI2, spi_rx_fifo_overflow_int); s_overflow++;
    }
}
#else
static void stop(void) { s_running = 0U; }
static int32_t start(void) { return -1; }
static void forward(void) {}
#endif

uint8_t spi_cdc_running(void) { return s_running; }
void spi_cdc_usb_reset(void) { s_reset_req = 1U; }
void spi_cdc_poll(void)
{
    if (s_reset_req || s_stop_req || s_fault) {
        uint8_t fault = s_fault;
        s_start_req = 0U; s_reset_req = 0U; s_stop_req = 0U;
        stop();
        if (fault) { s_errors++; }
        s_fault = 0U; s_result = fault ? -3 : 0;
    }
    if (s_start_req) { s_configuring = 1U; s_start_req = 0U; s_result = start(); s_configuring = 0U; }
    if (s_running) { forward(); }
}
static void put32(uint8_t *p, uint32_t word)
{
    p[0] = word; p[1] = word >> 8; p[2] = word >> 16; p[3] = word >> 24;
}
void spi_cdc_hid(uint8_t *request, uint8_t *response)
{
    uint8_t action = request[3];
    if (action == SPI_CDC_START) {
        if (request[4] > 3U || request[5] > 1U) { s_result = -5; }
        else if (s_running || s_start_req || s_stop_req || s_reset_req || s_configuring) { s_result = -2; }
        else {
            s_mode = request[4]; s_lsb = request[5];
            s_result = -100; s_generation++; s_start_req = 1U;
        }
    } else if (action == SPI_CDC_STOP) { s_result = -100; s_stop_req = 1U; }
    else if (action != SPI_CDC_STATUS) { s_result = -5; }
    uint32_t supported = 0U;
#if defined(BOARD_HAS_SPI_BRIDGE) && BOARD_HAS_SPI_BRIDGE
    supported = 1U;
#endif
    uint32_t words[] = {SPI_CDC_MAGIC, supported | ((uint32_t)s_running << 1) |
        ((uint32_t)(s_start_req || s_stop_req || s_reset_req || s_configuring) << 2), (uint32_t)s_result,
        s_mode | ((uint32_t)s_lsb << 8), s_size, s_received, s_forwarded, s_dropped,
        s_overflow, s_received - s_read, chry_ringbuffer_get_used(&g_uartrx), s_generation, s_errors};
    response[1] = 2U + sizeof(words); response[2] = SPI_CDC_CMD; response[3] = action;
    for (uint32_t i = 0U; i < sizeof(words) / sizeof(words[0]); i++) { put32(response + 4U + 4U * i, words[i]); }
}
