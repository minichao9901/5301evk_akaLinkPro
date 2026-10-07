/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 akaInstruments */

/* Probe-side SEGGER RTT bridge.
 *
 * Everything the host would normally do over CMSIS-DAP (read the control
 * block, drain the ring, write RdOff back) is done here, in-process, by
 * building the same DAP requests a host would send and feeding them straight
 * into DAP_ExecuteCommand(). That reuses the pin muxing, SWD clock, ACK
 * handling and WAIT retries of the DAP engine - only the USB round trips
 * disappear. Drained bytes land in the CDC ringbuffer, which the existing
 * CDC IN path already forwards to the host.
 *
 * Arbitration: the SWD bus belongs to the DAP. A poll only runs when no DAP
 * command has been executed for RTT_BRIDGE_DAP_IDLE_MS, and each poll is
 * bounded (a few hundred bytes), so a debug session sees at most ~1 ms of
 * extra latency and never a corrupted transfer.
 */

#include <string.h>
#include "adc_stream.h"

#include "board.h"
#include "hpm_common.h"
/* usb_composite.h pulls in DAP_config.h, which is what defines
 * __STATIC_FORCEINLINE and the DAP_SWD/DAP_JTAG switches DAP.h relies on. */
#include "usb_composite.h"
#include "DAP.h"
#include "cdc_interface.h"
#include "rtt_bridge.h"
#include "riscv_jtag.h"
#include "swd_host.h"   /* ARM DAPLink 官方 SWD 访问层（见 src/swd_host/） */
#include "hpm_clock_drv.h" /* clock_cpu_delay_ms() */

/* Authoritative service flags: cold gate reads words, producers write bytes. */
service_gate_t rtt_bridge_gate;
#define s_bench_pending (rtt_bridge_gate.flag[4])
#define s_raw_pending (rtt_bridge_gate.flag[3])
#define s_stop_pending (rtt_bridge_gate.flag[2])
#define s_start_pending (rtt_bridge_gate.flag[1])
#define s_running (rtt_bridge_gate.flag[0])

/* Debug port / AHB-AP 的寄存器与传输编码由官方 swd_host.c / debug_cm.h 负责，
 * 这里只留桥自己的配置常量。 */

/* 桥默认的 SWD 时钟档。
 *
 * 60 MHz 档现在能用了（斜坡换挡 + 换挡后热身 + 失败先清 sticky 错误），纯读 3312 KB/s、
 * 交付 2954 KB/s，比 45 MHz 快 17% —— **但它在长跑里不稳**：实测 3 次 x10 秒里有一次
 * 出现 rd_err=10 / wr_err=4 / 2 次重扫，并伴随约 1400 字节的流异常；45 MHz 同样条件
 * 3/3 次 0 错误。既然要求"全程零丢包"，默认取 45 MHz；想要极限速度可以用 HID CMD_RTT
 * action 7 切到 60（此时下面的自动降档会在链路抖动时把它自己退回稳的档）。
 *
 * 启动时若这一档用不了，会沿 45→36→30→20→10 自动往下找。 */
#define RTT_SWD_CLOCK_HZ 45000000UL

/* 累计多少次重扫（读失败触发的重新搜索）就自动降一档。60 MHz 那种"能跑但偶尔抖动"
 * 的档靠这个自己退回稳的档，而不是一直丢数据。 */
#define RTT_RESCANS_PER_STEP_DOWN 2U

/* SEGGER RTT layout (SEGGER_RTT.h): CB header 24 bytes, then per up-buffer
 * {name, pBuffer, SizeOfBuffer, WrOff, RdOff, Flags} = 24 bytes. */
#define RTT_CB_UP_OFFSET 24U
#define RTT_UP_DESC_SIZE 24U
#define RTT_UP_WROFF_OFF 12U
#define RTT_UP_RDOFF_OFF 16U

/* 单次块读的字节数（默认值，运行时可经 HID CMD_RTT action 7 改）。
 * 官方 swd_read_memory() 内部按 1 KB 页切分；实测纯 SWD 基准 512 B 3434 KB/s、
 * 2048 B 3476 KB/s（+1.2%），再大无收益，故默认取 2048。 */
#define RTT_SWD_CHUNK 2048U

/* 换挡斜坡的中间档（见 rtt_swd_init 里的说明）。 */
#define RTT_SWD_RAMP_HZ 20000000UL

/* 每次轮询最多搬运的字节数。 */
#define RTT_MAX_DRAIN 2048U

/* 找不到 RTT 控制块时的重试（见 rtt_bridge_start 里的说明）。 */
#define RTT_CB_SCAN_ATTEMPTS 3U
#define RTT_CB_SCAN_INTERVAL_MS 20U

/* 启动失败时自动降档的候选（只在低于请求频率时使用）。 */
#define RTT_CLOCK_LADDER {60000000UL, 45000000UL, 36000000UL, 30000000UL, 20000000UL, 10000000UL}
#define RTT_CLOCK_LADDER_LEN 6U

/* MCHTMR runs at 24 MHz (osc24m). */
#define RTT_MCHTMR_HZ 24000000UL
#define RTT_IDLE_TICKS (RTT_BRIDGE_DAP_IDLE_MS * (RTT_MCHTMR_HZ / 1000U))
#define RTT_ERROR_BACKOFF_MS 100U
#define RTT_RESCAN_AFTER 3U

typedef struct
{
    uint32_t name;
    uint32_t buf;
    uint32_t size;
    uint32_t wr;
    uint32_t rd;
    uint32_t flags;
} rtt_up_desc_t;

static uint8_t s_channel;
static uint32_t s_cb_addr;
static uint32_t s_search_addr;
static uint32_t s_search_size;
static uint32_t s_up_addr;
static uint32_t s_last_dap;
static uint32_t s_backoff_until;
/* 上次没写成功、待幂等补写的 RdOff（0 值有效，用 valid 标志区分） */
static uint32_t s_rd_pending;
static uint8_t s_rd_pending_valid;

/* 运行时可调参数（HID CMD_RTT action 7）：块读字节数、SWD 时钟、丢弃模式。
 * 块读越大，每块的固定开销（CSW/TAR/prime/RDBUFF 共 5 次传输）摊得越薄。 */
