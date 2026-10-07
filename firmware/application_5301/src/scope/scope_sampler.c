/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 akaInstruments */

/* 探针侧 HSS 采样器（J-Scope 波形页的数据源）。
 *
 * 骨架/仲裁/时钟换挡照 src/rtt/rtt_bridge.c：SWD 只在主循环里碰，绝不在 USB 中断里碰；
 * SWD 初始化复用桥那条路径（4 MHz 起手 → 20 MHz 斜坡 → 目标档 + 清 sticky + 失败降档）。
 *
 * ── 热路径（与参考草稿的差别）──────────────────────────────────────────────
 * 一次采样 = 按 span 块读。一次 span 块读的传输数是 **N+2**（TAR + prime + (N-1)×DRW
 * + RDBUFF），这已经是 AHB-AP 的理论最小 —— swd_host.c 里 CSW 与 DP_SELECT 都带缓存
 * （dap_state.csw / dap_state.select），所以第二次起写 CSW 是零传输，不是浪费。
 * 于是能省的就只剩**搬运**和**调用**：
 *
 *   1. **零拷贝**：span 内变量若首尾相接（无间隙）且起始 4 字节对齐，span 的字节序与
 *      帧内布局**逐字节相同**，直接把 SWD 读进包里该样本的槽位 —— 省掉参考草稿里
 *      "先读进 stage、再逐变量 memcpy 到 frame、再 memcpy 到包" 的两趟拷贝。
 *   2. **对齐读**：有间隙的 span 把起点向下、终点向上各对到 4 字节再读，避开
 *      swd_read_memory() 内部对未对齐头尾的逐字节慢路径。
 *   3. **不整包 memset**：只清 16 B 包头；载荷尾部那不到一帧的字节在推包时清一次
 *      （≤63 B），而不是每包无脑清 512 B。
 *
 * ⚠️ 包结构、类型编码、状态字位域必须与网页 app/scope/protocol.js 一一对应。
 */

#include <string.h>
#include "adc_stream.h"

#include "board.h"
#include "hpm_common.h"
#include "usb_composite.h"   /* usbd_ep_start_write / SWO_IN_EP / ATTR_PLACE_AT_NONCACHEABLE_BSS_* */
#include "rtt_bridge.h"      /* 复用的 SWD 原语（patch-notes §1 的 5 个 adapter） */
#include "swd_host.h"        /* swd_read_block4 / swd_read_word_hold_prepare / swd_read_word_pipe
                              * 的声明 —— 缺了它这三个调用是"隐式声明"（GCC 13 只是警告，
                              * GCC 14 起直接报错），编译产物的返回值约定全靠运气。 */
#include "riscv_jtag.h"      /* RISC-V/JTAG 后端：riscv_jtag_read / hold_prepare / hold_read */
#include "scope_sampler.h"

/* Authoritative service flags: cold gate reads words, producers write bytes. */
service_gate_t scope_sampler_gate;
#define s_bench_req (scope_sampler_gate.flag[3])
#define s_usb_reset_req (scope_sampler_gate.flag[2])
#define s_start_req (scope_sampler_gate.flag[1])
#define s_running (scope_sampler_gate.flag[0])

/* ------------------------------------------------------------------ 常量 */

#define SCOPE_MCHTMR_HZ   SCOPE_TIME_HZ          /* MCHTMR = osc24m（与 rtt_bridge.c 一致） */
#define SCOPE_HDR         16U
#define SCOPE_PAYLOAD     (SCOPE_PACKET - SCOPE_HDR)   /* 496 */

/* 变量宽度只允许 1/2/4/8（scope_var_t 的契约）。主机报文里那个 size 字节是
 * **唯一**决定 span 长度与帧内偏移的输入：不校验的后果是越界写，而且是静默的 ——
 *   · span 长度会到 256 B，而中转缓冲 s_stage 只有 SCOPE_SPAN_MAX+4 = 68 B；
 *   · 8 个宽变量的帧长可到 2016 B，直读落点会推到 512 B 包缓冲外面（最多 ~1.5 KB）。
 * 所以 configure 里整包拒绝、make_plan 里再守一道（防将来的调用方绕过）。 */
#define SCOPE_VAR_SIZE_OK(sz) (((sz) == 1U) || ((sz) == 2U) || ((sz) == 4U) || ((sz) == 8U))
#define SCOPE_ERR_BADVAR  (-6)                /* 配置被拒：变量宽度非法 */

/* 帧必须装得进一个包：宽度 ≤8 × 变量数 ≤8 ⇒ ≤64 B。宽度校验把这条变成不变量。 */
_Static_assert((SCOPE_MAX_VARS * 8U) <= SCOPE_PAYLOAD, "帧装不进一个包");
#define SCOPE_MAGIC       0x4A53U             /* 'J','S'（小端下发：53 4A） */
#define SCOPE_VER         1U
#define SCOPE_VER_TICKS   2U
#define SCOPE_KIND_DEF    1U
#define SCOPE_KIND_DATA   2U
#define SCOPE_KIND_STAT   3U
#define SCOPE_KIND_EVT    4U

#define SCOPE_MIN_PERIOD_US    2U             /* 一次次往下放过：5（"一次读 4.478 µs 做不完"）
                                              * → 3（单字快路径 2.681 µs）→ 2（流水读 1.589 µs，
                                              * 1 次传输/拍）。周期是整数 µs，所以 2 就是下限；
                                              * 再快要同时改协议的时间轴单位和 USB 那一段。 */
#define SCOPE_MAX_PERIOD_US    1000000UL
/* 合并阈值：与网页 planReads() 的成本模型同源。
 * 间隙 g 字节要多读 g×0.284 µs；拆成两个 span 则多付一次 TAR+prime+RDBUFF ≈
 * 3×1.155 µs @45 MHz → g 到 ~12 B 都还是并起来便宜。取 12（网页侧是 19，
 * 我们这边略保守；DEF 包里会回报 span 数，不一致时主机会立刻发现）。 */
#define SCOPE_MERGE_GAP        12U
#define SCOPE_STAT_EVERY       64U            /* 每多少个包插一个 STAT */
#define SCOPE_YIELD_TICKS      (SCOPE_MCHTMR_HZ / 50U) /* 20 ms 内有 DAP 活动就让路一拍 */
#define SCOPE_BENCH_MAX_ITERS  100000UL

/* 传输后端（见文件后半的 scope_be_* 分派） */
#define SCOPE_BE_SWD    0U
#define SCOPE_BE_RISCV  1U

/* ------------------------------------------------------------------ 状态 */

static volatile int8_t  s_start_rc = -100;    /* -100 = 还没启动过（与 RTT 桥同一哨兵） */
static uint8_t  s_swd_ready;
static uint8_t  s_backend;                    /* 0 = SWD/ARM、1 = RISC-V/JTAG（见 scope_be_* 分派） */

static scope_var_t s_var[SCOPE_MAX_VARS];     /* 按地址升序 */
static uint8_t  s_nvars;
static uint16_t s_frame_bytes;
static uint8_t  s_per_packet;                 /* floor(496 / frame_bytes) */
static uint16_t s_frame_off[SCOPE_MAX_VARS];  /* 每个变量在帧内的偏移 */
static uint32_t s_period_us = 100U;
static uint32_t s_time_step = 100U; /* v1: µs；v2: 24 MHz ticks，热路径只做加法 */
static uint8_t s_time_version = SCOPE_VER;
static uint8_t  s_flags;

typedef struct
{
    uint32_t start;      /* 读起点（已向下对齐到 4 字节） */
    uint16_t len;        /* 读长度（已向上对齐到 4 字节） */
    uint16_t frame_off;  /* 本 span 第一个变量在帧内的偏移 */
    uint8_t  first;      /* 变量表下标 */
    uint8_t  count;
    uint8_t  direct;     /* 1 = span 字节序与帧布局逐字节相同 → 直接读进包里（零拷贝） */
} scope_span_t;

static scope_span_t s_span[SCOPE_MAX_VARS];
static uint8_t  s_nspans;

/* 只有非零拷贝的 span 才用得到它；8 个 f64 = 64 B，再留 4 B 给对齐补头 */
static uint8_t  s_stage[SCOPE_SPAN_MAX + 4U];

/* 🚨 给 USB DMA 用的缓冲必须在**非 cacheable** 段，否则 CPU 写进 D-cache 而 DMA
 *    直接读内存 → 主机收到旧数据。uart_rx_buf 也是这么放的。 */
