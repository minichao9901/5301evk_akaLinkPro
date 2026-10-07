/*
 * Copyright (c) 2021 HPMicro
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <stdio.h>
#include "board.h"
#include "hpm_debug_console.h"
#include "hpm_gpio_drv.h"
#include "hpm_soc.h"
#include "hpm_usb_drv.h"
#include "hpm_interrupt.h"
#include "usb_config.h"
#include "hpm_dfu_trigger.h"
#include "api_param.h"
#include "usb_composite.h"
#include "riscv_svc.h"
#include "scope_sampler.h"
#include "spi_bridge.h"
#include "spi_cdc.h"
#include "i2c_bridge.h"
#include "led_state.h"

#if BOARD_HAS_USER_KEY_DFU
/* Long-press USER KEY while the APP runs to reboot into the DFU bootloader.
 * (Holding the key at reset enters the ROM ISP mode instead, so this is the
 * only button-driven DFU entry path on this board.) */
#define DFU_KEY_HOLD_US (1000000U)
#define MCHTMR_MTIME_LO_REG (*(volatile uint32_t *)(HPM_MCHTMR_BASE + 0x00))

/* 按键轮询的分频。按住 1 秒才算数，而这个主循环在采样时能跑到 20 万圈/秒 ——
 * 每圈都去读 GPIO 和 MCHTMR 是纯浪费，而且那两次外设总线读正是高频采样时
 * 每拍那几百周期余量里的一大块。每 256 圈看一次：最慢的主循环下也有几百 Hz 的
 * 采样率，1 秒的按住时长照样抓得住。 */
#define DFU_KEY_POLL_DIV (256U)

static void dfu_key_poll(void)
{
    static uint8_t pressed;
    static uint32_t press_start;
    static uint32_t mchtmr_freq;
    static uint32_t div = DFU_KEY_POLL_DIV;

    if (--div != 0U)
    {
        return;
    }
    div = DFU_KEY_POLL_DIV;

    uint8_t now_pressed = (gpio_read_pin(BOARD_APP_GPIO_CTRL,
                                         BOARD_APP_GPIO_INDEX,
                                         BOARD_APP_GPIO_PIN) == BOARD_BUTTON_PRESSED_VALUE) ? 1U : 0U;

    /* 没按过、当前也没按：连 MCHTMR 都不用读（松开后的复位靠下面那支兜住）。 */
    if (!now_pressed && !pressed)
    {
        return;
    }

    uint32_t now = MCHTMR_MTIME_LO_REG;

    if (mchtmr_freq == 0U)
    {
        mchtmr_freq = clock_get_frequency(clock_mchtmr0);
    }

    if (now_pressed && !pressed)
    {
        press_start = now;
    }
    else if (now_pressed && ((now - press_start) > (DFU_KEY_HOLD_US / 1000000U) * mchtmr_freq))
    {
        hpm_dfu_reboot_to_dfu();
    }
    pressed = now_pressed;
}
#else
static void dfu_key_poll(void)
{
}
#endif

int main(void)
{
    board_init();
    api_param_load();

    /* Initialize NOLOAD bridge state before USB callbacks can observe it. */
    spi_bridge_init();

    board_init_usb((USB_Type *)CONFIG_HPM_USBD_BASE);
    intc_set_irq_priority(CONFIG_HPM_USBD_IRQn, 2);
    chry_dap_init(0, CONFIG_HPM_USBD_BASE);

    /* Bring up the UART <-> CDC COM port bridge. DAP_SETUP() above parks
     * the VCOM pins as GPIO, so this must run afterwards to mux them to
     * the CDC UART. */
    uartx_preinit();

    /* Status LEDs, external reference ADC and the periodic LED tick. */
    led_state_init();

    /* USB→I2C 转发桥：同上，只清状态。 */
    i2c_bridge_init();

    while (1)
    {
        chry_dap_handle();
        /* CDC/串口桥：每轮两次关中断 + 三次环形缓冲查询 + 一次 DMA 寄存器读。
         * HID 0x34 可以把它整个关掉（采样器要跑满周期时缺的就是这几百周期）。 */
        if (usb2uart_bridge_enabled)
        {
            chry_dap_usb2uart_handle();
        }
        if (api_param_needs_service())
        {
            api_param_poll();
        }
        dfu_key_poll();
        /* Probe-side RTT bridge: polls the target itself (only while the DAP
         * is idle) and forwards the bytes over the CDC. */
        if (rtt_bridge_needs_service())
        {
            rtt_bridge_poll();
        }
        /* J-Scope HSS 采样器（探针自己按周期读目标 RAM，走 bulk IN 0x83）。
         * 与 RTT 桥互斥 —— 两边的 start 分支会互相 stop()。 */
        if (scope_sampler_needs_service())
        {
            scope_sampler_poll();
        }
        /* Probe-side RISC-V engine (JTAG): queued memory access / benchmarks. */
        if (riscv_svc_needs_service())
        {
            riscv_svc_poll();
        }
        if (spi_cdc_needs_service())
        {
            spi_cdc_poll();
        }
        /* USB→SPI/QSPI 转发桥（HID 0x35 控制面 + bulk 0x0B/0x8B 数据面）。
         * 空闲时仅检查服务标志；使能后每轮按预算处理若干帧。 */
        if (spi_bridge_needs_service())
        {
            spi_bridge_poll();
        }
        /* USB→I2C 转发桥（HID 0x36）：执行主机登记的事务（最长几毫秒，所以
         * 绝不在 HID 中断里做）。🚨 先内联读标志再决定要不要进函数：poll 在
         * flash 里，每圈都调它的话那次 XPI 取指会摊进采样周期（实测 SCOPE
         * 单变量标定 −3%）；内联判断后与 main 持平。 */
        if (i2c_bridge_busy())
        {
            i2c_bridge_poll();
        }
    }
    return 0;
}
