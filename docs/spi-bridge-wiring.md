# USB→SPI/QSPI 桥 接线速查（HPM5301EVKLite / J3 40pin）

> ★ = SPI 桥用到的脚　● = CDC 虚拟串口（VCOM）用的脚　⛔ = 本工程占用或无引出/不可用　○ = 空闲可挪作辅助脚
> 引脚表出处：官方 UG `docs/HPM5301EVKLite_UG_V1.1.pdf` 表 2（Rev1.1）+ 官方原理图
> `hpm5301evklite revb-1014.pdf`。
> **2026-09-30：SPI 桥从 SPI1 搬到了 SPI2**（原因见文末 §6），UART 也跟着换了一组脚。
> 辅助脚（DC/RST/BL/CS_AUX/TE）**不写死**，都可以用 HID `PIN_CFG` / `SET_CFG` 换到别的空闲脚。

## 1. 排针全貌（俯视，1/2 脚在 USB 那一端）

2026-10-08 起，SPI2 内部模块时钟固定为 **240 MHz**，SPI/QSPI 主机与 SPI转发从机都在启动时核验。主机对外 SCK 不超过请求值：10/20/40/60 MHz 精确生成，75/100 MHz 请求实际为 60 MHz（偶数分频，SDK 要求整数 Hz）。`module_clk_hz` 旧协议字段保留，写入后统一回读 240000000，不能再覆盖模块时钟。支持的最低 SCK 为 480 kHz；更低请求拒绝。详见 [实测报告](validation/2026-10-08-spi-fixed240.md)。

```
      奇数脚                                       偶数脚
  ┌──────────────────────────────────────────────────────────┐
  │  1 ● 3V3                              5V ●  2            │
  │  3 ● PB09  ● VCOM RX (UART2)          5V ●  4            │
  │  5 ● PB08  ● VCOM TX (UART2)         GND ●  6            │
  │  7 ● PA02  ○                    UART_TXD ●  8  PB15 ★ D3 │
  │  9 ● GND                        UART_RXD ● 10  PB14 ★ D2 │
  │ 11 ● PA31  ○(USB0_ID 网络)            NC ● 12            │
  │ 13 ● PB11  ★ SCLK                    GND ● 14            │
  │ 15 ● NC                               NC ● 16            │
  │ 17 ● 3V3                              NC ● 18            │
  │ 19 ● PA29  ⛔(USB0_OC 网络)          GND ● 20            │
  │ 21 ● PA28  ○                          NC ● 22            │
  │ 23 ● PA27  ○                    SPI_CS0 ● 24  PA26 ○     │
  │ 25 ● GND                         SPI_CS1 ● 26  PB10 ★ CS │
  │ 27 ● PB12  ★ D1/MISO                 GPIO ● 28  PB13 ★ D0/MOSI │
  │ 29 ● PY00  ⛔                         GND ● 30            │
  │ 31 ● PY01  ⛔                        GPIO ● 32  PA09  ○(按键) │
  │ 33 ● PA10  ○(板载 LED)               GND ● 34            │
  │ 35 ● NC                     UART_LOG_TX ● 36  PA00 ⛔(log) │
  │ 37 ● PA30  ⛔(USB0_PWR 被 Q1 短到地) UART_LOG_RX ● 38  PA01 ⛔(log) │
  │ 39 ● GND                              NC ● 40            │
  └──────────────────────────────────────────────────────────┘
```

## 2. 逐脚用途表

