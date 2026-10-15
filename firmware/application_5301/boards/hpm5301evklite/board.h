/*
 * Copyright (c) 2023-2026 HPMicro
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Board: HPM5301EVKLite (akaLinkPro DAP firmware port)
 *
 * DAP target-side pins are wired to the J5 20-pin JTAG header:
 *   J5.5  TDI   -> PA05
 *   J5.7  TMS   -> PA07 (SWDIO, single-pin bidirectional)
 *   J5.9  TCK   -> PA06 (SWCLK)
 *   J5.13 TDO   -> PA04
 *   J5.3  nTRST -> PA08 (fixed open-drain, mirrors DAP nRESET)
 *   J3.23 PA27 -> target nRESET (selectable open-drain/push-pull)
 *   J5.15 nSRST -> chip RESET_N (input only, cannot be driven by firmware)
 *
 * The same pins are the SoC's own JTAG port, so the board stays debuggable
 * (via J5 / the onboard FT2232) whenever the APP is not running (DFU
 * bootloader mode / ISP mode).
 */

#ifndef _HPM_BOARD_H
#define _HPM_BOARD_H
#include <stdio.h>
#include <stdarg.h>
#include "hpm_common.h"
#include "hpm_clock_drv.h"
#include "hpm_soc.h"
#include "hpm_soc_feature.h"
#include "pinmux.h"
#if !defined(CONFIG_NDEBUG_CONSOLE) || !CONFIG_NDEBUG_CONSOLE
#include "hpm_debug_console.h"
#endif

#define BOARD_NAME "hpm5301evklite"
#define BOARD_UF2_SIGNATURE (0x0A4D5048UL)
#define BOARD_DFU_SIGNATURE (0x48504D21UL)
#define BOARD_BGPR HPM_BGPR0

/* core section */
#ifndef BOARD_RUNNING_CORE
#define BOARD_RUNNING_CORE HPM_CORE0
#endif

/* uart section */
/* 2026-09-30：四线 QSPI 桥从 SPI1 搬到 SPI2，PB15/PB14（原 CDC 的 UART_TXD/RXD）
 * 要让给 SPI2 的 IO3/IO2，所以 **CDC VCOM 从 UART3(PB15/PB14) 换到 UART2(PB08/PB09)**；
 * log_printf/console 维持原样留在 UART0(PA00/PA01)。
 * （UART3 换不了别的脚：它在 QFN48 上只有 PB14/PB15 与未键合的 PA14/PA15。） */
#ifndef BOARD_APP_UART_BASE
#define BOARD_APP_UART_BASE HPM_UART2
#define BOARD_APP_UART_IRQ IRQn_UART2
#define BOARD_APP_UART_BAUDRATE (115200UL)
#define BOARD_APP_UART_CLK_NAME clock_uart2
#define BOARD_APP_UART_RX_DMA_REQ HPM_DMA_SRC_UART2_RX
#define BOARD_APP_UART_TX_DMA_REQ HPM_DMA_SRC_UART2_TX
#endif

#define BOARD_APP_UART_BREAK_SIGNAL_PIN IOC_PAD_PB08

#if !defined(CONFIG_NDEBUG_CONSOLE) || !CONFIG_NDEBUG_CONSOLE
#ifndef BOARD_CONSOLE_TYPE
#define BOARD_CONSOLE_TYPE CONSOLE_TYPE_UART
#endif

#if BOARD_CONSOLE_TYPE == CONSOLE_TYPE_UART
#ifndef BOARD_CONSOLE_UART_BASE
#define BOARD_CONSOLE_UART_BASE HPM_UART0
#define BOARD_CONSOLE_UART_CLK_NAME clock_uart0
#define BOARD_CONSOLE_UART_IRQ IRQn_UART0
#define BOARD_CONSOLE_UART_TX_DMA_REQ HPM_DMA_SRC_UART0_TX
#define BOARD_CONSOLE_UART_RX_DMA_REQ HPM_DMA_SRC_UART0_RX
#endif
#define BOARD_CONSOLE_UART_BAUDRATE (115200UL)
#endif
#endif

/* nor flash section */
#define BOARD_FLASH_BASE_ADDRESS (0x80000000UL) /* Check */
#define BOARD_FLASH_SIZE (SIZE_1MB)

/* XPI NOR configuration for ROM API (same as the DFU bootloader). */
#define BOARD_APP_XPI_NOR_XPI_BASE (HPM_XPI0)
#define BOARD_APP_XPI_NOR_CFG_OPT_HDR (0xfcf90002U)
#define BOARD_APP_XPI_NOR_CFG_OPT_OPT0 (0x00000006U)
#define BOARD_APP_XPI_NOR_CFG_OPT_OPT1 (0x00001000U)

