/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 akaInstruments */

/*
 * USB -> SPI/QSPI 转发协议（线路契约，唯一真源）
 *
 * 设计文档：docs/usb-spi-bridge-plan.md（§4 协议设计 / §4.7 面板档）
 * 本文件同时被固件与上位机（web）参照；改这里必须同步：
 *   - firmware/application_5301/src/spi_bridge/spi_bridge.c（实现）
 *   - script_test/spi_bridge_test.py（协议自检）
 *   - docs/web-handoff-spi-bridge.md（网页侧说明，P5 产出）
 *
 * 三层：
 *   1) HID CMD 0x35 —— 控制面：配置 / 面板档 / 状态 / 使能 / 复位 / 中止
 *   2) bulk OUT 0x0B —— 有序帧流：一切与**线序**有关的动作（传输、CS、DC、延时…）
 *   3) bulk IN  0x8B —— 应答流：读数据 + 每个 RSP 帧的应答（含 seq 配对）
 *
 * 顺序性约定：参与时序的动作**必须**走 bulk OUT（USB 两条管道之间没有顺序保证），
 * HID 只做与线序无关的配置与状态查询。
 */

#ifndef SPI_BRIDGE_PROTO_H
#define SPI_BRIDGE_PROTO_H

#include <stdint.h>
#define SPI_BRIDGE_MODULE_CLOCK_HZ 240000000UL

/* ============================ bulk 端点 ============================ */

#define SB_IN_EP 0x8B /* EP11 IN  —— 应答流 */
#define SB_OUT_EP 0x0B /* EP11 OUT —— 帧流 */

/* HS bulk 最大包；也是 OUT/IN 环的槽大小 */
#define SB_PKT_SIZE 512
/* 一帧最多占多少字节：整包减去 8 字节帧头 */
#define SB_FRAME_MAX (SB_PKT_SIZE - 8)

/* ============================ 帧（bulk OUT） ============================ */

/* 小端 u16；线上字节序 = 'S'(0x53), 'B'(0x42) */
#define SB_MAGIC 0x4253U

/* 帧头 8 字节，所有帧共用；一帧不跨 USB 包 */
typedef struct
{
    uint16_t magic; /* SB_MAGIC */
    uint8_t type;   /* sb_frame_type_t */
    uint8_t flags;  /* SB_F_* */
    uint16_t seq;   /* 主机分配；带 RSP 时原样回传，用于请求/应答配对 */
    uint16_t len;   /* 本帧 payload 字节数 */
} sb_frame_hdr_t;

typedef enum
{
    SB_T_XFER = 0x01,   /* 通用 SPI 事务（12 B 传输头 + TX 数据） */
    SB_T_CS = 0x02,     /* 手动 CS：u8 0=释放 1=拉低 */
    SB_T_GPIO = 0x03,   /* 辅助脚写：u8 line, u8 level */
    SB_T_DELAY = 0x04,  /* 延时：u32 微秒（非阻塞） */
    SB_T_PING = 0x05,   /* 保活/测往返（配 RSP） */
    SB_T_CFG = 0x06,    /* 数据面内改运行参数：u8 sub, ... */
    SB_T_STEP = 0x07,   /* 面板初始化步：u8 cmd, u8 nparams, u16 delay_ms, params[] */
    SB_T_RESET = 0x08,  /* 面板复位脉冲：u16 low_ms, u16 post_ms */
    SB_T_AUX_IN = 0x09, /* 回读辅助输入脚（配 RSP）：应答 payload = u8 位图 */
} sb_frame_type_t;

/* flags */
#define SB_F_RSP (1U << 0)       /* 要求回一个应答包 */
#define SB_F_CS_HOLD (1U << 1)   /* 本帧后保持 CS 有效 */
#define SB_F_CS_OFF (1U << 2)    /* 本帧后释放 CS */
#define SB_F_NO_DMA (1U << 4)    /* 强制轮询 */
#define SB_F_FORCE_DMA (1U << 5) /* 强制 DMA */
/* bit3 (0x08) 保留：曾叫 SB_F_CS_AUX，v1 固件从未实现过（CS 线由配置块的
 * cs_policy 决定，帧级没法逐帧改）。复用这位前先实现它。 */

/* ---- SB_T_XFER 的 12 B 传输头 ---- */
typedef struct
{
    uint8_t cmd;      /* 命令字节（tcfg.cmd_en = 1 时发送） */
    uint8_t tcfg;     /* SB_TCFG_* 位域 */
    uint8_t addr_len; /* 地址字节数 0..4（0 = 不发地址相位） */
    uint8_t dummy;    /* dummy 周期 0 = 无，1..4 */
    uint16_t tx_len;  /* 数据相位发送字节数 */
    uint16_t rx_len;  /* 数据相位接收字节数 */
    uint32_t addr;    /* 地址，MSB 在前发出 */
} sb_xfer_t;

