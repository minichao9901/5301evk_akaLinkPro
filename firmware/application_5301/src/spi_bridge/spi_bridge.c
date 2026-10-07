/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 akaInstruments */

/*
 * USB -> SPI/QSPI 转发桥
 *
 * 分层见 spi_bridge_proto.h；设计文档 docs/usb-spi-bridge-plan.md。
 *
 * 实现要点（P1：只有轮询收发，DMA 在 P2）：
 *   - bulk OUT 每一包（≤512 B）直接落进 AHB SRAM 的槽里（零拷贝：端点的收缓冲就是
 *     槽本身），主循环按帧解析、按 FIFO 顺序执行 —— 顺序性由此保证。
 *   - 帧不跨包 ⇒ 每帧 payload 物理连续，P2 的 DMA 可以直接指进槽里。
 *   - IN 侧同一时刻只允许一包在飞（cherryusb 对忙端点再调 start_write 会静默丢弃），
 *     完成回调里再推下一包。OUT 同理（busy 端点重复 start_read 也会被丢）。
 *   - 延时 / RESET 脉冲都是**非阻塞**调度：到点才继续处理后续帧，主循环照常跑
 *     DAP / RTT / Scope。
 *   - CS 默认走 GPIO（PB10）而不是硬件 CS0：面板初始化需要「一个 CS 窗口内
 *     命令 → 翻 DC → 参数」，硬件 CS 做不到这件事。
 */

#include <string.h>

#include "board.h"
#include "hpm_common.h"
#include "hpm_soc.h"
#include "hpm_interrupt.h"
#include "hpm_spi_drv.h"
#include "hpm_gpio_drv.h"
#include "hpm_gpiom_drv.h"
#include "hpm_clock_drv.h"
#include "hpm_dma_mgr.h"
#include "hpm_dmav2_drv.h"
#include "hpm_dmamux_src.h"
#include "pinmux.h"
#include "usb_composite.h"
#include "spi_bridge.h"
#include "i2c_bridge.h"   /* 交叉检查：I2C 桥使能时占着 PA28/PA29，别当辅助脚用 */
#include "bus_periodic.h"
#include "adc_stream.h"

/* Authoritative service flags: cold gate reads words, producers write bytes. */
service_gate_t spi_bridge_gate;

#if !defined(BOARD_HAS_SPI_BRIDGE) || (BOARD_HAS_SPI_BRIDGE == 0)

/* ---------------------------------------------------------------------------
 * 本板没有 SPI 排针引出（例如 akaLinkPro）：整个模块编成空实现，上层
 * （main / api_param / usb_composite）照旧可链接，HID 0x35 回"不支持"。
 * ------------------------------------------------------------------------- */

void spi_bridge_init(void)
{
}

void spi_bridge_poll(void)
{
}
uint8_t spi_bridge_periodic_ready(void) { return 0U; }
uint8_t spi_bridge_periodic_check(const uint8_t *p, uint16_t len) {
    (void)p; (void)len; return SB_E_DISABLED;
}
uint8_t spi_bridge_periodic_exec(const uint8_t *p, uint16_t len, uint8_t *data, uint8_t *n) {
    (void)p; (void)len; (void)data; *n = 0U; return SB_E_DISABLED;
}
void spi_bridge_periodic_release(void) {}
uint8_t spi_bridge_adc_claim(uint32_t **capture, uint8_t **transmit) {
    (void)capture; (void)transmit; return 0U;
}
void spi_bridge_adc_release(void) {}
uint8_t spi_bridge_adc_flags(void) { return 2U; }
uint8_t spi_bridge_slave_claim(uint8_t **receive, uint32_t *size) {
    (void)receive; (void)size; return 0U;
}
void spi_bridge_slave_release(void) {}

void spi_bridge_hid(uint8_t *req_hid, uint8_t *res_hid)
{
    res_hid[1] = 0x01;
    res_hid[2] = SB_HID_CMD;
    res_hid[3] = req_hid[3];
}

void spi_bridge_usb_ready(void)
{
}

void spi_bridge_out_done(uint32_t nbytes)
{
    (void)nbytes;
}

void spi_bridge_in_done(uint32_t nbytes)
{
    (void)nbytes;
}

void spi_bridge_usb_reset(void)
{
}

uint8_t spi_bridge_is_enabled(void)
{
    return 0U;
}

#else /* BOARD_HAS_SPI_BRIDGE */

/* ============================== 参数 ============================== */

/* 2026-09-30：桥从 SPI1 搬到 SPI2。原因见 boards/hpm5301evklite/pinmux.c ——
 * SPI1 的四线 IO2=PA30 是 USB0_PWR 网络，被板上 Q1(2N7002) 常态短到地，实测拉不动。
 * SPI2 的六根线（PB10 CS / PB11 SCLK / PB12 MISO / PB13 MOSI / PB14 DAT2 / PB15 DAT3）
 * 在 EVKLite 的 J3 上全引出，且 PB14/PB15 上的板载 CH340 是 NC。 */
#define SB_SPI HPM_SPI2
#define SB_SPI_CLK_NAME clock_spi2

/* 环：OUT 32×512 B = 16 KB，IN 16×512 B = 8 KB（都在 AHB SRAM） */
#define SB_OUT_SLOTS 32U
#define SB_IN_SLOTS 16U

/* 每轮主循环的处理预算：别让 DAP/Scope 等太久 */
#define SB_POLL_MAX_FRAMES 8U
#define SB_POLL_MAX_BYTES 2048U

/* 板级默认 */
#define SB_DEF_SCLK_HZ 20000000UL
#define SB_DEF_DMA_THRESHOLD 100U

/* 内部：让出主循环、下一轮再处理该帧（不是协议错误码） */
#define SB_WOULD_BLOCK 0xFFU

#define SB_MTIME (*(volatile uint32_t *)(HPM_MCHTMR_BASE + 0x00U))

/* HID 配置块按字节原样拷进 sb_cfg_t（SET_CFG），布局变了主机侧就错位 ——
 * 编译期钉死，改字段必须同步脚本（spi_bridge_test.py 按显式偏移打包）。 */
_Static_assert(sizeof(sb_cfg_t) == 32U, "sb_cfg_t layout changed: sync spi_bridge_proto.h + host tools");
_Static_assert(sizeof(sb_profile_t) == 16U, "sb_profile_t layout changed: sync host tools");

/* MCHTMR = osc24m，固定 24 MHz（见 sb_delay_us 的说明） */
#define SB_MCHTMR_HZ 24000000UL

/* ============================== 缓冲（AHB SRAM） ============================== */

ATTR_PLACE_AT_WITH_ALIGNMENT(".ahb_sram", 64)
static uint8_t s_out_buf[SB_OUT_SLOTS][SB_PKT_SIZE];
ATTR_PLACE_AT_WITH_ALIGNMENT(".ahb_sram", 64)
static uint8_t s_in_buf[SB_IN_SLOTS][SB_PKT_SIZE];

/* ============================== 状态 ============================== */

/*
 * 桥的全部状态都放 AHB SRAM：DLM 只剩 ~21 KB（83.8% 已用），而 32 KB 的 AHB SRAM
 * 只用了 24 KB（两个环）。另一个原因是 `.noncacheable` 段带 2 KB 对齐，往 .bss 里
 * 加一两百字节就会把这 2 KB 对齐空洞算进 DLM 用量 —— 实测就是 DLM +2048 B，搬到
 * AHB SRAM 之后 DLM 回到 106592 B。CPU 访问 AHB SRAM 略慢于 DLM，但本模块的瓶颈
 * 在 SPI 线速，这点差异无所谓。
 */
typedef struct
{
    sb_cfg_t cfg;
    sb_profile_t prof;
    sb_stats_t stats;
    uint32_t actual_sclk;
    uint32_t module_clk; /* 实际生效的 SPI2 模块时钟（诊断用） */

    volatile uint16_t out_w;
    volatile uint16_t out_r;
    volatile uint16_t out_used;
    volatile uint8_t out_inflight;
    /* 环的"代数"：复位时 +1。武装读/写时记下当时的代数，完成回调如果发现代数对不上，
     * 说明这一笔是**跨复位**收到的旧包，必须丢掉。
     * 踩过的坑：复位只把 out_w/r/used 清零、同时还把 out_inflight 清零，但端点上那一笔
     * 已武装的读**照样会完成**，回调于是往刚清零的环里写 —— 把复位前的旧帧"复活"成新帧
     * 执行。实测症状：`loop --lens 1` 的帧（seq=1、data=01）在十几分钟后、经过多次
     * enable/disable，仍被当作下一批命令的第一帧执行，还顺手把 CS_HOLD 的 CS 释放了。 */
    volatile uint8_t out_gen;
    volatile uint8_t out_armed_gen;
    volatile uint8_t in_gen;
    volatile uint8_t in_armed_gen;
    /* 每槽实际收到的字节数。**不能存在槽里**：槽的前 8 B 是第一帧的帧头，offset 6
     * 正是该帧的 len 字段，写在那里会把帧头改坏。 */
    volatile uint16_t out_pkt_len[SB_OUT_SLOTS];

    volatile uint16_t in_w;
    volatile uint16_t in_r;
    volatile uint16_t in_used;
    volatile uint8_t in_inflight;

    uint8_t pkt_active;
    uint16_t pkt_len;
    uint16_t pkt_off;

    uint8_t delay_active;
    uint32_t delay_until;
    uint8_t rst_state;    /* 0 空闲，1 拉低保持中，2 释放后等待 */
    uint16_t rst_post_ms; /* 释放后要等的毫秒数 */

    volatile uint8_t last_err; /* 最近一次错误码（SB_ST_SHIFT_ERR 报的就是它） */

    /* 上板诊断用（HID action 10 原样吐出来）。P1 第一次回环全是 TIMEOUT/BUSY，
     * 需要区分"复位位清不掉=IP 没时钟"和"FIFO 不流动=没在移位"，看寄存器最快。 */
    volatile uint32_t dbg[13];
    volatile uint8_t spi_need_reset; /* 上一笔失败/卡 ACTIVE：下一笔前先复位控制器 */

    /* P2：TX 数据相位走 DMA（tx_len ≥ tx_dma_threshold）。RX 一律 CPU 轮询。 */
    dma_resource_t dma;
    uint8_t dma_ok;          /* 通道申请到了没有；没有就整体退回轮询 */
    uint32_t last_ticks;     /* 上一笔事务耗时（mchtmr tick，24 MHz）——给阈值定档用 */

    /* Service flags are in spi_bridge_gate (DLM); bulk state stays in AHB SRAM. */
    /* 硬件重初始化请求（ISR 置位、主循环执行）：ENABLE=1 与 enabled 状态下的
     * SET_CFG / SET_PROFILE 都要走一次 sb_spi_hw_init —— 时钟源扫描 + SPI 复位
     * 轮询 + DMA 申请，可达数百 µs。绝不能在 USB 中断里同步做：既长时间挡住
     * USB/DAP 的中断，又会打断主循环正在跑的 CS 窗口（SPI 格式/时钟/DMA 通道
     * 在事务中间被换掉）。 */
    volatile uint8_t hw_req;

    uint16_t cs_pad; /* GPIO 模式下生效的 CS 脚；0 = 用硬件 CS0 */
    volatile uint8_t cs_asserted;

    uint16_t pad_dc; /* 辅助脚（0 = 未配置） */
    uint16_t pad_rst;
    uint16_t pad_bl;
    uint16_t pad_te;

    spi_format_config_t format; /* SPI 格式缓存（addr_len 变了要重写 TRANSFMT） */
    uint8_t fmt_addr_len;
} sb_state_t;

ATTR_PLACE_AT_WITH_ALIGNMENT(".ahb_sram", 8)
static sb_state_t s_st;

/* 代码里仍用原来的短名字（纯文本替换，逻辑不变） */
#define s_enabled (spi_bridge_gate.flag[0])
#define s_cfg (s_st.cfg)
#define s_prof (s_st.prof)
#define s_stats (s_st.stats)
#define s_actual_sclk (s_st.actual_sclk)
#define s_module_clk (s_st.module_clk)
#define s_out_w (s_st.out_w)
#define s_out_r (s_st.out_r)
#define s_out_used (s_st.out_used)
#define s_out_inflight (s_st.out_inflight)
#define s_out_gen (s_st.out_gen)
#define s_out_armed_gen (s_st.out_armed_gen)
#define s_in_gen (s_st.in_gen)
#define s_in_armed_gen (s_st.in_armed_gen)
#define s_out_pkt_len (s_st.out_pkt_len)
#define s_in_w (s_st.in_w)
#define s_in_r (s_st.in_r)
#define s_in_used (s_st.in_used)
#define s_in_inflight (s_st.in_inflight)
#define s_pkt_active (s_st.pkt_active)
#define s_pkt_len (s_st.pkt_len)
#define s_pkt_off (s_st.pkt_off)
#define s_delay_active (s_st.delay_active)
#define s_delay_until (s_st.delay_until)
#define s_rst_state (s_st.rst_state)
#define s_rst_post_ms (s_st.rst_post_ms)
#define s_last_err (s_st.last_err)
#define s_dbg (s_st.dbg)
#define s_spi_need_reset (s_st.spi_need_reset)
#define s_dma (s_st.dma)
#define s_dma_ok (s_st.dma_ok)
#define s_last_ticks (s_st.last_ticks)
#define s_reset_req (spi_bridge_gate.flag[1])
#define s_usb_reset_req (spi_bridge_gate.flag[2])
#define s_abort_req (spi_bridge_gate.flag[3])
#define s_drain_reads (spi_bridge_gate.flag[4])
#define s_hw_req (s_st.hw_req)
#define s_cs_pad (s_st.cs_pad)
#define s_cs_asserted (s_st.cs_asserted)
#define s_pad_dc (s_st.pad_dc)
#define s_pad_rst (s_st.pad_rst)
#define s_pad_bl (s_st.pad_bl)
#define s_pad_te (s_st.pad_te)
#define s_format (s_st.format)
#define s_fmt_addr_len (s_st.fmt_addr_len)
#define s_adc_owner (spi_bridge_gate.flag[5])
#define s_slave_owner (spi_bridge_gate.flag[6])