ATTR_PLACE_AT_NONCACHEABLE_BSS_WITH_ALIGNMENT(4)
static uint8_t s_pkt[SCOPE_TX_BUFS][SCOPE_PACKET];
static volatile uint8_t s_tx_busy[SCOPE_TX_BUFS];
static uint8_t  s_inflight[SCOPE_TX_BUFS];    /* 发送队列（FIFO）：回调不带下标，只能按序还 */
static uint8_t  s_if_head, s_if_count;
static uint8_t  s_tx_active;                  /* 端点上有 1 笔在飞（DWC2 只认一笔，见 scope_tx_kick） */
static uint8_t  s_tx_gen;                     /* 队列代数：START / USB 复位清账时 +1 */
static uint8_t  s_tx_armed_gen;               /* 当前在飞那一笔属于哪一代 */
static uint8_t  s_fill_buf;                   /* 正在填的缓冲；0xFF = 还没分配 */
static uint16_t s_fill_n;                     /* 已经填了几个样本 */
static uint32_t s_fill_t0;                    /* 本包第一个样本的 t_us */

static uint32_t s_seq;
static uint32_t s_t_time;                       /* 名义时间轴：v1 µs / v2 ticks，跳拍照样推进 */
static uint32_t s_next_tick;                  /* 下一次该采样的 MCHTMR 时刻 */

static uint32_t s_produced;                   /* 采到的样本数（含没推出去的） */
static uint32_t s_dropped;                    /* 追不上而跳掉的拍数（= 丢的样本数） */
static uint32_t s_usb_drop;                   /* 因为没空闲包缓冲而丢掉的样本数 */
static uint32_t s_swd_err;                    /* 读失败次数（那一拍作废） */
static uint32_t s_yield;                      /* 给 DAP 让路的次数 */
static uint32_t s_pkts;                       /* 推出去的包数 */
static uint32_t s_bytes;                      /* 推出去的字节数 */
static uint32_t s_discard_pkts;
static uint32_t s_last_cmd, s_last_rsp;
static uint32_t s_clock_hz;
static uint32_t s_last_sample_ticks;          /* 最近一次采样的实际耗时（标称 vs 实际） */
static uint32_t s_tx_done;                    /* USB 完成回调次数（诊断包缓冲为何耗尽） */
      /* USB 总线复位：ISR 置标志、主循环清账 */

static uint32_t s_bench_valid, s_bench_iters, s_bench_ticks;
static int32_t  s_bench_err;
static uint32_t s_hdr_t;                      /* 正在组的那一包的时间戳（DATA 用首样本时刻） */
static uint32_t s_period_ticks = 1U;          /* s_period_us 换算好的 MCHTMR tick 数（configure 时算一次） */
static uint8_t  s_cdc_suspended;              /* 是我们因为 SCOPE_FLAG_CDC_OFF 关掉 CDC 桥的吗 */
static uint8_t  s_pipe_ok;                    /* 本配置能用单字流水读（只有一个 4 字节直读 span）*/
static uint8_t *s_pipe_dst;                   /* 等待回填的样本槽位；NULL = 管线空，下次读的结果要丢 */

static void scope_push_stat(void);
static int  scope_pipe_flush(void);

/* MCHTMR 低 32 位（24 MHz → 约 179 s 绕回一次；这里只算"到点没有"，差值运算天然安全） */
static uint32_t mchtmr_now(void)
{
    return *(volatile uint32_t *)(HPM_MCHTMR_BASE + 0x00);
}

static uint32_t us_to_ticks(uint32_t us)
{
    return (uint32_t)(((uint64_t)us * SCOPE_MCHTMR_HZ) / 1000000ULL);
}

/* ------------------------------------------------------------------ 计划 */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* 这个 span 能不能**零拷贝**直读进帧里？条件是"span 的字节序与帧内布局逐字节相同"，
 * 必须**逐个变量验**，不能只看起始地址对齐 —— 结构体里的填充字节会让两者错位：
 *   g_pack: i_saw(u8)@0x11 → u_hi(u32)@0x14（中间 2 B 填充）
 *   帧内紧凑排：i_saw@17 → u_hi@18          ← 差 2 字节
 * 只验对齐的话这里就会把 u_hi 写到 2 字节之前的错误位置。
 * 所以两条都要满足：内存里首尾相接，且帧内偏移同步递增。 */
static int scope_span_is_direct(const scope_span_t *sp)
{
    uint32_t a = s_var[sp->first].addr;
    uint16_t f = s_frame_off[sp->first];

    if ((a & 3U) != 0U) { return 0; }          /* 起点不对齐会走逐字节慢路径 */

    for (uint8_t k = 0U; k < sp->count; k++)
    {
        uint8_t vi = (uint8_t)(sp->first + k);
        if (s_var[vi].addr != a) { return 0; }        /* 内存里不连续 */
        if (s_frame_off[vi] != f) { return 0; }       /* 帧里不同步 */
        a += s_var[vi].size;
        f = (uint16_t)(f + s_var[vi].size);
    }
    return (((a - s_var[sp->first].addr) & 3U) == 0U) ? 1 : 0;
}

/* 排读计划：变量按地址排序 → 间隙 ≤ SCOPE_MERGE_GAP 的并进同一个 span。
 * 与网页 planReads() 同源，两边的 span 数应当一致（DEF 包里回报，主机对账）。 */
static void scope_make_plan(void)
{
    /* 🚨 硬守卫（冷路径，配置时跑一次）：宽度非法就**不排计划** —— s_nspans/s_frame_bytes
     * 留 0，于是 scope_start_now()/scope_run_bench() 都会以 -3 退出，一次读都不会发。
     * 正常路径在 scope_sampler_configure() 已经整包拒绝过了，这里防的是"将来新增的
     * 调用方绕过那道校验"：一旦排出去，span 长度会超过 s_stage 与包缓冲，越界写是静默的。 */
    for (uint8_t i = 0U; i < s_nvars; i++)
    {
        if (!SCOPE_VAR_SIZE_OK(s_var[i].size))
        {
            s_nspans = 0U;
            s_frame_bytes = 0U;
            s_per_packet = 0U;
            return;
        }
    }

    /* 插入排序：只有 ≤8 个元素，不值得引 qsort */
    for (uint8_t i = 1U; i < s_nvars; i++)
    {
        scope_var_t v = s_var[i];
        int8_t j = (int8_t)i - 1;
        while ((j >= 0) && (s_var[j].addr > v.addr)) { s_var[j + 1] = s_var[j]; j--; }
        s_var[j + 1] = v;
    }

    /* 帧内偏移按**排序后**的顺序累加（主机解码也按这个顺序） */
    uint16_t off = 0U;
    for (uint8_t i = 0U; i < s_nvars; i++)
    {
        s_frame_off[i] = off;
        off = (uint16_t)(off + s_var[i].size);
    }
    s_frame_bytes = off;
    s_per_packet = (uint8_t)((s_frame_bytes > 0U) ? (SCOPE_PAYLOAD / s_frame_bytes) : 0U);
    if (s_per_packet == 0U) { s_per_packet = 1U; }

    s_nspans = 0U;
    for (uint8_t i = 0U; i < s_nvars; i++)
    {
        uint32_t end = s_var[i].addr + s_var[i].size;
        if ((s_nspans > 0U) &&
            ((s_var[i].addr - (s_span[s_nspans - 1U].start + s_span[s_nspans - 1U].len)) <= SCOPE_MERGE_GAP) &&
            ((end - s_span[s_nspans - 1U].start) <= SCOPE_SPAN_MAX))
        {
            scope_span_t *sp = &s_span[s_nspans - 1U];
            sp->len = (uint16_t)(end - sp->start);
            sp->count++;
        }
        else
        {
            scope_span_t *sp = &s_span[s_nspans];
            sp->start = s_var[i].addr;
            sp->len = (uint16_t)s_var[i].size;
            sp->frame_off = s_frame_off[i];
            sp->first = i;
            sp->count = 1U;
            s_nspans++;
        }
    }

    /* 每个 span 定下"怎么读"：先判零拷贝（用未扩边的原始范围），
     * 再把起点向下、终点向上各扩到 4 字节 —— 只是多读几个字节，不改语义。
     * 对零拷贝的 span 这一步是空操作（它们本来就两端对齐）。 */
    s_pipe_ok = 0U;
    for (uint8_t i = 0U; i < s_nspans; i++)
    {
        scope_span_t *sp = &s_span[i];

        sp->direct = (uint8_t)scope_span_is_direct(sp);

        uint32_t aligned = sp->start & ~3U;
        uint32_t aend = (sp->start + sp->len + 3U) & ~3U;
        sp->start = aligned;
        sp->len = (uint16_t)(aend - aligned);
    }

    /* 单字流水读的前提：**只有一个 span**，而且它是 4 字节直读。
     *
     * 🚨 守卫必须是 s_nspans == 1，不能只是"所有 span 都是单字"：两个远离的单字
     * span 交替读时每拍都要重写 TAR，而那条路的 TAR 写带一次 RDBUFF 收尾（2 次传输），
     * 比 swd_read_block 的裸 TAR 写（1 次）还贵 —— 实测 8.6 µs 变成 11.2 µs（-31%）。
     * `--set two` 就是守这条的用例。 */
    if ((s_nspans == 1U) && (s_span[0].direct != 0U) && (s_span[0].len == 4U))
    {
        s_pipe_ok = 1U;
    }
}

