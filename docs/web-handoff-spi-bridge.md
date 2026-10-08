# USB → SPI/QSPI 转发桥：网页侧（主机侧）实现说明

> 固件分支：`feature/usb-spi-bridge`　协议真源：`firmware/application_5301/src/spi_bridge/spi_bridge_proto.h`
> 方案与实测记录：`docs/usb-spi-bridge-plan.md`（§5.5–§5.8 是上板实测数据）
> **接线速查（40pin 排针全貌）**：`docs/spi-bridge-wiring.md`
> 自检工具：`script_test/spi_bridge_test.py`（HID 0x35 + bulk 0x0B/0x8B 的完整参考实现）；
> 真屏一键验证：`make panel`

这份文档是**网页组实现主机端所需要知道的一切**。协议本身以 `spi_bridge_proto.h` 为准，
本文负责把「怎么用、怎么排坑、实测能跑多快」讲清楚。

---

## 1. 五分钟上手

```
① GET_CFG          读配置块，确认固件在、看默认值
② SET_CFG          写 sclk_hz / mode / 辅助脚 / 阈值
③ SET_PROFILE      选面板档（raw / spi_dcx / qspi）
④ ENABLE 1         使能（此时才配引脚、开 DMA、武装 bulk OUT）
⑤ 灌帧             bulk OUT 发帧（RESET → STEP…×N → XFER 刷像素）
⑥ ENABLE 0         收工（引脚保持现状，环被清空）
```

**三个端点/通道**：
- 自定义 HID（老规矩，usage page `0xFF00`）：控制面 `CMD 0x35`
- bulk OUT `0x0B`：帧流（主机 → 探针）
- bulk IN `0x8B`：应答流（探针 → 主机）

VID/PID 不变（`0x0D28 / 0x0204`）。SPI 桥是**新增的第 5 个接口**（编号 4，
`bInterfaceClass = 0xFF`、`bNumEndpoints = 2`）。注意接口 0 也是 `0xFF`（CMSIS-DAP），
**用「0xFF 且恰好 2 个端点」来挑**，不要用接口号硬编码（HID/MSC 的编译开关会让接口号顺移）。

Windows 上这个接口靠 MS OS 2.0 描述符集里的第 4 条 WinUSB function subset 绑到 WinUSB，
libusb / WebUSB 都能正常打开。**如果你在旧固件上遇到「接口找得到但 claim 不了
（libusb NOT_SUPPORTED）」，那是固件太老。**

---

## 2. bulk OUT：帧流

**硬约束：一帧不能跨 USB 包**（包 ≤ 512 B）。好处是每帧 payload 在物理上连续。
主机必须自己切片：**payload ≤ 504 B**（`XFER` 的 `tx_len ≤ 492`）。

### 2.1 帧头（8 B，小端）

```
 0: u16 magic = 0x4253      ('S','B')
 2: u8  type
 3: u8  flags
 4: u16 seq                 主机分配，带 RSP 时原样回传
 6: u16 len                 payload 字节数
```

### 2.2 flags

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | `RSP` | 本帧要一个应答（bulk IN） |
| 1 | `CS_HOLD` | 本帧结束后**保持 CS 有效**（跨帧一个 CS 窗口） |
| 2 | `CS_OFF` | 本帧结束后释放 CS（自动 CS 模式下的默认行为，显式写也行） |
| 3 | `CS_AUX` | 本帧用辅助 CS（预留，v1 未实现） |
| 4 | `NO_DMA` | 本帧强制轮询 |
| 5 | `FORCE_DMA` | 本帧强制 DMA |

> **只有带 `RSP` 的帧才产生 IN 流量。** 批量刷像素时建议只有**最后一片**带 `RSP`，
> 中间片不带 —— 否则 IN 流量会拖慢灌数据。

### 2.3 帧类型

| type | 名称 | payload |
|---|---|---|
| `0x01` | `XFER` | 12 B 传输头 + TX 数据 |
| `0x02` | `CS` | `u8`：0=释放 1=拉低（**仅在 `cs_policy=2` 手动模式下有意义**） |
| `0x03` | `GPIO` | `u8 line, u8 level`；line：0=DC 1=RST 2=CS_AUX 3=BL |
| `0x04` | `DELAY` | `u32` 微秒（**非阻塞**，但会挡住它后面的帧） |
| `0x05` | `PING` | 空（配 `RSP` 用来测往返/保活） |
| `0x06` | `CFG` | `u8 sub, ...`；sub 0 = 改 `tx_dma_threshold`（跟一个 u16） |
| `0x07` | `STEP` | `u8 cmd, u8 nparams, u16 delay_ms, params[nparams]` —— 按当前**面板档**展开 |
| `0x08` | `RESET` | `u16 low_ms, u16 post_ms` —— 拉低 RST 保持 low_ms，释放后再等 post_ms |
| `0x09` | `AUX_IN` | 空（配 `RSP`）：回读辅助**输入**脚，应答 payload = `u8` 位图（bit0 = TE） |

