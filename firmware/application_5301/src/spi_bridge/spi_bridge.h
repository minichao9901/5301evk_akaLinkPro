/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 akaInstruments */

#ifndef __SPI_BRIDGE_H__
#define __SPI_BRIDGE_H__

#include <stdint.h>
#include "service_gate.h"
extern service_gate_t spi_bridge_gate;
static inline uint8_t spi_bridge_needs_service(void) { return service_gate_pending(&spi_bridge_gate); }
#include "spi_bridge_proto.h"

/*
 * USB -> SPI/QSPI 转发桥（探针侧）
 *
 * 控制面：HID CMD 0x35（spi_bridge_hid）
 * 数据面：bulk OUT 0x0B（帧流）/ bulk IN 0x8B（应答流）
 * 缓冲：32 KB AHB SRAM（0xF0400000，本工程此前完全没用过）
 *
 * 设计文档：docs/usb-spi-bridge-plan.md
 * 典型用法（网页侧）：GET_CFG -> SET_CFG -> SET_PROFILE -> ENABLE 1
 *                      -> RESET/STEP 帧灌初始化序列 -> XFER 帧刷像素
 *
 * 主循环先检查 spi_bridge_needs_service()，无运行/控制/清理工作时不调用。
 */

/* 一次性初始化（main() 里，board_init 之后）。只清状态，不动引脚。 */
void spi_bridge_init(void);

/* 主循环：推 IN、跑延时/复位脉冲、按预算处理 OUT 帧。 */
void spi_bridge_poll(void);
uint8_t spi_bridge_periodic_ready(void);
uint8_t spi_bridge_periodic_check(const uint8_t *p, uint16_t len);
uint8_t spi_bridge_periodic_exec(const uint8_t *p, uint16_t len, uint8_t *data, uint8_t *n);
void spi_bridge_periodic_release(void);

/* HID CMD 0x35：req/res 都是 64 B 的 HID 报文（约定见 api_param.c）。 */
void spi_bridge_hid(uint8_t *req_hid, uint8_t *res_hid);

/* ---- USB 侧胶水（usb_composite.c 调） ---- */

/* 主机把设备配置好（USBD_EVENT_CONFIGURED）时调：武装 bulk OUT 收第一包。
 * 未使能时不武装（主机侧 write 会 NAK，天然背压）。 */
void spi_bridge_usb_ready(void);

/* bulk OUT 收到一包（回调里调，ISR 上下文）。 */
void spi_bridge_out_done(uint32_t nbytes);

/* bulk IN 发完一包（回调里调，ISR 上下文）。 */
void spi_bridge_in_done(uint32_t nbytes);

/* 总线复位：在飞的传输全部作废、完成回调不会再来；真正的清账在主循环里做。 */
void spi_bridge_usb_reset(void);

/* 状态查询（给 HID STATUS 用；也便于单测）。 */
uint8_t spi_bridge_is_enabled(void);
/* ADC owns the existing rings exclusively; reject acquisition with any USB DMA live. */
uint8_t spi_bridge_adc_claim(uint32_t **capture, uint8_t **transmit);
void spi_bridge_adc_release(void);
uint8_t spi_bridge_adc_flags(void); /* bit0: OUT armed; bit1: other work/owner busy */
/* SPI slave uses the master's OUT ring as its circular DMA destination. */
uint8_t spi_bridge_slave_claim(uint8_t **receive, uint32_t *size);
void spi_bridge_slave_release(void);

/* 这根 pad 是否正被本桥当辅助脚占用（供 I2C 桥做反方向的引脚仲裁）。
 * pad 用 IOC_PAD_xx；返回 1 = 占用，别抢。 */
uint8_t spi_bridge_owns_pad(uint16_t pad);

#endif /* __SPI_BRIDGE_H__ */
