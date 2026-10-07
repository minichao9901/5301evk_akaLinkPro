/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 akaInstruments */

#ifndef __SCOPE_SAMPLER_H__
#define __SCOPE_SAMPLER_H__

#include <stdint.h>
#include "service_gate.h"
extern service_gate_t scope_sampler_gate;
static inline uint8_t scope_sampler_needs_service(void) { return scope_sampler_gate.word[0] != 0U; }


/* 探针侧 HSS 采样（给网页「J-Scope 波形」页用）。
 *
 * 目标固件**一行都不用改**：探针自己按设定的周期，用 SWD 去读目标 RAM 里那 1~8 个
 * 变量（地址来自目标 .elf 的 DWARF），组好 512 B 自描述包，从 **interface 0 上那个
 * 原本闲置的 bulk IN 端点 0x83**（SWO 端点，SWO_STREAM=0 所以一直没人写过）推给主机。
 *
 * 与 RTT 桥的关系：**互斥**（两者都要独占 SWD，且都只在主循环里碰链路）。
 * 见 api_param.c 里两边的 start 分支互相 stop()。
 *
 * 协议（HID 0x32 控制面 + 0x83 数据面）与网页侧实现：见另一仓库
 * web-serial-rtt-tools 的 docs/scope-page.md。字节布局改一边必须改另一边。
 */

#define SCOPE_TIME_HZ      24000000UL /* v2 时间戳和周期单位：MCHTMR tick */

#define SCOPE_MAX_VARS   8U     /* 8 个变量正好装进一条 63 B 的 HID 配置报文 */
#define SCOPE_PACKET     512U   /* HS bulk 的 wMaxPacketSize（也是 DAP_PACKET_SIZE） */
#define SCOPE_TX_BUFS    8U     /* 在飞的包缓冲数。512 B × 8 = 4 KB（DLM 现在很紧，别再加）。
                                 * 它是"探针能容忍主机多慢"的唯一缓冲：主机（尤其是串行读的
                                 * 调试脚本）两次下发之间的间隔一旦超过 BUFS×每包耗时，就开始
                                 * 丢拍。实测 4 个时 50 kHz 下丢 8.3%，8 个基本吃平。 */
#define SCOPE_SPAN_MAX   64U    /* 单个 span 的读缓冲上限：8 个 f64 = 64 B */

/* flags（HID 0x32 action 7 的第 5 字节） */
#define SCOPE_FLAG_ALLOW_60M 0x01U  /* 允许 60 MHz 档（长跑稳不稳取决于线材，见文档） */
#define SCOPE_FLAG_DISCARD   0x02U  /* 丢弃模式：照常采样但不推 USB（隔离 SWD 与 USB 两段） */
#define SCOPE_FLAG_TRIGGER   0x04U  /* 探针侧触发（v2；当前主机侧触发已够用） */
#define SCOPE_FLAG_NO_YIELD  0x08U  /* 不让路：独占链路，周期更稳，但会打断并发的调试会话 */
#define SCOPE_FLAG_DELAY0    0x10U  /* SWD 空闲拍压到 0（DAP_Data.clock_delay=0）。
                                     * 一次 AP 读约 52 个 SWD 时钟 = 8 头 + 1 转向 + 3 ACK
                                     * + 33 数据 + 1 转向 + idle，idle 是**可按线材缩短**的
                                     * 那一截；大块读时被摊薄看不出来，小周期读正好敏感。 */
#define SCOPE_FLAG_CDC_OFF   0x20U  /* 采样期间自动暂停主循环里的 CDC/串口桥，停采样时自动恢复。
                                     * 主循环每轮省下几百个 CPU 周期（两次关中断 + 三次环形缓冲
                                     * 查询 + 一次 DMA 寄存器读 + 按键轮询），实测单变量 u32
                                     * 5 us 周期：端到端 167 kHz → 195 kHz。
                                     * 代价：采样期间 COM 口不通（数据走另一条 bulk IN 0x83，
                                     * 不受影响），RTT-over-USB 也会停 —— 但两者本来就互斥。
                                     * 手动总开关见 HID 0x34 CMD_BRIDGE。 */
/* 目标类型：RISC-V/JTAG（HPM6800EVK 这类）。不带这一位时**跟随全局目标类型**
 * （HID CMD_RTT action 10，与 RTT 桥同一个开关）；带上就强制本会话用 RISC-V 后端。
 * DEF 包里回报的是**生效值**，所以主机能据此判断走的是哪条路。 */