**帧是严格有序的**：探针按到达顺序执行；`DELAY` / `RESET` / `STEP.delay_ms` 产生的等待
会**挡住后面所有帧**（面板初始化序列就靠这个："发完命令等 120 ms 再发下一条"）。
等待期间 USB 环照收，不丢数据，也不阻塞 DAP/RTT（非阻塞调度）。

### 2.4 `XFER` 的 12 B 传输头

```
 0: u8  cmd          命令字节（tcfg.cmd_en=1 时有效）
 1: u8  tcfg
 2: u8  addr_len     地址字节数 0~4（0 = 不发地址相位）
 3: u8  dummy        dummy 周期 0=无，1~4
 4: u16 tx_len       数据相位发送字节数
 6: u16 rx_len       数据相位接收字节数
 8: u32 addr         地址（**低 addr_len 个字节，MSB 在前**发给从器件）
12: ...              tx_len 字节 TX 数据
```

`tcfg` 位域：

| 位 | 名称 | 含义 |
|---|---|---|
| 1:0 | `lines` | 数据相位线数：0=1 线，1=2 线，2=4 线（QSPI） |
| 2 | `cmd_en` | 发命令相位 |
| 3 | `addr_en` | 发地址相位 |
| 4 | `addr_quad` | 地址相位用 2/4 线（否则单线） |
| 5 | `dc_en` | 传输前先设 DC 脚（面板档 1 会自动做，通用帧可用它） |
| 6 | `dc_level` | DC 电平（1 = 数据） |
| 7 | `token_en` | 发 0x69 token 相位 |

一次 `XFER` = **一次硬件 SPI 事务 = 一次 CS 窗口**（除非带 `CS_HOLD`）：

```
CS↓ ─ [cmd] ─ [addr] ─ [dummy] ─ [data(tx/rx, 1/2/4 线)] ─ CS↑
```

`tx_len` / `rx_len` 的分工：都非 0 → 全双工（**要求等长**，不等长请拆两帧）；
只有 tx → 只写；只有 rx → 只读（`dummy` 可配合）。

---

## 3. bulk IN：应答

```
 0: u16 magic = 0x4253
 2: u8  type     0x81 = RSP（对某个 RSP 帧的应答）；0x82 = EVT（异步事件/错误）
 3: u8  status   见下表
 4: u16 seq      对应帧的 seq
 6: u16 len      后面跟的读数据字节数（≤ 504）
 8: ...          len 字节读数据
```

**状态码**：`0` OK / `1` 未使能 / `2` 魔数错 / `3` 帧格式错 / `4` 参数越界 /
`5` SPI 超时 / `6` IN 环满 / `7` DMA 错 / `8` 忙 / `9` 辅助脚未配置。

> **`XFER` 的读数据即使 `status != 0` 也会照原样带回**（这是刻意的：调 SPI 时
> 「回读全 0 / 全 FF / 数据正确」是区分"线没接对"和"只是收尾没清"的唯一线索）。
> 所以判读时**先看 status，再看数据**，不要把"有数据"当成成功。

`EVT`（0x82）只在「帧没带 `RSP` 但执行出错」时出现；主机空闲时读掉即可。

**IN 环必须被及时取走**：环满（16 个槽）时桥会**暂停处理后续帧**等主机取数据，
状态字 bit3 会亮。批量写时别让每个分片都带 `RSP`。

---

## 4. HID 控制面：`CMD 0x35`

沿用现有约定：`req[2]=0x35`、`req[3]=action`、`req[4..]=参数`；
`res[2]=0x35`、`res[3]=action` 回显、`res[4..7]=u32 状态字`、`res[8..]=附加数据`。

