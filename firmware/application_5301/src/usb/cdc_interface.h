/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 akaInstruments */

#ifndef __CDC_INTERFACE__
#define __CDC_INTERFACE__

#include "board.h"

/* CDC VCOM bridge UART and its pins come from the board definition:
 * akaLinkPro:     UART2 on PA08/PA09 (shared with JTAG TDI/TDO)
 * hpm5301evklite: UART3 on PB15/PB14 (J3.8/J3.10, "UART_TXD/UART_RXD" silk) */
#define PIN_UART_TX BOARD_PIN_UART_TXD
#define PIN_UART_RX BOARD_PIN_UART_RXD

#ifdef __cplusplus
extern "C"
{
#endif

    void uartx_io_init(void);

    void uartx_preinit(void);
    void uartx_swo_reconfigure(uint32_t baud);
void uartx_get_rx_diag(uint32_t words[5]);
    /* HID 0x18: version, clock, requested/applied baud, OSR, cap, init status, clock register. */
    void uartx_get_diag(uint32_t words[8]);

    /* VCOM pins -> the CDC UART (COM mode): DAP in SWD mode, disconnected or idle.
     * On boards where the UART shares pins with JTAG TDI/TDO this muxes the pads
     * back to the UART; on dedicated-pin boards it is nearly a no-op. */
    void uartx_enter_com_mode(void);

    /* Hand the TDI/TDO pins to the JTAG engine. Only affects boards whose UART
     * pins overlap the JTAG pins; the CDC COM port stays enumerated but does not
     * carry data there. */
    void uartx_enter_jtag_mode(void);

    void usb2uart_handler(void);

    /* Drop the UART RX backlog accumulated while the CDC bridge was suspended
     * and resume position tracking from the current DMA write pointer. */
    void uartx_rx_resync(void);

    enum { CDC_SOURCE_UART = 0, CDC_SOURCE_RTT = 1, CDC_SOURCE_SPI = 2 };
    /* One producer feeds the shared CDC ring. Changing sources preserves any
     * in-flight USB buffer; never reset its read/write indices on handoff. */
    void uartx_set_cdc_source(uint8_t source);
    uint8_t uartx_get_cdc_source(void);

#ifdef __cplusplus
}
#endif

#endif //__CDC_INTERFACE__