#define SCOPE_FLAG_RISCV     0x40U
#define SCOPE_FLAG_FAST_BATCH 0x80U /* 单字 SWD、<=3 us：通常 16 拍；2 us 档为 64 拍并跨包继续 */

typedef struct
{
    uint32_t addr;      /* 目标 RAM 地址 */
    uint8_t  size;      /* 1 / 2 / 4 / 8 */
    uint8_t  type;      /* 0=u8 1=i8 2=u16 3=i16 4=u32 5=i32 6=f32 7=f64（与网页同一张表） */
    uint16_t rsv;
} scope_var_t;

/* HID 0x32 action 7：周期 + 变量表（地址排序由本模块自己做，主机只管给表）。
 * 返回 0 = 已采纳；-6 = 整包拒绝（变量宽度不是 1/2/4/8，见 scope_sampler.c 的说明）。
 * 被拒时**不采纳任何字段**、变量表清空；判定经 res[2] / 状态字 10 回报（0 或 -6）。 */
int scope_sampler_configure(uint32_t period_us, uint8_t flags, uint8_t nvars, const scope_var_t *vars);
/* HID action 10：周期以 24 MHz tick 配置，发 v2 包（时间戳/DEF/STAT 周期均为 tick）。 */
int scope_sampler_configure_ticks(uint32_t period_ticks, uint8_t flags, uint8_t nvars, const scope_var_t *vars);

/* HID 0x32 action 3：设 SWD 时钟（Hz，走 RTT 桥那套斜坡换挡）。0 = 不动。 */
void scope_sampler_set_clock(uint32_t hz);

/* HID 0x32 action 1：只登记请求，真正的 SWD 初始化在主循环里做（见 scope_sampler_poll）。 */
void scope_sampler_request_start(void);

/* 最近一次启动的返回码：0 正常 / -1 SWJ_Clock / -2 SWD 初始化 / -3 变量表为空 /
 * -4 该档链路不可用 / -6 配置被拒（变量宽度非法）/ -100 = 排队中（哨兵值，不是错误 ——
 * 与 RTT 桥同一约定）。-5 = 标定没有空闲包缓冲，只在 bench 结果里出现。 */
int  scope_sampler_start_result(void);

/* HID 0x32 action 0 */
void scope_sampler_stop(void);

/* 主循环里调（紧跟 rtt_bridge_poll()）：处理排队的启动/标定请求、到点采一拍、把满包推给 USB。 */
void scope_sampler_poll(void);

/* 状态字（12 个 u32，位域见 docs/scope-page.md §7.1）。返回写入的字数。 */
uint32_t scope_sampler_status(uint32_t *out, uint32_t words);
/* action 11: atomic timer/full-width counters, layout in docs/hss-tick-protocol.md */
uint32_t scope_sampler_metrics(uint32_t *out, uint32_t words);

/* HID 0x32 action 8/9：用当前计划空跑 iters 次，回报 MCHTMR ticks（24 MHz）。
 * 这是 M0 标定：拿到真实的 µs/样本，而不是模型估算。 */
void scope_sampler_request_bench(uint32_t iters);
int  scope_sampler_bench_result(uint32_t *iters, uint32_t *ticks, int32_t *err);

/* USB 完成回调（在 swo_in_callback 里调）：还回一个包缓冲。 */
void scope_sampler_tx_complete(void);

/* USB 总线复位（重枚举 / 驱动重启 / 睡眠唤醒）时调：在飞的 0x83 传输全被作废、
 * 完成回调不会再来，所以要把包缓冲的账一次清干净（真正的清账在主循环里做）。 */
void scope_sampler_usb_reset(void);

int      scope_sampler_is_running(void);
uint32_t scope_sampler_plan_hash(void);   /* 与网页 planHash 同一算法，用来对账"配置生效了吗" */

/* 探针按自己的合并规则算出的 span 数（DEF 包里上报，主机拿它跟本地计划对账） */
uint32_t scope_sampler_span_count(void);

/* 最近一次采样实际用时（MCHTMR ticks），给主机显示"标称 vs 实际" */
uint32_t scope_sampler_last_sample_ticks(void);

#endif /* __SCOPE_SAMPLER_H__ */