| action | 名称 | 请求 | 响应 |
|---|---|---|---|
| 0 | `STATUS` | — | 状态字 + 计数器（见下） |
| 1 | `ENABLE` | `req[4]`：0 关 / 1 开 | 状态字 |
| 2 | `RESET` | — | 清环、复位状态机、清计数器（不动配置） |
| 3 | `SET_CFG` | `req[4..35]` = 32 B 配置块 | 状态字（非法字段返回 `RANGE`） |
| 4 | `GET_CFG` | — | `res[4..35]` = 32 B 配置块 |
| 5 | `PIN_CFG` | `req[4]`=line, `req[5]`=pad 索引, `req[6]`=有效电平 | 状态字 |
| 6 | `ABORT` | — | 丢弃未处理帧与 IN 队列 |
| 7 | `SET_PROFILE` | `req[4..19]` = 16 B 面板档块 | 状态字 |
| 8 | `GET_PROFILE` | — | `res[4..19]` = 16 B 面板档块 |
| 10 | `DBG` | — | `res[4..55]` = 13 × u32 SPI 寄存器现场快照（**调板用**） |
| 11 | `PINTEST` | — | `res[4..7]`：把 MOSI/MISO 当 GPIO 验跳线通断（**调板用**） |
| 12 | `WIGGLE` | — | `res[4..7]`：在 SCLK/CS/MOSI 上发慢方波给 LA（**调板用**） |

### 4.1 状态字 `res[4..7]`（u32 小端）

| 位 | 含义 |
|---|---|
| 0 | 已使能 |
| 1 | 正在处理帧 |
| 2 | CS 当前有效 |
| 3 | IN 侧被流控暂停（等主机取数据） |
| 4 | OUT 环接近满 |
| 8-15 | **最近一次错误码**（不是计数） |

### 4.2 计数器（`STATUS` 的 `res[8..47]`，10 × u32）

| 偏移 | 名称 | 含义 |
|---|---|---|
| 8 | `frames_ok` | 成功执行的帧数 |
| 12 | `bytes_tx` | 数据相位发出的字节数（**不含** cmd/addr 相位） |
| 16 | `bytes_rx` | 数据相位收到的字节数 |
| 20 | `tx_poll_cnt` | 走轮询的数据相位次数 |
| 24 | `tx_dma_cnt` | 走 DMA 的数据相位次数 |
| 28 | `out_ring_overrun` | OUT 环溢出 |
| 32 | `in_ring_drop` | IN 应答被丢的次数 |
| 36 | `actual_sclk` | **实际生效**的 SCLK（Hz） |
| 40 | `frames_err` | 出错帧数 |
| 44 | `last_ticks` | 上一笔事务耗时（MCHTMR tick，24 MHz = 41.67 ns） |

### 4.3 配置块（32 B）

```
 0: u32 sclk_hz            期望 SCLK（0 = 默认 20 MHz；上限 100 MHz）
 4: u8  mode               SPI 模式 0~3（CPOL/CPHA 四种组合**都实测通过**）
 5: u8  bits               固定 8
 6: u8  cs_policy          0 = PB10 作 GPIO CS（默认，每帧自动一个 CS 窗口）
                           1 = 辅助 GPIO 作 CS
                           2 = 手动（只有 CS 帧能改 CS）
                           3 = 硬件 CS0（**不支持面板档 1 的"一个窗口内翻 DC"**）
 7: u8  tx_dma_threshold   0 = 全轮询；0xFF = 全 DMA；其余按 tx_len 比。默认 100
 8: u8  pad_dc             辅助脚 pad 索引（见 pad 表），0 = 不用
 9: u8  pad_rst
10: u8  pad_cs_aux
11: u8  pad_bl
12: u8  pad_active_low     位图：bit0 DC、bit1 RST、bit2 CS、bit3 BL
                           **默认 0x06**（RST/CS 低有效）
13: u8  pad_te             辅助**输入**脚（TE 撕裂信号）
14: u8  flags              bit0 = 随 ENABLE 一起清环（默认开）
15: u8  reserved0
16: u16 out_ring_kb        预留（v1 固定）
18: u16 in_ring_kb
20: u16 max_frame_bytes    固定 504
22: u16 reserved1
24: u32 module_clk_hz      SPI2 模块固定为 240000000；旧写入值统一归一化，保留字段布局
28: u8  reserved[4]
```

**pad 索引表**（协议内固定，别去猜 IOC 编号）：

| 索引 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 引脚 | 无 | PB11 | PB12 | PB13 | PB10 | PA02 | PA09 | PA00 | PA01 | PY00 | PY01 | PA10 | PA30 | PA31 | **PA26** | **PA27** | **PA28** | **PA29** |
| J3 脚 | — | 13 | 27 | 28 | 26 | 7 | 32 | 36 | 38 | 29 | 31 | 33 | 37 | 11 | **24** | **23** | **21** | **19** |