/* Shared buffers cannot change owners while native USB DMA or deferred bridge
 * work still references them. bit0 requests OUT retirement with a host ZLP;
 * bit1 requests bridge/IN/control retirement. Only bus reset may invalidate
 * in-flight bookkeeping without a completion callback. */
uint8_t spi_bridge_adc_flags(void)
{
    return (uint8_t)((s_out_inflight ? 1U : 0U) |
        ((s_enabled || s_in_inflight || s_in_used || s_out_used || s_pkt_active ||
          s_hw_req || s_reset_req || s_usb_reset_req || s_abort_req ||
          s_drain_reads || s_adc_owner || s_slave_owner || s_cs_asserted) ? 2U : 0U));
}

uint8_t spi_bridge_adc_claim(uint32_t **capture, uint8_t **transmit)
{
    /* Caller holds the IRQ lock. Failed claims must not change USB state. */
    if (spi_bridge_adc_flags() != 0U) { return 0U; }
    s_adc_owner = 1U;
    *capture = (uint32_t *)&s_out_buf[0][0];
    *transmit = &s_in_buf[0][0];
    return 1U;
}
void spi_bridge_adc_release(void) { s_adc_owner = 0U; }
uint8_t spi_bridge_slave_claim(uint8_t **receive, uint32_t *size)
{
    /* Caller holds the IRQ lock; native IN/OUT must have retired first. */
    if (spi_bridge_adc_flags() != 0U) { return 0U; }
    s_slave_owner = 1U;
    *receive = &s_out_buf[0][0];
    *size = sizeof(s_out_buf);
    return 1U;
}
void spi_bridge_slave_release(void) { s_slave_owner = 0U; }

/* ============================== 小工具 ============================== */

static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static inline void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

/* 环计数在 ISR 与主循环两边都改（++ / --），读改写必须互斥 */
static inline uint32_t sb_irq_save(void)
{
    return disable_global_irq(CSR_MSTATUS_MIE_MASK);
}

static inline void sb_irq_restore(uint32_t level)
{
    restore_global_irq(level);
}

/* pad 索引表（协议侧索引 -> IOC pad；0 = 不支持/未配置） */static const uint16_t s_pad_table[SB_PAD_MAX] = {
    0U,
    IOC_PAD_PB11,
    IOC_PAD_PB12,
    IOC_PAD_PB13,
    IOC_PAD_PB10,
    IOC_PAD_PA02,
    IOC_PAD_PA09,
    IOC_PAD_PA00,
    IOC_PAD_PA01,
    0U, /* PY00：PIOC 域，v1 不支持 */
    0U, /* PY01：同上 */
    IOC_PAD_PA10,
    IOC_PAD_PA30,
    IOC_PAD_PA31,
    /* 2026-09-30 释放：SPI1 时代显示接口用的四根，SPI2 迁移后固件里零引用（见 proto.h 说明） */
    IOC_PAD_PA26,
    IOC_PAD_PA27,
    IOC_PAD_PA28,
    IOC_PAD_PA29,
};

static uint16_t sb_pad_of(uint8_t idx)
{
    if (idx >= SB_PAD_MAX)
    {
        return 0U;
    }
    return s_pad_table[idx];
}

static void sb_gpiom_to_gpio0(uint16_t pad)
{
    gpiom_set_pin_controller(HPM_GPIOM,
                             GPIO_GET_PORT_INDEX(pad),
                             GPIO_GET_PIN_INDEX(pad),
                             gpiom_soc_gpio0);
    gpiom_enable_pin_visibility(HPM_GPIOM,
                               GPIO_GET_PORT_INDEX(pad),
                               GPIO_GET_PIN_INDEX(pad),
                               gpiom_soc_gpio0);
}

static void sb_pad_as_output(uint16_t pad, uint8_t level)
{
    HPM_IOC->PAD[pad].FUNC_CTL = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0);
    HPM_IOC->PAD[pad].PAD_CTL = IOC_PAD_PAD_CTL_PE_SET(0) | /* 不用内部上下拉 */
                                IOC_PAD_PAD_CTL_PS_SET(0) |
                                IOC_PAD_PAD_CTL_OD_SET(0) |
                                IOC_PAD_PAD_CTL_SR_SET(1) |  /* fast slew */
                                IOC_PAD_PAD_CTL_SPD_SET(3) | /* fastest */
                                IOC_PAD_PAD_CTL_DS_SET(4);
    sb_gpiom_to_gpio0(pad);
    gpio_set_pin_output(HPM_GPIO0, GPIO_GET_PORT_INDEX(pad), GPIO_GET_PIN_INDEX(pad));
    gpio_write_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(pad), GPIO_GET_PIN_INDEX(pad), level ? 1U : 0U);
}

static void sb_pad_as_input(uint16_t pad, uint8_t pullup)
{
    HPM_IOC->PAD[pad].FUNC_CTL = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0);
    HPM_IOC->PAD[pad].PAD_CTL = IOC_PAD_PAD_CTL_PE_SET(1) |
                                IOC_PAD_PAD_CTL_PS_SET(pullup ? 1U : 0U) |
                                IOC_PAD_PAD_CTL_HYS_SET(1);
    sb_gpiom_to_gpio0(pad);
    gpio_set_pin_input(HPM_GPIO0, GPIO_GET_PORT_INDEX(pad), GPIO_GET_PIN_INDEX(pad));
}

static inline void sb_pad_write(uint16_t pad, uint8_t level)
{
    gpio_write_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(pad), GPIO_GET_PIN_INDEX(pad), level ? 1U : 0U);
}

static inline uint8_t sb_pad_read(uint16_t pad)
{
    return (uint8_t)(gpio_read_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(pad), GPIO_GET_PIN_INDEX(pad)) ? 1U : 0U);
}

/* pad_active_low 位图：bit0 DC / bit1 RST / bit2 CS / bit3 BL */
static inline uint8_t sb_line_active_low(uint8_t line_no)
{
    return (uint8_t)((s_cfg.pad_active_low >> line_no) & 0x01U);
}

static void sb_delay_us(uint32_t us)
{
    /* MCHTMR 就是 osc24m，固定 24 MHz。**不要用 clock_get_frequency(clock_mchtmr0)**：
     * 本 SoC 的频率表里没给它兜底，返回 0 ⇒ ticks 被下面那句夹成 1 ⇒ 所有延时都变成
     * "让出一次主循环"，实测 500 ms 只等出 50 ms、2 ms 与 50 ms 全都贴着主机往返底噪。
     * 工程里其它模块（scope / rtt / riscv）也都是硬编码 24 MHz，这里跟它们保持一致。 */
    uint64_t ticks = ((uint64_t)us * (uint64_t)SB_MCHTMR_HZ) / 1000000ULL;
    if (ticks == 0ULL)
    {
        ticks = 1ULL; /* 至少让出一次主循环 */
    }
    if (ticks > 0x7FFFFFFFULL)
    {
        /* 到点判定用的是 (int32_t)(now - until) < 0：deadline 必须落在 now 的
         * +2^31 以内，否则符号比较直接失效（延时会被当成"早就到点"）。超长
         * 延时（≥ ~89 s）截到上界 —— 面板场景最长 200 ms，正常碰不到。 */
        ticks = 0x7FFFFFFFULL;
    }
    s_delay_until = SB_MTIME + (uint32_t)ticks;
    s_delay_active = 1U;
}

static inline uint8_t sb_delay_pending(void)
{
    if (s_delay_active == 0U)
    {
        return 0U;
    }
    if ((int32_t)(SB_MTIME - s_delay_until) < 0)
    {
        return 1U;
    }
    s_delay_active = 0U;
    return 0U;
}

/* ============================== CS ============================== */

/* 主 CS（PB10）与辅助 CS 都按 pad_active_low bit2 决定有效电平。
 * 默认配置里 bit2 = 1（CS 低有效），这是所有 SPI 从器件的惯例。 */
static void sb_cs_assert(void)
{
    if (s_cs_pad != 0U)
    {
        sb_pad_write(s_cs_pad, sb_line_active_low(2U) ? 0U : 1U);
    }
    s_cs_asserted = 1U;
}

static void sb_cs_release(void)
{
    if (s_cs_pad != 0U)
    {
        sb_pad_write(s_cs_pad, sb_line_active_low(2U) ? 1U : 0U);
    }
    s_cs_asserted = 0U;
}

/* ============================== SPI 硬件 ============================== */

static void sb_spi_apply_format(uint8_t addr_len_bytes)
{
    uint8_t alen = (addr_len_bytes == 0U) ? 1U : addr_len_bytes;
    if (alen == s_fmt_addr_len)
    {
        return;
    }
    s_format.master_config.addr_len_in_bytes = alen;
    spi_format_init(SB_SPI, &s_format);
    s_fmt_addr_len = alen;
}

/*
 * 选 SPI2 的模块时钟与分频，使 SCLK 尽量贴近 want_hz，返回**实际**得到的 SCLK。
 *
 * 两条硬约束（drivers/src/hpm_spi_drv.c:348）：SCLK = 模块时钟 / N，N 必须整除且为
 * **偶数**，N ≤ 510。
 *
 * ⚠️ 2026-09-29 上板教训：**不要再把模块时钟直接顶到 PLL0 原频 720 MHz**。
 * 之前这里是把 720/600/400 MHz 逐个试、取能整除的那个 —— 结果 20 MHz 落在
 * 720/36 上，然后 SPI 一次都不移位：LA 抓到的波形里 CS 拉了 614 µs、MOSI 只有
 * 几个毛刺、**SCLK 全程不动**，`spi_write_read_data` 卡在 5000 次重试后超时。
 * 板级默认（SDK 的 board_init_spi_clock 也只做 clock_add_to_group，不动源）其实是
 * 24 MHz，所以拿 720 MHz 喂这个 IP 是超出它工作范围的。
 *
 * 现在改成：在「时钟源 × 整数分频」里搜**能整除出目标 SCLK 的最小模块时钟**
 * （上限 SB_MAX_MODULE_CLK_HZ），既避开超频，又保留 20/40/60/100 MHz 这些档。
 */
#define SB_MAX_MODULE_CLK_HZ 240000000UL
#define SB_MAX_SCLK_HZ 100000000UL
#define SB_DEF_MODULE_CLK_HZ 24000000UL

static const clk_src_t s_clk_cands[] = {
    clk_src_pll0_clk0, clk_src_pll0_clk1, clk_src_pll0_clk2,
    clk_src_pll1_clk0, clk_src_pll1_clk1, clk_src_pll1_clk2, clk_src_pll1_clk3,
};

/* 给定模块时钟，返回它能给出的、最接近 want 的 "偶数 N" SCLK；0 = 给不出 */
static uint32_t sb_sclk_from_module(uint32_t module, uint32_t want)
{
    uint32_t n;

    if (module < 2U)
    {
        return 0U;
    }
    n = (module + want - 1U) / want; /* 向上取整 */
    if (n < 2U)
    {
        n = 2U;
    }
    if ((n & 1U) != 0U)
    {
        n++;
    }
    for (uint32_t k = 0U; (k < 16U) && (n <= 510U); k++, n += 2U)
    {
        if ((module % n) != 0U)
        {
            continue;
        }
        return module / n;
    }
    return 0U;
}