uint32_t scope_sampler_plan_hash(void)
{
    /* FNV-1a，与网页 app/scope/protocol.js 的 planHash() 逐字节一致 */
    uint32_t h = 0x811C9DC5UL;
    for (uint8_t i = 0U; i < s_nvars; i++)
    {
        const uint8_t bytes[6] = {
            (uint8_t)(s_var[i].addr), (uint8_t)(s_var[i].addr >> 8),
            (uint8_t)(s_var[i].addr >> 16), (uint8_t)(s_var[i].addr >> 24),
            s_var[i].size, s_var[i].type,
        };
        for (uint8_t k = 0U; k < 6U; k++) { h ^= bytes[k]; h *= 0x01000193UL; }
    }
    return h;
}

/* ------------------------------------------------------------------ 组包 */

/* 只清 16 B 包头。载荷里没被本样本覆盖的尾部字节由推包时统一清一次（见 scope_push_packet） */
static uint8_t *scope_pkt_header(uint8_t kind, uint16_t n, uint16_t aux)
{
    uint8_t *p = s_pkt[s_fill_buf];
    put16(p, SCOPE_MAGIC);
    p[2] = s_time_version;
    p[3] = kind;
    put32(p + 4U, s_seq);
    put32(p + 8U, s_hdr_t);          /* DATA 包这里是**首个**样本的时刻（主机按它建时间轴） */
    put16(p + 12U, n);
    put16(p + 14U, aux);
    return p;
}

/* 还回一个空闲缓冲；没有就返回 0xFF。
 * 🚨 必须跳过**正在填的那个**：它在推出去之前不算 busy，否则会把自己交出去覆盖掉。 */
static uint8_t scope_alloc_buf(void)
{
    for (uint8_t i = 0U; i < SCOPE_TX_BUFS; i++)
    {
        if (!s_tx_busy[i] && (i != s_fill_buf)) { return i; }
    }
    return 0xFFU;
}

/* ---- 发送队列 ---------------------------------------------------------------
 *
 * 🚨 端点**忙**的时候 usbd_ep_start_write 照样返回 0：移植层把底层
 *    `usb_device_edpt_xfer()` 的 bool 丢掉了（usb_dc_hpm.c:254），而 DWC2 对一个
 *    已经有在飞传输的端点会直接拒绝这一笔 —— 于是"发成功"是假的：不会有完成回调，
 *    这个包缓冲永远回不来（= 二轮审查的 N1）。实测（8 通道 200 µs 档跑 3 s）：
 *    推出 1005 包 / 主机收到 999 / 完成回调恰好也是 999 —— 差的 6 个全是 STAT，
 *    它们的缓冲永久占死，池子几秒内从 8 个缩到 2 个（之后主机稍一卡就丢样本）。
 *
 * 所以必须自己排队：**同一时刻只允许 1 笔在飞**（s_tx_active），其余按 seq 顺序
 * 躺在 s_inflight 环里，由完成回调接着踢下一包。"查在飞 + 启动"走关中断 ——
 * 入队（主循环）与出队（回调 ISR）都会进这里，两步之间被打断就会重复启动、
 * 又把缓冲漏掉。同一套推理与写法见 spi_bridge.c 的 sb_out_kick()。 */

static uint32_t scope_irq_save(void)
{
    return disable_global_irq(CSR_MSTATUS_MIE_MASK);
}

static void scope_irq_restore(uint32_t lvl)
{
    enable_global_irq(lvl);
}

static void scope_tx_kick(void)
{
    uint32_t lvl = scope_irq_save();

    if ((s_tx_active == 0U) && (s_if_count != 0U))
    {
        uint8_t buf = s_inflight[s_if_head];

        if (usbd_ep_start_write(0, SWO_IN_EP, s_pkt[buf], SCOPE_PACKET) == 0)
        {
            s_tx_active = 1U;
            s_tx_armed_gen = s_tx_gen;
        }
    }
    scope_irq_restore(lvl);
}

/* 入队一包（内容已经填好）。返回 1 = 已入队（由 kick 决定什么时候真的发出去）。
 * 返回 0 只可能是"队列里已经有 8 个缓冲"——而 buf 是刚从空闲池里拿的，凑不满，
 * 所以那是理论分支，调用方按"这一包没出去"如实记账即可。 */
static uint8_t scope_tx_enqueue(uint8_t buf)
{
    uint32_t lvl = scope_irq_save();
    uint8_t ok = 0U;

    if (s_if_count < SCOPE_TX_BUFS)
    {
        s_tx_busy[buf] = 1U;
        s_inflight[(s_if_head + s_if_count) % SCOPE_TX_BUFS] = buf;
        s_if_count++;
        ok = 1U;
    }
    scope_irq_restore(lvl);

    if (ok != 0U) { scope_tx_kick(); }
    return ok;
}

/* 把当前填满的包交给 USB */
static void scope_push_packet(void)
{
    s_hdr_t = s_fill_t0;                       /* DATA 包的时间戳 = 首样本时刻 */
    (void)scope_pkt_header(SCOPE_KIND_DATA, s_fill_n, (uint16_t)(s_fill_n * s_frame_bytes));

    /* 载荷尾部没用到的字节清一次（≤ 一帧，最多 63 B）——
     * 比每包 memset 512 B 便宜一个数量级，同时保证坏包/越界解析看到的是 0。 */
    uint16_t used = (uint16_t)(s_fill_n * s_frame_bytes);
    if (used < SCOPE_PAYLOAD)
    {
        memset(&s_pkt[s_fill_buf][SCOPE_HDR + used], 0, (size_t)(SCOPE_PAYLOAD - used));
    }

    s_seq++;
    s_pkts++;
    s_bytes += SCOPE_PACKET;

    if (s_flags & SCOPE_FLAG_DISCARD)
    {
        s_discard_pkts++;
    }
    else if (scope_tx_enqueue(s_fill_buf) == 0U)
    {
        /* 队列满（理论不可达，见 scope_tx_enqueue）：这一包的样本确实没出去，如实计数 */
        s_usb_drop += (uint32_t)s_fill_n;
    }

    /* 下一包。**拿不到就置 0xFF（"没有缓冲"），绝不退回某个固定下标** ——
     * 早先这里是 `s_fill_buf = 0`，而 0 号很可能正在飞：往在飞的缓冲里写数据，
     * 再对它调一次 usbd_ep_start_write 会因为端点忙而**静默不启动**（既不发也不回调），
     * 于是 s_if_count 只增不减、缓冲永远还不回来 —— 实测 txDone=912/1136 而
     * usbDrop 却等于"每包一次"（1133 次），打包率掉到 1/4，还丢了 5 个 seq。
     * 现在没缓冲就丢拍并如实计数，等回调还回来（见 scope_sampler_tx_complete）。 */
    s_fill_buf = scope_alloc_buf();
    s_fill_n = 0U;

    if ((s_pkts % SCOPE_STAT_EVERY) == 0U) { scope_push_stat(); }
}