- `PB10~PB13`（SPI2 的 SCLK/MISO/MOSI/CS）任何档位都不能当辅助脚；`PA30` 也别用
  （USB0_PWR 网络被板上 Q1 常态短到地，实测拉不动）；`PA31` 现在是自由脚。
- `PY00/PY01` 属于 PIOC 域，**v1 不支持**（固件返回 `RANGE`）。
- `PA09` 是板载 TinyUF2 按键脚、`PA10` 是板载 LED（且 LED 任务每 50 ms 写它，实测驱动不出持续电平）
  —— 列在表里只为完整性，**默认别选**。
- **`PA26/PA27/PA28/PA29`（索引 14~17）= 2026-09-30 释放**：它们原本是 SPI1 显示接口
  （板丝印 SPI_CS0 / SCLK / MISO / MOSI），桥搬到 SPI2 后这份固件里**零引用**
  （SWD 走 PA06/PA07、nRESET=PA08、CDC=PB08/PB09、控制台=PA00/PA01），实测可用。
  ⚠️ 若切到 `boards/akaLinkPro` 板级构建，PA26(nRESET/break)、PA27(SWCLK)、PA28(SWDIO)
  会被板级定义占走，届时要重新限制。
- 当前推荐接线（SPI2 时代，都在 J3 上）：`RST=PA02(J3.7)`、`BL=PA31(J3.11)`；
  SPI2 固定脚是 `SCLK=PB11(J3.13)`、`MISO=PB12(J3.27)`、`MOSI=PB13(J3.28)`、`CS=PB10(J3.26)`。

### 4.4 面板档块（16 B）

```
 0: u8 profile             0 = raw；1 = spi_dcx；2 = qspi
 1: u8 def_lines           档 0 的数据相位线数：1/2/4
 2: u8 dc_active_high      档 1：DC 高 = 数据（AXS15352 为 1）
 3: u8 cs_hold_in_step     档 1：翻 DC 时保持 CS（ESP-IDF 行为 = 1）
 4: u8 qspi_wr_opcode      档 2：命令写 opcode（默认 0x02）
 5: u8 qspi_color_opcode   档 2：像素写 opcode（默认 0x32，主机写像素时自己用它发 XFER）
 6: u8 qspi_addr_bytes     档 2：地址相位字节数（默认 3）
 7: u8 flags
 8: ...                    预留
```

---

## 5. 面板初始化序列怎么搬过来

网页侧手上的表是 `{cmd, data[n], n, delay_ms}`（见 `E:\esp-idf-wsh\资料\panel_init\`），
**一行编成一个 `STEP` 帧**即可：

```python
def step_frame(cmd, params, delay_ms=0, rsp=False):
    payload = struct.pack("<BBH", cmd, len(params), delay_ms) + params
    return frame(0x07, payload, flags=1 if rsp else 0, seq=next_seq())
```

| 表里的步骤 | 编成 |
|---|---|
| `{cmd, data, n, delay_ms}` | `STEP{cmd, n, delay_ms, data}` |
| `{"type":"delay", ms}` | 并进上一条 `STEP.delay_ms`，或单独 `DELAY` 帧（µs） |
| `{"type":"reset"}` | `RESET{low_ms, post_ms}` 帧 |

**两块屏的档位与线上效果**（都已在真机/LA 上验证过字节格式）：

**天马 2P01 / AXS15352（档 1，4 线 SPI + DC）—— 已上真屏验证，照抄即可**

```
配置: profile=1, dc_active_high=1, cs_hold_in_step=1, cs_policy=0(PA26 自动 CS),
      pad_dc=PB11(1), pad_rst=PB12(2), pad_bl=PB13(3), pad_te=PB10(4), sclk=40~75 MHz
接线: SCLK=J3[13] MOSI=J3[28] CS=J3[26] DC=J3[7] RST=J3[11] BL=J3[33]（2026-09-30 SPI2 接线）
      （屏的 VCI/VDDI 接 3V3；背光是裸 LED，LEDA/LEDK 要单独供，别指望 GPIO）