static uint32_t sb_pick_sclk(uint32_t want_hz)
{
    uint32_t best_sclk = 0U;
    uint32_t best_module = 0U;
    uint32_t best_diff = 0xFFFFFFFFUL;
    uint32_t best_div = 1U;
    clk_src_t best_src = clk_src_invalid;

    if (want_hz == 0U)
    {
        want_hz = SB_DEF_SCLK_HZ;
    }
    if (want_hz > SB_MAX_SCLK_HZ)
    {
        want_hz = SB_MAX_SCLK_HZ;
    }

    /* 显式指定的模块时钟：只试这一个目标，不用扫 */
    if (s_cfg.module_clk_hz != 0U)
    {
        uint32_t want_mod = s_cfg.module_clk_hz;
        for (uint32_t i = 0U; i < (sizeof(s_clk_cands) / sizeof(s_clk_cands[0])); i++)
        {
            if (clock_set_source_divider(SB_SPI_CLK_NAME, s_clk_cands[i], 1U) != status_success)
            {
                continue;
            }
            uint32_t src = clock_get_frequency(SB_SPI_CLK_NAME);
            if (src == 0U)
            {
                continue;
            }
            uint32_t d = (src + (want_mod / 2U)) / want_mod;
            if (d == 0U)
            {
                d = 1U;
            }
            uint32_t mod = src / d;
            uint32_t diff = (mod > want_mod) ? (mod - want_mod) : (want_mod - mod);
            if ((best_src == clk_src_invalid) || (diff < best_diff))
            {
                uint32_t sc = sb_sclk_from_module(mod, want_hz);
                if (sc != 0U)
                {
                    best_diff = diff;
                    best_module = mod;
                    best_src = s_clk_cands[i];
                    best_div = d;
                    best_sclk = sc;
                }
            }
        }
    }
    else
    {
        /* 自动：找能整除出目标 SCLK 的**最小**模块时钟（同分优先取更小的模块时钟） */
        for (uint32_t i = 0U; i < (sizeof(s_clk_cands) / sizeof(s_clk_cands[0])); i++)
        {
            if (clock_set_source_divider(SB_SPI_CLK_NAME, s_clk_cands[i], 1U) != status_success)
            {
                continue;
            }
            uint32_t src = clock_get_frequency(SB_SPI_CLK_NAME);
            if (src < 2U)
            {
                continue;
            }
            for (uint32_t d = 1U; d <= 32U; d++)
            {
                uint32_t mod = src / d;
                if (mod > SB_MAX_MODULE_CLK_HZ)
                {
                    continue;
                }
                if (mod < (want_hz * 2U))
                {
                    break; /* 再小就给不出这个 SCLK 了 */
                }
                uint32_t sc = sb_sclk_from_module(mod, want_hz);
                if (sc == 0U)
                {
                    continue;
                }
                uint32_t diff = (sc > want_hz) ? (sc - want_hz) : (want_hz - sc);
                if ((best_src == clk_src_invalid) || (diff < best_diff) ||
                    ((diff == best_diff) && (mod < best_module)))
                {
                    best_diff = diff;
                    best_sclk = sc;
                    best_module = mod;
                    best_src = s_clk_cands[i];
                    best_div = d;
                }
            }
        }
        if (best_src == clk_src_invalid)
        {
            /* 兜底：板级默认的 24 MHz（SDK 例程用的就是它） */
            for (uint32_t i = 0U; i < (sizeof(s_clk_cands) / sizeof(s_clk_cands[0])); i++)
            {
                if (clock_set_source_divider(SB_SPI_CLK_NAME, s_clk_cands[i], 1U) != status_success)
                {
                    continue;
                }
                uint32_t src = clock_get_frequency(SB_SPI_CLK_NAME);
                if (src < 2U)
                {
                    continue;
                }
                uint32_t d = src / SB_DEF_MODULE_CLK_HZ;
                if (d == 0U)
                {
                    d = 1U;
                }
                uint32_t mod = src / d;
                uint32_t sc = sb_sclk_from_module(mod, want_hz);
                if (sc != 0U)
                {
                    best_src = s_clk_cands[i];
                    best_div = d;
                    best_module = mod;
                    best_sclk = sc;
                    break;
                }
            }
        }
    }

    if (best_src == clk_src_invalid)
    {
        s_module_clk = 0U;
        return 0U;
    }

    (void)clock_set_source_divider(SB_SPI_CLK_NAME, best_src, best_div);
    s_module_clk = clock_get_frequency(SB_SPI_CLK_NAME);

    spi_timing_config_t timing;
    spi_master_get_default_timing_config(&timing);
    timing.master_config.clk_src_freq_in_hz = s_module_clk;
    timing.master_config.sclk_freq_in_hz = best_sclk;
    if (spi_master_timing_init(SB_SPI, &timing) != status_success)
    {
        return 0U;
    }

    return best_sclk;
}

/* TX DMA 三件套（实现在下面的 TX DMA 段；这里先声明，因为 sb_spi_hw_init 要用） */
static void sb_dma_init(void);
static hpm_stat_t sb_dma_tx_start(const uint8_t *tx, uint32_t len);
static hpm_stat_t sb_dma_tx_wait(void);
static hpm_stat_t sb_spi_rx_poll(uint8_t *rx, uint32_t rlen);
/* 硬件重初始化（实现在配置块段；spi_bridge_poll 在它之前就要用） */
static void sb_hw_apply(void);

static void sb_spi_hw_init(void)
{
    clock_add_to_group(SB_SPI_CLK_NAME, 0);

    uint8_t quad = (s_prof.profile == SB_PROFILE_QSPI) ? 1U : 0U;
    uint8_t hw_cs = (s_cfg.cs_policy == 3U) ? 1U : 0U;
    init_spi2_bridge_pins(quad, hw_cs);

    memset(&s_format, 0, sizeof(s_format));
    spi_master_get_default_format_config(&s_format);
    s_format.master_config.addr_len_in_bytes = 1U;
    s_format.common_config.data_len_in_bits = 8U;
    s_format.common_config.data_merge = false;
    s_format.common_config.mosi_bidir = false;
    s_format.common_config.lsb = false;
    s_format.common_config.mode = spi_master_mode;
    s_format.common_config.cpol = (s_cfg.mode & 0x02U) ? spi_sclk_high_idle : spi_sclk_low_idle;
    /* 标准 SPI mode = CPOL<<1 | CPHA，CPHA=1 在**后沿**采样（even）、CPHA=0 在
     * **前沿**（odd）。这里曾经把两个枚举写反（mode&1 选了 odd），回环测试发现
     * 不了 —— 跳线短接时主机的驱动与采样用同一个 cpha，自洽就回得来；只有接上
     * 固定 CPHA 的真器件才暴露。2026-09-30 由 web 侧 + LA + 真屏三方对账定位。 */
    s_format.common_config.cpha = (s_cfg.mode & 0x01U) ? spi_sclk_sampling_even_clk_edges
                                                       : spi_sclk_sampling_odd_clk_edges;
    spi_format_init(SB_SPI, &s_format);
    s_fmt_addr_len = 1U;

    s_actual_sclk = sb_pick_sclk(s_cfg.sclk_hz);
    sb_dma_init();

    /* CS 脚：0/2 = PB10 作 GPIO；1 = 辅助脚作 GPIO；3 = 硬件 CS0（SPI2 的 CS0 也是 PB10） */
    if (s_cfg.cs_policy == 1U)
    {
        s_cs_pad = sb_pad_of(s_cfg.pad_cs_aux);
    }
    else if (s_cfg.cs_policy == 3U)
    {
        s_cs_pad = 0U;
    }
    else
    {
        s_cs_pad = IOC_PAD_PB10;
    }
    if (s_cs_pad != 0U)
    {
        sb_pad_as_output(s_cs_pad, sb_line_active_low(2U) ? 1U : 0U); /* 空闲 = 无效电平 */
    }
    s_cs_asserted = 0U;

    /*
     * 自检：把三个复位位写下去，看它们能不能自清。
     * 清不掉 = SPI2 的寄存器时钟没打开（或 IP 被挂在复位上），这时后面每一次
     * spi_control_init() 都会超时、`spi_is_active()` 又永远为真，表现就是
     * "第一帧 TIMEOUT、之后一路 BUSY"。dbg[0] = 轮询次数（0xFFFFFFFF = 清不掉），
     * dbg[1] = 自检后的 CTRL。
     */
    s_dbg[0] = 0xFFFFFFFFUL;
    SB_SPI->CTRL |= SPI_CTRL_TXFIFORST_MASK | SPI_CTRL_RXFIFORST_MASK | SPI_CTRL_SPIRST_MASK;
    for (uint32_t i = 0U; i < 100000U; i++)
    {
        if ((SB_SPI->CTRL & (SPI_CTRL_TXFIFORST_MASK | SPI_CTRL_RXFIFORST_MASK | SPI_CTRL_SPIRST_MASK)) == 0U)
        {
            s_dbg[0] = i;
            break;
        }
    }
    s_dbg[1] = SB_SPI->CTRL;
    s_dbg[2] = s_module_clk;
    s_dbg[3] = s_actual_sclk;
}

/* ============================== TX DMA ============================== */

/*
 * TX 数据相位走 DMA：源是 OUT 环里的槽（AHB SRAM，HPM5301 没有 L1C，
 * 不需要 cache 维护，也不需要 core_local_mem_to_sys_address 转换），
 * 目的地固定是 SPI 的 DATA 寄存器，握手走 DMAMUX 的 HPM_DMA_SRC_SPI2_TX。
 *
 * 通道由 dma_mgr 动态申请（和 CDC 的 UART2 RX/TX 同一套，不会硬编码撞车）；
 * 申请失败就 s_dma_ok=0，整条桥静默退回轮询，不影响功能。
 */
static void sb_dma_init(void)
{
    dma_mgr_chn_conf_t cfg;

    s_dma_ok = 0U;
    /* 上一条通道还在就先还回去。这条路在再次 ENABLE=1 和 enabled 状态下的
     * SET_CFG / SET_PROFILE 都会重走（sb_spi_hw_init → 这里），不释放直接把
     * s_dma 覆盖掉的话，每改一次配置就泄漏一个 DMA 通道 —— 池本来就小（CDC 已占
     * 2 个），几轮重配后 dma_mgr_request_resource 必然失败，桥静默退回轮询且
     * 重启前无法恢复。 */
    if (s_dma.base != NULL)
    {
        (void)dma_mgr_release_resource(&s_dma);
    }
    memset(&s_dma, 0, sizeof(s_dma));
    if (dma_mgr_request_resource(&s_dma) != status_success)
    {
        s_dbg[12] = 0xF0U; /* 诊断：DMA 通道没申请到 */
        return;
    }

    dma_mgr_get_default_chn_config(&cfg);
    cfg.src_width = DMA_MGR_TRANSFER_WIDTH_BYTE;
    cfg.dst_width = DMA_MGR_TRANSFER_WIDTH_BYTE;
    cfg.src_mode = DMA_MGR_HANDSHAKE_MODE_NORMAL;
    cfg.src_addr_ctrl = DMA_MGR_ADDRESS_CONTROL_INCREMENT;
    cfg.dst_mode = DMA_MGR_HANDSHAKE_MODE_HANDSHAKE; /* 每要一个字节 SPI 拉一次请求 */
    cfg.dst_addr_ctrl = DMA_MGR_ADDRESS_CONTROL_FIXED;
    cfg.dst_addr = (uint32_t)&SB_SPI->DATA;
    cfg.en_dmamux = true;
    cfg.dmamux_src = HPM_DMA_SRC_SPI2_TX;
    cfg.linked_ptr = (uint32_t)NULL;
    cfg.en_infiniteloop = false;
    cfg.interrupt_mask = DMA_MGR_INTERRUPT_MASK_ALL; /* 不用中断，纯轮询 TC */
    if (dma_mgr_setup_channel(&s_dma, &cfg) != status_success)
    {
        (void)dma_mgr_release_resource(&s_dma);
        s_dbg[12] = 0xF1U;
        return;
    }
    s_dma_ok = 1U;
}

static hpm_stat_t sb_dma_tx_start(const uint8_t *tx, uint32_t len)
{
    if (dma_mgr_set_chn_src_addr(&s_dma, (uint32_t)tx) != status_success)
    {
        return status_fail;
    }
    if (dma_mgr_set_chn_transize(&s_dma, len) != status_success)
    {
        return status_fail;
    }
    return dma_mgr_enable_channel(&s_dma);
}

/* 等 DMA 搬完（TC）。带硬超时，绝不挂死主循环。 */
static hpm_stat_t sb_dma_tx_wait(void)
{
    for (uint32_t i = 0U; i < 2000000U; i++)
    {
        uint32_t st = dma_check_transfer_status(s_dma.base, (uint8_t)s_dma.channel);
        if ((st & DMA_CHANNEL_STATUS_TC) != 0U)
        {
            return status_success;
        }
        if ((st & (DMA_CHANNEL_STATUS_ERROR | DMA_CHANNEL_STATUS_ABORT)) != 0U)
        {
            return status_fail;
        }
    }
    return status_timeout;
}

/* 收 RX FIFO（数据位宽固定 8 bit，一次一个字节）。DMA 喂 TX 的同时由 CPU 干这个。 */
static hpm_stat_t sb_spi_rx_poll(uint8_t *rx, uint32_t rlen)
{
    uint32_t got = 0U;
    uint32_t retry = 0U;

    if (rx == NULL)
    {
        return status_invalid_argument;
    }
    while (got < rlen)
    {
        if ((SB_SPI->STATUS & SPI_STATUS_RXEMPTY_MASK) == 0U)
        {
            rx[got++] = (uint8_t)SB_SPI->DATA;
            retry = 0U;
        }
        else if (++retry > 200000U)
        {
            return status_timeout;
        }
    }
    return status_success;
}

/*
 * 一次 SPI 事务（一次 CS 窗口里的 cmd / addr / dummy / 数据相位）。
 * 返回 sb_status_t。
 *
 * TX 数据相位的两条路径由 flags 与 cfg.tx_dma_threshold 决定：
 *   - 轮询：spi_transfer()（SDK 的整帧阻塞 API，内部逐字节灌 TX FIFO + 收 RX FIFO）
 *   - DMA ：自己拼序列（spi_control_init → 武装并启动 DMA 通道 → 开 SPI 的 TX DMA 请求
 *           → 写 ADDR/CMD 触发 → RX 侧仍由 CPU 轮询 → 等 DMA TC → 收尾）
 * RX 一律轮询（方案 §6.2）：主机侧读长度已知，CPU 收 FIFO 最短路径。
 */
