/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 akaInstruments */

#ifndef DAP_MAIN_H
#define DAP_MAIN_H

#include "usbd_core.h"
#include "usbd_cdc.h"
#include "usbd_msc.h"
#include "usbd_hid.h"
#include "chry_ringbuffer.h"
#include "cdc_interface.h"
#include "rtt_bridge.h"
#include "DAP_config.h"
#include "DAP.h"

#define DAP_IN_EP  0x81
#define DAP_OUT_EP 0x02
#define SWO_IN_EP  0x83

#define CDC_IN_EP  0x84
#define CDC_OUT_EP 0x05
#define CDC_INT_EP 0x86

#define HID_IN_EP  0x87
#define HID_OUT_EP 0x08

#define MSC_IN_EP  0x89
#define MSC_OUT_EP 0x0A

/* USB→SPI/QSPI 桥（vendor specific 接口，见 src/spi_bridge/）。
 * 物理 EP11 双向：IN 应答流 / OUT 帧流。EP11~15 原本都空着，而 DCD 的 QHD/QTD
 * 竞技场是按 16 端点静态分配的，所以加这一对**不花 RAM**。 */
#define SPI_IN_EP  0x8B
#define SPI_OUT_EP 0x0B

#define USBD_VID           0x0D28
#define USBD_PID           0x0204
#define USBD_MAX_POWER     250
#define USBD_LANGID_STRING 1033

#ifdef CONFIG_USB_HS
#ifndef DAP_PACKET_SIZE
#define DAP_PACKET_SIZE 512
#define DAP_PACKET_COUNT 4
#endif
#if DAP_PACKET_SIZE != 512
#error "DAP_PACKET_SIZE must be 512 in hs"
#endif
#else
#ifndef DAP_PACKET_SIZE
#define DAP_PACKET_SIZE 64
#endif
#if DAP_PACKET_SIZE != 64
#error "DAP_PACKET_SIZE must be 64 in fs"
#endif
#endif

#ifdef CONFIG_USB_HS
#define HID_PACKET_SIZE 64
#else
#define HID_PACKET_SIZE 64
#endif

/* g_uartrx holds UART2 RX data until the main loop forwards it to the USB CDC
 * IN endpoint. It must absorb the interval during which the main loop is busy
 * with a long CMSIS-DAP/SWD block command (~0.9 KB/ms at 9 Mbps). */
#define CONFIG_UARTRX_RINGBUF_SIZE (32 * 1024)
#define CONFIG_USBRX_RINGBUF_SIZE  (8 * 1024)

#ifndef CONFIG_CHERRYDAP_USE_CUSTOM_HID
#define CONFIG_CHERRYDAP_USE_CUSTOM_HID 1
#endif

#ifndef CONFIG_CHERRYDAP_USE_MSC
#define CONFIG_CHERRYDAP_USE_MSC 0
#endif

/* DAP command processing (DAP_Setup + USB<->UART ringbuffers) is not wired up
 * yet — the DAP layer library is not linked into this build. Keep it disabled
 * so USB enumeration / DFU runtime can be brought up independently. Set to 1
 * once the CMSIS-DAP command engine (DAP.c, chry_ringbuffer.c) is added. */
#ifndef CONFIG_CHERRYDAP_DAP_CMD_ENABLE
#define CONFIG_CHERRYDAP_DAP_CMD_ENABLE 1
#endif

#ifdef __cplusplus
extern "C"
{
#endif

extern char serial_number_dynamic[33];
extern struct usbd_interface hid_intf;

extern chry_ringbuffer_t g_uartrx;
extern chry_ringbuffer_t g_usbrx;

void chry_dap_init(uint8_t busid, uint32_t reg_base);

void chry_dap_handle(void);

void chry_dap_usb2uart_handle(void);

/* 主循环级 CDC/串口桥总开关（HID 0x34 CMD_BRIDGE 控制）。
 * 0 = main() 不再调 chry_dap_usb2uart_handle()，每轮省下几百周期；
 * 代价是暂停期间 COM 口与 RTT-over-USB 都不通。默认 1（开）。
 * 高频 J-Scope 采样前把它关掉，是端到端从 167 kHz 往上走的必要条件。 */
extern volatile uint8_t usb2uart_bridge_enabled;
void    chry_dap_usb2uart_set_enabled(uint8_t enable);
uint8_t chry_dap_usb2uart_is_enabled(void);
/* Apply the last CDC line coding in main after a producer hands back UART. */
void chry_dap_usb2uart_request_config(void);

/* implment by user */
extern void chry_dap_usb2uart_uart_config_callback(struct cdc_line_coding *line_coding);

/* implment by user */
extern void chry_dap_usb2uart_uart_send_bydma(uint8_t *data, uint16_t len);

void chry_dap_usb2uart_uart_send_complete(uint32_t size);

/* implment by user */
extern void hid_custom_notify_handler(uint8_t busid, uint8_t event, void *arg);

/* implment by user */
extern void usbd_hid_custom_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);

/* implment by user */
extern void usbd_hid_custom_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);

#ifdef __cplusplus
}
#endif

#endif