序列:
  RESET{low_ms=10, post_ms=120}
  STEP{0x36, [0x00]}                 ★ MADCTL: 0x00 = RGB 顺序（推荐，见下方说明）
  STEP{0x3A, [0x55]}                 ★ COLMOD: RGB565/16bpp（厂家表里没有，必须补！）
  ...厂家 30 条（0xCE 5A A5 开头，含 0x11 + Delay(100ms) + 0x29）...
  STEP{0x2A, [00 00 00 EF]}          列 0..239
  STEP{0x2B, [00 00 01 27]}          行 0..295
  XFER{dc_en, dc_level=0, tx=0x2C}  flags=CS_HOLD    ← RAMWR（命令，DC 低）
  XFER{dc_en, dc_level=1, tx=480B}  flags=CS_HOLD    ← 像素（数据，DC 高），CS 一直不抬
  ...重复...                        flags=0           ← 最后一片释放 CS
```

⚠️ **三条"屏黑"级注意事项**：
1. 厂家给的初始化表**不含 `0x36`/`0x3A`**，这两条必须自己补在 vendor 序列**之前**
   （ESP-IDF 的 `esp_lcd_st77916` 组件就是这么做的）。缺了 → 全黑。
2. **色序有两处会互相抵消的开关，别两个一起改**：
   - `MADCTL(0x36)` 的 **bit3 = BGR**：`0x00` = RGB 顺序（推荐），`0x08` = BGR 顺序。
   - **RGB565 的字节序**：这块屏收**高字节在前**（`struct.pack(">H")`，与跑通的
     ESP-IDF 工程一致 —— 那份工程 `rgb_ele_order = RGB → madctl_val = 0`，
     并且填充时用 `(c >> 8) | (c << 8)` 把字节序换过来）。
   - 「`0x00` + 高字节在前」是**正确组合**；「`0x08` + 低字节在前」是**两次交换互相抵消** ——
     红/蓝这种只有 R、B 分量的颜色看着一样，**绿色会偏蓝**，很容易蒙混过关。
   - 颜色不对时**只动一个**：先试 `--madctl 0x08`，再试低字节在前，别两个一起翻。
3. 刷图**只有最后一片带 `RSP`**。每片都带会把吞吐砍半（实测 4.22 → 2.08 MB/s）。
   另外**一帧一次 bulk 写**，把多帧拼成一个 blob 会被 512 B 包边界切断 → `BAD_FRAME`。

实测：40 MHz 整屏 142 KB 用 32.1 ms（**4.22 MB/s，线速的 88%**）；
60/75 MHz 吞吐饱和在 ~4.7 MB/s（瓶颈转到主机侧每帧开销），75 MHz 无花屏。

**ST77916（档 2，QSPI）**
```
profile=2, qspi_wr_opcode=0x02, qspi_addr_bytes=3
STEP{cmd=0xF0, params=[28]}  →  线上：CS↓ 02 | 00 F0 00 | 28 | CS↑
像素（整屏 360×360×2 = 259200 B，主机切片）：
  XFER{cmd=0x32, addr_len=3, addr=0x002C00, tcfg.lines=4, tx_len=480} × 540 片