static uint8_t sb_spi_xfer(const sb_xfer_t *x, uint8_t flags, const uint8_t *tx, uint8_t *rx)
{
    spi_control_config_t ctl;
    uint8_t cmd = x->cmd;
    uint32_t addr = x->addr;
    uint32_t wlen = x->tx_len;
    uint32_t rlen = x->rx_len;
    uint8_t cmd_en = (uint8_t)((x->tcfg & SB_TCFG_CMD_EN) ? 1U : 0U);
    uint8_t addr_en = (uint8_t)(((x->tcfg & SB_TCFG_ADDR_EN) && (x->addr_len > 0U)) ? 1U : 0U);
    uint8_t lines = (uint8_t)(x->tcfg & SB_TCFG_LINES_MASK);
    uint8_t trans_mode;
    uint8_t use_dma = 0U;
    uint32_t wcnt;
    uint32_t rcnt;
    uint32_t t0;
    hpm_stat_t stat;

    if ((cmd_en == 0U) && (addr_en == 0U) && (wlen == 0U) && (rlen == 0U))
    {
        return SB_E_BAD_FRAME;
    }
    /* 全双工（write/read together）要求收发等长；不等长请拆两帧 */
    if ((wlen != 0U) && (rlen != 0U) && (wlen != rlen))
    {
        return SB_E_RANGE;
    }

    if ((wlen != 0U) && (rlen != 0U))
    {
        trans_mode = spi_trans_write_read_together;
    }
    else if (wlen != 0U)
    {
        trans_mode = spi_trans_write_only;
    }
    else if (rlen != 0U)
    {
        trans_mode = (x->dummy != 0U) ? spi_trans_dummy_read : spi_trans_read_only;
    }
    else
    {
        trans_mode = spi_trans_no_data; /* 只有 cmd/addr 相位（例如 0x11 / 0x29） */
    }

    /* 阈值判定（方案 §6.1）：看 tx_len，不看整帧长度。
     *   0x00 = 全轮询；0xFF = 全 DMA；其余按 tx_len 比。
     * 单帧可以用 NO_DMA / FORCE_DMA 覆盖；DMA 通道没申请到就整体退回轮询。 */
    if ((s_dma_ok != 0U) && (wlen != 0U) && ((flags & SB_F_NO_DMA) == 0U))
    {
        if ((flags & SB_F_FORCE_DMA) != 0U)
        {
            use_dma = 1U;
        }
        else if (s_cfg.tx_dma_threshold == 0xFFU)
        {
            use_dma = 1U;
        }
        else if ((s_cfg.tx_dma_threshold != 0U) && (wlen >= (uint32_t)s_cfg.tx_dma_threshold))
        {
            use_dma = 1U;
        }
    }

    sb_spi_apply_format(x->addr_len);

    /*
     * ⚠️ 必须先清零再取默认值：SDK 的 `spi_master_get_default_control_config()`
     * **不初始化 `slave_config.slave_data_only`**（那是 `spi_slave_get_default_control_config()`
     * 才管的字段），而 `spi_control_init()` 会无条件把它写进 `TRANSCTRL.SLVDATAONLY`。
     * ctl 是栈上局部变量 ⇒ 那个字段是**栈垃圾**，偶尔为 1。
     *
     * 实测症状（2026-09-29 抓了一晚上）：回环**偶发**某笔事务读回全 00 / 全 FF，
     * 而 `status` 仍然是 OK、计数器也不报错；dbg 快照里能看到
     * `TRANSCTRL = 0x8001F81F`（bit31 被置上）而不是正常的 0x0001F81F。
     * 频度取决于栈布局 —— 把一段复位代码挪出本函数后就从"几乎不出现"变成"每几十笔一次"。
     */
    memset(&ctl, 0, sizeof(ctl));
    spi_master_get_default_control_config(&ctl);
    ctl.master_config.cmd_enable = (cmd_en != 0U);
    ctl.master_config.addr_enable = (addr_en != 0U);
    ctl.master_config.addr_phase_fmt = (x->tcfg & SB_TCFG_ADDR_QUAD)
                                           ? spi_address_phase_format_dualquad_io_mode
                                           : spi_address_phase_format_single_io_mode;
    ctl.master_config.token_enable = (x->tcfg & SB_TCFG_TOKEN_EN) ? true : false;
    ctl.master_config.token_value = spi_token_value_0x69;
    ctl.common_config.trans_mode = trans_mode;
    ctl.common_config.data_phase_fmt = (lines == SB_TCFG_LINES_4)   ? (uint8_t)spi_quad_io_mode
                                       : (lines == SB_TCFG_LINES_2) ? (uint8_t)spi_dual_io_mode
                                                                    : (uint8_t)spi_single_io_mode;
    ctl.common_config.dummy_cnt = (x->dummy > 0U) ? (uint8_t)(x->dummy - 1U) : (uint8_t)0;
    ctl.common_config.cs_index = spi_cs_0;

    /* no_data 模式下驱动仍会写 WR/RD_TRANS_CNT = count-1，不能传 0 */
    wcnt = (trans_mode == spi_trans_no_data) ? 1U : wlen;
    rcnt = (trans_mode == spi_trans_no_data) ? 1U : rlen;

    /* dbg[11] = bit0 调用前总线是否已经"在忙"，bit16.. wcnt */
    s_dbg[11] = (spi_is_active(SB_SPI) ? 1U : 0U) | ((wcnt & 0xFFFFU) << 16);

    s_dbg[12] = 1U; /* stage: 已就绪，准备启动 */

    t0 = SB_MTIME;

    if (use_dma != 0U)
    {
        /*
         * DMA 路径。顺序照 SDK 头文件的原话：
         * "disable the SPI TX DMA request before configuring and enabling the DMA
         *  channels, and then start the SPI transfer"。
         * 也就是：关 TXDMAEN → 配通道/设地址与长度 → 使能通道 → 开 TXDMAEN → 写 CMD 触发。
         */
        SB_SPI->CTRL &= ~SPI_CTRL_TXDMAEN_MASK;

        stat = spi_control_init(SB_SPI, &ctl, wcnt, rcnt);
        s_dbg[12] = 3U;
        if (stat == status_success)
        {
            stat = sb_dma_tx_start(tx, wlen);
            s_dbg[12] = 4U;
        }
        if (stat == status_success)
        {
            SB_SPI->CTRL |= SPI_CTRL_TXDMAEN_MASK;
            stat = spi_write_address(SB_SPI, spi_master_mode, &ctl, (addr_en != 0U) ? &addr : NULL);
            s_dbg[12] = 5U;
        }
        if (stat == status_success)
        {
            stat = spi_write_command(SB_SPI, spi_master_mode, &ctl, (cmd_en != 0U) ? &cmd : NULL);
            s_dbg[12] = 6U;
        }
        if ((stat == status_success) && (rlen != 0U))
        {
            /* RX 与 DMA 并行：CPU 收 FIFO */
            stat = sb_spi_rx_poll(rx, rlen);
            s_dbg[12] = 7U;
        }
        {
            hpm_stat_t dstat = sb_dma_tx_wait();
            if (stat == status_success)
            {
                stat = dstat;
            }
            s_dbg[12] = 8U;
        }
        SB_SPI->CTRL &= ~SPI_CTRL_TXDMAEN_MASK;
        (void)dma_mgr_disable_channel(&s_dma);
        if (stat == status_success)
        {
            stat = spi_wait_for_idle_status(SB_SPI);
            s_dbg[12] = 9U;
        }
    }
    else
    {
        stat = spi_transfer(SB_SPI, &ctl,
                            (cmd_en != 0U) ? &cmd : NULL,
                            (addr_en != 0U) ? &addr : NULL,
                            (uint8_t *)tx, wcnt, rx, rcnt);
        s_dbg[12] = 2U; /* stage: spi_transfer 已返回 */
    }

    s_last_ticks = SB_MTIME - t0;

    /* 现场快照：出错时把 SPI 的寄存器原样留着，主机用 HID action 10 取。
     * dbg[4]=STATUS dbg[5]=CTRL dbg[6]=TRANSCTRL dbg[7]=TRANSFMT dbg[8]=TIMING
     * dbg[9]=写/读计数 dbg[10]=SDK 返回码 */
    s_dbg[4] = SB_SPI->STATUS;
    s_dbg[5] = SB_SPI->CTRL;
    s_dbg[6] = SB_SPI->TRANSCTRL;
    s_dbg[7] = SB_SPI->TRANSFMT;
    s_dbg[8] = SB_SPI->TIMING;
    s_dbg[9] = (SB_SPI->WR_TRANS_CNT & 0xFFFFU) | ((SB_SPI->RD_TRANS_CNT & 0xFFFFU) << 16);
    s_dbg[10] = (uint32_t)stat;
    if (stat != status_success)
    {
        s_spi_need_reset = 1U;
        if (stat == status_spi_master_busy)
        {
            return SB_E_BUSY;
        }
        return (stat == status_timeout) ? SB_E_TIMEOUT : SB_E_DMA;
    }

    s_stats.bytes_tx += x->tx_len;
    s_stats.bytes_rx += x->rx_len;
    if ((wlen != 0U) || (rlen != 0U))
    {
        if (use_dma != 0U)
        {
            s_stats.tx_dma_cnt++;
        }
        else
        {
            s_stats.tx_poll_cnt++;
        }
    }
    return SB_OK;
}

/* ============================== OUT 环 ============================== */

/* 武装下一包 OUT。**必须在关中断下做「查在飞 + 武装」**：本函数既被主循环调，
 * 也被 OUT 完成回调（ISR）调，两步之间被打断就会重复 usbd_ep_start_read ——
 * DWC2 端口层对忙端点再来一笔是**静默顶掉**（返回 0、回调永不来），
 * 那样 s_out_inflight 就永远停在 1，桥从此收不到任何帧。 */
static void sb_out_kick(void)
{
    uint32_t lvl = sb_irq_save();
    if ((s_enabled == 0U) || (s_out_inflight != 0U))
    {
        sb_irq_restore(lvl);
        return;
    }
    if ((SB_OUT_SLOTS - s_out_used) < 2U)
    {
        sb_irq_restore(lvl);
        return; /* 环满：不武装，主机侧收到 NAK（背压） */
    }
    if (usbd_ep_start_read(0, SB_OUT_EP, s_out_buf[s_out_w], SB_PKT_SIZE) == 0)
    {
        s_out_armed_gen = s_out_gen; /* 记下这一笔属于哪一代 */
        s_out_inflight = 1U;
    }
    sb_irq_restore(lvl);
}

void spi_bridge_out_done(uint32_t nbytes)
{
    s_out_inflight = 0U;
    if (s_out_armed_gen != s_out_gen)
    {
        /* 跨复位收到的旧包：丢掉，绝不入环（否则会被当新帧执行） */
        sb_out_kick();
        return;
    }
    if (nbytes != 0U)
    {
        if (nbytes > SB_PKT_SIZE)
        {
            nbytes = SB_PKT_SIZE;
        }
        s_out_pkt_len[s_out_w] = (uint16_t)nbytes;
        s_out_w = (uint16_t)((s_out_w + 1U) % SB_OUT_SLOTS);
        s_out_used++;
    }
    sb_out_kick();
}

/* ============================== IN 环 ============================== */

/* 同上：主循环（sb_in_commit）与 IN 完成回调（ISR）都会调，必须整体互斥。 */
static void sb_in_kick(void)
{
    uint32_t lvl = sb_irq_save();
    if ((s_in_inflight != 0U) || (s_in_used == 0U))
    {
        sb_irq_restore(lvl);
        return;
    }
    uint16_t slot = s_in_r;
    uint16_t len = (uint16_t)(rd_u16(&s_in_buf[slot][6]) + 8U);
    if (len > SB_PKT_SIZE)
    {
        len = SB_PKT_SIZE;
    }
    if (usbd_ep_start_write(0, SB_IN_EP, s_in_buf[slot], len) == 0)
    {
        s_in_armed_gen = s_in_gen;
        s_in_inflight = 1U;
    }
    sb_irq_restore(lvl);
}

/* 取一个空槽用于应答；返回槽首地址（前 8 B 留给应答头），NULL = 环满 */
static uint8_t *sb_in_alloc(void)
{
    if ((SB_IN_SLOTS - s_in_used) < 2U)
    {
        return NULL; /* 留一格余量，避免和正在发的包抢 */
    }
    return s_in_buf[s_in_w];
}

static void sb_in_commit(uint16_t payload_len)
{
    uint8_t *p = s_in_buf[s_in_w];
    uint32_t lvl;
    wr_u16(&p[6], payload_len);
    lvl = sb_irq_save();
    s_in_w = (uint16_t)((s_in_w + 1U) % SB_IN_SLOTS);
    s_in_used++;
    sb_irq_restore(lvl);
    sb_in_kick();
}

static uint8_t *sb_rsp_init(uint8_t *slot, uint8_t type, uint8_t status, uint16_t seq)
{
    wr_u16(&slot[0], SB_MAGIC);
    slot[2] = type;
    slot[3] = status;
    wr_u16(&slot[4], seq);
    wr_u16(&slot[6], 0U);
    return &slot[8];
}

void spi_bridge_in_done(uint32_t nbytes)
{
    (void)nbytes;
    if (s_in_armed_gen != s_in_gen)
    {
        /* 跨复位的那一包：环已经清过，别再去减 s_in_used（会下溢） */
        s_in_inflight = 0U;
        sb_in_kick();
        return;
    }
    if (s_in_used != 0U)
    {
        s_in_r = (uint16_t)((s_in_r + 1U) % SB_IN_SLOTS);
        s_in_used--;
    }
    s_in_inflight = 0U;
    sb_in_kick();
}

/* ============================== 面板档 ============================== */

static inline uint8_t sb_def_lines_tcfg(void)
{
    return (s_prof.def_lines == 2U)   ? (uint8_t)SB_TCFG_LINES_2
           : (s_prof.def_lines == 4U) ? (uint8_t)SB_TCFG_LINES_4
                                      : (uint8_t)SB_TCFG_LINES_1;
}

/* DC 线：is_data = 1 表示"数据"，0 表示"命令" */
static void sb_set_dc(uint8_t is_data)
{
    uint8_t lvl = is_data ? (uint8_t)(s_prof.dc_active_high ? 1U : 0U)
                          : (uint8_t)(s_prof.dc_active_high ? 0U : 1U);
    if (sb_line_active_low(0U) != 0U)
    {
        lvl = (uint8_t)(!lvl);
    }
    sb_pad_write(s_pad_dc, lvl);
}