/* Configuration store: the last two 4K sectors of the 1MB QSPI NOR
 * (0x800FE000 - 0x800FFFFF) are reserved for the APP-owned parameter store.
 * They sit at the tail of the APP region, never in the bootloader region. */
#define BOARD_PARAM_RESERVED_SIZE (0x2000UL)

/* gpiom section */
#define BOARD_APP_GPIOM_BASE HPM_GPIOM
#define BOARD_APP_GPIOM_USING_CTRL HPM_FGPIO
#define BOARD_APP_GPIOM_USING_CTRL_NAME gpiom_core0_fast

/* User button: USER KEY on PA03 (pressed = high, pad has pull-down).
 * Note: holding it at reset enters the ROM ISP mode; while the APP runs it
 * is a plain GPIO (long-press enters DFU mode, see main.c). */
#define BOARD_APP_GPIO_CTRL HPM_GPIO0
#define BOARD_APP_GPIO_INDEX GPIO_DI_GPIOA
#define BOARD_APP_GPIO_PIN 3
#define BOARD_APP_GPIO_IRQ IRQn_GPIO0_A
#define BOARD_BUTTON_PRESSED_VALUE 1

#ifndef BOARD_SHOW_CLOCK
#define BOARD_SHOW_CLOCK 1
#endif
#ifndef BOARD_SHOW_BANNER
#define BOARD_SHOW_BANNER 1
#endif

/* ------------------------------------------------------------------ */
/* DAP target-side (debug port) pins, all on the J5 header.            */
/* ------------------------------------------------------------------ */
#define BOARD_PIN_JTCK          IOC_PAD_PA06
#define BOARD_PIN_JTMS          IOC_PAD_PA07
#define BOARD_PIN_JTDI          IOC_PAD_PA05
#define BOARD_PIN_JTDO          IOC_PAD_PA04
#define BOARD_PIN_nRESET        IOC_PAD_PA27
/* The header label does not change the target signal: PA08 wired to target
 * TRST only resets the JTAG TAP. A system reset requires PA27 wired to NRST. */
#define BOARD_PIN_nRESET2       IOC_PAD_PA08

/* CDC VCOM bridge: UART2 on PB08 (TXD, J3.5) / PB09 (RXD, J3.3) - the pins
 * labelled I2C_SCL/I2C_SDA on the EVKLite J3 header (only R21/R22 10k pull-ups
 * sit on them, harmless for UART). Moved here from UART3/PB15+PB14 because the
 * SPI bridge now owns PB14/PB15 as QSPI IO2/IO3. */
#define BOARD_PIN_UART_TXD      IOC_PAD_PB08
#define BOARD_PIN_UART_RXD      IOC_PAD_PB09
#define BOARD_UART_TX_FUNC      IOC_PB08_FUNC_CTL_UART2_TXD
#define BOARD_UART_RX_FUNC      IOC_PB09_FUNC_CTL_UART2_RXD
#define BOARD_CDC_UART_BASE     HPM_UART2
#define BOARD_CDC_UART_IRQ      IRQn_UART2
#define BOARD_CDC_UART_CLK_NAME clock_uart2
#define BOARD_CDC_UART_RX_DMA   HPM_DMA_SRC_UART2_RX
#define BOARD_CDC_UART_TX_DMA   HPM_DMA_SRC_UART2_TX

/* Status LED: single board LED on PA10, active low. */
#define BOARD_LED1_PIN          IOC_PAD_PA10
#define BOARD_LED2_PIN          IOC_PAD_PA10

#define PIN_GPIOM_BASE    HPM_GPIOM
#define PIN_GPIO          HPM_FGPIO
#define PIN_GPIOM         gpiom_core0_fast

/* ------------------------------------------------------------------ */
/* Board feature flags consumed by the shared application sources.     */
/* ------------------------------------------------------------------ */
/* SWD bit-bang blobs pre-generated for SWCLK=PA06 / SWDIO=PA07. */
#define BOARD_SWD_BLOB_EVKLITE (1)
/* The EVKLite wires SWDIO directly to PA07: single bidirectional pin, no
 * external level shifter, no direction pin. */