/* DEF：变量表（主机先拿它建类型表，再按顺序解码 DATA） */
static void scope_push_def(void)
{
    uint8_t save = s_fill_buf;
    uint8_t buf = scope_alloc_buf();
    if (buf == 0xFFU) { return; }
    s_fill_buf = buf;
    s_hdr_t = s_t_time;
    uint8_t *p = scope_pkt_header(SCOPE_KIND_DEF, 0U, s_nvars);
    uint8_t *q = p + SCOPE_HDR;
    put32(q, s_clock_hz);
    put32(q + 4U, s_time_step);
    put16(q + 8U, (uint16_t)(s_flags |
                             ((s_backend == SCOPE_BE_RISCV) ? SCOPE_FLAG_RISCV : 0U)));
    q[10] = s_nvars;
    q[11] = s_nspans;                     /* 主机拿它跟自己的计划对账 */
    uint8_t *e = q + 12U;
    for (uint8_t i = 0U; i < s_nvars; i++)
    {
        put32(e, s_var[i].addr);
        e[4] = s_var[i].size;
        e[5] = s_var[i].type;
        e += 8U;
    }
    s_seq++;
    s_pkts++;
    s_bytes += SCOPE_PACKET;
    if (!(s_flags & SCOPE_FLAG_DISCARD))
    {
        /* 入发送队列（DEF/STAT 不带样本，排队失败就丢掉这一包，不计 usb_drop） */
        (void)scope_tx_enqueue(buf);
    }
    s_fill_buf = save;
}

static void scope_push_stat(void)
{
    uint8_t save = s_fill_buf;
    uint8_t buf = scope_alloc_buf();
    if (buf == 0xFFU) { return; }
    s_fill_buf = buf;
    s_hdr_t = s_t_time;
    uint8_t *p = scope_pkt_header(SCOPE_KIND_STAT, 0U, 0U);
    uint8_t *q = p + SCOPE_HDR;
    put32(q, s_produced);
    put32(q + 4U, s_dropped + s_usb_drop);
    put32(q + 8U, s_pkts);
    put16(q + 12U, (uint16_t)(s_usb_drop & 0xFFFFU));
    put16(q + 14U, (uint16_t)(s_swd_err & 0xFFFFU));
    put32(q + 16U, s_time_step);
    q[20] = (uint8_t)(s_clock_hz / 1000000UL);   /* 当前 SWD 档位（MHz） */
    q[21] = (uint8_t)((s_flags & SCOPE_FLAG_DISCARD) ? 1U : 0U);
    s_seq++;
    s_pkts++;
    s_bytes += SCOPE_PACKET;
    if (!(s_flags & SCOPE_FLAG_DISCARD))
    {
        /* 同上（scope_push_def）：入队，失败就丢这一包。 */
        (void)scope_tx_enqueue(buf);
    }
    s_fill_buf = save;
}

/* ------------------------------------------------------------------ 采样 */

/* ---- 批量读路径 ----------------------------------------------------------
 * 逐字走 `swd_host.c` 时，每次传输要付 ~138 个 CPU 周期（swd_transfer_retry 的重试
 * 循环 → SWD_Transfer 的端口判断 → SWD_Read 的头部计算 → 函数指针）—— 实测块读的
 * 渐近成本是 1.166 µs/字（70.8 个 SWD 时钟 @60 MHz，理想只要 47），且**不随块长摊薄**，
 * 说明它是"每次传输"而不是"每块"的开销，占一个样本的 ~30%。
 *
 * 这里改用 CMSIS-DAP 引擎自己的 `DAP_TransferBlock`：它一次调用就完成
 *     post AP read（prime） → N 次 DRW 读 → 自动把最后一次换成 DP RDBUFF
 * 循环体里只剩重试判断 + SWD_Read + 存 4 字节，每次传输的开销掉到 ~35 周期。
 * TAR 还是得每次采样写一遍（自增把它推走了），用一条 `DAP_Transfer` 写。
 *
 * 传输数不变（N+2），省的是 C 层。⚠️ DAP 引擎**不会写 CSW**（那是 swd_host 的活），
 * 所以启动时先用一次普通读把 CSW 落到硬件上，见 scope_start_now。 */
#define SCOPE_REQ_TAR_WRITE 0x05U   /* APnDP | ((AP_TAR 0x04) >> 2) << 2 */
#define SCOPE_REQ_DRW_READ  0x0FU   /* APnDP | RnW | ((AP_DRW 0x0C) >> 2) << 2 */

static uint8_t s_req_tar[8];                    /* {ID_DAP_Transfer, idx, cnt, req, addr[4]} */
static uint8_t s_req_blk[5];                    /* {ID_DAP_TransferBlock, idx, cnt_lo, cnt_hi, req} */
static uint8_t s_rsp_dummy[8];
/* 🚨 响应布局实测（别照 CMSIS-DAP 文档推）：
 *   DAP_Transfer       -> [ID][count][resp]
 *   DAP_TransferBlock  -> [ID][cnt_lo][cnt_hi][resp][data...]
 * 即 DAP_ProcessCommand 会把**命令 ID 回显在 response[0]**，payload 从 [1] 起。
 * 少算这一个字节就会把 resp 当数据、把数据当 resp —— 表现为"块读永远不 OK"。 */
#define SCOPE_RSP_HDR   4U                      /* ID + cnt_lo + cnt_hi + resp */
static uint8_t s_rsp_blk[SCOPE_RSP_HDR + SCOPE_SPAN_MAX];

static void scope_req_init(void)
{
    s_req_tar[0] = 0x05U;                       /* ID_DAP_Transfer */
    s_req_tar[1] = 0U;                          /* DAP index */
    s_req_tar[2] = 1U;                          /* 1 次传输 */
    s_req_tar[3] = SCOPE_REQ_TAR_WRITE;

    s_req_blk[0] = 0x06U;                       /* ID_DAP_TransferBlock */
    s_req_blk[1] = 0U;                          /* DAP index */
    s_req_blk[4] = SCOPE_REQ_DRW_READ;
}

/* ---- 传输后端分派（SWD/ARM 与 RISC-V/JTAG）----------------------------------
 *
 * 采样逻辑（计划、帧布局、组包、丢拍统计）与目标无关，**只有这 4 个原语不同**：
 *   link_ready   —— 把链路拉起来（SWD：斜坡换挡 + 一次验收读；RISC-V：开 TAP/DM）
 *   block_read   —— 一段连续内存读进缓冲（两边都支持未对齐头尾）
 *   hold_prepare —— 把读**抱在固定地址**上，幂等（每拍调也没关系）
 *   pipe_read    —— 收上一拍投出去的读结果 + 投下一拍（两边都是一拍延迟）
 * 热路径上每次调用多一个可预测的分支，实测 <1%（DISCARD 基准对照）。
 *
 * 后端选择：跟随**全局目标类型**（HID CMD_RTT action 10，与 RTT 桥同一个开关），
 * 或者由 HID 0x32 配置报文里的 SCOPE_FLAG_RISCV 位强制指定。 */
static uint8_t scope_be_block_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    if (s_backend == SCOPE_BE_RISCV)
    {
        return (uint8_t)(riscv_jtag_read(addr, dst, len) == 0);   /* 内含按块重试 */
    }
    return (uint8_t)(swd_read_block4(addr, dst, len) != 0U);
}

static uint8_t scope_be_hold_prepare(uint32_t addr)
{
    if (s_backend == SCOPE_BE_RISCV)
    {
        return (uint8_t)(riscv_jtag_hold_prepare(addr) == 0);
    }
    return swd_read_word_hold_prepare(addr);
}

static uint8_t scope_be_pipe_read(uint32_t *val)
{
    if (s_backend == SCOPE_BE_RISCV)
    {
        return (uint8_t)(riscv_jtag_hold_read(val) == 0);
    }
    return swd_read_word_pipe(val);
}

static int scope_be_link_ready(void);        /* 见文件后半（依赖 span/stage 的验收读） */

/* 采一个 span，数据落在 s_stage。
 *
 * ⚠️ 这里**曾经**改成走 CMSIS-DAP 引擎的 `DAP_TransferBlock`（一次调用完成
 * prime + N×DRW + 自动收尾 RDBUFF），指望省掉 `swd_host` 的 C 调用链开销。
 * **实测反而慢 6.5%**（60 MHz：12.31 µs vs 11.55 µs），原因见下 —— 代码留着做对照，
 * 但不要启用。
 *
 * 为什么省不下来：把一个传输拆开量过，420 个 CPU 周期里 **282 是 47 bit × 6 周期**
 * 的协议下限（60M blob 的包头/数据相位都是 6 条指令/bit，已经是手工优化的），
 * 剩下 ~138 周期在 ACK/转向相位与 GPIO 总线延迟上 —— 那部分**每次传输都逃不掉**，
 * 跟外面包了几层 C 函数无关。所以唯一有效的方向是**减少传输次数**，不是优化调用链。 */