/* 档 1（spi_dcx）：一个 CS 窗口内「命令(8bit) → 翻 DC → 参数」。
 * 注意：命令是**当数据相位**发出去的（DC=命令电平），不是 CMD 相位 —— SPI 屏的
 * 「命令/数据」全靠 DC 区分，硬件并没有独立的命令相位概念。 */
static uint8_t sb_step_spi_dcx(uint8_t cmd, uint8_t nparams, const uint8_t *params)
{
    sb_xfer_t x;
    uint8_t st;
    uint8_t cs_auto = (uint8_t)((s_cfg.cs_policy == 0U) || (s_cfg.cs_policy == 1U)) ? 1U : 0U;

    if (s_pad_dc == 0U)
    {
        return SB_E_GPIO;
    }
    if (cs_auto != 0U)
    {
        sb_cs_assert();
    }

    sb_set_dc(0U); /* 命令 */
    memset(&x, 0, sizeof(x));
    x.tcfg = sb_def_lines_tcfg();
    x.tx_len = 1U;
    st = sb_spi_xfer(&x, 0U, &cmd, NULL);

    if ((st == SB_OK) && (nparams != 0U))
    {
        sb_set_dc(1U); /* 数据 */
        memset(&x, 0, sizeof(x));
        x.tcfg = sb_def_lines_tcfg();
        x.tx_len = nparams;
        st = sb_spi_xfer(&x, 0U, params, NULL);
    }

    if (cs_auto != 0U)
    {
        sb_cs_release();
    }
    return st;
}

/* 档 2（qspi）：opcode + 24 bit 地址（命令字在 bits[23:16]）+ 1 线参数，
 * 线上 = `02 | 00 <cmd> 00 | params`（ST77916 类 QSPI 屏的通行帧型） */
static uint8_t sb_step_qspi(uint8_t cmd, uint8_t nparams, const uint8_t *params)
{
    sb_xfer_t x;
    uint8_t opcode = (s_prof.qspi_wr_opcode != 0U) ? s_prof.qspi_wr_opcode : 0x02U;
    uint8_t abytes = (s_prof.qspi_addr_bytes != 0U) ? s_prof.qspi_addr_bytes : 3U;
    uint8_t cs_auto = (uint8_t)((s_cfg.cs_policy == 0U) || (s_cfg.cs_policy == 1U)) ? 1U : 0U;
    uint8_t st;

    memset(&x, 0, sizeof(x));
    x.cmd = opcode;
    x.tcfg = SB_TCFG_CMD_EN | SB_TCFG_ADDR_EN | SB_TCFG_LINES_1;
    x.addr_len = abytes;
    /*
     * ⚠️ 命令字放在地址的 bits[23:16]（线上第二个字节）：addr = cmd << 8，
     * 线上 = `02 | 00 <cmd> 00 | params`。历史上这里错过两版：
     *   第一版 `cmd << 16`（LA 实测发出 `02 F0 00 00 28`，屏只看到地址 0xF00000）；
     *   第二版改成 `addr = cmd` —— 依据是"硬件按 addr_len 发低 addr_len 个字节、
     *   MSB 在前"（这一点 LA 验过、至今成立），但**推错了面板要的命令字位置**，
     *   发成 `02 | 00 00 cmd`。LA 当时只验了地址字段的字节序，没验"哪个字节
     *   装命令"——真屏从未在 QSPI 档接过，这个错误一直没暴露。
     * 订正依据（2026-09-30，web 侧对账提出，见其 docs/spi-bridge-page.md §11.12）：
     *   1. 本固件自己的像素写约定就是 addr=0x002C00（RAMWR 0x2C << 8）——
     *      命令与像素走同一条线，编码位置必须一致，此前自相矛盾；
     *   2. ST77916 类 QSPI 屏的通行帧型（ESP-IDF esp_lcd 等生态）均为
     *      `00 <cmd> 00`：写命令 0x02、写像素 0x32@0x002C00、读 0x0B@0x002E00。
     * 网页侧已先用直接 XFER（addr=0x002A00/0x002B00 开窗）绕开本函数；
     * 本修复让 STEP 与之逐字节一致。真屏（ST77916）尚未接上，上板终验待做。
     */
    x.addr = ((uint32_t)cmd << 8);
    x.tx_len = nparams;

    if (cs_auto != 0U)
    {
        sb_cs_assert();
    }
    st = sb_spi_xfer(&x, 0U, params, NULL);
    if (cs_auto != 0U)
    {
        sb_cs_release();
    }
    return st;
}

/* 档 0（raw）：cmd 相位 + 参数数据相位，同线数 */
static uint8_t sb_step_raw(uint8_t cmd, uint8_t nparams, const uint8_t *params)
{
    sb_xfer_t x;
    uint8_t cs_auto = (uint8_t)((s_cfg.cs_policy == 0U) || (s_cfg.cs_policy == 1U)) ? 1U : 0U;
    uint8_t st;

    memset(&x, 0, sizeof(x));
    x.cmd = cmd;
    x.tcfg = SB_TCFG_CMD_EN | sb_def_lines_tcfg();
    x.tx_len = nparams;

    if (cs_auto != 0U)
    {
        sb_cs_assert();
    }
    st = sb_spi_xfer(&x, 0U, params, NULL);
    if (cs_auto != 0U)
    {
        sb_cs_release();
    }
    return st;
}

static uint8_t sb_step_exec(uint8_t cmd, uint8_t nparams, const uint8_t *params)
{
    switch (s_prof.profile)
    {
    case SB_PROFILE_SPI_DCX:
        return sb_step_spi_dcx(cmd, nparams, params);
    case SB_PROFILE_QSPI:
        return sb_step_qspi(cmd, nparams, params);
    default:
        return sb_step_raw(cmd, nparams, params);
    }
}

/* ============================== 帧执行 ============================== */

static uint8_t s_periodic_executing;
static uint8_t sb_exec_frame(const uint8_t *hdr, const uint8_t *pl, uint16_t plen,
                             uint8_t *rsp_payload, uint16_t *rsp_len)
{
    uint8_t type = hdr[2];
    uint8_t flags = hdr[3];
    uint8_t st = SB_OK;

    *rsp_len = 0U;
    if (!s_periodic_executing && bus_periodic_owns(BP_SPI)) return SB_E_BUSY;

    switch (type)
    {
    case SB_T_XFER:
    {
        sb_xfer_t x;
        uint8_t manual_cs = (s_cfg.cs_policy == 2U) ? 1U : 0U;
        uint8_t lines;

        if (plen < 12U)
        {
            return SB_E_BAD_FRAME;
        }
        x.cmd = pl[0];
        x.tcfg = pl[1];
        x.addr_len = pl[2];
        x.dummy = pl[3];
        x.tx_len = rd_u16(&pl[4]);
        x.rx_len = rd_u16(&pl[6]);
        x.addr = rd_u32(&pl[8]);

        lines = (uint8_t)(x.tcfg & SB_TCFG_LINES_MASK);
        if (lines > SB_TCFG_LINES_4)
        {
            return SB_E_RANGE;
        }
        if (((uint32_t)x.tx_len + 12U) > (uint32_t)plen)
        {
            return SB_E_BAD_FRAME;
        }
        if ((uint32_t)x.rx_len > SB_FRAME_MAX)
        {
            return SB_E_RANGE;
        }
        if ((x.rx_len != 0U) && (rsp_payload == NULL))
        {
            return SB_E_IN_FULL;
        }

        if ((x.tcfg & SB_TCFG_DC_EN) != 0U)
        {
            if (s_pad_dc == 0U)
            {
                return SB_E_GPIO;
            }
            uint8_t lvl = (x.tcfg & SB_TCFG_DC_LEVEL) ? 1U : 0U;
            sb_pad_write(s_pad_dc, sb_line_active_low(0U) ? (uint8_t)(!lvl) : lvl);
        }

        if ((manual_cs == 0U) && (s_cs_asserted == 0U))
        {
            sb_cs_assert();
        }

        st = sb_spi_xfer(&x, flags, &pl[12], (x.rx_len != 0U) ? rsp_payload : NULL);

        if ((manual_cs == 0U) && ((flags & SB_F_CS_HOLD) == 0U))
        {
            sb_cs_release();
        }
        /*
         * 读到的字节**即使事务报错也照原样带回**。上板调 SPI 时，"回读是全 0 / 全 FF /
         * 还是正确的数据"是区分"线没接对"和"只是收尾状态没清"的唯一线索；只回状态码
         * 会让这两种情况长得一模一样。
         */
        if (x.rx_len != 0U)
        {
            *rsp_len = x.rx_len;
        }
        break;
    }

    case SB_T_CS:
        if (plen < 1U)
        {
            return SB_E_BAD_FRAME;
        }
        if (pl[0] != 0U)
        {
            sb_cs_assert();
        }
        else
        {
            sb_cs_release();
        }
        break;

    case SB_T_GPIO:
    {
        uint16_t pad;
        uint8_t line_no;

        if (plen < 2U)
        {
            return SB_E_BAD_FRAME;
        }
        switch (pl[0])
        {
        case SB_LINE_DC:
            pad = s_pad_dc;
            line_no = 0U;
            break;
        case SB_LINE_RST:
            pad = s_pad_rst;
            line_no = 1U;
            break;
        case SB_LINE_CS_AUX:
            pad = sb_pad_of(s_cfg.pad_cs_aux);
            line_no = 2U;
            break;
        case SB_LINE_BL:
            pad = s_pad_bl;
            line_no = 3U;
            break;
        default:
            return SB_E_GPIO;
        }
        if (pad == 0U)
        {
            return SB_E_GPIO;
        }
        sb_pad_write(pad, sb_line_active_low(line_no) ? (uint8_t)(!pl[1]) : pl[1]);
        break;
    }

    case SB_T_DELAY:
        if (plen < 4U)
        {
            return SB_E_BAD_FRAME;
        }
        sb_delay_us(rd_u32(pl));
        break;

    case SB_T_PING:
        break;

    case SB_T_CFG:
        if (plen < 3U)
        {
            return SB_E_BAD_FRAME;
        }
        if (pl[0] == SB_CFG_TX_DMA_THRESHOLD)
        {
            s_cfg.tx_dma_threshold = pl[1];
        }
        break;

    case SB_T_STEP:
        if (plen < 4U)
        {
            return SB_E_BAD_FRAME;
        }
        {
            uint8_t cmd = pl[0];
            uint8_t nparams = pl[1];
            if (((uint32_t)nparams + 4U) > (uint32_t)plen)
            {
                return SB_E_BAD_FRAME;
            }
            st = sb_step_exec(cmd, nparams, &pl[4]);
            if (st == SB_OK)
            {
                uint16_t dms = rd_u16(&pl[2]);
                if (dms != 0U)
                {
                    sb_delay_us((uint32_t)dms * 1000U);
                }
            }
        }
        break;

    case SB_T_RESET:
        if (plen < 4U)
        {
            return SB_E_BAD_FRAME;
        }
        if (s_pad_rst == 0U)
        {
            return SB_E_GPIO;
        }
        /* 拉低保持 -> 释放 -> 等待，两段都由 poll 非阻塞推进。
         * ⚠️ low_ms 是**毫秒**，必须 ×1000 再交给 sb_delay_us()（那个接口吃微秒）。
         * 少了这个 ×1000，请求 2 ms 的复位脉冲实际只有 2 µs —— LA 实测 9.75 µs，
         * 面板根本复位不了（差 1000 倍，而且症状是"屏不亮"这种最难查的）。 */
        sb_pad_write(s_pad_rst, sb_line_active_low(1U) ? 0U : 1U);
        s_rst_post_ms = rd_u16(&pl[2]);
        s_rst_state = 1U;
        sb_delay_us((uint32_t)rd_u16(pl) * 1000U);
        break;

    case SB_T_AUX_IN:
        if (s_pad_te == 0U)
        {
            return SB_E_GPIO;
        }
        if (rsp_payload == NULL)
        {
            return SB_E_IN_FULL;
        }
        rsp_payload[0] = (uint8_t)(sb_pad_read(s_pad_te) ? SB_AUXIN_TE : 0U);
        *rsp_len = 1U;
        break;

    default:
        return SB_E_BAD_FRAME;
    }

    return st;
}

/* ============================== 主循环 ============================== */

static void sb_out_release_front(void)
{
    uint32_t lvl = sb_irq_save();
    s_out_r = (uint16_t)((s_out_r + 1U) % SB_OUT_SLOTS);
    s_out_used--;
    sb_irq_restore(lvl);
    s_pkt_active = 0U;
    s_pkt_off = 0U;
    s_pkt_len = 0U;
}