#define BOARD_HAS_SWDIO_DIR (0)
/* No dedicated JTAG nTRST output pin (PA31 is a plain J3 header pin here). */
#define BOARD_HAS_JTRST (0)
/* nRESET (PA27) drives the target reset line directly, active low. */
#define BOARD_NRESET_ACTIVE_LOW (1)
/* No 5V level-shifter supply and no external VREF divider on this board. */
#define BOARD_HAS_5V_EN (0)
#define BOARD_HAS_VREF_ADC (0)
/* LED PA10 is active low. */
#define BOARD_LED_ACTIVE_LOW (1)
/* UART2 pins (PB08/PB09) do not overlap the JTAG pins. */
#define BOARD_UART2_SHARES_JTAG_PINS (0)
/* No board pin needs to be parked when the JTAG engine owns the pads. */
/* BOARD_JTAG_PARK_PIN intentionally not defined. */
/* Long-press USER KEY (PA03) while running enters DFU mode. */
#define BOARD_HAS_USER_KEY_DFU (1)
/* No DTR/RTS nets on this board. */
#define BOARD_UART_DTR_PAD (0U)
#define BOARD_UART_RTS_PAD (0U)

/* USB→SPI/QSPI 转发桥：J3 排针上引出完整 **SPI2**（PB10~PB15，含 quad 的 IO2/IO3），
 * 见 docs/usb-spi-bridge-plan.md 与 docs/spi-bridge-wiring.md。
 * 2026-09-30 从 SPI1 搬来：SPI1 的 IO2=PA30(USB0_PWR) 被板上 Q1 常态短到地，四线不可用。
 * 其他板子（akaLinkPro）没这套排针定义，置 0。 */
#define BOARD_HAS_SPI_BRIDGE (1)

/* USB→I2C 转发桥：**I2C3** 的 PA28=SDA / PA29=SCL，J3[21] / J3[19]。
 * 这是 J3 排针上唯一一对引出来的硬件 I2C 脚（I2C 的全部功能脚 × J3 交叉比对：
 * 其它组合的另一半分别是 USER 按键 PA03、nRESET PA08、CDC 的 UART2 PB08、
 * USB0 的 PA24/PA25 —— 都占着）。与 SPI2(PB10~PB15)、SWD(PA04~PA08)、
 * UART0(PA00/PA01) 均不冲突，见 docs/web-handoff-i2c-bridge.md 的接线表。
 * ⚠️ 板丝印还停在 SPI1 时代（MISO/MOSI）；⚠️ PA29 与 USB0_OC 网络共用
 * （AP2151 nFAULT + R6 10k 上拉）：开漏使用没问题，USB 限流报故障时会把 SCL 拉低。
 * ⚠️ 这两根脚同时也在 SPI 桥的辅助脚 pad 表里（索引 16/17）—— 桥使能时
 * i2c_bridge_owns_pad() 会让 SPI 桥拒掉它们，避免两个模块抢同一根脚。 */
#define BOARD_HAS_I2C_BRIDGE (1)
#define BOARD_I2C_BRIDGE_BASE HPM_I2C3
#define BOARD_I2C_BRIDGE_CLK clock_i2c3
#define BOARD_I2C_BRIDGE_SDA_PAD IOC_PAD_PA28
#define BOARD_I2C_BRIDGE_SCL_PAD IOC_PAD_PA29
#define BOARD_I2C_BRIDGE_SDA_FUNC IOC_PA28_FUNC_CTL_I2C3_SDA
#define BOARD_I2C_BRIDGE_SCL_FUNC IOC_PA29_FUNC_CTL_I2C3_SCL
#define BOARD_I2C_BRIDGE_SDA_LABEL "PA28/J3[21]"
#define BOARD_I2C_BRIDGE_SCL_LABEL "PA29/J3[19]"

#if defined(__cplusplus)
extern "C"
{
#endif /* __cplusplus */

    typedef void (*board_timer_cb)(void);

    void board_init_gpio_pins(void);
    void board_init_usb(USB_Type *ptr);
    void board_init_console(void);
    void board_init_uart(UART_Type *ptr);

    void board_init(void);
    void board_init_usb_dp_dm_pins(void);
    void board_init_clock(void);
    void board_delay_us(uint32_t us);
    void board_delay_ms(uint32_t ms);
    void board_ungate_mchtmr_at_lp_mode(void);

    void board_init_pmp(void);
    uint32_t board_init_uart_clock(UART_Type *ptr);
    void init_uart_pins(UART_Type *ptr);
    void init_usb_pins(USB_Type *ptr);
#if defined(__cplusplus)
}
#endif /* __cplusplus */
#endif /* _HPM_BOARD_H */