static int scope_read_span(const scope_span_t *sp)
{
    /* 直接走对齐块读的快速路径：span 的起点/长度在 scope_make_plan() 里已经扩到
     * 4 字节对齐，也不会跨 1 KB 自增页（SCOPE_SPAN_MAX=64），所以能跳过
     * rtt_bridge_read → rtt_read_bytes 的分块循环和 swd_read_memory 的头尾/分页处理。 */
    return scope_be_block_read(sp->start, s_stage, sp->len) ? 0 : -1;
}

/* 按变量宽度搬字节。
 * 🚨 别用 memcpy()：1/2/4 字节的 memcpy 会**真的走一次函数调用**，而一个样本有 8 个
 *    变量 —— 实测这一项就是框架开销里的大头（纯传输 10.30 µs vs 带框架 11.55 µs）。 */
static inline void scope_copy_var(uint8_t *dst, const uint8_t *src, uint8_t size)
{
    switch (size)
    {
    case 1U: dst[0] = src[0]; break;
    case 2U: dst[0] = src[0]; dst[1] = src[1]; break;
    case 4U: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3]; break;
    default: memcpy(dst, src, size); break;      /* f64 */
    }
}

/* 采一拍，把变量字节写进"本样本在包里的槽位"。
 * 返回 0 = 成功；-1 = 某个 span 读失败（这一拍作废，计入 swd_err）。 */
static int scope_sample_bytes(uint8_t *dst)
{
    /* Single-word plans have one aligned direct span. Avoid the generic span loop. */
    if (s_pipe_ok)
    {
        uint32_t v;
        if (!scope_be_hold_prepare(s_span[0].start) || !scope_be_pipe_read(&v)) { return -1; }
        if (s_pipe_dst != NULL) { put32(s_pipe_dst, v); }
        s_pipe_dst = dst;
        return 0;
    }
    for (uint8_t sp = 0U; sp < s_nspans; sp++)
    {
        const scope_span_t *s = &s_span[sp];
        uint8_t *fdst = dst + s->frame_off;

        if ((s->direct != 0U) && (((uint32_t)(uintptr_t)fdst & 3U) == 0U))
        {
            /* **零拷贝**：span 的字节序 == 帧内布局，且落点 4 字节对齐 —— 直接把 SWD
             * 读进包里的槽位，连 s_stage 那趟中转都省了。单变量时这是每样本省一次
             * memcpy（小 memcpy 会真的走函数调用）。落点不对齐就退回下面。 */
            if (s_pipe_ok)
            {
                /* 单字流水读：**每拍只发一次 DRW 读**，而它返回的是上一次 DRW 读的结果
                 * （AHB-AP 的读是 posted 的）。于是把拿到的值回填给**上一拍自己的槽位**
                 * —— 时间戳、帧布局、包边界全都不用动，只是写入晚了一拍。
                 * 包里最后一拍的值由 scope_pipe_flush() 在推包前收回来。 */
                uint32_t v;
                if (scope_be_hold_prepare(s->start) == 0U) { return -1; }
                if (scope_be_pipe_read(&v) == 0U) { return -1; }
                if (s_pipe_dst != NULL) { put32(s_pipe_dst, v); }
                s_pipe_dst = fdst;
            }
            else if (scope_be_block_read(s->start, fdst, s->len) == 0U) { return -1; }
            continue;
        }

        if (scope_read_span(s) != 0) { return -1; }

        if (s->direct != 0U)
        {
            /* span 的字节序 == 帧内布局：整段搬（24 B 就是 6 个字），这条路仍用 memcpy
             * 划算 —— 一次 24 B 的调用比 6 次展开赋值还省 */
            memcpy(fdst, s_stage, s->len);
        }
        else
        {
            for (uint8_t k = 0U; k < s->count; k++)
            {
                uint8_t vi = (uint8_t)(s->first + k);
                uint32_t src = (s_var[vi].addr - s->start);
                scope_copy_var(fdst + (s_frame_off[vi] - s->frame_off), &s_stage[src],
                               s_var[vi].size);
            }
        }
    }
    return 0;
}

static int scope_sample_once(void)
{
    /* 没有空闲包缓冲：丢这一拍并如实计数（**绝不写进在飞的缓冲**）。
     * 缓冲由 USB 完成回调还给采样器，见 scope_sampler_tx_complete。 */
    if (s_fill_buf >= SCOPE_TX_BUFS)
    {
        s_usb_drop++;
        s_t_time += s_time_step;             /* 时间轴照常推进：主机看到的是"有洞"，不是"被压缩" */
        /* 掉拍会把流水线撕开一个口子：下一次读回来的值属于**掉拍之前**那一拍，
         * 而那一拍的槽位可能已经在飞的包里了 —— 写进去就是篡改已提交的包。
         * 清空管线，让下一拍把它的读结果丢掉、重新起链（丢一拍的值，符合这里的语义）。
         * 不变量：s_fill_buf 变成"没有缓冲"只可能发生在推包之后，而推包前一定会 flush
         * 把 s_pipe_dst 清成 NULL —— 所以这里通常本来就是 NULL，这行是保险。 */
        s_pipe_dst = NULL;
        return 0;
    }

    /* 🚨 热路径上**故意不带诊断计时**：每次采样多一次 mchtmr_now()（volatile 读）+
     * 减法，在 200 kHz 这个量级上是实打实的开销。s_last_sample_ticks 只在标定
     * （scope_run_bench）里更新，那里不在乎这点开销。 */
    uint8_t *dst = &s_pkt[s_fill_buf][SCOPE_HDR + ((uint32_t)s_fill_n * s_frame_bytes)];

    /* 读失败：管线状态已经不可信（那次 DRW 可能发出去了也可能没发），清空它 ——
     * 下一拍读回来的值会被丢掉、重新起链。不这么做的话，这一拍的值会被写进
     * 上一拍的槽位，而上一拍的值永远收不回来。 */
    if (scope_sample_bytes(dst) != 0) { s_pipe_dst = NULL; return -1; }

    if (s_fill_n == 0U) { s_fill_t0 = s_t_time; }
    s_fill_n++;
    s_produced++;
    s_t_time += s_time_step;                 /* 名义时间轴：跳拍也要推进，否则主机的轴会压缩 */

    if (s_fill_n >= s_per_packet)
    {
        /* 单字流水：包满了，最后一拍的值还押在管线里（要等下一次 DRW 读才回来），
         * 所以推包前先补一次读把它收回来 —— 每包一次，摊到每拍是 1/124 次传输。
         * 收不回来也照推：那一拍的值是旧的，但**绝不能把"包已满"这个状态留着**，
         * 否则下一拍的 dst 会算到帧区外面去。 */
        int frc = scope_pipe_flush();
        scope_push_packet();
        if (frc != 0) { return -1; }
    }
    return 0;
}

/* 把管线里押着的那一拍的值收回来（一次 DRW 读）。收完管线清空：下一次读回来的值
 * 对应的是"补读那一刻"，不属于任何一拍，由调用方丢掉。0 = ok。 */
static int scope_pipe_flush(void)
{
    uint32_t v;

    if (s_pipe_dst == NULL) { return 0; }
    if (scope_be_pipe_read(&v) == 0U) { s_pipe_dst = NULL; return -1; }
    put32(s_pipe_dst, v);
    s_pipe_dst = NULL;
    return 0;
}