/* tcfg 位域（线数与相位开关） */
#define SB_TCFG_LINES_MASK 0x03U /* 0 = 1 线，1 = 2 线，2 = 4 线 */
#define SB_TCFG_LINES_1 0x00U
#define SB_TCFG_LINES_2 0x01U
#define SB_TCFG_LINES_4 0x02U
#define SB_TCFG_CMD_EN (1U << 2)     /* 发命令相位 */
#define SB_TCFG_ADDR_EN (1U << 3)    /* 发地址相位 */
#define SB_TCFG_ADDR_QUAD (1U << 4)  /* 地址相位 2/4 线（否则单线） */
#define SB_TCFG_DC_EN (1U << 5)      /* 传输前先设 DC 脚 */
#define SB_TCFG_DC_LEVEL (1U << 6)   /* DC 电平（1 = 数据） */
#define SB_TCFG_TOKEN_EN (1U << 7)   /* 发 0x69 token 相位 */

/* ---- SB_T_GPIO / SB_T_AUX_IN 的线号 ---- */
#define SB_LINE_DC 0U     /* 命令/数据选择（输出） */
#define SB_LINE_RST 1U    /* 面板复位（输出） */
#define SB_LINE_CS_AUX 2U /* 备用片选（输出） */
#define SB_LINE_BL 3U     /* 背光（输出） */
#define SB_LINE_TE 4U     /* 撕裂信号（**输入**） */

/* ---- SB_T_CFG 的子命令 ---- */
#define SB_CFG_TX_DMA_THRESHOLD 0U /* u16 阈值 */

/* ---- 配置块 flags ---- */
#define SB_CFG_F_CLEAR_ON_ENABLE (1U << 0) /* ENABLE=1 时先清环/复位状态机 */

/* ============================ 应答（bulk IN） ============================ */

typedef enum
{
    SB_R_RSP = 0x81, /* 对某个 RSP 帧的应答 */
    SB_R_EVT = 0x82, /* 异步事件（未带 RSP 的帧出错、环溢出等） */
} sb_rsp_type_t;

typedef enum
{
    SB_OK = 0,           /* 成功 */
    SB_E_DISABLED = 1,   /* 桥未使能 */
    SB_E_BAD_MAGIC = 2,  /* 帧头魔数错 */
    SB_E_BAD_FRAME = 3,  /* 帧格式/长度非法 */
    SB_E_RANGE = 4,      /* 参数越界（线数/tx_len/rx_len…） */
    SB_E_TIMEOUT = 5,    /* SPI 超时 */
    SB_E_IN_FULL = 6,    /* IN 环满，应答被丢弃 */
    SB_E_DMA = 7,        /* DMA 错误 */
    SB_E_BUSY = 8,       /* 前一帧尚未结束 */
    SB_E_GPIO = 9,       /* 辅助脚未配置 */
} sb_status_t;

/* 应答包：8 字节头 + 读数据 */
typedef struct
{
    uint16_t magic;  /* SB_MAGIC */
    uint8_t type;    /* sb_rsp_type_t */
    uint8_t status;  /* sb_status_t */
    uint16_t seq;    /* 对应帧的 seq */
    uint16_t len;    /* 后面跟的读数据字节数（≤ SB_FRAME_MAX） */
} sb_rsp_hdr_t;

/* SB_T_AUX_IN 的应答位图 */
#define SB_AUXIN_TE (1U << 0)

/* ============================ HID 控制面（CMD 0x35） ============================ */

/*
 * 报文约定沿用 CMD 0x31/0x32/0x34：
 *   req[2] = 0x35，req[3] = action，req[4..] = 参数
 *   res[2] = 0x35，res[3] = action 回显，res[4..7] = u32 状态字，res[8..] = 附加数据
 */
#define SB_HID_CMD 0x35