uint8_t spi_bridge_periodic_ready(void) {
    return s_enabled && !s_hw_req && !s_reset_req && !s_abort_req && !s_usb_reset_req &&
        !s_out_used && !s_pkt_active && !s_delay_active && !s_rst_state;
}
uint8_t spi_bridge_periodic_check(const uint8_t *p, uint16_t len) {
    if (len < 8U || len > 128U || rd_u16(p) != SB_MAGIC || rd_u16(p + 6) != len - 8U ||
        (p[3] & ~(SB_F_RSP | SB_F_CS_HOLD | SB_F_CS_OFF | SB_F_NO_DMA | SB_F_FORCE_DMA))) return SB_E_RANGE;
    uint16_t size = (uint16_t)(len - 8U); const uint8_t *b = p + 8;
    switch (p[2]) {
    case SB_T_XFER:
        if (size < 12U || size != 12U + rd_u16(b + 4) || rd_u16(b + 6) > BP_DATA ||
            b[2] > 4U || b[3] > 4U || (b[1] & SB_TCFG_LINES_MASK) > SB_TCFG_LINES_4 ||
            (rd_u16(b + 4) && rd_u16(b + 6) && rd_u16(b + 4) != rd_u16(b + 6))) return SB_E_RANGE;
        break;
    case SB_T_CS: if (size != 1U || b[0] > 1U) return SB_E_RANGE; break;
    case SB_T_GPIO: if (size != 2U || b[0] > SB_LINE_BL || b[1] > 1U) return SB_E_RANGE; break;
    case SB_T_PING: case SB_T_AUX_IN: if (size != 0U) return SB_E_RANGE; break;
    default: return SB_E_RANGE; /* DELAY is a scheduler record, never a blocking SPI delay. */
    }
    return SB_OK;
}
uint8_t spi_bridge_periodic_exec(const uint8_t *p, uint16_t len, uint8_t *data, uint8_t *n) {
    *n = 0U;
    if (!s_enabled || s_hw_req || s_usb_reset_req) return SB_E_DISABLED;
    uint8_t err = spi_bridge_periodic_check(p, len); if (err) return err;
    uint16_t count = 0U;
    s_periodic_executing = 1U;
    err = sb_exec_frame(p, p + 8, (uint16_t)(len - 8U), data, &count);
    s_periodic_executing = 0U;
    if ((p[3] & SB_F_CS_OFF) || err) sb_cs_release();
    *n = (uint8_t)count;
    return err;
}
void spi_bridge_periodic_release(void) { if (s_enabled) sb_cs_release(); }

static void sb_process_packets(void)
{
    uint32_t frames = 0U;
    uint32_t bytes = 0U;

    while (s_out_used != 0U)
    {
        if (s_pkt_active == 0U)
        {
            s_pkt_active = 1U;
            s_pkt_len = s_out_pkt_len[s_out_r];
            s_pkt_off = 0U;
            if (s_pkt_len > SB_PKT_SIZE)
            {
                s_pkt_len = SB_PKT_SIZE;
            }
        }

        if (s_pkt_off >= s_pkt_len)
        {
            sb_out_release_front();
            continue;
        }
        if ((frames >= SB_POLL_MAX_FRAMES) || (bytes >= SB_POLL_MAX_BYTES))
        {
            return; /* 让出主循环，下一轮接着来 */
        }

        const uint8_t *pkt = s_out_buf[s_out_r];
        const uint8_t *hdr = &pkt[s_pkt_off];
        uint16_t len;

        if ((uint16_t)(s_pkt_len - s_pkt_off) < 8U)
        {
            sb_out_release_front(); /* 尾部残渣 */
            continue;
        }
        if (rd_u16(&hdr[0]) != SB_MAGIC)
        {
            s_stats.frames_err++;
            s_last_err = SB_E_BAD_MAGIC;
            sb_out_release_front();
            continue;
        }
        len = rd_u16(&hdr[6]);
        if (((uint32_t)len + 8U) > (uint32_t)(s_pkt_len - s_pkt_off))
        {
            s_stats.frames_err++;
            s_last_err = SB_E_BAD_FRAME;
            sb_out_release_front();
            continue;
        }

        {
            uint8_t flags = hdr[3];
            uint8_t need_rsp = (uint8_t)((flags & SB_F_RSP) ? 1U : 0U);
            uint8_t *slot = NULL;
            uint8_t *rsp_payload = NULL;
            uint16_t rsp_len = 0U;
            uint8_t st;

            if (need_rsp != 0U)
            {
                slot = sb_in_alloc();
                if (slot == NULL)
                {
                    return; /* IN 环满：不消费这一帧，等主机取走数据 */
                }
                rsp_payload = &slot[8];
            }

            st = sb_exec_frame(hdr, &hdr[8], len, rsp_payload, &rsp_len);
            if (st == SB_WOULD_BLOCK)
            {
                return;
            }

            if (st == SB_OK)
            {
                s_stats.frames_ok++;
            }
            else
            {
                s_stats.frames_err++;
                s_last_err = st; /* 状态字 bit8..15 报的是**最近一次错误码**，不是计数 */
            }

            if (slot != NULL)
            {
                sb_rsp_init(slot, SB_R_RSP, st, rd_u16(&hdr[4]));
                sb_in_commit(rsp_len);
            }
            else if (st != SB_OK)
            {
                uint8_t *evt = sb_in_alloc();
                if (evt != NULL)
                {
                    sb_rsp_init(evt, SB_R_EVT, st, rd_u16(&hdr[4]));
                    sb_in_commit(0U);
                }
                else
                {
                    s_stats.in_ring_drop++;
                }
            }

            frames++;
            bytes += (uint32_t)len + 8U;
            s_pkt_off = (uint16_t)(s_pkt_off + 8U + len);
            if (s_pkt_off >= s_pkt_len)
            {
                sb_out_release_front();
            }

            /*
             * 延时 / 复位脉冲是**非阻塞**的，但它是**有序帧流里的一个动作**：
             * 挡在它后面的帧必须等它到点才轮到（面板初始化序列就是"发完命令等
             * 120 ms 再发下一条"，而一个 USB 包里经常挤着好几条 STEP）。
             * 只在一个包处理完才检查延时是不够的 —— 实测 DELAY(2000µs)+PING 在同一
             * 个包里时，PING 的应答会立刻回来，延时形同虚设。
             */
            if ((sb_delay_pending() != 0U) || (s_rst_state != 0U))
            {
                return;
            }
        }
    }
}

/* EP11-only teardown: one short response completes one host IN request.
 * Runs while disabled too; no SPI transfer, target access, GPIO or USB reset. */
static void sb_drain_pending_reads(void)
{
    if (s_drain_reads == 0U) { return; }
    uint8_t *slot = sb_in_alloc();
    if (slot == NULL) { return; }
    sb_rsp_init(slot, SB_R_EVT, SB_OK, 0xFFFFU);
    sb_in_commit(0U);
    s_drain_reads--;
}

void spi_bridge_poll(void)
{
    if (s_slave_owner) { return; }
    if (s_adc_owner) { adc_stream_poll(); return; }
    if (s_usb_reset_req != 0U)
    {
        /* USB 总线复位：端点上在飞的传输被硬件作废、完成回调**不会再来**，
         * 所以这里必须把在飞标志清掉，否则桥从此再也不武装端点。
         * 代数 +1 是为了让万一迟到的回调被识别成旧包丢弃。 */
        s_usb_reset_req = 0U;
        s_drain_reads = 0U;
        s_out_gen++;
        s_in_gen++;
        s_out_w = 0U;
        s_out_r = 0U;
        s_out_used = 0U;
        s_out_inflight = 0U;
        s_in_w = 0U;
        s_in_r = 0U;
        s_in_used = 0U;
        s_in_inflight = 0U;
        s_pkt_active = 0U;
        s_pkt_off = 0U;
        s_pkt_len = 0U;
        s_delay_active = 0U;
        s_rst_state = 0U;
    }
    if (s_reset_req != 0U)
    {
        /*
         * 清环（HID RESET / ENABLE 时的 CLEAR_ON_ENABLE）：只丢"已收但还没处理"的帧。
         * 两个刻意的选择：
         *   ① **不动 s_out_inflight / s_in_inflight**：端点上那一笔已武装的传输照样会
         *      完成，置 0 会让 sb_*_kick() 重复武装，而 DWC2 对忙端点再来一笔是静默顶掉
         *      （回调永不来）→ 在飞标志永久卡住。
         *   ② **也不 bump 代数**：那一笔完成时收到的是"复位之后主机才发来的新数据"，
         *      必须当新帧执行（实测按代数丢弃会把 enable 后的第一帧直接吃掉）。
         *      代数只在 USB 总线复位（传输被硬件作废）时才 +1。
         */
        s_reset_req = 0U;
        /* 清环的读-改-写必须与 out_done/in_done（ISR）互斥：不关中断的话，
         * "s_out_r = s_out_w" 与 ISR 的 "s_out_w++ / s_out_used++" 交错会把
         * 刚收下的那包卡在 used=0 的环里（永远不被执行，槽位静默泄漏）。 */
        {
            uint32_t lvl = sb_irq_save();
            s_out_r = s_out_w;
            s_out_used = 0U;
            s_in_r = s_in_w;
            s_in_used = 0U;
            sb_irq_restore(lvl);
        }
        s_pkt_active = 0U;
        s_pkt_off = 0U;
        s_pkt_len = 0U;
        s_delay_active = 0U;
        s_rst_state = 0U;
    }
    if (s_abort_req != 0U)
    {
        s_abort_req = 0U;
        /* ABORT 的契约是"丢弃未处理帧**与 IN 队列**"（spi_bridge_proto.h）——
         * OUT 与 IN 两侧一起清，同样要跟完成回调互斥。 */
        {
            uint32_t lvl = sb_irq_save();
            s_out_r = s_out_w;
            s_out_used = 0U;
            s_in_r = s_in_w;
            s_in_used = 0U;
            sb_irq_restore(lvl);
        }
        s_pkt_active = 0U;
        s_pkt_off = 0U;
        s_pkt_len = 0U;
        s_delay_active = 0U;
        s_rst_state = 0U;
    }

    if (s_drain_reads != 0U)
    {
        sb_drain_pending_reads();
    }

    if (s_enabled == 0U)
    {
        /* Disabled cleanup must not carry a reconfiguration request or held CS. */
        s_hw_req = 0U;
        if (s_cs_asserted != 0U) { sb_cs_release(); }
        return;
    }

    if (s_hw_req != 0U)
    {
        /* ISR 登记的硬件重初始化在这里落地（ENABLE / SET_CFG / SET_PROFILE）。
         * 放在 sb_process_packets 之前，保证任何帧执行时硬件已经就绪。 */
        s_hw_req = 0U;
        sb_hw_apply();
    }

    sb_in_kick();
    sb_out_kick();

    /* 复位脉冲：拉低 -> 释放 -> 等待 */
    if (s_rst_state == 1U)
    {
        if (sb_delay_pending() == 0U)
        {
            if (s_pad_rst != 0U)
            {
                sb_pad_write(s_pad_rst, sb_line_active_low(1U) ? 1U : 0U); /* 释放 */
            }
            if (s_rst_post_ms != 0U)
            {
                s_rst_state = 2U;
                sb_delay_us((uint32_t)s_rst_post_ms * 1000U);
            }
            else
            {
                s_rst_state = 0U;
            }
        }
        return;
    }
    if (s_rst_state == 2U)
    {
        if (sb_delay_pending() == 0U)
        {
            s_rst_state = 0U;
        }
        return;
    }

    if (sb_delay_pending() != 0U)
    {
        return; /* 延时期间不推进帧（USB 环照收） */
    }

    /*
     * 上一笔事务失败后控制器会**卡在 ACTIVE**（SPIACTIVE 恒 1）：之后每一笔
     * spi_transfer() 都在 `spi_is_active()` 那道检查上直接返回 BUSY，长度扫描
     * 从第二条起就全是 BUSY、什么也测不出来。所以失败后要复位控制器。
     *
     * 复位**放在主循环里做**（这时 CS 一定是释放的），而不是等下一笔事务进来再做：
     * 后者会让这一次复位（实测 ~14 µs）落在**下一个 CS 窗口里**，把面板命令的
     * 窗口无谓撑长（LA 实测过：1 字节只写事务的 CS 窗口 21.06 µs，其中 13.98 µs
     * 就是这次复位，而它不出现在 last_ticks 里，很容易查错方向）。
     */
    if (s_spi_need_reset != 0U)
    {
        s_spi_need_reset = 0U;
        spi_reset(SB_SPI);
        (void)spi_poll_reset_complete(SB_SPI, spi_reset_all, 100000U);
    }

    sb_process_packets();
}

/* ============================== 配置块 ============================== */

static uint8_t sb_pad_ok(uint8_t idx, uint8_t quad_on)
{
    (void)quad_on; /* SPI2 时代：quad 与否都不再占用 PA30/PA31，脚位限制看 reserved[] */

    if (idx == SB_PAD_NONE)
    {
        return 1U;
    }
    if (idx >= SB_PAD_MAX)
    {
        return 0U;
    }
    if (s_pad_table[idx] == 0U)
    {
        return 0U; /* PY00/PY01：v1 不支持 */
    }
    /*
     * 2026-09-30：桥搬到 SPI2 之后，quad 档占的是 PB10~PB15，与辅助脚表不再冲突
     * （PB10~PB13 那四项由 sb_cfg_validate 的 reserved[] 兜底拒绝）。
     * 旧的"quad 下 PA30/PA31 是 IO2/IO3"规则必须去掉，否则 PA31 当 RST 会被拒（RANGE），
     * 而 PA30 现在也不该被推荐 —— 它是 USB0_PWR 网络，被板上 Q1 常态短到地。
     */
    return 1U;
}