```
（当时 LA 解出的 MOSI = `02 00 00 F0 28` —— 那验证的是**实现符合当时的（错误）预期**，不是面板要的格式。）

> ⚠️ **档 2 的命令字放在地址的 bits[23:16]（线上第二个字节）**：线上是 `02 | 00 <cmd> 00`。
> （2026-09-30 订正：此前这里写"最低字节"是错的 —— 当初 LA 只验了地址字段的字节序，
> 没验哪个字节装命令；与像素写 `0x32@0x002C00`、读 `0x0B@0x002E00` 的通行帧型对齐后，
> 固件 `sb_step_qspi` 已改为 `addr = cmd << 8`，与网页侧直接发 XFER 开窗逐字节一致。）
> 固件已经处理好了，网页侧只需要把面板命令字填进 `STEP.cmd`。

**像素字节序（RGB565 高字节在前）由主机负责**，探针是字节透明转发。

---

## 6. 实测性能（跳线回环，HPM5301EVKLite）

| 项目 | 结果 |
|---|---|
| SCLK 实际档位 | 20 / 40 / 60 / **75** MHz 回环全部通过；**真屏（AXS15352）在 75 MHz 下整屏刷图无花屏/闪点**（80/100 MHz 只有"一根杜邦线"的回环验过不去，不代表芯片或屏不行） |
| 真屏整屏刷图 | 240×296×2 = 142 KB：**40 MHz 32.1 ms（4.22 MB/s = 线速 88%）**；60/75 MHz 饱和在 ~4.7 MB/s |
| 单笔事务耗时 | 基本贴着 SPI 线速（20 MHz 写 492 B ≈ 200 µs，理论 196.8 µs） |
| 轮询 vs DMA | 低时钟轮询略快（DMA 固定开销 1~4.5 µs）；40 MHz 约 200 B 交叉、75 MHz 约 128 B 交叉（492 B 时 DMA 快 12%~38%） |
| 每帧固定开销 | ~2~5 µs（解析 + 事务启动 + 收尾）；**协议一帧不跨包 ⇒ 每帧最多 480 像素**，高时钟下这个开销就是吞吐上限 |

**给面板刷图的建议**：`tx_len` 尽量取满（480~492 B），只有最后一片带 `RSP`。

---

## 7. 排坑清单（都是上板真踩过的）

1. **bulk OUT 只能在 ENABLE 之后写**。未使能时端点不武装，主机会一直 NAK/超时。
2. **IN 环要主动排空**（尤其是带 `RSP` 的批量操作之后），否则状态字 bit3 亮、桥暂停处理。
3. **一帧不跨包**：`payload ≤ 504`、`tx_len ≤ 492`。超了帧会被判 `BAD_FRAME`。
4. **`DELAY` 是有序的**：它会挡住后面的帧。如果只是想"尽快把数据灌进去"，别在中间插延时。
5. **自动 CS 模式下每个 `XFER` 默认自成一个 CS 窗口**。要跨帧保持（例如命令+参数分两帧发），
   给前面的帧加 `CS_HOLD`，最后一帧不加。
6. **只读帧的 `dummy`**：很多 SPI 器件（NOR、传感器）读寄存器需要 1~4 个 dummy 周期，
   `tcfg` 里没有 dummy 位，用 `XFER` 头部的 `dummy` 字段。
7. **全双工必须等长**（`tx_len == rx_len`），不等长返回 `RANGE`（拆两帧即可）。
8. **`SET_CFG` 里改 `sclk_hz` 会在使能状态下立刻生效**（内部会重新配时钟与分频），
   但**实际档位**要看 `STATUS.actual_sclk` —— 不是任意频率都能精确得到。
9. **`cs_policy=3`（硬件 CS0）不支持面板档 1**：那种模式需要"一个 CS 窗口内翻 DC"。
10. **Windows 上必须是新固件**：老固件没有 SPI 接口的 WinUSB subset，接口能枚举但打不开。
11. **`ENABLE 0` 之后引脚保持现状**（不还原成默认复用）。所以用过 SPI 桥之后，
    `PA26`（它同时是 CDC 的 UART break 脚）会一直留在 SPI/GPIO 状态，直到探针重启 ——
    实测**不影响 RTT/CDC 数据通路**，只是 UART break 那个小功能要复位后才能用。
12. **和既有通道互不干扰，可以放心并发**：实测一边整屏刷屏（4.1~4.4 MB/s 走新端点 + SPI DMA）
    一边跑探针侧 RTT（2.49 MB/s）时，RTT 交付率只掉 0.2% 且**字节级零丢包**；
    DAP（OpenOCD 调目标）、CDC、HID、DFU、WebUSB 全部正常。SPI 桥未使能时
    `spi_bridge_poll()` 就一条分支返回（开着的代价约 0.1%）。

---

## 8. 出错时怎么定位（不用示波器）

固件留了三个上板诊断 action（见 §4 表）：

- `DBG`（10）：读 SPI 寄存器快照。里面有 `STATUS/CTRL/TRANSCTRL/TRANSFMT/TIMING`、
  写读计数、SDK 返回码、以及**失败时卡在哪一步**（stage）。
  「复位位清不掉」= IP 没时钟；「`TRANSCTRL` 里计数不对」= 参数没生效。
- `PINTEST`（11）：把 MOSI/MISO 摘下来当普通 GPIO，直接验 **J3[28]↔J3[27] 跳线通不通**、
  pad 输入通路好不好。回环测试失败时**先跑它**，能立刻区分"线没插"和"控制器的问题"。
- `WIGGLE`（12）：在 SCLK/CS/MOSI 上发慢方波，给逻辑分析仪验接线。

参考实现：`script_test/spi_bridge_test.py`（`info/cfg/profile/enable/status/frames/loop/dbg/
pintest/wiggle/bench` 全部子命令）、`tools/kingst_la.py`（金沙滩 LA 的采集与分析）。