typedef enum
{
    SB_ACT_STATUS = 0,       /* 读状态字 + 计数器 */
    SB_ACT_ENABLE = 1,       /* req[4]：0 关 / 1 开 */
    SB_ACT_RESET = 2,        /* 清环、复位状态机、清计数器（保留配置） */
    SB_ACT_SET_CFG = 3,      /* req[4..] = 配置块 */
    SB_ACT_GET_CFG = 4,      /* res[4..] = 配置块 */
    SB_ACT_PIN_CFG = 5,      /* req[4]=line, req[5]=pad 索引, req[6]=有效电平 */
    SB_ACT_ABORT = 6,        /* 丢弃未处理帧与 IN 队列 */
    SB_ACT_SET_PROFILE = 7,  /* req[4..] = 面板档块 */
    SB_ACT_GET_PROFILE = 8,  /* res[4..] = 面板档块 */
    SB_ACT_DRAIN = 9,        /* req[4]=1..16：在 EP11 回短应答，收尾主机挂起的 IN 读 */
    SB_ACT_DBG = 10,         /* 上板诊断：res[4..] = 12 × u32 SPI 寄存器现场快照 */
    SB_ACT_PINTEST = 11,     /* 上板诊断：把 MOSI/MISO 当普通 GPIO 验跳线通断 */
    SB_ACT_WIGGLE = 12,      /* 上板诊断：在 SCLK/CS/MOSI 脚上发慢方波（给 LA 看接线） */
} sb_hid_action_t;

/* SB_ACT_PINTEST 的 res[4] 结果位 */
#define SB_PIN_MOSI_LOW (1U << 0)   /* 驱动 MOSI=0 时 MISO 读到 0 */
#define SB_PIN_MOSI_HIGH (1U << 1)  /* 驱动 MOSI=1 时 MISO 读到 1（这两位都置 = 跳线通） */
#define SB_PIN_FLOAT_PD (1U << 2)   /* MISO 悬空 + 下拉，读到 0（输入通路正常） */
#define SB_PIN_FLOAT_PU (1U << 3)   /* MISO 悬空 + 上拉，读到 1 */
#define SB_PIN_MISO_DRV (1U << 4)   /* 反过来驱动 MISO，MOSI 能读到（双向都通） */
#define SB_PIN_DONE (1U << 7)       /* 自检跑完了 */

/* 状态字 res[4..7]（u32 小端） */
#define SB_ST_ENABLED (1U << 0)  /* 桥已使能 */
#define SB_ST_ACTIVE (1U << 1)   /* 正在处理帧 */
#define SB_ST_CS (1U << 2)       /* CS 当前有效 */
#define SB_ST_IN_FLOW (1U << 3)  /* IN 侧被流控暂停 */
#define SB_ST_OUT_FULL (1U << 4) /* OUT 环接近满 */
#define SB_ST_SHIFT_ERR 8U       /* 最近错误码（bit8..15） */

/* ---- 配置块（SET_CFG / GET_CFG），sizeof = 32 B（自然对齐；spi_bridge.c 有
 *      _Static_assert 把关，改字段必须同步主机侧打包偏移） ---- */
typedef struct
{
    uint32_t sclk_hz;       /* 期望 SCLK，0 = 板级默认（20 MHz） */
    uint8_t mode;           /* SPI 模式 0..3（CPOL/CPHA） */
    uint8_t bits;           /* 数据位宽（v1 固定 8） */
    uint8_t cs_policy;      /* 0=硬件 CS0 自动；1=辅助 CS 自动；2=手动 */
    uint8_t tx_dma_threshold; /* 默认 100；0 = 全轮询 */
    uint8_t pad_dc;         /* 辅助脚 pad 索引（见 sb_pad_t），0 = 不用 */
    uint8_t pad_rst;
    uint8_t pad_cs_aux;
    uint8_t pad_bl;
    uint8_t pad_active_low; /* 位图：bit0 DC、bit1 RST、bit2 CS、bit3 BL（默认 0x06：RST/CS 低有效） */
    uint8_t pad_te;         /* 辅助**输入**脚（TE） */
    uint8_t flags;          /* bit0 = ENABLE 时自动清环 */
    uint8_t reserved0;
    uint16_t out_ring_kb;   /* 请求的 OUT 环大小（受总预算夹取） */
    uint16_t in_ring_kb;
    uint16_t max_frame_bytes; /* v1 固定 504 */
    uint16_t reserved1;
    /* SPI2 固定 240 MHz；保留协议字段，旧主机写入的提示值统一归一化为 240 MHz。 */
    uint32_t module_clk_hz;
    uint8_t reserved[4];
} sb_cfg_t;