static uint8_t sb_cfg_validate(const sb_cfg_t *c)
{
    uint8_t quad = (s_prof.profile == SB_PROFILE_QSPI) ? 1U : 0U;

    if (c->bits != 8U)
    {
        return 0U;
    }
    if (c->mode > 3U)
    {
        return 0U;
    }
    if (c->cs_policy > 3U)
    {
        return 0U;
    }
    if (!sb_pad_ok(c->pad_dc, quad) || !sb_pad_ok(c->pad_rst, quad) ||
        !sb_pad_ok(c->pad_cs_aux, quad) || !sb_pad_ok(c->pad_bl, quad) ||
        !sb_pad_ok(c->pad_te, quad))
    {
        return 0U;
    }
    /* 辅助脚不能撞 SPI2 的固定脚（PB10~PB15 全是桥的信号线） */
    static const uint16_t reserved[] = {IOC_PAD_PB10, IOC_PAD_PB11, IOC_PAD_PB12,
                                        IOC_PAD_PB13, IOC_PAD_PB14, IOC_PAD_PB15};
    const uint8_t aux[5] = {c->pad_dc, c->pad_rst, c->pad_cs_aux, c->pad_bl, c->pad_te};
    for (uint32_t i = 0U; i < 5U; i++)
    {
        uint16_t pad = sb_pad_of(aux[i]);
        if (pad == 0U)
        {
            continue;
        }
        for (uint32_t j = 0U; j < (sizeof(reserved) / sizeof(reserved[0])); j++)
        {
            if (pad == reserved[j])
            {
                return 0U;
            }
        }
    }
    return 1U;
}

/* 辅助脚索引在这套固件里还能不能用（PB10~PB15 是 SPI2 的信号线，PA30 被板上 Q1 短到地）。
 * 另外还要问一句 I2C 桥：它使能时占着 PA28/PA29（I2C3 的 SDA/SCL），
 * 那两根在 pad 表里是 16/17 —— 两个模块抢同一根脚的后果是"谁后配谁赢"，
 * 静默且难查，所以这里直接拒掉。I2C 桥没使能时行为与以前完全一致。 */
static uint8_t sb_pad_usable(uint16_t pad)
{
    if (pad == 0U)
    {
        return 0U;
    }
    static const uint16_t reserved[] = {IOC_PAD_PB10, IOC_PAD_PB11, IOC_PAD_PB12,
                                        IOC_PAD_PB13, IOC_PAD_PB14, IOC_PAD_PB15,
                                        IOC_PAD_PA30};
    for (uint32_t i = 0U; i < (sizeof(reserved) / sizeof(reserved[0])); i++)
    {
        if (pad == reserved[i])
        {
            return 0U;
        }
    }
    if (i2c_bridge_owns_pad(pad) != 0U)
    {
        return 0U;
    }
    return 1U;
}

static void sb_apply_aux_pins(void)
{
    /*
     * ⚠️ 参数区（EasyFlash）里可能还留着**上一版固件的配置**：SPI1 时代辅助脚默认是
     * PB11/PB12/PB13（DC/RST/BL）、PB10（TE），而那几根现在已经是 SPI2 的
     * SCLK/MISO/MOSI/CS。旧配置里 sb_cfg_validate() 会拒，但"读回来的旧配置"这条路
     * 没人拦 —— 直接照它把脚配成 GPIO 就会把桥自己的信号线接管掉。
     * 所以这里按"这根 pad 现在还能不能当辅助脚"过滤一遍：不能用的一律当"不用"。
     */
    s_pad_dc = sb_pad_usable(sb_pad_of(s_cfg.pad_dc)) ? sb_pad_of(s_cfg.pad_dc) : 0U;
    s_pad_rst = sb_pad_usable(sb_pad_of(s_cfg.pad_rst)) ? sb_pad_of(s_cfg.pad_rst) : 0U;
    s_pad_bl = sb_pad_usable(sb_pad_of(s_cfg.pad_bl)) ? sb_pad_of(s_cfg.pad_bl) : 0U;
    s_pad_te = sb_pad_usable(sb_pad_of(s_cfg.pad_te)) ? sb_pad_of(s_cfg.pad_te) : 0U;

    if (s_pad_dc != 0U)
    {
        sb_pad_as_output(s_pad_dc, 0U);
    }
    if (s_pad_rst != 0U)
    {
        sb_pad_as_output(s_pad_rst, sb_line_active_low(1U) ? 1U : 0U); /* 复位无效电平 */
    }
    if (s_pad_bl != 0U)
    {
        sb_pad_as_output(s_pad_bl, sb_line_active_low(3U) ? 1U : 0U); /* 默认关背光 */
    }
    if (s_pad_te != 0U)
    {
        sb_pad_as_input(s_pad_te, 1U);
    }
}

/* 硬件落地（时钟源扫描 / SPI 复位自检 / DMA 申请 / 辅助脚）。重活：ISR 只登记
 * s_hw_req，真正执行在主循环 spi_bridge_poll() 里 —— 帧只在 poll 里执行、且
 * hw_req 的处理排在 sb_process_packets 之前，所以硬件就绪前主机塞进来的帧只会
 * 在环里排队，不会拿着没初始化的 SPI 跑。 */
static void sb_hw_apply(void)
{
    sb_spi_hw_init();
    sb_apply_aux_pins();
}

static void sb_set_enabled(uint8_t on)
{
    if (on != 0U)
    {
        if (s_enabled == 0U)
        {
            if ((s_cfg.flags & SB_CFG_F_CLEAR_ON_ENABLE) != 0U)
            {
                s_reset_req = 1U;
            }
            s_hw_req = 1U;   /* 不在 USB ISR 里做时钟扫描/SPI 复位/DMA 申请 */
        }
        s_enabled = 1U;
        sb_out_kick();
    }
    else
    {
        s_enabled = 0U;
        s_hw_req = 0U;       /* 都要关了，挂着的重初始化不必再做 */
        s_reset_req = 1U; /* 关掉时把环与状态清干净；引脚保持现状 */
    }
}

/* ============================== 初始化 ============================== */

void spi_bridge_init(void)
{
    /* .ahb_sram is NOLOAD and is not cleared by c_startup. Initialize the
     * entire state, including hw_req/DMA/format fields, before USB is started.
     * Service flags are separate BSS storage; clear them for explicit re-init. */
    memset(&s_st, 0, sizeof(s_st));
    memset(&spi_bridge_gate, 0, sizeof(spi_bridge_gate));

    s_cfg.sclk_hz = 0U; /* 0 = 板级默认 20 MHz */
    s_cfg.mode = 0U;
    s_cfg.bits = 8U;
    s_cfg.cs_policy = 0U;
    s_cfg.tx_dma_threshold = SB_DEF_DMA_THRESHOLD;
    s_cfg.pad_dc = SB_PAD_PB11;
    s_cfg.pad_rst = SB_PAD_PB12;
    s_cfg.pad_cs_aux = SB_PAD_PB10;
    s_cfg.pad_bl = SB_PAD_PB13;
    s_cfg.pad_te = SB_PAD_PB10;
    /* bit0 DC / bit1 RST / bit2 CS / bit3 BL：CS 与 RST 都是低有效（业界惯例），
     * DC 数据=高（AXS15352），BL 高=亮。不这么给的话，RST 上电就会被拉在低电平
     * （一直摁住面板复位），CS 的有效电平也说不清。 */
    s_cfg.pad_active_low = 0x06U;
    s_cfg.flags = SB_CFG_F_CLEAR_ON_ENABLE;
    s_cfg.max_frame_bytes = SB_FRAME_MAX;

    s_prof.profile = SB_PROFILE_RAW;
    s_prof.def_lines = 1U;
    s_prof.dc_active_high = 1U;
    s_prof.cs_hold_in_step = 1U;
    s_prof.qspi_wr_opcode = 0x02U;
    s_prof.qspi_color_opcode = 0x32U;
    s_prof.qspi_addr_bytes = 3U;


}

void spi_bridge_usb_ready(void)
{
    sb_out_kick();
}

void spi_bridge_usb_reset(void)
{
    /* 总线复位（重枚举/驱动重启/唤醒）：在飞的 0x0B/0x8B 传输全部作废、完成回调
     * 不会再来，所以要清账。这里只置标志，真正的清账在主循环里做（不和回调抢状态）。 */
    s_usb_reset_req = 1U;
}

uint8_t spi_bridge_is_enabled(void)
{
    return s_enabled;
}

/* 这根 pad 现在是否被本桥当成辅助脚占着（DC / RST / CS_AUX / BL / TE 任一个）。
 *
 * 给 I2C 桥用：I2C 使能要占 PA28/PA29，而这两根也在本桥的辅助脚候选表里，
 * 若先被本桥配成辅助脚，I2C 就不该再抢 —— 原来只有"SPI 问 I2C"这一个方向
 * （sb_pad_usable → i2c_bridge_owns_pad），反方向是漏的，注释里写的
 * "谁后配谁赢"在 I2C 先使能、SPI 后配的情况下成立，反过来就变成 I2C 硬抢。
 * 关桥时这些变量被清零（且 s_enabled = 0），所以"桥没开"自然返回 0。 */
uint8_t spi_bridge_owns_pad(uint16_t pad)
{
    if ((s_enabled == 0U) || (pad == 0U))
    {
        return 0U;
    }
    return ((pad == s_pad_dc) || (pad == s_pad_rst) || (pad == s_pad_bl) ||
            (pad == s_pad_te) || (pad == s_cs_pad)) ? 1U : 0U;
}

/* ============================== 上板诊断：跳线通断 ============================== */

static void sb_pin_settle(void)
{
    for (volatile uint32_t i = 0U; i < 4000U; i++)
    {
    }
}

static uint8_t sb_gpio_read(uint16_t pad)
{
    return (uint8_t)(gpio_read_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(pad), GPIO_GET_PIN_INDEX(pad)) ? 1U : 0U);
}

/*
 * 把 MOSI(PB13)/MISO(PB12) 从 SPI 复用上摘下来当普通 GPIO，直接验三件事：
 *   1) 悬空 + 下拉 / 上拉 → 读回来的电平跟得上（pad 输入通路 + 内部上下拉都好）
 *   2) 驱动 MOSI=0/1 → 读 MISO（**J3[28]↔J3[27] 那根跳线到底在不在**）
 *   3) 反向驱动 MISO → 读 MOSI（双向都验，排除只坏一个 pad）
 * 跑完把 SPI 复用装回去并复位控制器，免得影响后续事务。
 *
 * 为什么需要它：回环读回来的字节开头是一长串 0，而发送数据里没有这么长的连续 0 ——
 * 既不是相位错位（那会是固定位移的乱码），也不像噪声，最可能就是 MISO 线上没有电平。
 * 光靠 SPI 事务分不清"线没插"和"pad 没配好"，这个自检能一次分清。
 */
static uint32_t sb_pin_test(void)
{
    uint32_t r = 0U;

    HPM_IOC->PAD[IOC_PAD_PB12].FUNC_CTL = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0);
    HPM_IOC->PAD[IOC_PAD_PB13].FUNC_CTL = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0);
    sb_gpiom_to_gpio0(IOC_PAD_PB12);
    sb_gpiom_to_gpio0(IOC_PAD_PB13);

    gpio_set_pin_input(HPM_GPIO0, GPIO_GET_PORT_INDEX(IOC_PAD_PB12), GPIO_GET_PIN_INDEX(IOC_PAD_PB12));
    gpio_set_pin_output(HPM_GPIO0, GPIO_GET_PORT_INDEX(IOC_PAD_PB13), GPIO_GET_PIN_INDEX(IOC_PAD_PB13));
    gpio_write_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(IOC_PAD_PB13), GPIO_GET_PIN_INDEX(IOC_PAD_PB13), 0U);

    /* 悬空 + 下拉：应读到 0 */
    HPM_IOC->PAD[IOC_PAD_PB12].PAD_CTL = IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(0);
    sb_pin_settle();
    if (sb_gpio_read(IOC_PAD_PB12) == 0U)
    {
        r |= SB_PIN_FLOAT_PD;
    }
    /* 悬空 + 上拉：应读到 1 */
    HPM_IOC->PAD[IOC_PAD_PB12].PAD_CTL = IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1);
    sb_pin_settle();
    if (sb_gpio_read(IOC_PAD_PB12) != 0U)
    {
        r |= SB_PIN_FLOAT_PU;
    }

    /* 跳线通断：MOSI 驱动 0/1，MISO 侧用下拉（悬空读 0，只有真通了才会读到 1） */
    HPM_IOC->PAD[IOC_PAD_PB12].PAD_CTL = IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(0);
    sb_pin_settle();
    if (sb_gpio_read(IOC_PAD_PB12) == 0U)
    {
        r |= SB_PIN_MOSI_LOW;
    }
    gpio_write_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(IOC_PAD_PB13), GPIO_GET_PIN_INDEX(IOC_PAD_PB13), 1U);
    sb_pin_settle();
    if (sb_gpio_read(IOC_PAD_PB12) != 0U)
    {
        r |= SB_PIN_MOSI_HIGH;
    }

    /* 反向：驱动 MISO，MOSI 侧下拉读 */
    gpio_set_pin_output(HPM_GPIO0, GPIO_GET_PORT_INDEX(IOC_PAD_PB12), GPIO_GET_PIN_INDEX(IOC_PAD_PB12));
    gpio_set_pin_input(HPM_GPIO0, GPIO_GET_PORT_INDEX(IOC_PAD_PB13), GPIO_GET_PIN_INDEX(IOC_PAD_PB13));
    HPM_IOC->PAD[IOC_PAD_PB13].PAD_CTL = IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(0);
    gpio_write_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(IOC_PAD_PB12), GPIO_GET_PIN_INDEX(IOC_PAD_PB12), 1U);
    sb_pin_settle();
    if (sb_gpio_read(IOC_PAD_PB13) != 0U)
    {
        r |= SB_PIN_MISO_DRV;
    }

    /* 装回 SPI 复用 + 复位控制器 */
    init_spi2_bridge_pins((uint8_t)((s_prof.profile == SB_PROFILE_QSPI) ? 1U : 0U),
                          (uint8_t)((s_cfg.cs_policy == 3U) ? 1U : 0U));
    if (s_cs_pad != 0U)
    {
        sb_pad_as_output(s_cs_pad, sb_line_active_low(2U) ? 1U : 0U);
    }
    spi_reset(SB_SPI);
    (void)spi_poll_reset_complete(SB_SPI, spi_reset_all, 100000U);

    return r | SB_PIN_DONE;
}