static uint32_t s_chunk = RTT_SWD_CHUNK;
static uint32_t s_swd_clock_req = RTT_SWD_CLOCK_HZ; /* 用户请求的档位 */
static uint32_t s_swd_clock_hz = RTT_SWD_CLOCK_HZ;  /* 当前实际使用的档位 */
static uint8_t s_discard;
static uint8_t s_swd_ready;
static uint8_t s_delay_override = 0xFFU; /* 0xFF = 用 Set_Clock_Delay() 的档位值 */
static uint8_t s_rescans_since_step;     /* 攒够次数自动降档，见 rtt_bridge_poll */
/* STOP remains deferred to the main loop to flush RdOff before release. */

static uint32_t s_drained;
static uint32_t s_polls;
static uint32_t s_drains;
static uint32_t s_read_err;
static uint32_t s_write_err;
static uint32_t s_rescans;
static uint32_t s_gate_hits;
static uint32_t s_zips;
static uint32_t s_last_poll_bytes;
/* last DAP response, for diagnosing an unresponsive target */
static uint32_t s_last_cmd;
static uint32_t s_last_rsp;

static uint8_t s_req[16];
static uint8_t s_resp[8U];
static uint8_t s_stage[RTT_MAX_DRAIN];

static uint32_t mchtmr_now(void)
{
    return *(volatile uint32_t *)(HPM_MCHTMR_BASE + 0x00);
}

/* ------------------------------------------------------------------ */
/* SWD plumbing —— 直接使用 ARM DAPLink 官方 swd_host.c                */
/*                                                                     */
/* 桥的 SWD 访问全部走官方 API：                                       */
/*   swd_init()        —— DAP_Setup + PORT_SWD_SETUP + debug_port=SWD   */
/*   swd_init_debug()  —— JTAG2SWD 切换 + 清错 + DP 上电 + SELECT=0     */
/*   swd_read_memory() —— 块读（内部即 swd_read_block/AP 自动递增）     */
/*   swd_write_word()  —— 写 32 位字                                    */
/* 底层单次传输 SWD_Transfer() 由 swd_host_port.c 分派到               */
/* SWD_Read()/SWD_Write()（SW_DP.c，与 DAP 主机通路同一套 bit-bang）。  */
/* ------------------------------------------------------------------ */

/* The only DAP command the bridge still issues: SWJ_Clock, to load the fast
 * bit-bang blob and set DAP_Data.clock_delay. */
static void dap_cmd(const uint8_t *req, uint8_t *resp)
{
    (void)DAP_ExecuteCommand(req, resp);
    s_last_cmd = req[0];
    s_last_rsp = (uint32_t)resp[0] | ((uint32_t)resp[1] << 8) |
                 ((uint32_t)resp[2] << 16) | ((uint32_t)resp[3] << 24);
}

/* 单次块读的字节数：见文件头部的 RTT_SWD_CHUNK（默认值），此处用的是运行时值。 */

/* 0 = SWD/ARM（官方 swd_host），1 = RISC-V（JTAG DMI + SBA，见 src/riscv/）。
 * 目标类型由 HID CMD_RTT action 10 设定；桥本身的逻辑（找控制块、搬环形缓冲、
 * 回写 RdOff）两边完全一样，只有底下三个原语不同。 */
static uint8_t s_target;

void rtt_bridge_set_target(uint32_t kind)
{
    s_target = (kind != 0U) ? 1U : 0U;
}

/* 供 scope 采样器选择传输后端：目标类型是**全局**的（同一个开关既管 RTT 桥也管
 * J-Scope），这样网页不做任何改动就能采 RISC-V 目标。 */
uint8_t rtt_bridge_target_is_riscv(void)
{
    return (s_target == 1U) ? 1U : 0U;
}

/* 写目标内存里的一个字。返回 0 = 成功、-1 = 失败（与 rtt_read_bytes 同约定）。
 *
 * 两个后端底层的返回约定相反，必须在这里归一：
 *   swd_write_word()        1 = 成功、0 = 失败
 *   riscv_jtag_write_word() 0 = 成功、负 = 失败
 * 早先这个包装漏了对 RISC-V 取反，于是"回写 RdOff 成功"被判成失败：桥搬完第一块
 * 就永久卡在补写分支里（每个 poll 补写一次、write_err 每轮必涨、drained 再也不动，
 * 连控制块都不再读）。 */
static int rtt_write_word(uint32_t addr, uint32_t val)
{
    if (s_target == 1U)
    {
        return (riscv_jtag_write_word(addr, val) == 0) ? 0 : -1;
    }
    return swd_write_word(addr, val) ? 0 : -1;
}

/* 清掉链路上的 sticky 错误，成功后重试才有意义。
 *
 * 必须分目标：SWD 的清错是往 DP_ABORT 写 DAPLink 的四个 CLR 位；JTAG 下这条路径
 * 会把 SWD 引擎的时钟直接打在 JTAG 引脚上，TAP 状态机当场被打散 —— 之后每一次
 * DMI 扫描都只能读到 IDCODE（实测 s_dbg 四条全是 0x…1000563D），比不清还糟。
 * RISC-V 这边对应的是 SBCS 的 sbbusyerror / sberror（写 1 清零）。 */
static void rtt_clear_link_errors(void)
{
    if (s_target == 1U)
    {
        (void)riscv_jtag_clear_errors();
        return;
    }
    (void)swd_clear_errors();
}

/* Load the fast bit-bang blob and set the SWD timing. Uses the DAP command
 * only to reach Set_Clock_Delay(); it performs no SWD traffic. */
static int rtt_swd_set_clock(uint32_t hz)
{
    if (s_target == 1U)
    {
        if (hz < 256U) { riscv_jtag_set_delay(hz); }  /* RISC-V: hz 复用为 idle 周期数；SWD 换挡阶梯是百万级数字，不能落到 idle 计数上 */
        return 0;
    }
    s_req[0] = ID_DAP_SWJ_Clock;
    s_req[1] = (uint8_t)(hz >> 0);
    s_req[2] = (uint8_t)(hz >> 8);
    s_req[3] = (uint8_t)(hz >> 16);
    s_req[4] = (uint8_t)(hz >> 24);
    dap_cmd(s_req, s_resp);
    /* SWJ_Clock 的响应是 [0]=命令回显、[1]=状态 —— 状态字节不是 [2]。
     * 早先写成 s_resp[2]（靠静态零初始化恒等于 DAP_OK）→ 恒判成功，
     * 换挡失败被静默吞掉，斜坡逻辑失去失败感知。 */
    return (s_resp[1] == DAP_OK) ? 0 : -1;
}