| 脚 | 板载丝印 | MCU | 现在的角色 |
|---|---|---|---|
| 1 / 17 | 3.3V | — | 给屏供电（VCC） |
| 2 / 4 | 5.0V | — | 一般不用（屏多是 3.3V） |
| **3** | I2C_SDA | **PB09** | ● **CDC VCOM RX**（UART2_RXD） |
| **5** | I2C_SCL | **PB08** | ● **CDC VCOM TX**（UART2_TXD） |
| **6 / 9 / 14 / 20 / 25 / 30 / 34 / 39** | **GND** | — | **地，必须接** |
| 7 | GPIO | PA02 | ○ 空闲（推荐当 RST/DC） |
| **8** | UART_TXD | **PB15** | ★ **QSPI D3 / IO3**（quad 档） |
| **10** | UART_RXD | **PB14** | ★ **QSPI D2 / IO2**（quad 档）← 就是当年 SPI1 上做不了的那根 |
| 11 | GPIO | PA31 | ○ 空闲（USB0_ID 网络：R2 100k 上拉 + BAT54A + R3 10k → Q1 栅极，拉高会顺手把 OTG 的 VBUS 使能拉低，慢速信号无所谓） |
| **13** | GPIO | **PB11** | ★ **SCLK** |
| 15 / 16 / 18 / 22 / 35 / 40 | NC | — | — |
| 19 | SPI_MOSI | PA29 | ⛔ USB0_OC 网络（AP2151 的 nFAULT + R6 10k 上拉）：当输出会被它拉，别用 |
| 21 | SPI_MISO | PA28 | ○ 空闲 |
| 23 | SPI_SCLK | PA27 | ○ 空闲 |
| 24 | SPI_CS0 | PA26 | ○ 空闲 |
| **26** | SPI_CS1 | **PB10** | ★ **CS**（SPI2 的 CS0） |
| **27** | GPIO | **PB12** | ★ **D1 / MISO**（注意：PB12 是 MISO，PB13 才是 MOSI） |
| **28** | GPIO | **PB13** | ★ **D0 / MOSI** |
| 29 / 31 | GPIO | PY00/PY01 | ⛔ 属于 PIOC 域，固件 v1 不支持 |
| 32 | GPIO | PA09 | ○ 空闲（USER/BOOT 按键脚，慎用） |
| 33 | GPIO | PA10 | ○ 空闲（板载 LED，当 BL 会顺手点亮它） |
| 36 / 38 | UART_LOG | PA00/PA01 | ⛔ **log_printf（UART0 console）**占用 |
| 37 | GPIO | PA30 | ⛔ **USB0_PWR 网络**：经 0Ω 的 R5 挂在 AP2151(USB0 电源开关) 的 EN 节点上，节点还有 2N7002(Q1) 的漏极，Q1 栅极由 CC1/CC2 → BAT54A 常态拉高 ⇒ Q1 常态导通，把这根网络低阻短到地。**实测配成 GPIO 也拉不动**（TE+内部上拉读回恒 0），别接 |

## 3. 三种接法

### A. 回环自测（不要屏，验固件的 P1/P2 验收项）

```
J3[28] MOSI ──跳线── J3[27] MISO       一根线，别的都不用接
```

跑：`python script_test/spi_bridge_test.py loop`（16 个长度 × 轮询/DMA）。
失败先跑 `pintest` —— 它会直接把"跳线没插好"和"控制器的问题"分开。

### B. 天马 2P01 / AXS15352（`profile = 1`，4 线 SPI + DC）

```
屏 VCC   ← J3[1]  或 J3[17]     3V3
屏 GND   ← J3[6]/[9]/[14]/[20]/[25]/[30]/[34]/[39]
屏 SCL   ← J3[13]  PB11         SCLK
屏 SDA   ← J3[28]  PB13         MOSI / D0
屏 DC    ← J3[7]   PA02         DC        (pad_dc   = 5 = PA02)
屏 RST   ← J3[11]  PA31         RESET     (pad_rst  = 13 = PA31)
屏 BL    ← J3[33]  PA10         背光      (pad_bl   = 11 = PA10)
屏 TE    ← 先不接（PB10 已归 SPI2 的 CS）
（这块屏没有 MISO，J3[27] 空着）
```

配置：`profile=1`、`dc_active_high=1`、`cs_policy=0`（PB10 自动 GPIO CS）、
`sclk_hz = 20~40 MHz`。初始化表一行 = 一个 `STEP` 帧。

### C. ST77916（`profile = 2`，QSPI 四线）

```
屏 VCC   ← J3[1]/[17] 3V3      屏 GND ← 任一 GND
屏 SCL   ← J3[13]  PB11   SCLK
屏 D0    ← J3[28]  PB13   MOSI / IO0
屏 D1    ← J3[27]  PB12   MISO / IO1
屏 D2    ← J3[10]  PB14   IO2
屏 D3    ← J3[8]   PB15   IO3
屏 CS    ← J3[26]  PB10   CS
屏 RST   ← J3[11]  PA31   RESET   (pad_rst = 13)
屏 BL    ← J3[33]  PA10   背光    (pad_bl  = 11)
```

配置：`profile=2`、`qspi_wr_opcode=0x02`、`qspi_addr_bytes=3`、`sclk_hz=40 MHz`。
刷像素：`XFER{cmd=0x32, addr_len=3, tcfg.lines=4, tx_len≤492}` 切片发（末片才放开 CS）。
2026-09-30 实测（LA，SCLK 2 MHz 便于解码）：初始化 194 个 CS 窗口**逐字节对账 194/194**
（993 B）；`0x32` 四线帧 51 个窗口 / 49 个四线窗口，**D0~D3 都有跳变**。

## 4. 用逻辑分析仪抓包时的通道对照（当前接法）

| LA 通道 | 接 | 看什么 |
|---|---|---|
| CH0 | J3[26] PB10 | CS（片选窗口，用来框住一次事务） |
| CH1 | J3[13] PB11 | SCLK |
| CH2 | J3[28] PB13 | D0 / MOSI |
| CH3 | J3[27] PB12 | D1 / MISO |
| CH4 | J3[10] PB14 | D2 / IO2 |
| CH5 | J3[8] PB15 | D3 / IO3 |
| — | 任一 GND | **GND 一定要接** |