/* 用指定后端把链路拉起来。0 = ok，其它 = 后端初始化码。 */
static int scope_link_try(uint8_t be)
{
    s_backend = be;

    if (s_backend == SCOPE_BE_RISCV)
    {
        /* RISC-V：TAP + DM 打开一次就一直开着（同一个引擎 CMD_RISCV 与 RTT 桥也在用），
         * 所以只有"没开"时才真的去开。**不动 delay**：它的默认 8 是实测出来的安全值
         * （idle < 6 时 HPM6880 的 DTM 会不应答），要改走 CMD_RISCV 的 config。 */
        if (s_swd_ready && riscv_jtag_is_open()) { return 0; }
        s_swd_ready = 0U;

        int rc = riscv_jtag_open();
        if (rc != 0) { return -2; }

        /* 验收读：链路真的读得动才算 ready（与 SWD 那条同一个约定） */
        if (riscv_jtag_read(s_span[0].start, s_stage, 4U) != 0) { return -4; }

        s_swd_ready = 1U;
        return 0;
    }

    /* 🚨 必须问桥那一侧的链路状态，不能只看自己这份 s_swd_ready：主机碰过 DAP
     * （rtt_bridge_note_dap_activity）、桥 stop 过、或换过 SWD 档位，都会把桥那份
     * 清掉，而这份还是 1 —— 于是既不重新初始化、也拿不到新装的时钟 blob，
     * 表现就是"改了频率没反应"甚至"换挡后第一次访问 -4"。 */
    if (s_swd_ready && rtt_bridge_swd_is_ready()) { return 0; }
    s_swd_ready = 0U;

    int rc = rtt_bridge_swd_ensure_ready();      /* 复用桥的 SWD 初始化（含斜坡换挡） */
    if (rc != 0) { return rc; }

    scope_req_init();                            /* 批量路径的请求模板（当前作为对照保留） */

    /* 先做一次真实读：既是"链路真的读得动"的验收，也把目标 AP 的 CSW 落到硬件上
     * （32 位自增）—— 逐字路径自己会写 CSW，但如果以后重新启用批量路径，那次写是不做的。
     *
     * 🚨 失败要**重新初始化**再读一次，不能只重试：换挡后的第一次 AP 访问会瞬态失败
     * （实测 60 MHz 档约 13% 的 START 踩到、45 MHz 及以下 0/301；同一档位稳定跑起来
     * 之后不再出现），而光重试没用 —— 失败后 0/2/5/10/20/50/100 ms 连探 18/18 全失败
     * （清 sticky 也不够），只有重走 rtt_swd_init() 才恢复。桥那边的 CB 扫描/bench
     * 早就是"清错 + 重试"的写法，这里漏了，于是瞬态直接冒成 -4 把整轮编排打断。 */
    if (rtt_bridge_read(s_span[0].start, s_stage, 4U) != 0)
    {
        if ((rtt_bridge_link_recover() != 0) ||
            (rtt_bridge_read(s_span[0].start, s_stage, 4U) != 0))
        {
            s_swd_ready = 0U;
            return -4;
        }
        /* 恢复路径可能降了一档：状态字要报**实际**生效的档位，不然网页会以为还是原档。 */
        s_clock_hz = rtt_bridge_swd_clock_hz();
    }

    s_swd_ready = 1U;
    return 0;
}

/* RISC-V 后端的"就绪"必须交叉校验引擎还在不在：s_swd_ready 是采样器自己记的，
 * 引擎可能已经被主机关掉（CMD_RISCV stop、selfcheck 的收尾清理）—— 只看自己
 * 这份会拿着死链路直接进采样循环（实测：stop 之后 bench 稳定 -4，且不会自愈）。
 * SWD 侧对应的是 scope_link_try 里 rtt_bridge_swd_is_ready() 那道交叉检查。 */
static void scope_be_recheck(void)
{
    if ((s_backend == SCOPE_BE_RISCV) && s_swd_ready && (riscv_jtag_is_open() == 0))
    {
        s_swd_ready = 0U;
    }
}

/* 把链路准备好（含批量路径需要的那一次 CSW 落地）。0 = ok，其它 = 后端初始化码。
 *
 * 后端不匹配时**允许换一条路再试一次**：全局目标类型是"粘"的（上次采过 RISC-V 的板子，
 * 这次采 ARM 就会撞上），而网页的波形页未必有目标类型开关。只有在主机用 flags bit6
 * **明确强制**了 RISC-V 时才不换路（那时报错更诚实）。 */
static int scope_be_link_ready(void)
{
    if (s_nspans == 0U) { scope_make_plan(); }
    scope_be_recheck();

    int rc = scope_link_try(s_backend);
    if ((rc == 0) || ((s_flags & SCOPE_FLAG_RISCV) != 0U)) { return rc; }

    int rc2 = scope_link_try((s_backend == SCOPE_BE_SWD) ? SCOPE_BE_RISCV : SCOPE_BE_SWD);
    if (rc2 == 0) { return 0; }

    /* 两条都不行：报**主机选的那条**的错（更贴近它的意图），但后端留在这条上。 */
    s_backend = (uint8_t)((s_flags & SCOPE_FLAG_RISCV) ? SCOPE_BE_RISCV : SCOPE_BE_SWD);
    return rc;
}

static int scope_start_now(void)
{
    if (adc_stream_enabled()) return -14;
    if ((s_nvars == 0U) || (s_frame_bytes == 0U)) { return -3; }
    scope_make_plan();

    int rc = scope_be_link_ready();
    if (rc != 0) { s_swd_ready = 0U; return rc; }

    s_seq = 0U; s_t_time = 0U; s_produced = 0U; s_dropped = 0U; s_usb_drop = 0U;
    s_swd_err = 0U; s_yield = 0U; s_pkts = 0U; s_bytes = 0U; s_discard_pkts = 0U;
    s_last_sample_ticks = 0U;
    s_if_head = 0U; s_if_count = 0U;
    memset((void *)s_tx_busy, 0, sizeof(s_tx_busy));
    /* 队列代数 +1：上一轮遗留的在飞传输（主机不读时它可能还挂着）即使回调迟到，
     * 也不会来动这一轮的队列。**不动 s_tx_active** —— 那一笔是真的在飞，
     * 必须等它的回调把端点交还（踢早了会被 DWC2 静默拒绝，又漏缓冲）。 */
    s_tx_gen++;
    s_fill_buf = scope_alloc_buf();
    s_fill_n = 0U;
    s_fill_t0 = 0U;
    s_pipe_dst = NULL;                           /* 单字流水从空管线开始 */

    scope_push_def();                            /* 先发变量表 */
    s_next_tick = mchtmr_now() + s_period_ticks;
    s_running = 1U;

    /* SCOPE_FLAG_CDC_OFF：采样期间把主循环里的 CDC/串口桥让出去。
     * 只记"是我们关的"，停采样时只恢复自己关过的那一次 —— 免得把主机
     * 用 HID 0x34 手动关掉的状态也给"恢复"了。 */
    if ((s_flags & SCOPE_FLAG_CDC_OFF) && chry_dap_usb2uart_is_enabled())
    {
        chry_dap_usb2uart_set_enabled(0U);
        s_cdc_suspended = 1U;
    }
    return 0;
}

/* 标定：用当前计划空跑 iters 次，量真实的 µs/样本（M0 那一步的答案）。
 * 🚨 要先自己把链路拉起来 —— 网页的「标定真实速率」是在**启动推流之前**点的。 */
static void scope_run_bench(void)
{
    /* Never measure into a USB-owned packet. A stopped stream can still have
     * all packet slots queued, and M0 does not need a TX buffer at all. */
    if (s_running)
    {
        s_bench_err = -5;
        s_bench_valid = 1U;
        return;
    }
    if (s_nspans == 0U)
    {
        s_bench_err = -3;
        s_bench_valid = 1U;
        return;
    }
    /* s_swd_ready 可能是引擎被主机关掉之前的陈旧值（见 scope_be_recheck）——
     * 不校验的话这里会直接跳过链路初始化、拿死链路采样，稳定 -4。 */
    scope_be_recheck();
    if (!s_swd_ready)
    {
        int rc = scope_be_link_ready();
        if (rc != 0) { s_bench_err = rc; s_bench_valid = 1U; return; }
    }

    uint8_t dst[SCOPE_MAX_VARS * 8U] __attribute__((aligned(4)));
    s_pipe_dst = NULL;

    uint32_t t0 = mchtmr_now();
    int32_t err = 0;

    for (uint32_t i = 0U; (err == 0) && (i < s_bench_iters); i++)
    {
        if (scope_sample_bytes(dst) != 0) { err = -4; }
    }
    s_bench_ticks = mchtmr_now() - t0;
    s_pipe_dst = NULL; /* The posted-read destination must not outlive this stack. */
    s_bench_err = err;
    s_bench_valid = 1U;
    /* 热路径上不再逐拍测这个值（见 scope_sample_once 的说明），标定时补一次 */
    s_last_sample_ticks = (s_bench_iters != 0U) ? (s_bench_ticks / s_bench_iters) : 0U;
}