/* Debug-port bring-up：官方 swd_host 的 swd_init() + swd_init_debug()。
 * 顺序不能反：swd_init() 里的 DAP_Setup() 会把时钟重置成默认档并加载 Slow
 * blob，所以 SWJ_Clock 必须在其之后、swd_init_debug() 之前调用。
 * swd_init_debug() 内部即 JTAG2SWD（52 高 + 0xE79E + 57 高 + 读 IDCODE）、
 * 清 sticky 错误、SELECT=0、DP 上电并等待 CSYSPWRUPACK|CDBGPWRUPACK、
 * 再设 TRNNORMAL|MASKLANE 并回到 SELECT=0；失败时它还带 4 次重试
 * （含一次 nRESET 硬复位）。 */
static int rtt_swd_init(void)
{
    if (s_target == 1U)
    {
        /* RISC-V: 没有 SWD 握手，直接开 TAP + 加载 IR=DMI + 唤醒 DM。 */
        int rc = riscv_jtag_open();

        s_swd_ready = (rc == 0) ? 1U : 0U;
        return rc;
    }

    uint32_t idcode = 0U;

    swd_init();     /* DAP_Setup + PORT_SWD_SETUP + DAP_Data.debug_port = SWD */

    /* 先在默认（4 MHz Slow 档）完成 JTAG2SWD 握手与 DP 上电 —— 和真实主机一样，
     * 连接阶段不用高速档。注意 swd_init_debug() 内部还会再调一次 swd_init()，
     * 其 DAP_Setup() 会把时钟档重置回默认，所以提速只能放在它之后。 */
    if (swd_init_debug() == 0U)
    {
        return -2;
    }

    /* 斜坡换挡：先切到 20 MHz 档（把 20M blob 装进去），再切到目标档。
     * 实测「4 MHz Slow 档直跳 60/80 MHz」在换挡后的头几次访问上就失败，而
     * 「20 MHz 档跳到 60/80 MHz」是稳的（低档初始化后切上去，60/80 MHz 稳态
     * 能跑 3431/3476 KB/s），这里就显式复现那条能工作的路径。 */
    (void)rtt_swd_set_clock(RTT_SWD_RAMP_HZ);
    (void)swd_clear_errors();

    if (rtt_swd_set_clock(s_swd_clock_hz) != 0)
    {
        return -1;
    }
    if (s_delay_override != 0xFFU)
    {
        DAP_Data.clock_delay = s_delay_override; /* SWJ_Clock 会重设，压回覆盖值 */
    }

    /* 换挡后的第一次访问最容易踩到瞬态；先读一次 DP IDCODE 把它吃掉。真读不动
     * 就清 sticky 错误再试一次，仍不行返回 -4 让上层降档（-4 = 链路在该档不可用，
     * 与握手失败的 -1/-2 区分开）。 */
    for (uint32_t attempt = 0U; attempt < 2U; attempt++)
    {
        if (swd_read_dp(0U /* DP_IDCODE */, &idcode) != 0U)
        {
            s_swd_ready = 1U;
            return 0;
        }
        (void)swd_clear_errors();
    }

    return -4;
}

/* Read `len` bytes from an arbitrary (possibly unaligned) address.
 * 官方 swd_read_memory() 自己会处理未对齐的头/尾字节，这里只负责把它切成
 * s_chunk 大小的块（目标运行中长块读会 WAIT/FAULT）。 */