解码：`python tools/la/kingst_la.py spi <cap.csv> --cs 0 --clk 1 --d0 2 --d1 3 --d2 4 --d3 5`
（单线事务只看 CS/CLK/D0 即可；`--edge auto` 会挑更像整字节的那个沿，本机实测用下降沿。）

## 5. 辅助脚想换到别的脚

协议里的 pad 表（`SET_CFG` 的 pad 字段 / `PIN_CFG` 的 line+索引）：

| 索引 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 引脚 | 无 | ~~PB11~~ | ~~PB12~~ | ~~PB13~~ | ~~PB10~~ | PA02 | PA09 | PA00 | PA01 | PY00 | PY01 | PA10 | PA30 | PA31 | **PA26** | **PA27** | **PA28** | **PA29** |
| J3 脚 | — | 13 | 27 | 28 | 26 | 7 | 32 | 36 | 38 | 29 | 31 | 33 | 37 | 11 | **24** | **23** | **21** | **19** |
| 可用 | — | ✗ | ✗ | ✗ | ✗ | ✓ | △ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ |

- 1~4 是 **SPI2 的 SCLK/MISO/MOSI/CS**（固件 `reserved[]` 直接拒）；
- 7/8 是 log 口（UART0），9/10 是 PIOC 域不支持，12 = PA30 被 Q1 短到地，6 = TinyUF2 按键脚；
- **11（PA10）2026-09-30 改判为不可用**：板载 LED 由 LED 任务每 50 ms 写一次，配成辅助脚后
  LA 上只有 ~20 ns 宽、周期精确 50 ms 的毛刺，**永远出不来持续电平**（同一份代码换到 PA02
  就干净利落）⇒ 想用它得先让固件里的 `led_write()` 避让；
- **14~17（PA26/PA27/PA28/PA29）2026-09-30 释放**：SPI1 时代显示接口的那四根
  （板丝印 SPI_CS0 / SCLK / MISO / MOSI），桥搬到 SPI2 之后这份固件里**零引用**
  （SWD 走 PA06/PA07、nRESET=PA08、CDC=PB08/PB09、控制台=PA00/PA01），实测可用。
  ⚠️ 若切到 `boards/akaLinkPro` 板级构建，PA26(nRESET/break)/PA27(SWCLK)/PA28(SWDIO) 会被占；
- 推荐：**RST = 5（PA02，J3[7]）**、**BL = 13（PA31，J3[11]）**。

`PIN_CFG` 的 line 号：**0=DC、1=RST、2=CS_AUX、3=BL、4=TE**（TE 是 4，别写成 5）。
换完记得 `ENABLE 0 → ENABLE 1` 让引脚重新配置。

## 6. 为什么要从 SPI1 搬到 SPI2（2026-09-30 结论，别再往回搬）

- EVKLite 上 SPI1 的六根脚是 PA26~PA31，其中 **PA30 = USB0_PWR、PA31 = USB0_ID**，
  都被 USB0 OTG 那套电路占着（原理图 03.POWER-USB-OTG-DEBUG 页）；
- **PA30 是致命的**：`PA30 —(R5 0Ω)— EN 节点`，EN 节点上还有 R4(10k 上拉)、
  Q1(2N7002) 漏极、U2(AP2151) 的 EN。Q1 栅极由 CC1/CC2 经 BAT54A/R3 驱动，
  R2(100k) 平时就把栅极拉高 ⇒ Q1 常态导通 ⇒ PA30 被低阻短到地；
- 实测证据：把 PA30 配成 GPIO 翻转，LA 上全程 0 跳变；把固件 TE 脚指到 PA30
  （输入 + 内部上拉）读回恒 0，同一套配置读 PA31 得 1 ⇒ 是外部把它按住了，不是配置问题；
- 想继续用 SPI1 只能硬件**拆掉 R5**（PA30 立刻自由，代价是 MCU 不再控制 OTG 的 VBUS 开关），
  否则四线 QSPI 的 IO2 在这块板上无解；
- SPI2 的六根（PB10~PB15）在 J3 上全引出，PB14/PB15 上原本的板载 CH340 是 **NC**
  （原理图 U6 = NC/CH340E，R94/R95 也是 NC），正好空着；
- UART 只能跟着动：CDC VCOM 从 UART3(PB15/PB14) 换到 **UART2(PB08/PB09)**，
  log_printf 仍在 UART0(PA00/PA01)。UART3 换不了别的脚 —— 它在 QFN48 上只有
  PB14/PB15 与未键合的 PA14/PA15（数据手册表 45：QFN48 "SPI3 未引出"）。