/*
 * 在 SCLK(PB11) / CS(PB10) / MOSI(PB13) 三根脚上发**慢方波**（频率各不相同、便于分辨），
 * 纯粹验"这几根脚的焊盘和外部接线能不能动"。给 LA 用：SPI 事务看不到 SCLK 时，
 * 先用它把"控制器没出时钟"和"脚/夹子有问题"分开。
 */
static uint32_t sb_wiggle_test(void)
{
    const uint16_t pads[3] = {IOC_PAD_PB11, IOC_PAD_PB10, IOC_PAD_PB13};

    for (uint32_t i = 0U; i < 3U; i++)
    {
        HPM_IOC->PAD[pads[i]].FUNC_CTL = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0);
        HPM_IOC->PAD[pads[i]].PAD_CTL = IOC_PAD_PAD_CTL_PE_SET(0) | IOC_PAD_PAD_CTL_PS_SET(0) |
                                        IOC_PAD_PAD_CTL_OD_SET(0) | IOC_PAD_PAD_CTL_SR_SET(1) |
                                        IOC_PAD_PAD_CTL_SPD_SET(3) | IOC_PAD_PAD_CTL_DS_SET(4);
        sb_gpiom_to_gpio0(pads[i]);
        gpio_set_pin_output(HPM_GPIO0, GPIO_GET_PORT_INDEX(pads[i]), GPIO_GET_PIN_INDEX(pads[i]));
        gpio_write_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(pads[i]), GPIO_GET_PIN_INDEX(pads[i]), 0U);
    }

    for (uint32_t n = 0U; n < 100U; n++)
    {
        gpio_write_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(IOC_PAD_PB11), GPIO_GET_PIN_INDEX(IOC_PAD_PB11), n & 1U);
        gpio_write_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(IOC_PAD_PB10), GPIO_GET_PIN_INDEX(IOC_PAD_PB10),
                       (n >> 2) & 1U);
        gpio_write_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(IOC_PAD_PB13), GPIO_GET_PIN_INDEX(IOC_PAD_PB13),
                       (n >> 4) & 1U);
        for (volatile uint32_t d = 0U; d < 800U; d++)
        {
        }
    }

    for (uint32_t i = 0U; i < 3U; i++)
    {
        gpio_write_pin(HPM_GPIO0, GPIO_GET_PORT_INDEX(pads[i]), GPIO_GET_PIN_INDEX(pads[i]), 0U);
    }

    /* 装回 SPI 复用 */
    init_spi2_bridge_pins((uint8_t)((s_prof.profile == SB_PROFILE_QSPI) ? 1U : 0U),
                          (uint8_t)((s_cfg.cs_policy == 3U) ? 1U : 0U));
    if (s_cs_pad != 0U)
    {
        sb_pad_as_output(s_cs_pad, sb_line_active_low(2U) ? 1U : 0U);
    }
    spi_reset(SB_SPI);
    (void)spi_poll_reset_complete(SB_SPI, spi_reset_all, 100000U);
    return SB_PIN_DONE;
}

/* ============================== HID 0x35 ============================== */

static uint32_t sb_status_word(void)
{
    uint32_t status = 0U;
    if (s_enabled != 0U)
    {
        status |= SB_ST_ENABLED;
    }
    if (s_pkt_active != 0U)
    {
        status |= SB_ST_ACTIVE;
    }
    if (s_cs_asserted != 0U)
    {
        status |= SB_ST_CS;
    }
    if ((SB_IN_SLOTS - s_in_used) < 2U)
    {
        status |= SB_ST_IN_FLOW;
    }
    if ((SB_OUT_SLOTS - s_out_used) < 2U)
    {
        status |= SB_ST_OUT_FULL;
    }
    status |= (uint32_t)(s_last_err & 0xFFU) << SB_ST_SHIFT_ERR;
    return status;
}

void spi_bridge_hid(uint8_t *req_hid, uint8_t *res_hid)
{
    uint8_t action = req_hid[3];

    res_hid[2] = SB_HID_CMD;
    res_hid[3] = action;

    if ((s_adc_owner || s_slave_owner) && action != SB_ACT_STATUS && action != SB_ACT_GET_CFG &&
        action != SB_ACT_GET_PROFILE && action != SB_ACT_DBG) {
        wr_u32(res_hid + 4, sb_status_word() | ((uint32_t)SB_E_BUSY << SB_ST_SHIFT_ERR));
        res_hid[1] = 8U; return;
    }

    if ((bus_periodic_owns(BP_SPI) || bus_periodic_owns(BP_ADC)) && action != SB_ACT_STATUS && action != SB_ACT_GET_CFG &&
        action != SB_ACT_GET_PROFILE && action != SB_ACT_DBG && action != SB_ACT_DRAIN) {
        wr_u32(res_hid + 4, sb_status_word() | ((uint32_t)SB_E_BUSY << SB_ST_SHIFT_ERR));
        res_hid[1] = 8U;
        return;
    }

    switch (action)
    {
    case SB_ACT_STATUS:
        wr_u32(&res_hid[4], sb_status_word());
        wr_u32(&res_hid[8], s_stats.frames_ok);
        wr_u32(&res_hid[12], s_stats.bytes_tx);
        wr_u32(&res_hid[16], s_stats.bytes_rx);
        wr_u32(&res_hid[20], s_stats.tx_poll_cnt);
        wr_u32(&res_hid[24], s_stats.tx_dma_cnt);
        wr_u32(&res_hid[28], s_stats.out_ring_overrun);
        wr_u32(&res_hid[32], s_stats.in_ring_drop);
        wr_u32(&res_hid[36], s_actual_sclk);
        wr_u32(&res_hid[40], s_stats.frames_err);
        /* P2：上一笔事务耗时（mchtmr tick，24 MHz）——主机拿它对比轮询/DMA 两条路径 */
        wr_u32(&res_hid[44], s_last_ticks);
        res_hid[1] = 48U;
        break;

    case SB_ACT_ENABLE:
        sb_set_enabled((uint8_t)(req_hid[4] ? 1U : 0U));
        wr_u32(&res_hid[4], sb_status_word());
        res_hid[1] = 8U;
        break;

    case SB_ACT_RESET:
        s_reset_req = 1U;
        memset(&s_stats, 0, sizeof(s_stats));
        s_last_err = 0U;
        wr_u32(&res_hid[4], sb_status_word());
        res_hid[1] = 8U;
        break;

    case SB_ACT_SET_CFG:
    {
        sb_cfg_t c;
        memcpy(&c, &req_hid[4], sizeof(c));
        if (!sb_cfg_validate(&c))
        {
            wr_u32(&res_hid[4], sb_status_word() | ((uint32_t)SB_E_RANGE << SB_ST_SHIFT_ERR));
            res_hid[1] = 8U;
            break;
        }
        c.max_frame_bytes = SB_FRAME_MAX;
        s_cfg = c;
        if (s_enabled != 0U)
        {
            s_hw_req = 1U;   /* 硬件重初始化下沉主循环（见 sb_hw_apply） */
        }
        wr_u32(&res_hid[4], sb_status_word());
        res_hid[1] = 8U;
        break;
    }

    case SB_ACT_GET_CFG:
    {
        /*
         * 回报前先"消毒"：参数区（EasyFlash）里可能还留着上一版固件的辅助脚索引
         * （SPI1 时代默认 DC=PB11 / RST=PB12 / BL=PB13 / TE=PB10），而那几根现在
         * 是 SPI2 的 SCLK/MISO/MOSI/CS。旧值照样回报出去，主机侧的引脚图就会把
         * DC/RST 画到 SCLK/MISO 上 —— 看着"乱套"，实际是没生效的废值。
         * 这里统一按"现在还能不能当辅助脚"过滤，废值一律报 0（不用）。
         */
        sb_cfg_t out = s_cfg;
        if (!sb_pad_usable(sb_pad_of(out.pad_dc)))    { out.pad_dc = 0U; }
        if (!sb_pad_usable(sb_pad_of(out.pad_rst)))   { out.pad_rst = 0U; }
        if (!sb_pad_usable(sb_pad_of(out.pad_cs_aux))){ out.pad_cs_aux = 0U; }
        if (!sb_pad_usable(sb_pad_of(out.pad_bl)))    { out.pad_bl = 0U; }
        if (!sb_pad_usable(sb_pad_of(out.pad_te)))    { out.pad_te = 0U; }
        memcpy(&res_hid[4], &out, sizeof(out));
        res_hid[1] = (uint8_t)(4U + sizeof(out));
        break;
    }

    case SB_ACT_PIN_CFG:
    {
        uint8_t line = req_hid[4];
        uint8_t idx = req_hid[5];
        sb_cfg_t c = s_cfg;

        if (!sb_pad_ok(idx, (uint8_t)((s_prof.profile == SB_PROFILE_QSPI) ? 1U : 0U)))
        {
            wr_u32(&res_hid[4], sb_status_word() | ((uint32_t)SB_E_RANGE << SB_ST_SHIFT_ERR));
            res_hid[1] = 8U;
            break;
        }
        switch (line)
        {
        case SB_LINE_DC:
            c.pad_dc = idx;
            break;
        case SB_LINE_RST:
            c.pad_rst = idx;
            break;
        case SB_LINE_CS_AUX:
            c.pad_cs_aux = idx;
            break;
        case SB_LINE_BL:
            c.pad_bl = idx;
            break;
        case SB_LINE_TE:
            c.pad_te = idx;
            break;
        default:
            break;
        }
        if (!sb_cfg_validate(&c))
        {
            wr_u32(&res_hid[4], sb_status_word() | ((uint32_t)SB_E_RANGE << SB_ST_SHIFT_ERR));
            res_hid[1] = 8U;
            break;
        }
        s_cfg = c;
        if (s_enabled != 0U)
        {
            sb_apply_aux_pins();
        }
        wr_u32(&res_hid[4], sb_status_word());
        res_hid[1] = 8U;
        break;
    }

    case SB_ACT_DRAIN:
        s_drain_reads = (req_hid[4] > 16U) ? 16U : req_hid[4];
        wr_u32(&res_hid[4], sb_status_word());
        wr_u32(&res_hid[8], 0x314E5244U); /* DRN1: old firmware cannot claim support */
        res_hid[1] = 12U;
        break;

    case SB_ACT_ABORT:
        s_abort_req = 1U;
        wr_u32(&res_hid[4], sb_status_word());
        res_hid[1] = 8U;
        break;

    case SB_ACT_SET_PROFILE:
    {
        sb_profile_t p;
        memcpy(&p, &req_hid[4], sizeof(p));
        if (p.profile > SB_PROFILE_QSPI)
        {
            p.profile = SB_PROFILE_RAW;
        }
        if ((p.def_lines != 1U) && (p.def_lines != 2U) && (p.def_lines != 4U))
        {
            p.def_lines = 1U;
        }
        s_prof = p;
        if (!sb_cfg_validate(&s_cfg))
        {
            /* 开了 quad 之后原来的辅助脚可能落在 PA30/PA31 上：退掉它们 */
            if ((s_prof.profile == SB_PROFILE_QSPI))
            {
                if ((s_cfg.pad_dc == SB_PAD_PA30) || (s_cfg.pad_dc == SB_PAD_PA31))
                {
                    s_cfg.pad_dc = SB_PAD_NONE;
                }
                if ((s_cfg.pad_rst == SB_PAD_PA30) || (s_cfg.pad_rst == SB_PAD_PA31))
                {
                    s_cfg.pad_rst = SB_PAD_NONE;
                }
                if ((s_cfg.pad_cs_aux == SB_PAD_PA30) || (s_cfg.pad_cs_aux == SB_PAD_PA31))
                {
                    s_cfg.pad_cs_aux = SB_PAD_NONE;
                }
                if ((s_cfg.pad_bl == SB_PAD_PA30) || (s_cfg.pad_bl == SB_PAD_PA31))
                {
                    s_cfg.pad_bl = SB_PAD_NONE;
                }
                if ((s_cfg.pad_te == SB_PAD_PA30) || (s_cfg.pad_te == SB_PAD_PA31))
                {
                    s_cfg.pad_te = SB_PAD_NONE;
                }
            }
        }
        if (s_enabled != 0U)
        {
            s_hw_req = 1U;   /* 换档可能要重配引脚/时钟（quad↔单线），同样下沉主循环 */
        }
        wr_u32(&res_hid[4], sb_status_word());
        res_hid[1] = 8U;
        break;
    }

    case SB_ACT_GET_PROFILE:
        memcpy(&res_hid[4], &s_prof, sizeof(s_prof));
        res_hid[1] = (uint8_t)(4U + sizeof(s_prof));
        break;

    case SB_ACT_DBG:
        for (uint32_t i = 0U; i < 13U; i++)
        {
            wr_u32(&res_hid[4U + (i * 4U)], s_dbg[i]);
        }
        res_hid[1] = 4U + 13U * 4U;
        break;

    case SB_ACT_PINTEST:
        wr_u32(&res_hid[4], sb_pin_test());
        res_hid[1] = 8U;
        break;

    case SB_ACT_WIGGLE:
        wr_u32(&res_hid[4], sb_wiggle_test());
        res_hid[1] = 8U;
        break;

    default:
        wr_u32(&res_hid[4], sb_status_word());
        res_hid[1] = 8U;
        break;
    }
}

#endif /* BOARD_HAS_SPI_BRIDGE */