/* USB 总线复位后的清账（在主循环里做，避免和正在推包的状态抢）。
 *
 * 复位会把所有在飞的 bulk IN 传输一并作废 —— 它们**不会有完成回调**，于是
 * scope_sampler_tx_complete 永远等不到，8 个包缓冲就被"记成在飞"占死：表现为
 * 重枚举之后 usb_drop 狂涨，只能 STOP/START（甚至拔插）才恢复。 */
static void scope_usb_reset_apply(void)
{
    /* 管线里可能还押着一拍读的结果：先交付回它该在的样本槽，再清账。 */
    if (s_pipe_dst != NULL) { (void)scope_pipe_flush(); }

    for (uint8_t i = 0U; i < SCOPE_TX_BUFS; i++)
    {
        s_tx_busy[i] = 0U;
        s_inflight[i] = 0U;
    }
    s_if_head = 0U;
    s_if_count = 0U;
    /* 在飞那一笔随总线复位一起作废（回调不会来），代数 +1 让任何迟到的回调
     * 都不会去动已经被清空的队列。 */
    s_tx_active = 0U;
    s_tx_gen++;

    /* 正在填的那个缓冲按设计不会是"在飞"的，内容还作数；只有本来就是"没有缓冲"
     * （0xFF 哨兵）时才需要重新要一个。 */
    if (s_fill_buf >= SCOPE_TX_BUFS)
    {
        s_fill_buf = scope_alloc_buf();
        s_fill_n = 0U;
    }
    s_pipe_dst = NULL;
}

void scope_sampler_usb_reset(void)
{
    s_usb_reset_req = 1U;
}

void scope_sampler_poll(void)
{
    if (s_usb_reset_req)
    {
        s_usb_reset_req = 0U;
        scope_usb_reset_apply();
    }
    if (s_start_req)
    {
        s_start_req = 0U;
        s_start_rc = (int8_t)scope_start_now();
    }
    if (s_bench_req)
    {
        s_bench_req = 0U;
        scope_run_bench();
    }

    if (!s_running) { return; }

    uint32_t now = mchtmr_now();
    if ((int32_t)(now - s_next_tick) < 0) { return; }       /* 还没到点 */

    /* 让路：最近 SCOPE_YIELD_TICKS 内有 DAP 命令 → 跳过这一拍（周期会豁一个口，
     * 但不会把正在调试的会话打断）。要绝对稳的周期就设 SCOPE_FLAG_NO_YIELD。 */
    if (!(s_flags & SCOPE_FLAG_NO_YIELD))
    {
        uint32_t last_dap = rtt_bridge_last_dap_ticks();
        if ((last_dap != 0U) && ((uint32_t)(now - last_dap) < SCOPE_YIELD_TICKS))
        {
            s_yield++;
            s_next_tick = now + s_period_ticks;
            return;
        }
    }

    /* Keep interrupts enabled. Only amortize main-loop services for an explicitly
     * enabled, bounded single-word session; slower/multi-span/JTAG plans stay scalar.
     * STOP/configuration/reset requests interrupt the deadline wait. Only the 2 us
     * mode keeps its remaining budget across packet boundaries: an extra main-loop
     * visit there costs samples even when reads and USB throughput have headroom. */
    uint8_t batched = ((s_flags & SCOPE_FLAG_FAST_BATCH) && s_pipe_ok &&
                       s_backend == SCOPE_BE_SWD && s_period_ticks <= 72U);
    uint8_t tight = batched && s_period_ticks <= 48U;
    uint8_t budget = batched ? (tight ? 64U : 16U) : 1U;
    while (budget-- != 0U)
    {
        if (scope_sample_once() != 0)
        {
            s_swd_err++;
            s_next_tick = now + s_period_ticks;
            /* RISC-V 引擎被主机关掉时自愈重连（recheck 只在引擎确实关了时才清标志，
             * SWD 的瞬态读错误不受影响）；不这么做的话每个 tick 都空转报错。 */
            scope_be_recheck();
            if (!s_swd_ready)
            {
                (void)scope_be_link_ready();
            }
            return;
        }

        /* 追不上就跳拍：把 next_tick 推到将来，并把**真正跳过**的整拍数计入 dropped。
         * 🚨 这里必须只算整拍：早先写成 `(now-next)/period + 1`，于是"晚 1 个 tick（42 ns）"
         *    也被记成丢了 1 拍 —— 实测 10 kHz 采样下 produced=31222、dropped=31174，
         *    界面上会显示成丢了一半，而 seq 缺口是 0、实际速率也正好 10 kHz。
         *    现在用 while 逐拍推进，只有 now 真的越过了下一拍的时刻才算丢。 */
        s_next_tick += s_period_ticks;
        while ((int32_t)(now - s_next_tick) > 0)
        {
            s_next_tick += s_period_ticks;
            s_dropped++;
            s_t_time += s_time_step;
        }
        if (budget == 0U || s_fill_buf >= SCOPE_TX_BUFS || (!tight && s_fill_n == 0U)) { return; }
        do
        {
            if (!s_running || s_usb_reset_req || s_start_req || s_bench_req) { return; }
            now = mchtmr_now();
        } while ((int32_t)(now - s_next_tick) < 0);
    }

}

/* ------------------------------------------------------------------ 控制面 */

/* 停采样，并把"是因为采样才关掉的"那一次 CDC/串口桥还回去。
 * 两条路走它：① 运行中改配置（半新半旧地跑要不得）；② 配置被拒（整包拒绝时也得停）。
 * 不动 s_start_rc —— 由调用方决定报什么码。 */
static void scope_stop_for_reconfig(void)
{
    s_running = 0U;
    if (s_cdc_suspended)
    {
        s_cdc_suspended = 0U;
        chry_dap_usb2uart_set_enabled(1U);
    }
}

static int scope_configure(uint32_t period, uint8_t version, uint8_t flags, uint8_t nvars, const scope_var_t *vars)
{
    if (nvars > SCOPE_MAX_VARS) { nvars = SCOPE_MAX_VARS; }

    /* 🚨 主机输入面：变量宽度必须合法（见 SCOPE_VAR_SIZE_OK 的说明）。不合法**整包拒绝**
     * —— 不采纳、不建计划、把变量表清空（半解析的 plan 更难查），并把返回码经
     * res[2]（start_result）与状态字 10 报给主机：-6 = 配置被拒。
     * 之后主机再发 START 会拿到 -3（变量表为空），不会拿着旧计划偷偷跑。 */
    for (uint8_t i = 0U; i < nvars; i++)
    {
        if (!SCOPE_VAR_SIZE_OK(vars[i].size))
        {
            scope_stop_for_reconfig();
            s_nvars = 0U;
            s_nspans = 0U;
            s_frame_bytes = 0U;
            s_per_packet = 0U;
            s_start_rc = (int8_t)SCOPE_ERR_BADVAR;
            s_last_cmd = 7U;
            return SCOPE_ERR_BADVAR;
        }
    }

    /* 必须先停止旧采样，再安装新单位/周期/变量表。 */
    scope_stop_for_reconfig();
    uint32_t scale = (version == SCOPE_VER_TICKS) ? (SCOPE_TIME_HZ / 1000000UL) : 1U;
    if (period < SCOPE_MIN_PERIOD_US * scale) { period = SCOPE_MIN_PERIOD_US * scale; }
    if (period > SCOPE_MAX_PERIOD_US * scale) { period = SCOPE_MAX_PERIOD_US * scale; }
    s_time_version = version;
    s_time_step = period;
    s_period_us = (period + scale - 1U) / scale; /* 仅旧状态显示用，采样不取这个近似值 */
    s_period_ticks = (version == SCOPE_VER_TICKS) ? period : us_to_ticks(period);
    s_flags = flags;
    s_nvars = nvars;
    for (uint8_t i = 0U; i < nvars; i++) { s_var[i] = vars[i]; }

    /* 后端选择：强制位优先，否则**跟随全局目标类型**（HID CMD_RTT action 10，
     * 与 RTT 桥同一个开关 —— 这样网页不做任何改动也能采 RISC-V）。
     * 换后端必须把"链路已就绪"清掉，逼下一次重新初始化。 */
    {
        uint8_t be = ((flags & SCOPE_FLAG_RISCV) != 0U) ? SCOPE_BE_RISCV :
                     (rtt_bridge_target_is_riscv() ? SCOPE_BE_RISCV : SCOPE_BE_SWD);
        if (be != s_backend)
        {
            s_backend = be;
            s_swd_ready = 0U;
        }
    }

    /* 计划要先排出来：标定（action 8）不启动也能跑，而且状态字里要报 span 数 */
    scope_make_plan();

    /* clock_delay 覆盖：SWD 空闲拍那一截（每条 AP 读约 52 个时钟里有 6 拍是它）。
     * 只在明确要求时改；否则走"请求档位"那条路（见 rtt_bridge_request_swd_clock）——
     * 它保证下次用链路时会重新初始化并按新档装载 blob。
     * RISC-V 侧没有"时钟档"这回事（JTAG 时序由 DMI 汇编里的旋钮 + delay 决定），
     * 所以这一整段只对 SWD 后端有意义。 */
    if (s_backend == SCOPE_BE_SWD)
    {
        if (flags & SCOPE_FLAG_DELAY0)
        {
            DAP_Data.clock_delay = 0U;
        }
        else
        {
            rtt_bridge_request_swd_clock(s_clock_hz ? s_clock_hz : rtt_bridge_swd_clock_hz());
            s_swd_ready = 0U;
        }
    }

    if (s_running)
    {
        /* 运行中改配置：停掉再等主机启动 —— 免得半新半旧地跑（周期/变量表混用） */
        scope_stop_for_reconfig();
    }
    s_last_cmd = 7U;

    /* 配置动作的应答里那个返回码（res[2] / 状态字 10）报的就是**本次配置的判定**：
     * 0 = 已采纳、-6 = 被拒。不写这一行的话它会停在上一次的启动结果上（例如 -3），
     * 主机读到的就是陈旧值 —— 验收脚本里当场抓到过。 */
    s_start_rc = 0;
    return 0;
}