static int rtt_read_bytes(uint32_t addr, uint8_t *dst, uint32_t len)
{
    uint32_t done = 0U;

    while (done < len)
    {
        uint32_t n = len - done;
        if (n > s_chunk)
        {
            n = s_chunk;
        }
        if (s_target == 1U)
        {
            if (riscv_jtag_read(addr + done, &dst[done], n) != 0)
            {
                return -1;
            }
        }
        else if (swd_read_memory(addr + done, &dst[done], n) == 0U)
        {
            return -1;
        }
        done += n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Control block discovery                                             */
/* ------------------------------------------------------------------ */

/* 512 B 重叠窗扫描：窗口 512 B、步进 503 B（重叠 9 B，保证跨窗签名不漏）。
 * 早先的 4 字节步进小读每步要 11 次传输（2 字块读 + 2 次字节读），扫 64 KB
 * 就是 18 万次传输 —— 在 45 MHz 那档上必然踩到临界，导致控制块找不到。
 * 换成块读后传输数少一个数量级，扫描本身也快得多。 */
#define RTT_SCAN_BLOCK 512U
#define RTT_SCAN_STEP  (RTT_SCAN_BLOCK - RTT_BRIDGE_SIG_LEN + 1U)

static uint8_t s_scan[RTT_SCAN_BLOCK];

static int rtt_find_cb(void)
{
    const uint32_t end = s_search_addr + s_search_size;
    uint32_t off;

    s_cb_addr = 0U;
    for (off = 0U; (s_search_addr + off + RTT_BRIDGE_SIG_LEN) <= end; off += RTT_SCAN_STEP)
    {
        uint32_t want = end - (s_search_addr + off);
        if (want > RTT_SCAN_BLOCK)
        {
            want = RTT_SCAN_BLOCK;
        }
        if (rtt_read_bytes(s_search_addr + off, s_scan, want) != 0)
        {
            s_read_err++;
            /* 一次失败先清 sticky 错误重试同一段（换挡后的瞬态很常见）；仍失败才
             * 认为已经走出目标 SRAM（搜索区间默认 64 KB，而 F103C8 只有 20 KB），
             * 后面全是未映射区，再扫没有意义，直接收工。 */
            rtt_clear_link_errors();
            if (rtt_read_bytes(s_search_addr + off, s_scan, want) != 0)
            {
                s_read_err++;
                break;
            }
        }
        for (uint32_t i = 0U; (i + RTT_BRIDGE_SIG_LEN) <= want; i++)
        {
            if (memcmp(&s_scan[i], RTT_BRIDGE_SIGNATURE, RTT_BRIDGE_SIG_LEN) == 0)
            {
                s_cb_addr = s_search_addr + off + i;
                s_up_addr = s_cb_addr + RTT_CB_UP_OFFSET + (uint32_t)s_channel * RTT_UP_DESC_SIZE;
                return 0;
            }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Draining                                                            */
/* ------------------------------------------------------------------ */

static int rtt_poll_once(uint32_t *moved)
{
    rtt_up_desc_t up;
    uint32_t avail, target, first, free_space;

    *moved = 0U;

    /* RdOff 写失败的幂等重试：RdOff 是绝对值，把同一个值再写一遍永远安全，
     * 所以先补写上次没写成功的那次，再继续搬新数据。不这样做的话，
     * swd_write_data() 里收尾的 dummy RDBUFF 读一旦失败（此时写其实已经落到
     * 目标上了）就会被误判为"没写成功"，两边对不齐 —— 要么丢一段、要么重一段。 */
    if (s_rd_pending_valid)
    {
        if (rtt_write_word(s_up_addr + RTT_UP_RDOFF_OFF, s_rd_pending) != 0)
        {
            s_write_err++;
            return -1; /* 下轮再补 */
        }
        s_rd_pending_valid = 0U;
    }

    if (rtt_read_bytes(s_up_addr, (uint8_t *)&up, RTT_UP_DESC_SIZE) != 0)
    {
        s_read_err++;
        return -1;
    }
    if ((up.size == 0U) || (up.rd >= up.size) || (up.wr > up.size))
    {
        /* Control block looks bogus: re-scan. */
        s_read_err++;
        return -1;
    }

    avail = (up.wr >= up.rd) ? (up.wr - up.rd) : (up.size - up.rd);
    if (avail == 0U)
    {
        s_zips++;
        return 0;
    }

    free_space = chry_ringbuffer_get_free(&g_uartrx);
    target = avail;
    if (target > RTT_MAX_DRAIN)
    {
        target = RTT_MAX_DRAIN;
    }
    if (s_discard == 0U)
    {
        if (target > free_space)
        {
            target = free_space;
        }
        if (target == 0U)
        {
            return 0; /* the host is not draining fast enough */
        }
    }

    first = up.size - up.rd;
    if (first > target)
    {
        first = target;
    }
    if (rtt_read_bytes((uint32_t)up.buf + up.rd, s_stage, first) != 0)
    {
        s_read_err++;
        return -1;
    }
    if (target > first)
    {
        if (rtt_read_bytes((uint32_t)up.buf, &s_stage[first], target - first) != 0)
        {
            s_read_err++;
            return -1;
        }
    }

    /* 先交付到 CDC 环，再推进目标侧 RdOff：这样即使 RdOff 写失败，字节也已经
     * 送到了主机，绝不会丢；RdOff 由上面的幂等重试补齐，也不会重。
     * 丢弃模式（基准测试用）只计数、不送环，用来量纯 SWD 侧的天花板。 */
    if (s_discard == 0U)
    {
        chry_ringbuffer_write(&g_uartrx, s_stage, target);
    }

    s_drained += target;
    s_drains++;
    *moved = target;

    uint32_t new_rd = (up.rd + target) % up.size;
    if (rtt_write_word(s_up_addr + RTT_UP_RDOFF_OFF, new_rd) != 0)
    {
        s_write_err++;
        s_rd_pending = new_rd;
        s_rd_pending_valid = 1U;
        return -1;
    }

    return 0;
}

/* 链路变差时自愈：降一档（只降不升，避免来回抖）。返回 1 表示档位变了。 */
static int rtt_clock_step_down(void)
{
    static const uint32_t ladder[RTT_CLOCK_LADDER_LEN] = RTT_CLOCK_LADDER;

    for (uint32_t i = 0U; i < RTT_CLOCK_LADDER_LEN; i++)
    {
        if (ladder[i] < s_swd_clock_hz)
        {
            s_swd_clock_hz = ladder[i];
            return 1;
        }
    }
    return 0;
}

/* 链路上的"重来一次"（导出给 scope 的验收读用，见 rtt_bridge.h）。
 *   SWD    —— 能降就降一档（高频档抖了就往稳的档退）+ 清 sticky + 重新初始化。
 *             已经在最低档时没有档可降，但**照样要重新初始化**：实测换挡后的瞬态读失败
 *             光重试/光清 sticky 都不恢复（0~100 ms 连探都读不动），只有重走
 *             rtt_swd_init()（JTAG2SWD + DP 上电 + 斜坡换挡）才回来。
 *   RISC-V —— 没有档位可降：DMI 一旦不应答，唯一的出路是重走 TAP 复位并把
 *             IR 重新加载成 DMI，而 riscv_jtag_open() 干的正好是这件事。
 * 返回 0 = 重新初始化成功。 */
int rtt_bridge_link_recover(void)
{
    if (s_target == 1U)
    {
        s_swd_ready = 0U;
        return rtt_swd_init();
    }
    if (rtt_clock_step_down() == 0)
    {
        rtt_clear_link_errors();
    }
    s_swd_ready = 0U;
    return rtt_swd_init();
}

/* 桥自己的自愈入口（rtt_bridge_poll 里那两处调用） */
static void rtt_link_recover(void)
{
    (void)rtt_bridge_link_recover();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int rtt_bridge_start(uint32_t addr, uint32_t size, uint8_t channel)
{
    if (uartx_get_cdc_source() == CDC_SOURCE_SPI) return -15;
    if (adc_stream_enabled()) return -14; /* shared ADC must retire before core streaming */
    rtt_bridge_stop();

    s_channel = channel;
    s_search_addr = (addr != 0U) ? addr : RTT_BRIDGE_DEFAULT_ADDR;
    s_search_size = (size != 0U) ? size : RTT_BRIDGE_DEFAULT_SIZE;
    s_swd_clock_hz = s_swd_clock_req; /* 每次启动都从用户请求的档位开始 */

    /* 每次启动清零计数器：一次实验的读数只反映这一次。 */
    s_drained = 0U;
    s_polls = 0U;
    s_drains = 0U;
    s_read_err = 0U;
    s_write_err = 0U;
    s_rescans = 0U;
    s_gate_hits = 0U;
    s_zips = 0U;
    s_last_poll_bytes = 0U;
    s_rd_pending_valid = 0U;
    s_rescans_since_step = 0U;

    int rc = -3;
    uint32_t req_hz = s_swd_clock_hz;
    uint32_t try_hz[RTT_CLOCK_LADDER_LEN];
    uint32_t ntry = 0U;
    static const uint32_t ladder[RTT_CLOCK_LADDER_LEN] = RTT_CLOCK_LADDER;

    /* 高频档在这条链路上是间歇性的：同样的 36 MHz，长块读能跑满而控制块扫描
     * 会失败（blob 是固定时序，短读/字节读的余量小得多）。与其赌一个档位，
     * 不如从请求频率往下走，用第一次能扫到控制块的档位 —— 用户直接要
     * 60 MHz 也能自动落到当下可用的最快档。 */
    try_hz[ntry++] = req_hz;
    for (uint32_t i = 0U; i < RTT_CLOCK_LADDER_LEN && ntry < RTT_CLOCK_LADDER_LEN; i++)
    {
        uint32_t x = ladder[i];
        if (x < req_hz)
        {
            try_hz[ntry++] = x;
        }
    }

    for (uint32_t a = 0U; a < ntry; a++)
    {
        s_swd_clock_hz = try_hz[a];
        rc = rtt_swd_init();
        if (rc != 0)
        {
            continue;
        }
        /* swd_init_debug() 的 abort 路径会拉一次目标 nRESET（DAPLink 的既定
         * 行为），目标此时可能才刚开始启动、RTT 控制块还没建好；给它一点时间，
         * 而不是一次扫描失败就换档。 */
        for (uint32_t attempt = 0U; attempt < RTT_CB_SCAN_ATTEMPTS; attempt++)
        {
            if (rtt_find_cb() == 0)
            {
                rc = 0;
                break;
            }
            clock_cpu_delay_ms(RTT_CB_SCAN_INTERVAL_MS);
        }
        if (rc == 0)
        {
            break;
        }
        rc = -3;
    }

    if (rc != 0)
    {
        s_swd_clock_hz = req_hz; /* 都没成功：留住用户要的档位，下次再试 */
        return rc;
    }

    /* The UART must not feed the same CDC ringbuffer while RTT does. */
    uartx_set_cdc_source(1);

    s_last_dap = mchtmr_now();
    s_backoff_until = 0U;
    s_running = 1U;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 收尾补写：把还没落地的 RdOff 写回去                                   */
/* ------------------------------------------------------------------ */

/* 为什么必须补：RdOff 写失败的兜底是"记 pending、下一轮幂等补写"（rtt_poll_once
 * 开头）。但如果是**会话收尾**那一笔失败，就再也没人补了 —— 目标侧 RdOff 停在
 * "少 ≤RTT_MAX_DRAIN 一段"的位置，下一次启动桥会把这段**重读一遍**：字节一个不少，
 * 但流里多出 ≤2 KB 重复（实测：固定图案的完整性检查把接缝看成"丢几个字节"，
 * 带序号靶子看到的是记录号回退）。
 *
 * 所以 STOP 不能在 USB 中断里直接落地：那里不能碰 SWD。改成排队，由主循环执行
 * 这个补写（幂等，最多 4 次 + 清 sticky；失败就放弃 —— 只是重读，不会丢数据）。 */
static void rtt_bridge_flush_pending_rd(void)
{
    if (!s_rd_pending_valid) { return; }

    for (uint32_t i = 0U; i < 4U; i++)
    {
        if (rtt_write_word(s_up_addr + RTT_UP_RDOFF_OFF, s_rd_pending) == 0)
        {
            s_rd_pending_valid = 0U;
            return;
        }
        /* 这一笔多半是踩到换挡/链路瞬态：清掉 AP 上的 sticky 错误再补一次。
         * 换个节拍再来 —— 连 100 ms 都读不动的瞬态也见过，所以不要死磕。 */
        rtt_clear_link_errors();
        clock_cpu_delay_ms(2U);
    }
    s_write_err++;
    s_rd_pending_valid = 0U;   /* 补不上就放弃：目标侧落后只会导致下次重读 */
}

void rtt_bridge_request_stop(void)
{
    s_stop_pending = 1U;
}

void rtt_bridge_stop(void)
{
    if (s_running)
    {
        uartx_set_cdc_source(0);
    }
    s_running = 0U;
    s_cb_addr = 0U;
    s_up_addr = 0U;
    s_rd_pending_valid = 0U;
    s_swd_ready = 0U; /* 主机随时可能重新接管 SWD，下次用前重新初始化 */
}

int rtt_bridge_is_running(void)
{
    return s_running ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 给 scope 采样器复用的 adapter（声明见 rtt_bridge.h 末尾）            */
/* ------------------------------------------------------------------ */

int rtt_bridge_swd_ensure_ready(void)
{
    if (s_swd_ready) { return 0; }
    return rtt_swd_init();       /* 成功时它自己会把 s_swd_ready 置 1 */
}

int rtt_bridge_swd_is_ready(void)
{
    return (s_swd_ready != 0U) ? 1 : 0;
}

void rtt_bridge_request_swd_clock(uint32_t hz)
{
    if (hz == 0U) { return; }
    s_swd_clock_req = hz;
    s_swd_clock_hz = hz;
    /* 关键：把链路标成"下次要重新初始化"，让 rtt_swd_init() 去走 20 MHz 斜坡
     * 并按 s_swd_clock_hz 装载 blob。直接在旧档上调 rtt_swd_set_clock() 会在
     * 链路没起来时静默变成"只记录不装载"。 */
    s_swd_ready = 0U;
}

int rtt_bridge_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    return rtt_read_bytes(addr, dst, len);
}

uint32_t rtt_bridge_last_dap_ticks(void)
{
    return s_last_dap;
}

void rtt_bridge_set_swd_clock(uint32_t hz)
{
    if (hz == 0U) { return; }
    s_swd_clock_req = hz;
    if (s_swd_ready)
    {
        if (rtt_swd_set_clock(hz) == 0) { s_swd_clock_hz = hz; }
    }
    else
    {
        s_swd_clock_hz = hz;
    }
}

uint32_t rtt_bridge_swd_clock_hz(void)
{
    return s_swd_clock_hz;
}

void rtt_bridge_note_dap_activity(void)
{
    s_last_dap = mchtmr_now();
    /* 主机用过 DAP（例如 OpenOCD 刚连过），DP 的时钟档/供电状态就不再由我们
     * 掌握：下次启动或基准测试必须重新初始化，不能复用旧状态。 */
    s_swd_ready = 0U;
    /* 那份"待补写的 RdOff"同理不再可信：主机可能刚把目标复位/重烧过，控制块是全新的
     * （RdOff 归零），把上一轮的旧位置写进去会让采样位置凭空前进一段 = **真丢数据**。
     * 宁可放弃补写（代价只是下次会话重读 ≤2 KB）。 */
    s_rd_pending_valid = 0U;
    /* 同理，swd_host 对 AP/DP 的影子寄存器缓存（SELECT / CSW / TAR）也会失真 ——
     * 主机那条路走 SWD_Read/SWD_Write，完全绕过 swd_host。不清掉的话，
     * scope 采样器可能拿着"CSW 还是自增"的旧认知去读**别的地址**。 */
    swd_invalidate_ap_cache();
    /* RISC-V 那边同理，而且更狠：主机用 DAP 复位/烧录会把 DM 的 SBCS 清零，而
     * riscv_jtag 的"配置已写好"缓存看不出来 —— 不清掉的话 SBA 读会**静默返回 0**
     * （实测：靶子刚被 ndmreset 烧完，探针读回全 0，OpenOCD 读同一地址却正常）。 */
    riscv_jtag_invalidate_cache();
}

/* Bring-up debugging: keep the last host DAP request/response so the exact
 * bytes OpenOCD sends can be compared with the ones the bridge builds.
 * (32 bytes request + 32 bytes response, read back with the HID PEEK action.) */
volatile uint8_t g_dap_trace[64];

void rtt_bridge_trace_dap(const uint8_t *req, const uint8_t *resp)
{
    for (uint32_t i = 0U; i < 32U; i++)
    {
        g_dap_trace[i] = req[i];
        g_dap_trace[32U + i] = resp[i];
    }
}

/* ------------------------------------------------------------------ */
/* Deferred execution (never touch SWD from the USB interrupt)         */
/* ------------------------------------------------------------------ */

static uint8_t s_raw_reject;        /* 1 = 入口挡下的请求（只回 DAP_ERROR，不执行） */
static uint32_t s_req_addr, s_req_size;
static uint8_t s_req_channel;
static uint8_t s_raw_req[24];
static uint32_t s_raw_req_len;
/* 响应缓冲按**一条完整 DAP 响应**给：DAP_TransferBlock 的 count 是请求里声明的
 * 2 字节字段（一条 5 字节请求就能声明 0xFFFF，响应 4+4×count ≈ 256 KB），
 * 原来 24 字节的缓冲一碰就溢出。长度上界由 rtt_bridge_request_raw() 在入口保证。 */
static uint8_t s_raw_rsp[DAP_XFER_SIZE];
static volatile uint32_t s_raw_rsp_len;
static volatile int8_t s_start_rc = -100;
static uint32_t s_bench_addr;
static uint32_t s_bench_bytes;
static uint32_t s_bench_iters;

static volatile uint8_t s_bench_valid;
static uint32_t s_bench_moved;
static uint32_t s_bench_ticks;
static volatile int32_t s_bench_err;

void rtt_bridge_request_start(uint32_t addr, uint32_t size, uint8_t channel)
{
    s_req_addr = addr;
    s_req_size = size;
    s_req_channel = channel;
    s_start_rc = -100;
    s_start_pending = 1U;
}

void rtt_bridge_request_raw(const uint8_t *req, uint32_t len)
{
    if (len > sizeof(s_raw_req))
    {
        len = sizeof(s_raw_req);
    }
    for (uint32_t i = 0U; i < len; i++)
    {
        s_raw_req[i] = req[i];
    }
    s_raw_req_len = len;
    s_raw_rsp_len = 0U;

    /* 有几类请求的响应长度 / 请求消费长度**不由请求长度限定**，必须在这里就挡住：
     *   - 空请求：req[0] 还是上一条命令的残留，会被原样再执行一次；
     *   - ExecuteCommands(0x7F)：嵌套命令的响应叠加，还能再套一层 —— raw 通道的
     *     语义本来就是"发一条命令"，直接拒绝；
     *   - SWD/JTAG Sequence(0x1D/0x14)：序列数是声明值，DIN 序列每个往响应里追加
     *     (bits+7)/8 字节（最多 ~2 KB），handler 还会按声明逐序列消费请求字节、
     *     走出 s_raw_req —— 同样直接拒绝（位级序列不是 raw 通道的用途；
     *     SWJ_Sequence 0x12 没有 DIN 响应，不受影响）；
     *   - TransferBlock(0x06)：count 是请求里声明的 2 字节字段，把 count 钳到
     *     响应缓冲装得下的上界；
     *   - Transfer(0x05)：count 也是**声明值**，而 TIMESTAMP_CLOCK 是开着的（带
     *     时间戳的读项响应 8 B），count=255 时 3+255×8=2043 B 会写穿 1 KB 缓冲 ——
     *     按请求里装得下的**完整 item 数**（读 1 B / 写 5 B）把 count 收进来，
     *     响应侧随之有界（24 B 请求最多 21 项 → 3+21×8 = 171 B）。
     * 其余命令的响应长度确实被 24 字节请求长度限死，装得下。 */
    if ((len == 0U) || (s_raw_req[0] == ID_DAP_ExecuteCommands) ||
        (s_raw_req[0] == ID_DAP_SWD_Sequence) || (s_raw_req[0] == ID_DAP_JTAG_Sequence))
    {
        s_raw_rsp[0] = DAP_ERROR;      /* 给主机一个看得见的拒绝 */
        s_raw_rsp_len = 1U;
        s_raw_reject = 1U;
        s_raw_pending = 1U;
        return;
    }
    if ((len >= 4U) && (s_raw_req[0] == ID_DAP_TransferBlock))
    {
        uint32_t count = (uint32_t)s_raw_req[2] | ((uint32_t)s_raw_req[3] << 8);
        uint32_t limit = (uint32_t)((sizeof(s_raw_rsp) - 4U) / 4U);
        if (count > limit)
        {
            s_raw_req[2] = (uint8_t)limit;
            s_raw_req[3] = (uint8_t)(limit >> 8);
        }
    }
    if ((len >= 4U) && (s_raw_req[0] == ID_DAP_Transfer))
    {
        uint32_t count = s_raw_req[2];
        uint32_t off = 3U;
        uint32_t fit = 0U;

        while ((fit < count) && (off < len))
        {
            /* 请求字节数：读 = 1（带 MATCH_VALUE 再 +4 的匹配值）；写 = 1+4。
             * 响应侧每项最多 4 数据 + 4 时间戳 = 8 B，钳完自然有界
             * （24 B 请求最多 ~19 项 → 3+19×8 = 155 B）。 */
            uint32_t item = s_raw_req[off];
            uint32_t rbytes = ((item & DAP_TRANSFER_RnW) != 0U)
                                  ? (((item & DAP_TRANSFER_MATCH_VALUE) != 0U) ? 5U : 1U)
                                  : 5U;
            off += rbytes;
            if (off <= len)
            {
                fit++;
            }
        }
        if (fit < count)
        {
            s_raw_req[2] = (uint8_t)fit;   /* 声明多少不算数，装得下多少才算数 */
        }
    }
    s_raw_reject = 0U;
    s_raw_pending = 1U;
}

uint32_t rtt_bridge_raw_result(uint8_t *out, uint32_t max)
{
    uint32_t n = s_raw_rsp_len;
    if (n > max)
    {
        n = max;
    }
    for (uint32_t i = 0U; i < n; i++)
    {
        out[i] = s_raw_rsp[i];
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 运行时可调参数 + 纯 SWD 读基准                                       */
/* ------------------------------------------------------------------ */

void rtt_bridge_configure(uint32_t swd_clock_hz, uint32_t chunk_bytes, uint8_t discard,
                          uint8_t delay_override)
{
    if (chunk_bytes != 0U)
    {
        if (chunk_bytes < 64U)
        {
            chunk_bytes = 64U;
        }
        if (chunk_bytes > 4096U)
        {
            chunk_bytes = 4096U;
        }
        s_chunk = chunk_bytes;
    }

    if (delay_override != 0xFFU)
    {
        s_delay_override = delay_override;
    }

    if (swd_clock_hz != 0U)
    {
        s_swd_clock_req = swd_clock_hz;
        s_swd_clock_hz = swd_clock_hz;
        if (s_swd_ready)
        {
            (void)rtt_swd_set_clock(swd_clock_hz); /* 运行中也能改档 */
        }
    }

    /* clock_delay 是 blob 唯一暴露的时序参数，主机档位表之外还能再压一档。 */
    if (s_delay_override != 0xFFU)
    {
        DAP_Data.clock_delay = s_delay_override;
    }

    s_discard = (discard != 0U) ? 1U : 0U;
}

void rtt_bridge_request_bench(uint32_t addr, uint32_t bytes, uint32_t iters)
{
    if (bytes == 0U)
    {
        bytes = 512U;
    }
    if (bytes > RTT_MAX_DRAIN)
    {
        bytes = RTT_MAX_DRAIN;
    }
    if (iters == 0U)
    {
        iters = 1U;
    }
    if (iters > 64U)
    {
        iters = 64U; /* 一次最多 128 KB，别让主循环停太久 */
    }

    s_bench_addr = addr;
    s_bench_bytes = bytes;
    s_bench_iters = iters;
    s_bench_valid = 0U;
    s_bench_pending = 1U;
}

int rtt_bridge_bench_result(uint32_t *bytes, uint32_t *ticks, int32_t *err)
{
    if (s_bench_valid == 0U)
    {
        return 0;
    }
    *bytes = s_bench_moved;
    *ticks = s_bench_ticks;
    *err = s_bench_err;
    return 1;
}

/* Runs from the main loop: reads a fixed target region over and over through
 * the very same swd_host path the bridge uses, so the number is the SWD side
 * only (no CDC, no USB, no target-side producer). */
static void rtt_bridge_run_bench(void)
{
    s_bench_err = 0;
    s_bench_moved = 0U;
    s_bench_ticks = 0U;

    if (s_swd_ready == 0U)
    {
        s_bench_err = (int32_t)rtt_swd_init();
        if (s_bench_err != 0)
        {
            s_bench_valid = 1U;
            return;
        }
    }

    uint32_t t0 = mchtmr_now();
    uint32_t i = 0U;
    uint32_t retried = 0U;

    while (i < s_bench_iters)
    {
        if (rtt_read_bytes(s_bench_addr, s_stage, s_bench_bytes) != 0)
        {
            /* 换挡后的瞬态读失败：清掉 AP 上的 sticky 错误再试同一轮；只有连重试
             * 都失败才判 -4（初始化成功但读不动），与初始化步骤的 -1/-2 区分开。 */
            if (retried == 0U)
            {
                retried = 1U;
                rtt_clear_link_errors();
                continue;
            }
            s_bench_err = -4;
            break;
        }
        s_bench_moved += s_bench_bytes;
        i++;
    }
    s_bench_ticks = mchtmr_now() - t0;
    s_bench_valid = 1U;
}

/* Runs from the main loop: performs any queued SWD work. */
static void rtt_bridge_service_requests(void)
{
    /* 停桥排在最前面（它要补写 RdOff，且 scope 的 START 依赖"桥已经停了"才独占 SWD）。 */
    if (s_stop_pending)
    {
        s_stop_pending = 0U;
        /* STOP cancels a START queued before it. Otherwise this same service
         * pass would stop the engine and immediately start it again below.
         * Publish completion from the main loop, after deferred work is safe. */
        s_start_pending = 0U;
        s_start_rc = 0;
        if (s_running)
        {
            rtt_bridge_flush_pending_rd();
        }
        rtt_bridge_stop();
    }
    if (s_raw_pending)
    {
        s_raw_pending = 0U;
        if (s_raw_reject)
        {
            /* 入口挡下的请求：s_raw_rsp 里已经是给主机的 DAP_ERROR，不再执行。 */
            s_raw_reject = 0U;
        }
        else
        {
            for (uint32_t i = 0U; i < sizeof(s_raw_rsp); i++)
            {
                s_raw_rsp[i] = 0U;
            }
            (void)DAP_ExecuteCommand(s_raw_req, s_raw_rsp);
            s_raw_rsp_len = sizeof(s_raw_rsp);
            /* 这是唯一一条"主机碰了 DAP 却不报备"的入口（主通路 chry_dap_handle
             * 每条命令都报）：raw 命令会直接改写 DP_SELECT / AP_CSW / AP_TAR，
             * 而 swd_host 的影子缓存并不知道 —— 不报备的话，随后用 scope/RTT
             * 会拿着旧认知去读**别的地址**（静默读错）。 */
            rtt_bridge_note_dap_activity();
        }
    }
    if (s_start_pending)
    {
        s_start_pending = 0U;
        /* 重启前也把没落地的 RdOff 补一笔：语义与"停桥"一样（幂等、RdOff 是绝对值），
         * 但只有**会话还活着**（s_running）时才能做 —— 主机碰过 DAP 的话，pending 已经
         * 在 rtt_bridge_note_dap_activity() 里作废了（目标可能刚被复位/重烧）。 */
        if (s_running)
        {
            rtt_bridge_flush_pending_rd();
        }
        s_start_rc = (int8_t)rtt_bridge_start(s_req_addr, s_req_size, s_req_channel);
    }
    if (s_bench_pending)
    {
        s_bench_pending = 0U;
        rtt_bridge_run_bench();
    }
}

int rtt_bridge_start_result(void)
{
    return (int)s_start_rc;
}

void rtt_bridge_poll(void)
{
    uint32_t moved = 0U;
    static uint8_t err_run;

    /* Queued SWD work (bridge start / raw passthrough) always runs here, in
     * the main loop: the bit-bang engine is timing critical and must not be
     * driven from the USB interrupt. */
    rtt_bridge_service_requests();

    if (!s_running)
    {
        return;
    }

    uint32_t now = mchtmr_now();
    if ((uint32_t)(now - s_last_dap) < RTT_IDLE_TICKS)
    {
        s_gate_hits++;
        return; /* the host is debugging: leave the SWD bus alone */
    }
    if ((s_backoff_until != 0U) && ((int32_t)(now - s_backoff_until) < 0))
    {
        return;
    }

    s_polls++;
    if (rtt_poll_once(&moved) != 0)
    {
        s_last_poll_bytes = 0U;
        if (++err_run >= RTT_RESCAN_AFTER)
        {
            err_run = 0U;
            s_rescans++;
            /* 关键：AHB-AP 上一次瞬态错误会在 CTRL/STAT 里留下 sticky 标志
             * （STICKYERR/STICKYORUN/WDERR…），之后**每一次**访问都直接返回 FAULT。
             * 不主动清，一次瞬态就会变成永久失效 —— 高频档尤其容易踩到。
             * swd_clear_errors() 即 DAPLink 的 DP_ABORT 写（STKCMPCLR|STKERRCLR|
             * WDERRCLR|ORUNERRCLR）；RISC-V 下换成 SBCS 的写 1 清零。 */
            rtt_clear_link_errors();
            s_cb_addr = 0U;
            if (rtt_find_cb() != 0)
            {
                /* 重扫也找不到：这才是「这一档真的用不了」，降一档重来。
                 * （注意不能一有错就降档：目标运行中出现几次读失败是正常的瞬态，
                 *   那样会把 45 MHz 平白降到 20 MHz。） */
                rtt_link_recover();
                s_backoff_until = now + (RTT_ERROR_BACKOFF_MS * (RTT_MCHTMR_HZ / 1000U));
                return;
            }
            /* 重扫成功但**反复**需要重扫 = 这一档在抖（典型是 60 MHz：能跑，
             * 但每十几秒来一次采样错误，会打断数据流）。攒够次数就自动降一档，
             * 别一直丢数据。 */
            if (++s_rescans_since_step >= RTT_RESCANS_PER_STEP_DOWN)
            {
                s_rescans_since_step = 0U;
                rtt_link_recover();
            }
        }
        s_backoff_until = now + (RTT_ERROR_BACKOFF_MS * (RTT_MCHTMR_HZ / 1000U));
        return;
    }
    err_run = 0U;
    s_last_poll_bytes = moved;
}

uint32_t rtt_bridge_status(uint32_t *out, uint32_t words)
{
    uint32_t w[12];

    w[0] = (uint32_t)s_running | ((uint32_t)s_channel << 8) |
           ((DAP_Data.debug_port == DAP_PORT_SWD) ? (1UL << 16) : 0UL) |
           ((uint32_t)DAP_Data.clock_delay << 24);
    w[1] = s_cb_addr;
    w[2] = s_up_addr;
    w[3] = s_drained;
    w[4] = s_polls | (s_drains << 16);
    w[5] = s_read_err | (s_write_err << 16);
    w[6] = s_last_poll_bytes | (s_zips << 16);
    w[7] = s_gate_hits | (s_rescans << 16);
    w[8] = s_last_cmd;
    w[9] = s_last_rsp; /* [0]=id [1]=count_lo [2]=count_hi/port [3]=response value */
    w[10] = 0U;        /* 调用方（api_param）会把它覆盖成 start_rc */
    w[11] = s_chunk | ((uint32_t)s_discard << 16) |
            ((uint32_t)(s_swd_clock_hz / 1000000U) << 24); /* 低16=块字节, bit16=丢弃, 高8=时钟MHz */

    for (uint32_t i = 0; i < words && i < 12U; i++)
    {
        out[i] = w[i];
    }
    return (words < 10U) ? words : 10U;
}