/* ---- 面板档块（SET_PROFILE / GET_PROFILE），总长 16 B ---- */
typedef struct
{
    uint8_t profile;          /* 0 = raw；1 = spi_dcx；2 = qspi */
    uint8_t def_lines;        /* 档 0 数据相位线数：1/2/4 */
    uint8_t dc_active_high;   /* 档 1：DC 高 = 数据 */
    uint8_t cs_hold_in_step;  /* 档 1：翻 DC 时保持 CS */
    uint8_t qspi_wr_opcode;   /* 档 2：命令写 opcode（默认 0x02） */
    uint8_t qspi_color_opcode;/* 像素写 opcode（默认 0x32） */
    uint8_t qspi_addr_bytes;  /* 地址相位字节数（默认 3） */
    uint8_t flags;
    uint8_t reserved[8];
} sb_profile_t;

typedef enum
{
    SB_PROFILE_RAW = 0,     /* 一次 XFER：cmd + params 同线数 */
    SB_PROFILE_SPI_DCX = 1, /* 一个 CS 窗口内：命令(8bit) -> 翻 DC -> 参数 */
    SB_PROFILE_QSPI = 2,    /* opcode(默认 0x02) + 24bit 地址 + 1 线参数；
                             * 线上是 `02 | 00 <面板命令字> 00`（命令字在地址 bits[23:16]，2026-09-30 订正） */
} sb_profile_kind_t;

/* ---- pad 索引表（协议内固定，避免主机猜 IOC 编号） ---- */
typedef enum
{
    SB_PAD_NONE = 0,
    SB_PAD_PB11 = 1, /* J3[13]，**2026-09-30 起是 SPI2_SCLK，不能当辅助脚** */
    SB_PAD_PB12 = 2, /* J3[27]，同上：SPI2_MISO */
    SB_PAD_PB13 = 3, /* J3[28]，同上：SPI2_MOSI */
    SB_PAD_PB10 = 4, /* J3[26]（板上标注 SPI_CS1），同上：SPI2_CS0 */
    SB_PAD_PA02 = 5, /* J3[7]  */
    SB_PAD_PA09 = 6, /* J3[32]（TinyUF2 按键脚，慎用） */
    SB_PAD_PA00 = 7, /* J3[36]（UART0 TX / log 口） */
    SB_PAD_PA01 = 8, /* J3[38]（UART0 RX / log 口） */
    SB_PAD_PY00 = 9, /* J3[29] */
    SB_PAD_PY01 = 10,/* J3[31] */
    SB_PAD_PA10 = 11,/* J3[33]（板载 LED，慎用） */
    SB_PAD_PA30 = 12,/* J3[37]（USB0_PWR）：被板上 Q1 常态短到地，实测拉不动，别用 */
    SB_PAD_PA31 = 13,/* J3[11]（USB0_ID 网络，可当慢速输出） */
    /* 2026-09-30 新增：J3 上原本留给 **SPI1** 显示接口的四根。桥搬到 SPI2 之后，
     * 这份固件里它们**零引用**（SWD 走 PA06/PA07、nRESET=PA08、CDC=PB08/PB09、
     * 控制台=PA00/PA01），板上丝印还停在 SPI1 时代 —— 实测可用，故释放成普通辅助脚。
     * ⚠️ 若将来切到 boards/akaLinkPro 板级构建，PA26(nRESET/break)/PA27(SWCLK)/
     * PA28(SWDIO) 会被板级定义占走，届时这三项必须重新限制。 */
    SB_PAD_PA26 = 14,/* J3[24]（板上丝印 SPI_CS0） */
    SB_PAD_PA27 = 15,/* J3[23]（板上丝印 SCLK） */
    SB_PAD_PA28 = 16,/* J3[21]（板上丝印 MISO / IO1） */
    SB_PAD_PA29 = 17,/* J3[19]（板上丝印 MOSI / IO0） */
    SB_PAD_MAX = 18,
} sb_pad_t;

/* 内部计数器累加结构。⚠️ 它**不是** STATUS 的线序布局：STATUS（res[8..47)，10 × u32）
 * 按下面的顺序发，frames_err 在 actual_sclk **之后**（末尾第二个），别按本结构
 * 的字段顺序去解线上的包（见 docs/web-handoff-spi-bridge.md §4.2）：
 *   [8] frames_ok  [12] bytes_tx  [16] bytes_rx  [20] tx_poll_cnt  [24] tx_dma_cnt
 *   [28] out_ring_overrun  [32] in_ring_drop  [36] actual_sclk  [40] frames_err
 *   [44] last_ticks */
typedef struct
{
    uint32_t frames_ok;
    uint32_t frames_err;
    uint32_t bytes_tx;
    uint32_t bytes_rx;
    uint32_t tx_poll_cnt;
    uint32_t tx_dma_cnt;
    uint32_t out_ring_overrun;
    uint32_t in_ring_drop;
} sb_stats_t;

#endif /* SPI_BRIDGE_PROTO_H */