int scope_sampler_configure(uint32_t period_us, uint8_t flags, uint8_t nvars, const scope_var_t *vars)
{
    return scope_configure(period_us, SCOPE_VER, flags, nvars, vars);
}

int scope_sampler_configure_ticks(uint32_t period_ticks, uint8_t flags, uint8_t nvars, const scope_var_t *vars)
{
    return scope_configure(period_ticks, SCOPE_VER_TICKS, flags, nvars, vars);
}

void scope_sampler_set_clock(uint32_t hz)
{
    if (hz == 0U) { return; }
    /* 走"请求 + 下次重新初始化"，不要直接在旧档上调 set_swd_clock()：
     * 链路没起来时那只是把值记下来，硬件里还是旧 blob，而状态字会报新频率。 */
    rtt_bridge_request_swd_clock(hz);
    s_swd_ready = 0U;
    s_clock_hz = rtt_bridge_swd_clock_hz();
    s_last_cmd = 3U;
}

void scope_sampler_request_start(void)
{
    s_start_req = 1U;
    s_start_rc = -100;                 /* 排队中：与 RTT 桥的约定一致 */
}

int scope_sampler_start_result(void)
{
    return (int)s_start_rc;
}

void scope_sampler_stop(void)
{
    s_running = 0U;
    s_start_req = 0U;
    s_start_rc = -100;
    s_last_cmd = 0U;

    /* 只在"是我们关的"那一次恢复（见 scope_start_now 的说明） */
    if (s_cdc_suspended)
    {
        s_cdc_suspended = 0U;
        chry_dap_usb2uart_set_enabled(1U);
    }
}

int scope_sampler_is_running(void)
{
    return (int)s_running;
}

uint32_t scope_sampler_span_count(void)
{
    return (uint32_t)s_nspans;
}

uint32_t scope_sampler_last_sample_ticks(void)
{
    return s_last_sample_ticks;
}

void scope_sampler_request_bench(uint32_t iters)
{
    if (iters == 0U) { iters = 1000U; }
    if (iters > SCOPE_BENCH_MAX_ITERS) { iters = SCOPE_BENCH_MAX_ITERS; }
    s_bench_iters = iters;
    s_bench_valid = 0U;
    s_bench_req = 1U;
}

int scope_sampler_bench_result(uint32_t *iters, uint32_t *ticks, int32_t *err)
{
    if (!s_bench_valid) { return 0; }
    if (iters) { *iters = s_bench_iters; }
    if (ticks) { *ticks = s_bench_ticks; }
    if (err)   { *err = s_bench_err; }
    return 1;
}

void scope_sampler_tx_complete(void)
{
    s_tx_done++;

    /* 一笔传完：端点空了，接着把队列里的下一包踢出去。
     * 跨代的那笔（清账之前发出去的）**不还缓冲** —— 队列已经被清、缓冲已经被回收，
     * 再去 pop 只会把新队列里的某一包误还掉（spi_bridge 的 armed_gen 同一个道理）。 */
    uint32_t lvl = scope_irq_save();
    s_tx_active = 0U;
    if ((s_tx_armed_gen == s_tx_gen) && (s_if_count != 0U))
    {
        uint8_t idx = s_inflight[s_if_head];
        s_if_head = (uint8_t)((s_if_head + 1U) % SCOPE_TX_BUFS);
        s_if_count--;
        if (idx < SCOPE_TX_BUFS) { s_tx_busy[idx] = 0U; }
    }
    scope_irq_restore(lvl);

    /* 刚还回来的缓冲如果正是"缺的那个"，立刻接手继续填 —— 不然会一直丢拍到下一次推包 */
    if ((s_fill_buf >= SCOPE_TX_BUFS) && s_running)
    {
        s_fill_buf = scope_alloc_buf();
        s_fill_n = 0U;
    }

    scope_tx_kick();
}

/* 12 个状态字，位域见 docs/scope-page.md §7.1（网页 parseScopeStatus 按同一张表解） */
uint32_t scope_sampler_status(uint32_t *out, uint32_t words)
{
    if (words < 12U) { return 0U; }
    out[0] = (uint32_t)(s_running ? 1U : 0U) |
             (1U << 2) | /* 支持 action 10 / v2 tick 时间轴 */
             (1U << 3) | /* 支持 flags bit7 短批次 */
             (1U << 4) | /* 支持 action 11 完整计数快照 */
             ((uint32_t)((s_backend == SCOPE_BE_RISCV) ? 1U : 0U) << 1) |   /* bit1 = 生效后端是 RISC-V */
             ((uint32_t)s_nspans << 8) |
             ((uint32_t)(s_swd_ready ? 1U : 0U) << 16) |
             ((uint32_t)s_nvars << 24);
    out[1] = s_clock_hz;
    out[2] = s_produced;
    out[3] = s_dropped + s_usb_drop;
    out[4] = (s_bytes & 0xFFFFU) | ((s_usb_drop & 0xFFFFU) << 16);
    out[5] = (s_swd_err & 0xFFFFU) | ((s_yield & 0xFFFFU) << 16);
    out[6] = s_seq;
    out[7] = (s_dropped & 0xFFFFU) | ((s_discard_pkts & 0xFFFFU) << 16);
    out[8] = scope_sampler_plan_hash();
    out[9] = (s_last_cmd & 0xFFU) | ((s_last_rsp & 0xFFU) << 8) | ((s_tx_done & 0xFFFFU) << 16);
    out[10] = (uint32_t)(int32_t)s_start_rc;
    out[11] = (s_time_step & 0xFFFFU) |
              ((uint32_t)(s_time_version == SCOPE_VER_TICKS) << 17) |
              ((uint32_t)((s_flags & SCOPE_FLAG_DISCARD) ? 1U : 0U) << 16) |
              ((uint32_t)(s_clock_hz / 1000000UL) << 24);
    return 12U;
}

/* Control-plane only: snapshot timer and counters in one short critical section.
 * No diagnostic timer reads are added to the per-sample path. */
uint32_t scope_sampler_metrics(uint32_t *out, uint32_t words)
{
    if (words < 12U) { return 0U; }
    uint32_t lvl = scope_irq_save();
    out[0] = 0x31535348U; /* HSS1 */
    out[1] = mchtmr_now();
    out[2] = SCOPE_TIME_HZ;
    out[3] = s_produced;
    out[4] = s_dropped;
    out[5] = s_usb_drop;
    out[6] = s_swd_err;
    out[7] = s_yield;
    out[8] = s_tx_done;
    out[9] = s_bytes;
    out[10] = s_period_ticks;
    out[11] = (uint32_t)s_flags | ((uint32_t)s_running << 8);
    scope_irq_restore(lvl);
    return 12U;
}
