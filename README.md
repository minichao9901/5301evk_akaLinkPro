# akaLinkPro

> ## 用 J-Link PRO **1% 的价格**（**¥69** vs **¥7000**），拿到它 **90% 的性能**
>
> 整机 = 一块 HPM5301EVKLite 开发板（**¥69**）+ 一根 USB 线。**零安装、零驱动、零配置** ——
> 浏览器打开一个网址，就是一套完整的调试工作台：调试、烧录、串口、RTT、变量示波、点屏、摸总线。

| # | 能力 | 实测数字 |
| --- | --- | --- |
| ① | **调试与传输**（对标 J-Link PRO 的下载 / 调试通路） | 60 MHz 档写 **3384** / 读 **2928** KB/s（OpenOCD 纯传输口径）；探针侧 RTT→CDC **2954 KB/s = 主机轮询上限的 2.6 倍**；RISC-V 目标 SRAM 读写 **约 1510 KB/s = OpenOCD 主机驱动的 9 倍** |
| ② | **变量示波（J-Scope / HSS）** | 单变量 u32 端到端 **330 kHz**（探针本体 637 kHz）、双变量 **114 kHz**、8 通道 **82 kHz**。同样口径下 **J-Link PRO 的 J-Scope 单变量是 100 kHz** —— 这里是它的 **3.3 倍**；而且**目标固件一行都不用改** |
| ③ | **零安装上位机**（WebUSB / WebHID） | **13 个标签页**：DAPLink 源码调试器（gdb 风格命令行）· 串口助手 · Xshell 式终端 · RTT Viewer · RTT→串口超高速转发 · J-Scope 变量示波 · 零安装 Flash 烧录 · USB→SPI/QSPI 调试助手 · SPI/QSPI 点屏助手 · SPI→USB 从机转发 · USB→I2C 调试助手 · USB→ADC/DAC · 典型工程 Makefile 模板生成 |

**③ 就在这里，不用装任何东西**：👉 <https://minichao9901.github.io/web-serial-rtt-tools/>
（桌面版 Chrome / Edge，插上探针授权一次即可；源码在
[web-serial-rtt-tools](https://github.com/minichao9901/web-serial-rtt-tools)，逐页说明见
[上位机（零安装网页工作台）](#上位机零安装网页工作台)）

> 一句话：**同样的活它能干，价格只有它的 1/100**（¥69 对 ¥7000）。下面每一格数字都是本仓库里
> 可复现的上板实测，不是标称值 —— 口径、靶子型号、测量脚本都在 [`script_test/`](script_test)。

<br>

akaLinkPro 是一个基于 HPM5301 的高性能 CMSIS-DAP 调试器。同一套应用源码支持两块硬件：

| 板级 | 板级目录 | DAP 目标侧输出 | CDC 虚拟串口 |
| --- | --- | --- | --- |
| akaLinkPro（原板） | `firmware/*/boards/akaLinkPro` | 板载排针 | UART2，PA08/PA09 |
| HPM5301EVKLite（移植） | `firmware/*/boards/hpm5301evklite` | J5 20 针 JTAG 座 | UART2，PB08/PB09 = J3.5/J3.3 |

- 主固件：`firmware/application_5301`
- DFU/MSC Bootloader：`firmware/bootloader_dfu`
- 配置上位机（WebHID）：`docs/index.html`
- USB 标识：APP `VID_0D28 PID_0204`（CMSIS-DAP + CDC + HID + WebUSB + DFU Runtime），
  DFU `VID_0D28 PID_0207`（DFU + MSC 虚拟 U 盘 `AKALINKPRO`）

板级通过 `board.h` 中的特性宏适配（`BOARD_UART2_SHARES_JTAG_PINS`、
`BOARD_HAS_SWDIO_DIR`、`BOARD_NRESET_ACTIVE_LOW`、`BOARD_LED_ACTIVE_LOW`、
`BOARD_HAS_VREF_ADC`、`BOARD_SWD_BLOB_EVKLITE` 等），两块板共用同一套 `src/`。

**目标侧**支持两类调试通路，同一份固件按需切换：

- **ARM（SWD 为主，也支持 JTAG）** —— 主力通路，按速度预编译的 bit-bang blob。
- **RISC-V（JTAG-only）** —— RISC-V Debug Module（DMI + SBA）+ 探针侧搬运引擎，
  见 [HPM6800EVK（HPM6880，RISC-V）目标调试](#hpm6800evkhpm6880risc-v目标调试)。

## 主要特性

- **CMSIS-DAP 调试器**：USB-HS 复合设备（DAP + CDC + 自定义 HID + WebUSB + DFU Runtime），
  SWJ 支持 SWD/JTAG；bit-bang 引擎按速度预编译（20/30/36/45/60 MHz 档 + Slow C 版），
  主机驱动（OpenOCD 纯传输口径）60M 档实测写 **3384** / 读 **2928** KB/s
  （F103ZE @96MHz，09-30 回归；两种口径的差别与历程见实测记录）。
- **探针侧 SEGGER RTT→CDC 桥**（HID `CMD_RTT` 0x31）：把 RTT 轮询从主机下沉进探针固件
  （J-Link 式），主机只读一个串口。**默认 45 MHz 档：2527 KB/s（2.47 MB/s）零丢包**；
  切 60 MHz 档：**2954 KB/s（2.89 MB/s）** —— 比主机轮询上限（1140 KB/s）快 **2.6 倍**。
  详见 [探针侧 RTT→CDC 桥](#探针侧-rttcdc-桥)。
- **J-Scope 波形（探针侧 HSS 采样，HID `CMD_SCOPE` 0x32 + bulk IN `0x83`）**：类 SEGGER
  J-Scope 的变量示波器 —— 探针自己按周期用 SWD 读目标 RAM 里 1~8 个变量，组 512 B
  自描述包推给主机，**目标固件一行都不用改**（变量地址来自目标 `.elf` 的 DWARF）。
  单变量 u32 实测**端到端 329 kHz**（3 µs 周期，探针侧零丢；探针本体 629 kHz），
  8 通道 6 字 span 89.3 kHz。采样期间可一键让出 CDC/串口桥（`flags bit5` 或 HID `0x34`）。
  详见 [J-Scope 波形（探针侧 HSS 采样）](#j-scope-波形探针侧-hss-采样)。
- **支持 RISC-V 目标（JTAG-only，HID `CMD_RISCV` 0x33）**：新增探针侧 RISC-V
  Debug Module 引擎（DMI + SBA，`src/riscv/` + 专用 DMI 扫描汇编，TDI 预置 + 循环
  展开）。调 HPM6800EVK（HPM6880）实测 SRAM 读 **1504.5 KB/s**、写 **1511.8 KB/s**，
  比主机驱动 OpenOCD 快 **9 倍**；RTT 交付 **1385 KB/s 且字节级零丢包**（28.5 MB
  不丢不重），有效 TCK 20.7 MHz（目标规格上限 25 MHz）。
  详见 [HPM6800EVK（HPM6880，RISC-V）目标调试](#hpm6800evkhpm6880risc-v目标调试)。
- **USB→SPI/QSPI 转发桥（HID `CMD_SPI` 0x35 + bulk `0x8B/0x0B`）**：把探针当"USB 转
  SPI/QSPI 主机"用，专为驱动 LCD/QSPI 屏与各类 SPI 器件。硬件自带 cmd/addr/dummy/token
  相位与单/双/**四线**，一次 CS 窗口跑完；面板档（`spi_dcx` / `qspi`）让网页把厂规初始化
  表 `{cmd, params, delay_ms}` 一行编成一个 `STEP` 帧原样下发。实测（跳线回环）单线
  **20/40/60/75 MHz 全部通过**，DMA 与轮询两条 TX 路径可配。详见
  [USB→SPI/QSPI 转发桥](#usbspiqspi-转发桥) 与
  [`docs/web-handoff-spi-bridge.md`](docs/web-handoff-spi-bridge.md)。
- **USB→I2C 转发桥（HID `CMD_I2C` 0x36，只走 HID、不用 DMA）**：I2C3 = **PA28/SDA(J3.21)**
  + **PA29/SCL(J3.19)**（J3 上唯一一对引出来的硬件 I2C 脚，与 SPI2/SWD/UART 都不冲突）。
  一次事务 = 子地址 + 写数据 + repeated START + 读（写 ≤51 B / 读 ≤54 B），主机轮询取结果；
  100 kHz / 400 kHz / 1 MHz 三档，带总线扫描、引脚自检、总线恢复。实测 AT24Cxx：
  扫描 0x50、页写回读逐字节一致、54 B 块读 100 kHz **5243 µs** → 400 kHz **1309 µs**。
  详见 [`docs/web-handoff-i2c-bridge.md`](docs/web-handoff-i2c-bridge.md) 与
  [`docs/usb-i2c-bridge-plan.md`](docs/usb-i2c-bridge-plan.md)。
- **DFU/MSC Bootloader**：长按 USER 键进 DFU，虚拟 U 盘 `AKALINKPRO` 拖入 `.bin` 即升级；
  APP 带签名 + 长度 + CRC32 校验，校验失败停在 DFU。
- **配置持久化 + WebHID 上位机**：配置存 QSPI NOR（EasyFlash），`docs/index.html` 可直接改。
- **两块硬件一套源码**：akaLinkPro 原板与 HPM5301EVKLite 移植板，靠 `board.h` 特性宏切换。

## 上位机（零安装网页工作台）

**纯静态网页，直接托管在 GitHub Pages，打开就能用：**
👉 <https://minichao9901.github.io/web-serial-rtt-tools/>

桌面版 Chrome / Edge；串口、HID、USB 设备在页面里**授权一次**即可，不装驱动、不装 OpenOCD、
不装 gdb、不装 J-Link 软件包。源码与逐页文档在另一个仓库
[web-serial-rtt-tools](https://github.com/minichao9901/web-serial-rtt-tools)。

| 标签页 | 干什么 |
| --- | --- |
| **调试器** | 网页里的极简 DAPLink：暂停 / 继续 / **单步** / 复位、寄存器表（回车即改）、内存 hexdump（点字节即改）、**FPB 硬件断点**、按符号名的 **gdb 风格命令行**，旁边顺手看 RTT。载入 `.elf` 后可按**源码行**下断点，停下来时源码区跟着 PC 走（DWARF 行号表） |
| **烧录器** | `.elf/.hex/.bin` 写进目标：**零安装 WebUSB**（页面里跑 flashloader，擦 / 写 / 校验 / 复位一条龙），或可选本地桥 + OpenOCD |
| **串口助手** | SSCOM 那套核心功能：ASCII/HEX 收发、**ANSI 彩色接收**、时间戳、定时发送、快捷发送、存盘、**高速自动关显示**（>50 KB/s 停渲染、数据照收） |
| **终端** | Xshell 式串口终端：xterm.js 渲染 ANSI、本地回显、回车/退格映射、粘贴发送；侧栏可直接开 RTT→CDC 转发 |
| **RTT Viewer** | SEGGER RTT 多通道查看 + 下行输入 + 复位目标，**目标类型 SWD/ARM 或 RISC-V/JTAG 可选**（HPM 系列走 JTAG+DMI+SBA），四种后端 |
| **RTT 转发** | 本探针的**探针侧** RTT→CDC：探针自己轮询目标控制块、把数据塞进自己的 CDC 口，**主机只读一个 COM 口** —— 网页上它就长成"一个超高速串口" |
| **J-Scope 波形** | 变量示波器：探针自己按固定周期读目标 RAM（HSS），多通道波形 + 触发 + CSV 导出 + 原始包回放 |
| **USB→SPI/QSPI** | USB→SPI/QSPI 调试助手：命令表 / 脚本 / **外接 NOR flash 测试**（读 ID、SFDP、读测速、擦写校验）/ 回环自检 |
| **SPI/QSPI 屏** | 点屏助手：刷屏（内置图案 / 拖入图片 / 动图 / 视频，**局部刷新**只发与上一帧不同的包围盒）、厂规面板初始化表解析与重放（每个字节可改、可点开看 8 个 bit）、读回 GRAM 还原成图 |
| **SPI→USB** | 外部 SPI 主机 → 探针从机循环 DMA → 现有 CDC 串口；复用 SPI 引脚、16 KiB 环与 CDC 会话，F103ZE 18 MHz 实收约 **2.250 MB/s**、字节/序号零错误。见 [功能与协议](docs/spi-cdc.md)、[验收数据](docs/validation/2026-10-08-spi-cdc.md) |
| **USB→I2C** | USB 转 I2C 主机：总线扫描 / 命令表 / 脚本（`loop 100ms … end` 就是 `while(1)` 定时读写）/ 实时值解码（把字节变成 g / ℃ / V + 迷你曲线），长读自动分片 |
| **工程生成** | 拖进 Keil `.uvprojx`，生成 `Makefile.jlink` / `jlink_gdb.script` / `Makefile.pyocd` / `Makefile.openocd`（含 `rtt_logger.py`）/ `test_sram.bin`，参数可填可勾、产物实时预览 |

> 为什么零安装这件事不容易：J-Link 与 OpenOCD 都是**本机程序**，浏览器无权启动进程、
> 也无权开 TCP。所以本项目的零安装通路是 **WebUSB 直连 CMSIS-DAP 探针**（RTT / J-Scope /
> 烧录都走它），想用 J-Link 或 OpenOCD 时再启动那个可选的本地桥（`bridge/`）。

## 最新进展（2026-10-02）

### USB→I2C 桥：修掉「收到第一条 0x36 命令就把整个探针挂死」（`a4d5205`）

**症状**：只有 I2C 分支会出现——探针照样枚举、EP0 还能读描述符，但 HID 一个命令也不回、
CDC 端口打不开，**只能拔插/复位**；main 上怎么折腾都不出现（所以一开始误判成"新功能把 USB 搞坏了"）。

**真因**：`ib_status_word()` 被**每条** 0x36 命令调到，而它以前**无条件**读 `IB_I2C->STATUS`；
可 `init_board_clock()` 里**没有 `clock_i2c3`** ⇒ 上电时 I2C3 的 IP 时钟关着，只有 `ENABLE` 才打开。
**给"没开时钟的 IP"发一次 AHB 读，事务可能永远不完成** ⇒ CPU 停在 USB 中断里 ⇒ 全机哑掉。
它时好时坏，所以早先 15 轮压测复现不出来（压测时桥是已使能的，时钟开着，根本踩不到）。

**修法**（双保险）：`i2c_bridge_init()` 上电即 `clock_add_to_group(clock_i2c3, 0)`；
`ib_status_word()` / `DBG` **未使能时一个寄存器都不读**。
**验证**：`info/enable/pintest/scan(0x50)/eeprom/err` 门禁全过，`eeprom` 连跑 **10 遍 10/10**，之后探针健在。

**排查时踩到的三个"假象"**（详见 [`docs/usb-i2c-bridge-plan.md`](docs/usb-i2c-bridge-plan.md) §8）：
① 浏览器 **WebHID 会抢 HID IN 的应答**（页面开着时命令行测什么都像"HID 死了"）；
② HID 报文的 **`req[1]`（长度）写 0 会被当空请求丢掉**；
③ **Windows 侧 USB 管道卡住**时报 EP0 `Pipe error` / CDC "设备没有发挥作用"，只有拔插能清。

### 第三轮代码审查处置 + 探针侧发送队列（N1）：吞吐反而涨了

完整处置表见 [`docs/代码审查报告.md`](docs/代码审查报告.md)。本轮修掉两条 P1/P2，外加把
二轮遗留的 **N1** 一并收口（它才是 DEF/STAT 到不了主机的真因）：

- 🚨 **HID `0x32` 配置的变量宽度未校验 → 越界写（P1，已修）**：主机那个 size 字节是
  **唯一**决定 span 长度与帧内偏移的输入。宽度 0xFF 时 span 长度 256 B，而中转缓冲
  `s_stage` 只有 68 B ⇒ 越界写 ~188 B；8 个宽变量时帧长可到 2016 B（一个包只装 496），
  直读落点还会被推到 512 B 包缓冲外面最多 ~1.5 KB。两条 HID 报文即可触发、且不报错。
  现在宽度只认 1/2/4/8，否则**整包拒绝**（回 `-6`、变量表清空），另加编译期断言 +
  排计划硬守卫。门禁 `script_test/scope_cfg_bound_test.py`。
- **DEF/STAT 推包记账（P2，已修）**：端点未使能时（USB 复位→SET_CONFIGURATION 之间）
  写失败不会有完成回调，记了账的包缓冲就永远回不来。现在失败不记账。
- 🚨 **bulk IN 发送队列（= 二轮审查 N1，已修）**：DWC2 端点**只认一笔在飞传输**，而移植层
  把底层 `usb_device_edpt_xfer()` 的 bool 丢掉了（`usb_dc_hpm.c:254`）—— 端点忙时
  `usbd_ep_start_write` 照样返回 0，"后推的那一包"其实没发出去：没有完成回调、缓冲永久
  占死。实测（修复前，8 通道 200 µs 档 3 s）**推出 1005 包 / 主机收到 999 / 完成回调恰好
  999** —— 差的 6 个全是 STAT（它紧跟 DATA 推包、间隔 <1 µs，必然撞上在飞），池子几秒内
  从 8 个缩到 2 个。STAT 到不了 ⇒ 网页那三个数（探针丢样本 / 排空不及 / 实际周期）永远是
  死的，DEF 也会偶尔被吞。现在固件自己排队：**同一时刻只放一笔在飞**，其余按 seq 躺在环里，
  完成回调接着踢下一包（队列代数防跨 START / USB 复位的迟到回调）。门禁
  `script_test/scope_tx_queue_test.py`（推出多少收到多少 / 回调数 == 收到包数 / STAT 到齐 /
  seq 无缺口，修复前必挂）。

**上板结果**（STM32F103ZE @96 MHz，同一台机器同一根线）：

| 指标 | 修复前 | 修复后 | 原基线 |
| --- | --- | --- | --- |
| pack 端到端 @12 µs（8 通道 6 字 span） | 75.7 kHz，丢 9.7% | **82.3 kHz，丢 1.5%** | 76.2 kHz |
| 单变量端到端 @3 µs | 316.8 kHz，丢 5.5% | **334.5 kHz，丢 0.2%** | ~329 kHz |
| 纯探针侧 DISCARD @3 µs（把 USB 那段摘掉） | 345~348 kHz | **349.0 kHz，丢 3 拍** | 346.7 kHz |
| M0 单字标定 @60 MHz | 1.588 µs → 629 kHz | **1.569 µs → 637 kHz** | 1.589 µs |
| 门禁四项（推出 / 收到 / 完成回调 / STAT） | 1005 / 999 / 999 / 0 | **1014 / 1014 / 1014 / 15** | — |

即：**修掉漏缓冲之后，原先被它吃掉的吞吐自己回来了**（pack +8.7%、单变量 +5.6%），
采样热路径一行没动、DLM 零增长（106592 B）。**网页侧不需要改代码** —— `parseDef`/`parseStat`
与固件字节布局本来就逐字段一致，变化只是这些包现在真的会到。

### 新功能：USB→I2C 转发桥（HID `0x36`，分支 `feature/usb-i2c-bridge`）

把探针当"USB 转 I2C 主机"用：网页发一次事务（子地址 + 写数据 + repeated START + 读），
探针在总线上跑完并把数据带回来。**只走 HID、不做 DMA**（I2C 慢且事务小，DMA 与 bulk
那套在这里都不划算），一次 HID 报文装一次事务（写 ≤51 B / 读 ≤54 B），主机轮询 `RESULT`
取结果 —— 事务最长 ~5 ms，只能这么干（HID 中断里绝不能跑）。

| 项 | 值 |
| --- | --- |
| 引脚 | **PA28=SDA(J3[21]) / PA29=SCL(J3[19])**（I2C3）；J3 上唯一一对引出来的硬件 I2C 脚 |
| 档位 | 100 kHz / 400 kHz / 1 MHz（`scl_hz` 是档位选择器，实际值回报在 `actual_scl_hz`） |
| 动作 | STATUS / ENABLE / RESET(总线恢复) / SET_CFG / GET_CFG / XFER / RESULT / SCAN + DBG / PINTEST |
| 上拉 | SCL 板上有 R6 10k；**SDA 没有**（外接 4.7k~10k，或用 `pullup=1` 开内部上拉应急） |

**上板实测**（AT24Cxx @0x50；`python script_test/i2c_bridge_test.py eeprom` 一条命令跑完）：

| 项 | 结果 |
| --- | --- |
| 扫描 0x08..0x77 | 找到 **0x50** |
| 页写 8 B + 回读对账 | **逐字节一致**（A5 5A DE AD BE EF 12 34） |
| 54 B 块读（地址自增） | 读到 EEPROM 里的 `"WELCOM TO RTT…"` |
| 同一笔 54 B 读：100 kHz → 400 kHz | **5243 µs → 1309 µs**（4.0×，与档位一致）；1 MHz 该模块也跟得上 |
| 错误路径 | 无器件 NACK / 参数越界 / 未使能 / 事务中重发 —— 全部按预期报码（`... err` 全绿） |

**踩过的两个坑**（已写进文档）：① 子地址原来按"u32 小端取低位"解释，`addr_len=1` 时发出去的
恒是 0x00（每次读都从地址 0 开始，白查一轮）—— 现在协议就是"字节数组按原序发出"；
② GPIO 接管焊盘必须 `gpiom_set_pin_controller()` + `gpiom_enable_pin_visibility()` 两步，
只做后者的话 `gpio_write_pin()` 写了不出去，现象和"脚没接上"一模一样。

网页侧按 [`docs/web-handoff-i2c-bridge.md`](docs/web-handoff-i2c-bridge.md) 做即可
（协议逐字节、取结果流程、排障三件套、页面形态建议都在里面）。

## 最新进展（2026-09-30）

### 一键回归双绿：SWD（F103ZE）与 RISC-V（HPM6800EVK）

新增一键回归（用法见 [§测试 的一键回归小节](#一键回归2026-09-30)）：
`make regression-swd` / `make regression-riscv`，每阶段/每频率档实时出结果
（`build/regression/*.log + .jsonl`），吞吐 < 基线 ×80% 或误码/丢失非 0 立即
ERROR 退出。两块靶子当天全绿：

**SWD 回归（STM32F103ZE @96MHz，65.4s，14 项全 PASS）**

| 阶段 | 实测 |
| --- | --- |
| SRAM（OpenOCD 20KB load/dump） | 1~60MHz 8 档逐字节 verified；60M xfer 写 **3384** / 读 **2928** KB/s（wall 3101/2595） |
| RTT 交付（探针侧桥） | 20M **1391.5** / 45M **2512.0** / 60M **2954.4** KB/s，全部 LOSSLESS、rd_err=wr_err=0 |
| HSS bench 单变量 | 60/45/30/20M = **604.9 / 530.7 / 420.6 / 320.6** kHz；拟合 **T(µs) = 0.911 + 44.1×(MHz/f_swd)**，R²=0.9999 |
| HSS bench 8 通道 | **88.9** kHz @60M |
| HSS run 端到端 | 单变量@3µs **321.3** kHz（探针跳拍 4.8%）；8 通道@12µs **75.9** kHz，u_hi 误码 **0** |

> 注：bench 单变量 604.9 kHz 与 09-28 定稿的 629.3 kHz 相差 ~4%，是 300-iter 短标定的
> run-to-run 波动（两读数都在 80% floor 之上）；RTT 三档 `wr_err` 本次全 0 —— 09-28 记录的
> 「F103 @60M wr_err=1 既有现象」本次未复现。

**RISC-V 回归（HPM6800EVK / HPM6880，40s，全 PASS）**

| 阶段 | 实测 |
| --- | --- |
| DMI/SBA 引擎 | rbench **1506.4** / wbench **1525.8** KB/s；sbastat sticky 全 0；selfcheck PASS |
| RTT 交付 | **1385.5** KB/s，14.2MB 字节流 **0 丢 0 重** |
| HSS 单字流水完整性 | u_hi 契约 **32364 样本 0 违例**（本节修复前每 32 拍静默坏 1 个） |
| HSS bench | 单变量 **317.3** kHz、8 通道 **27.7** kHz（边际 ≈4.7 µs/字） |
| HSS run | rv 8 字段契约 **PASS**（5.1 kHz）；单变量@10µs **100.5** kHz |

### 二轮代码审查修复（全部上板验证）

完整处置表见 [`docs/代码审查报告.md`](docs/代码审查报告.md)，按价值挑重点：

- 🚨 **RISC-V 单字流水每 32 拍静默毁一个样本（P1，已修）**：`hold_read` 的周期性
  SBCS 检查借 `dmi_read()`，其第一个 `dmi_post` 把上一拍样本当 pending 响应丢掉，
  本拍交付的是 NOP 链上的空值 —— 实测 1324/42408 违例（恰 = 样本数/32，坏值全 0，
  不报任何错）。改成两次原生 `dmi_post` 的流水保持式检查后 **0 违例**，顺带单字
  M0 4.25 → **3.17 µs**（检查从 3 次扫描省成 2 次）。守门脚本
  `script_test/riscv_pipe_integrity_test.py`。
- **raw 通道响应缓冲挡截补全（P1，已修）**：`DAP_Transfer(0x05)` 的 count 是声明
  值、TIMESTAMP 开着 ⇒ 一条 24B 请求可声明 255 项读（响应 2043B 写穿 1KB 缓冲）；
  SWD/JTAG Sequence 同类。现在 Sequence 直接拒绝、Transfer 的 count 按"请求里
  装得下的完整 item 数"钳位。守门脚本 `script_test/dap_raw_boundary_test.py`
  （支持 `--no-target`，无 ARM 靶子也能跑拒绝/钳位/存活）。
- **SPI 桥 DMA 通道泄漏（P2，已修）**：每次 SET_CFG/SET_PROFILE/重 ENABLE 泄漏一个
  DMA 通道，几轮后申请必然失败、桥静默退回轮询 —— 重配先 release；实测 16 轮
  重配后 `tx_dma_cnt` 仍正常。
- **SPI 桥硬件初始化下沉主循环（P2，已修）**：ENABLE/SET_CFG/SET_PROFILE 原先在
  USB 中断里同步做时钟源扫描 + SPI 复位轮询 + DMA 申请（数百 µs，还会打断主循环
  正在跑的 CS 窗口）—— ISR 只置标志；ENABLE 到应答 4ms。
- **scope 引擎被主机 stop 后自愈（已修）**：`s_swd_ready` 不交叉校验
  `riscv_jtag_is_open()`，`CMD_RISCV stop` 之后 bench/采样稳定 -4 且永不自愈 ——
  recheck 挂进链路/bench/采样错误路径三处。
- **一批 P3**：ABORT 现在连 IN 队列一起清、清环挪进临界区；`sb_delay_us` 超长延时
  截断（防 `(int32_t)` 到点判定失效）；scope bench 拒绝运行态调用；6 个 .ps1 补
  UTF-8 BOM + flash.ps1 对 OpenOCD stderr 降级 —— 都是 PowerShell 5.1 与 pwsh 7
  的行为差异，脚本经 `powershell -File`（回归编排器）调用才会踩到，手动 pwsh 跑
  从来不会暴露。

## 快速上手

### 构建

依赖 HPM SDK 1.11.0 环境（含 `rv32imac_zicsr_zifencei_multilib_b_ext-win` 工具链），
默认路径 `E:\sdk_env_v1.11.0`，用环境变量 `HPM_SDK_ENV_DIR` 覆盖。

```bat
make build          :: bootloader + APP
make build-boot     :: 仅 DFU bootloader -> build_xip_evklite\
make build-app      :: 仅 APP（DFU 布局）-> build_dfu_evklite\，产出 _pack.bin/_pack.hex
make clean
```

也可以直接跑板级脚本：`firmware/application_5301/build_dfu_evklite.bat`、
`firmware/bootloader_dfu/build_xip_evklite.bat`。

> **SDK 1.11 兼容说明**：新版 SDK 移除了 `flash_dfu` 构建类型，EVKLite 脚本改用
> `HPM_BUILD_TYPE=flash_xip` + 自定义链接脚本 `linker/flash_dfu_app.ld`，并由 CMake
> 追加 `-DFLASH_XIP=0 -DFLASH_DFU=1`，得到与原 `flash_dfu` 一致的镜像布局
> （APP 头在 `0x80020000`，入口 `0x80020100`）。
>
> APP 固件区尾部两个 4K 扇区（`0x800FE000`/`0x800FF000`）留给参数存储
> （EasyFlash），构建已把 `_flash_size` 收窄 8K，不会被代码占用。


### 烧录

| 场景 | 方法 |
| --- | --- |
| 首次烧录（空片） | J-Link 接 J5：`make flash`（bootloader + APP 一次烧完） |
| 日常只更新 APP | `make flash-app`（J-Link，保留 bootloader） |
| DFU 升级 | 长按 USER KEY 1s 或发 HID `CMD_ENTER_DFU(0xFF)` → 把 `akaLinkPro_App_pack.bin` 拖进虚拟 U 盘 |
| dfu-util | `make dfu`（需自行安装 dfu-util 并加入 PATH） |
| 救砖 | 按住 USER KEY 上电/复位进 ROM ISP，用 hpm_manufacturing_tool 经 USB/UART0 下载 |

> **烧录前提**：APP 运行时 PA04–PA08 被 DAP 占用（就是芯片自身的 JTAG 脚），
> J-Link 连不上，必须先让板子进入 DFU / ISP 模式。
>
> DFU 虚拟盘写入偶尔不触发 bootloader 提交，重新写一次文件即可。


### 测试

```bat
make sram-test   :: STM32F103 SRAM 读写测速（CMSIS-DAP + OpenOCD，1~60 MHz，逐字节校验）
make rtt-test    :: STM32F103 SEGGER RTT 吞吐（OpenOCD rtt server）
make rtt-max     :: RTT 取数上限（轮询跑在 OpenOCD 内部，无 telnet 往返）
make rtt-link    :: 目标运行中 SWD 读的可靠性矩阵
make uart-echo   :: EVKLite CDC 回环快检（先短接 J3.8 <-> J3.10）
make uart-loop   :: EVKLite CDC 全速率回环扫描
```

探针侧 RTT 桥的三个脚本（需要目标板跑 `script_test/stm32f103_rtt_speed`，默认自带 96 MHz 超频）：

```bat
python script_test\rtt_probe_bridge.py COM52 10       :: 桥测速 + 全流零丢包校验（最常用）
python script_test\rtt_probe_bridge.py COM52 10 0 60  :: 同上，第 4 参指定 SWD 档（MHz，0=默认阶梯）
python script_test\rtt_rate_matrix.py COM52           :: 逐档对照：SWD 读速 / 交付率 / 占比 / 谁主导
python script_test\rtt_bridge_sweep.py COM52 --clk=60 :: 调优扫描：时钟 x 块大小 x 丢弃模式
```

#### 一键回归（2026-09-30）

两个编排器把上面的单项测试串成回归，**每阶段/每频率档实时出结果**（控制台 +
`build/regression/<family>.log` 人读 + `.jsonl` 机读）：

| 命令 | 目标 | 内容 |
| --- | --- | --- |
| `make regression-swd` | F103ZE @96MHz | SRAM 1~60M 逐字节校验 + RTT 交付率/零丢（20/45/60M）+ HSS 单·多变量 bench（多档 + 拟合 `T=a+b·MHz/f`）与 run（端到端 + u_hi 误码） |
| `make regression-riscv` | HPM6800EVK | selfcheck + 块读/写基准 + sbastat + RTT 交付/零丢（烧 flood 固件）+ HSS 单字流水完整性/契约/bench（自动换烧 scope 固件） |
| `make regression-swd-bg` / `-riscv-bg` | 同上 | 后台执行，`make regression-swd-log` / `-riscv-log`（或 `-jsonl`）随时看进度 |

判定规则：吞吐 ≥ 基线 × **80%**（`--floor` 可调，基线表在两个脚本头部、全部注明
README 出处）；丢失/误码（RTT lost/dup、HSS u_hi 违例、SBA sticky）**必须为 0**；
任一阶段 FAIL ⇒ 立即停止退出码 2；全程预算 `--budget` 硬超时防卡死。探针 CDC 口
自动探测（VID/PID 匹配），要强制指定用 `make regression-swd COMREG=COM7`。
靶子固件由编排器自动构建/烧录（`--skip-flash` 跳过）。


## 硬件接线（EVKLite）

### HPM5301EVKLite 引脚与接线

### DAP 目标调试口（J5，同时是芯片自身 JTAG）

| 信号 | HPM5301 | J5 | 说明 |
| --- | --- | --- | --- |
| SWCLK / TCK | PA06 | J5.9 | FGPIO 位带输出 |
| SWDIO / TMS | PA07 | J5.7 | 单脚双向（无方向控制脚） |
| TDI | PA05 | J5.5 | 仅 JTAG 模式 |
| TDO | PA04 | J5.13 | 仅 JTAG 模式 |
| nRESET（目标复位） | PA08 | J5.3 | 直出低有效 |
| VTref / GND | — | J5.1 / J5.4·6·8… | 板上 3.3V |

> ⚠️ **J5.15 是这块板子自己的 RESET_N**，固件无法驱动它。用 20 针排线直连目标板
> 时别让目标板的复位网络反灌 J5.15；SWD 目标建议直接用杜邦线取上表信号。

### 板载资源

| 功能 | 引脚 | 位置 | 说明 |
| --- | --- | --- | --- |
| CDC 虚拟串口 TXD | PB08 | J3.5 | 板上丝印 `I2C_SCL`，UART2（2026-09-30 从 UART3/PB15 迁来） |
| CDC 虚拟串口 RXD | PB09 | J3.3 | 板上丝印 `I2C_SDA`，UART2（2026-09-30 从 UART3/PB14 迁来） |
| UART0 console | PA00 / PA01 | J3.36 / J3.38 | `printf` 调试口，115200-8N1 |
| USER KEY | PA03 | 板载按键 | 按下为高；**上电时按住进 ROM ISP**，运行时长按 1s 进 DFU |
| 状态 LED | PA10 | 板载 LED2 | 低电平点亮 |
| USB0 OTG（上行口） | PA24 / PA25 | J1 Type-C | USB 2.0 高速设备 |

J3.3 / J3.5（PB09 / PB08，丝印 I2C_SDA/SCL）在部分固件里曾是 VCOM，现在是空闲
I2C 脚；要换回去只改板级 `board.h` 的 `BOARD_PIN_UART_TX/RX` 与
`BOARD_CDC_UART_*` 即可，应用源码不用动。

### 串口回环测试接线

**必须把 J3.8(TXD) 和 J3.10(RXD) 实际短接**——接逻辑分析仪不算回环。固件侧
（引脚复用、时钟、DMA、CDC 桥）已验证正常，测不到回显时优先查这两根线。

```bat
:: EVKLite CDC 在设备管理器里是复合设备的 MI_01（"USB 串行设备 (COMx)"）
make uart-echo COM=COM7     :: 写 27 字节比对回环
make uart-loop COM=COM7     :: 9600 ~ 10 Mbps 全速率扫描
```

实测：9600 ~ 10 Mbps 全部通过，10 Mbps → 974 KB/s（线速效率 99.7%）。


## 实测记录（时间线）

> 按日期排的实测记录。三个专题大节（RISC-V 调试 / J-Scope / SPI 桥）在这一节之后，
> 各自内部也按天展开；09-29 / 09-30 的内容在这三处与顶部「最新进展」。

### 2026-09-27 · RTT 提速：从 919 KB/s 到 2.9 MB/s，探针侧桥落地

#### SEGGER RTT 吞吐：从 919 KB/s 到 2.9 MB/s

RTT 是**主机轮询**模型：每次取数要 3 个 host↔探针来回（读 WrOff/RdOff → 读环形
缓冲 → 写回 RdOff），而 SRAM 测速是一次大块流水传输，所以两者不可比。实测阶梯
（目标均归一到 64 MHz）：

| 配置 | 吞吐 |
| --- | --- |
| 目标停在复位默认 8 MHz（RTT 生产者在目标侧，此时封顶） | 277 KB/s |
| `make rtt-test`（OpenOCD rtt server） | **919 KB/s** |
| `make rtt-max`（轮询在 OpenOCD 内 + 32 位分块读 + 12 KB 环） | **1140 KB/s** |
| **探针侧 RTT 桥**（`CMD_RTT` 0x31，固件自己轮询 RTT + CDC 转发） | **2527 KB/s（2.47 MB/s）零丢包**；切到 60 MHz 档可到 **2954 KB/s（2.89 MB/s）** |


#### 探针侧 RTT→CDC 桥

把轮询从主机搬到探针固件里，是 J-Link 式 RTT 的做法（参考实现：
[MicroLink](https://github.com/minichao9901) 的同款 5301 工程）：固件自己在主循环里
读 RTT 控制块 → 搬环形缓冲 → 写回 RdOff，数据直接进 CDC 的 `g_uartrx` 环，主机只管
收串口。省掉的正是那 3 个 host↔探针来回。

- **SWD 访问直接用 ARM DAPLink 官方 `swd_host.c`**（见 `firmware/application_5301/src/swd_host/`），
  只裁掉 flash 算法/目标状态机部分，时序逻辑一字未改；平台胶水 `swd_host_port.c`
  把 `SWD_Transfer()` 分派到本工程 `SW_DP.c` 的 `SWD_Read()/SWD_Write()`，即与 DAP
  主机通路同一套按速度预编译的 bit-bang blob。
- **握手用默认低速档、之后再提速**（真实主机也是这个顺序）：`swd_init_debug()` 内部会
  再调一次 `swd_init()` → `DAP_Setup()` 把时钟重置回默认档，所以提速必须放在它之后。
- **背压 + 幂等重试保证不丢不重**：每轮只搬 `min(目标可读, 2048 B, CDC 环剩余空间)`；
  先交付到环再推进目标 RdOff，RdOff 写失败则记下来下轮补写（RdOff 是绝对值，重写无害）。
  实测 2×13.5 MB 全流校验 0 丢包 0 重包。
- 与 DAP 主机通路**互斥**：只在 DAP 空闲 ≥20 ms 时轮询，调试时最多多 ~1 ms 抖动。

启动方式（HID 自定义命令 `CMD_RTT` 0x31）：

```powershell
python script_test\rtt_probe_bridge.py COM52 6 36000   # 自动 boost 目标 + 启动桥 + 测速
```

吞吐随 SWD 时钟上升，但会撞到两侧不同的天花板（详见
[`docs/HPM5301EVKLite_port.md` §5.4](docs/HPM5301EVKLite_port.md#54-调优实测天花板在哪一侧2026-09-27)）：

| 量的是什么 | 数字 |
| --- | --- |
| SWD 侧（纯读目标 SRAM） | 20/30/36/45/60 MHz → 1476/2053/2359/2788/**3312** KB/s |
| 桥的搬运（丢弃模式，不送 CDC）@60 MHz | 3232 KB/s |
| 端到端（CDC 读走）@45 MHz（默认）/ @60 MHz | **2527** / **2954 KB/s，零丢包** |

60 MHz 档原来做完整初始化会失败（换挡瞬态），已用「斜坡换挡 + 换挡后热身 + 失败先清
sticky 错误再判死」修好；但它**长跑偶尔抖动**（约每 5~10 次 10 秒一次），所以默认仍取
稳定的 45 MHz，想要极限速度可用 HID `CMD_RTT` action 7 切 60 MHz（固件带自动降档兜底）。
现在 60 MHz 下的瓶颈已转到 USB/CDC（丢弃 3232 vs 端到端 2954），详见
[`docs/HPM5301EVKLite_port.md` §5.8/§5.9](docs/HPM5301EVKLite_port.md#58-swd-读速-vs-rtt-交付率逐档对照表)。

> ⚠️ 测交付率时主机侧读法影响极大：Windows 上 pyserial 的 `ser.read(n)` 会把主机侧压到
> 2169 KB/s，`ser.readinto(大缓冲)` 才有 2956 KB/s（差 36%）。所有脚本已改用 readinto。

块大小 512 B → 2048 B 多 1.2%（每块固定开销本来就只有 5 次传输）；
`clock_delay` 覆盖无差别；`__inline__` 无收益（`-O3` 已把
`swd_read_block`/`swd_transfer_retry`/`swd_read_word` 全部内联，符号表里已不存在）。

高频档是**间歇性**的，所以固件带三处自愈：启动时从请求档位往下找可用档
（60→45→36→30→20→10）、运行中连续出错则降一档重来（只在重扫也失败时降，否则一次瞬态
就会白白降档）、换挡后先读一次 DP IDCODE 热身并清 sticky 错误；控制块扫描
也从 4 字节小读改成 512 B 重叠窗块读（传输数少一个数量级）。扫描脚本
`script_test/rtt_bridge_sweep.py` 一次跑完「纯 SWD / 丢弃 / 端到端」三段对照。

另外：目标**运行中**时长块 SWD 读会失败（内核抢总线），必须限长分块 + 重试
（桥里默认 512 B/块）。详见
[`script_test/README.md`](script_test/README.md#rtt-测速为什么慢实测结论2026-09-27)。

脚本说明见 [`script_test/README.md`](script_test/README.md)；`sram/rtt` 脚本的工具路径
可用 `OPENOCD_EXE`、`OPENOCD_SCRIPTS`、`HPM_SDK_ENV_DIR` 覆盖。


### 2026-09-28 · SRAM 双口径、瓶颈收口、RISC-V 首秀

#### SRAM 吞吐：主机驱动 vs 纯 SWD 链路

这两个数字**不是一回事**，放一起看才不会被误导：

**① 纯 SWD 链路天花板**（探针内部基准：`CMD_RTT` action 8，读目标 SRAM，
不经 USB、不经主机、不含 RTT 描述符开销）—— 这才是"链路本身能跑多快"：

| SWD 档 | 20 MHz | 30 MHz | 36 MHz | 45 MHz | **60 MHz** |
| --- | --- | --- | --- | --- | --- |
| 纯 SWD 读 | 1469 | 2041 | 2351 | 2766 | **3281 KB/s（3.2 MB/s）** |
| RTT 交付 | 1379 | 1882 | 2171 | 2485 | **2932 KB/s** |

复现：`python script_test\rtt_rate_matrix.py COM52 --sec 5`
（同一张表里还给出交付/读的占比与"谁主导"的判定）。
60/80/100 MHz 都落在同一个 60M blob 上，所以 60 MHz 起就 plateau 了。

**② 主机驱动**（`make sram-test`：OpenOCD `load_image`/`dump_image` 走 CMSIS-DAP
USB 批量端点）—— 比链路天花板低一档，但**不是因为"每趟往返开销"**：OpenOCD 用
4 深 pending FIFO + 异步 URB 已经把每包的固定开销藏掉了，实测把 CMSIS-DAP 包大小
从 512 提到 1024（固件现在按 2×mps 报 `DAP_Info(PKT_SZ)`，块读从 127 ops/包变成
254 ops/包）速度**完全不变**——证据见 `docs/experiment-dap-resp-size.md`。
⇒ 这条路的真正上限是**探针的 SWD 位翻转率**（同档 in-probe 读 3281 KB/s）叠加 USB
搬运，不是往返次数。数字本身受主机栈与目标主频双重影响
（下表为 **目标 STM32F103 @96 MHz** 实测，复现误差 ±1~10%）：

| SWD 时钟 | 写 | 读 |
| --- | --- | --- |
| 1 MHz | 85.6 KB/s | 85.8 KB/s |
| 2 MHz | 176.6 KB/s | 175.4 KB/s |
| 4 MHz | 339.9 KB/s | 336.5 KB/s |
| 10 MHz | 797.6 KB/s | 766.0 KB/s |
| 20 MHz | 1355.9 KB/s | 1287.3 KB/s |
| 36 MHz | 2117.5 KB/s | 1924.8 KB/s |
| 45 MHz | 2420.4 KB/s | 2180.8 KB/s |
| 60 MHz | 2843.9 KB/s | 2443.0 KB/s |

> 这两个口径的差别（60 MHz 档：墙钟读 2.5 MB/s、纯传输读 2.9 MB/s，链路天花板
> 3.3 MB/s），正是"链路天花板 3.3 MB/s"与"主机驱动 2.4 MB/s"看起来矛盾的原因：
> 表①量的是探针把 SWD 数据搬进自己内存的速度，表②量的是主机经 USB 把数据取走
> 的速度 —— 探针同一颗 CPU 要**串行地**干这两件事（一条条处理 DAP 命令），所以
> 两者耗时基本相加。想拿满链路带宽，就得像探针侧 RTT 桥那样**把轮询下沉进固件**
> （见下一节）。

> **口径提醒**：上表是**墙钟**计时（`sram_speed_test.py` 自己掐表），每条
> `load_image`/`dump_image` 都要经一次 telnet 往返（实测 ~1 ms/条），在 20 KB 的
> 量级上占掉约 19%。OpenOCD 自己打印的**纯传输计时**（`dumped N bytes in X s
> (Y KiB/s)`，原作者 `script_test/swd/benchmark_readback.tcl` 用的就是这个口径、
> 64 KB × 10 轮）在同一档位是 **读 2941 / 写 3386 KB/s** ⇒ 读已到 in-probe 链路
> 天花板的 **90%**。两个口径都对，引用时要说清是哪个。

> **`adapter speed` 是档位选择器，不是实际频率**：固件 `Set_Clock_Delay()` 把它
> 映射到 6 个固定引擎（60/45/36/30/20 MHz 的 ASM blob，其余走 `swd_speed_calc()`
> 的延时环）。所以"请求 20 MHz"实际得到的是 20M blob（本机实测读 1469 KB/s），
> **不是** 20 MHz 的精确链路。另有 `clock_accel_mode`（HID `CMD_SET_CONFIG`
> byte[5]，默认关）会把请求值 **×10** —— 于是"请求 20 MHz"会被顶到 60M 引擎，
> 这正是原作者截图里"20 MHz 却跑到 3.6 MB/s"的原因（物理上 20 MHz SWD 的写上限
> 只有 ~1.74 MB/s）。本机用**他本人的脚本**、在 **F103ZET6（64 KB，与他同样的块长）**
> 上复现：60M 引擎下 **写 3471 / 读 2958**，与他 3646/3045 只差 **4.8% / 2.9%**；
> 而字面 `adapter speed 20000` 只有 1487/1400 —— 截图那个数不可能来自 20M 引擎。
> 完整复现方法（含脚本搜索路径的坑）与对照表见 **`docs/upstream-speed-claims.md`**，
> 该文档也记录了 F103ZET6 狂发例程（`build.ps1 -Board ze`，RTT 上行 32 KB，
> 实测交付 2486 KB/s 无损）。

> **回归复测：J-Scope 那串提速改动之后，两条老基线都重测过（无回归）**
>
> 单字快路径那一串改的正是**共用的** `swd_host.c`（TAR 缓存 + 每处 DRW 访问前的失效 +
> 单字流水读），所以把老基线全部重测了一遍（目标同一块 F103ZET6 @96 MHz）：
>
> | | 老基线 | 改动后 | 差 |
> |---|---|---|---|
> | 主机驱动 SRAM 写 @60M（OpenOCD 纯传输口径） | 3386 KB/s | 3319 / 3371 | −2.0% / −0.4% |
> | 主机驱动 SRAM 读 @60M（同上） | 2941 KB/s | 2833 / 2919 | −3.7% / −0.8% |
> | 探针内纯 SWD 块读 @60M（走 `swd_host`） | 3312 KB/s | 3301 KB/s | −0.3% |
> | **RTT 交付率 @60M** | **2954 KB/s 无损** | **2953.7 / 2954.9 KB/s 无损** | **≈0** |
>
> 都在基线自身的复现误差（±1~10%）内。
> - OpenOCD 那条路走 `DAP_SWD_Transfer` → `SWD_Read/SW_Write`，**根本不经过 `swd_host`**，
>   所以它本来也不该被那几个缓存改动影响 —— 但既然动了共用层，就得测而不是推理。
> - **墙钟读数列跑一次一个样**（20 MHz 读两次实测 1306 / 970，摆动 26%），那是 telnet
>   往返被机器负载带的，不是探针；看稳定性要用 OpenOCD 自己的 xfer 计时。
> - RTT 那两次都有 `wr_err=1`（流依然字节级无损）：这是 **F103 在 60 MHz 档上的既有现象**
>   （H743 全档 `wr_err=0`），`docs/HPM5301EVKLite_port.md` 里早有记录，重写 RdOff 是幂等的。


#### 收口（2026-09-28）：瓶颈在**探针侧**，不在目标侧

三个目标、两套取数工具（本探针 vs SEGGER J-Link）跑下来，结论收敛了。

**RTT 交付（探针侧 RTT→CDC 桥，同一套脚本 `rtt_probe_bridge.py` / `rtt_h743_bridge.py`）：**

| SWD 档 | **H743 @480 MHz** | F103 @96 MHz | SEGGER J-Link @50 MHz |
| --- | --- | --- | --- |
| 20 MHz | 1379.6 | 1379 | — |
| 36 MHz | 2162.3 | 2162 | — |
| 45 MHz | 2486.6 | 2484 ~ 2487 | — |
| **60 MHz** | **2931.2 KB/s** | **2932 ~ 2954 KB/s** | 1473.0 KB/s |
| （对照）H743 复位默认 64 MHz 时 | ~700 KB/s，**曲线是平的** ⇒ 那时是目标受限 | — | 692.9 |

**SRAM 主机驱动（64 KB，60000 档，OpenOCD 自带计时/纯传输口径，逐字节校验通过）：**

| 平台 | 写 | 读 |
| --- | --- | --- |
| **H743 @480 MHz** | **3430** | **2689** |
| H743 @64 MHz（复位默认） | 2590 | 2764 |
| F103ZET6 @96 MHz | 3471 | 2958 |
| （对照）原作者 ZET6 截图 | 3645.959 | 3045.310 |

**四条结论：**

1. **两个完全不同的目标撞在同一个数字上**（RTT 交付 2931 vs 2934，差 0.1%；SRAM 写
   3430 vs 3471，差 1.2%）⇒ **瓶颈已经是探针自己的 USB/CPU 吞吐（~2.9 MB/s），
   目标侧不再是瓶颈**。想再快只能动探针（USB 吞吐 / 位翻转引擎）。
2. **目标主频确实重要，但 400 MHz 以上就饱和**：H743 从 64 → 480 MHz，RTT 交付
   693 → 1473（J-Link 口径）；400 与 480 **完全一样**（1472.7 vs 1473.0）。
   ⇒ H743 固件现在**上电自动升到 480 MHz**（`script_test/stm32h743_rtt_speed`，
   照抄厂商 `Stm32_Clock_Init` 序列；坑见源码注释与 §6）。
3. **探针比 J-Link 快一倍**（同目标同主频：2931 vs 1473），J-Link 的 CLI 取数更低
   （692）⇒ SEGGER 那套数字**不能**当"目标侧天花板"的参照，它先撞自己的上限。
4. **SWD 时钟是唯一还线性有效的旋钮**：探针口径下 20→60 MHz 交付 1379→2931 一路上涨
   （60 MHz 偶有长跑抖动，默认仍保守用 45 MHz，见 `rtt_rate_matrix.py`）。

> `script_test/stm32f103_rtt_speed` 固件自己就超频到 **96 MHz**（HSE 8 MHz ×12，
> `main.c` 的 `clock_init()`），所以上面的数字是 96 MHz 目标的实测值；更早的
> 64 MHz 基线（60 MHz 档写 2640 / 读 2376 KB/s）略低 —— 提目标主频只换来约 8%，
> 说明这条通路的瓶颈**不在目标侧**，而在探针（SWD 位翻转 + USB 搬运）。
>
> ⚠️ 这个数字还受**目标机主频**限制（每次 SWD AHB-AP 事务要花几个目标 HCLK）：
> STM32F103 上电默认 HSI 8 MHz 时，无论 SWD 时钟拉到多高都会卡在 ~1.4 MB/s，
> 而 F1 的 `reset halt` 是核心级复位、不清 RCC，所以数字会随目标上电后的状态
> 变化一倍（`sram_speed_test.py` 会打印实测时钟，`--no-boost` 可关闭它的补偿；
> 它现在还会识别"固件已跑在 PLL 上"从而不去动 RCC）。
>


#### RISC-V 走 JTAG（2026-09-28 首秀）：主机侧往返受限，探针侧才有速度

第一次用本探针调 **RISC-V**（HPM6800EVK / HPM6880，只有 JTAG、没有 SWD）。这一块
内容已经长成独立一章 —— 接线与前置条件、**必须烧 ELF 的启动头坑**、引擎与 RTT
交付率、TCK 频率上限分析、复现清单，全部见下面的
**[HPM6800EVK（HPM6880，RISC-V）目标调试](#hpm6800evkhpm6880risc-v目标调试)**，
细节在 [`docs/hpm6800evk-jtag.md`](docs/hpm6800evk-jtag.md)。

一句话结论：主机驱动下每个 abstract command 要一个 USB 往返（97 µs），读速
95.9 KB/s；把搬运下沉进探针固件后 **1504.5 KB/s（9 倍）**，RTT 交付
**1385 KB/s 且字节级零丢包**（28.5 MB，0 丢 0 重）。


## HPM6800EVK（HPM6880，RISC-V）目标调试

第一块用本探针调的 **RISC-V** 目标，也是第一次走 **JTAG-only** 通路（HPM6880 没有
SWD）。它的调试模块是标准的 **RISC-V Debug Module**（DMI + SBA），跟 ARM 的
DAP/AHB-AP 完全不是一回事，所以固件里新增了一整套 `src/riscv/`。

> 完整记录（三个 DTM 时序坑、失败实验的原始读数、逐步复现清单）在
> **[`docs/hpm6800evk-jtag.md`](docs/hpm6800evk-jtag.md)**，本节只放结论。

### 接线与前置条件

| 信号 | 探针（HPM5301EVKLite J5） | HPM6800EVK |
| --- | --- | --- |
| TCK | PA06 / J5.9 | JTAG TCK |
| TMS | PA07 / J5.7 | JTAG TMS |
| TDI | PA05 / J5.5 | JTAG TDI |
| TDO | PA04 / J5.13 | JTAG TDO |
| GND | J5.4·6·8… | GND |

三条必须知道的前提：

1. **探针要先切到 SWD+JTAG 模式**：`python script_test\hpm6800_probe.py set-mode 1`
   （HID `CMD_SET_CONFIG` 的 `output_mode`；0 = SWD+VCOM 会**拒绝 JTAG**）。
   这个设置**只存在 RAM 里，探针一复位/重插就丢**，每次上电都要重设。
2. **排线第 15 脚是探针自己的 `RESET_N`**：`openocd_hpm6800evk_dap.cfg` 里必须
   `reset_config none`，让复位走 DM 的 `ndmreset`。否则 OpenOCD 一复位**把探针自己
   打掉**（实测掉过两次，靠给上游 USB Hub 断电才救回来）。
3. **`adapter speed` 对 JTAG 扫描完全无效** —— JTAG 汇编把 delay 写死传 0，
   改它没有任何效果（真正的旋钮见下面的 TCK 一节）。

### 烧录：**必须烧 ELF，SDK 生成的 `.bin` 里没有启动头**

这是最容易白掉半天的一条。SDK 输出的 `.bin` 里 **`.boot_header` 整段是全 0**，
ROM 认不出来，复位后 PC 停在 boot ROM `0x2001d4c8` 一动不动 —— 看起来像"BOOT 跳线
配错了"，其实跳线 `BOOT0=0 / BOOT1=0` 本来就是对的（NOR 启动）：

| 文件 | `0x80001000`（启动头） |
| --- | --- |
| `demo.elf` 的 `.boot_header` | `bf109000…`（tag `0x009010BF`，正常） |
| 它导出的 `demo.bin` | **全 0** ← 烧这个就不启动 |

```bat
python script_test\hpm6800_flash_target.py     :: 默认就是狂发固件 ELF，并打印复位后 PC 自检
```

烧完的自检三件套：`mdw 0x80001000` = `009010bf`、复位后 `pc` = `0x80003000`、
`mdw 0x1240000` 读到 `"SEGGER RTT"`。

### 速度：主机驱动 vs 探针侧引擎（**9 倍差距**）

| 路径 | 写 | 读 |
| --- | --- | --- |
| OpenOCD `progbuf` 后端（三个后端里最好） | 154.0 KB/s | 95.9 KB/s |
| OpenOCD `sba` 后端 | 87.0 | 85.3 |
| OpenOCD `abstract` 后端 | 14.5 | 14.4 |
| **探针侧 DMI/SBA 引擎**（新增 `src/riscv/`） | **1511.8 KB/s** | **1504.5 KB/s** |

差 9 倍的原因是**往返**：主机驱动下每个 abstract command（最多 4 个字）就要一次
USB 往返（实测 **97 µs**），而 40 µs/字的读速正好等于这个往返 —— 测出来就是
"一个往返换一个字"；`sba`/`abstract` 后端更差。**只有把搬运下沉进探针固件才有速度。**

探针侧的做法：加载一次 `IR = 0x11`（DMI）之后，**一次 DMI 访问 = 一次 41 位 DR 扫描**
（`{op[1:0], data[31:0], addr[6:0]}`），而且响应**滞后一拍**，所以连续的 posted 请求可以
一个字一次扫描地流水，没有任何往返。块搬运走 Debug Module 的 **SBA**（系统总线访问，
硬件自增地址）。

> 关于"上游是不是有个没用上的优化汇编"：**没有**。`JTAG_Sequence()` 一直在调
> `JTAG_Sequence_GPIO_ASM_45M`，且与上游 `akkako/akaLinkPro` **逐指令相同**（只差
> 引脚参数化）。真正的差距是 SWD 那套是 **6 周期/bit**，JTAG 这份是 **19 周期/bit**。
> 本次另写了专用 DMI 扫描汇编 `JTAG_DP_GPIO_ASM_DMI.S`（一次访问收进一个函数、
> 41 位请求在寄存器里移位），把探针侧从 954 一路做到 **1504/1512 KB/s**（见下一节）。

### RTT 交付率：**1385 KB/s，字节级零丢包**

狂发固件在 `script_test/hpm6800evk_rtt_flood/`（`flash_xip`，RTT 上行 32 KB，
`BLOCK_IF_FIFO_FULL`，死循环发 `hello world!\n`），控制块 `_SEGGER_RTT` 在
**0x01240000**（AXI SRAM，探针可直接读写）。

```bat
python script_test\hpm6800_rtt_delivery.py COM5 5     :: 交付率
python script_test\hpm6800_rtt_loss.py COM5 20        :: 字节流丢包校验
```

```
host read 7208960 bytes in 5.08s -> 1385.0 KB/s
bridge: drained=7211008 bytes, polls=3521, moves=3521, rderr=0, wderr=0

host received   28516352 bytes in 20.046s -> 1389.2 KB/s
probe drained   28516352 bytes (polls=13924 moves=13924 rderr=0 wderr=0 zips=0)
probe-host delta: 0 bytes
records=2193565 lost=0 dup=0
```

狂发固件写的是**固定 13 字节记录**，丢一个字节模式必然错位 —— 所以
"2193565 条完整记录、0 丢 0 重、probe-host delta 恰好 0"比计数器更能说明问题：
**28.5 MB 一个字节不差**。

> ⚠️ **测交付率时主机侧读法同样决定结果**：pyserial 的 `ser.read(n)` 每次新分配
> 缓冲，实测把主机侧压到 ~2169 KB/s；交付率一高就变成"主机侧丢字节"的假象
> （探针 `rderr/wderr` 全是 0，流里却少几个字节）。本项目所有测速脚本一律用
> `readinto()` + 复用同一个 1 MB 缓冲，并且**先停桥再收尾巴** —— 生产者停了以后
> 还缺的才算真丢包。

> 还踩过一个**跨后端适配层**的坑，很有代表性：`rtt_write_word()` 的两个后端
> 成功/失败方向相反（`swd_write_word()` 1 = 成功，`riscv_jtag_write_word()` 0 = 成功），
> RISC-V 分支忘了取反 ⇒ **回写 RdOff 成功被判成失败**，桥搬完第一块 2048 B 就永久
> 卡在"幂等补写"分支里打转，连控制块都不再读。它不报错、不崩，只表现为"速率是 0"。
> 定位过程（靠 `s_write_err` 在涨、`s_rd_pend_v` 却是 0 这对矛盾读数）见文档 §5.3。

### TCK 频率：从 16.4 MHz 提到 20.7 MHz，规格上限 25 MHz

一次 DMI 访问 = `idle(8) + 导航(5) + 移位(41)` = **54 TCK**，所以吞吐直接由"每 bit
多少拍"决定：

| 版本 | 每 bit 结构 | 读 | 有效 TCK |
| --- | --- | --- | --- |
| 初版专用汇编 | 低相位 8 条指令专等 TDI 建立 + 高相位 8 拍 nop 等 TDO | 1189.5 KB/s | 16.4 MHz（上限的 66%） |
| **TDI 预置 + 循环展开**（现行） | 低相位只剩 `sw DO_CLR`，TDI 提前到上一位的高相位摆好 | **1504.5 KB/s** | **20.7 MHz（83%）** |

关键点是：**TCK 已经是高的时候，把 `DO_VAL` 写成 `TCK|TDI` 只是改 TDI 电平、
不产生任何边沿**。所以"给下一位摆 TDI"可以提前到上一位的高相位去做，低相位就
退化成一条 store，只受目标最小 TCK 低电平宽度约束（实测 4 拍），不再受 TDI 建立
时间约束（那正是旧结构低相位那 8 拍的来源）。再把整个 32 位位移循环展开，每条位
又省下 `addi`+`bnez` 两拍。

按 54 TCK/字算，25 MHz 下的理论上限是 **1.85 MB/s**，当前 1.5 MB/s 是它的 81%。
剩下的空间在**导航相位**（13 个导航/idle 时钟 × 每个约 10 拍）：用同一招把 TMS 也
提前摆好，理论上还能再要百分之十几。

四个时序旋钮**都已经在最小值上**（每一点都用自检判死活 —— `hpm6800_selfcheck.py`
写已知图案再读回比对校验和，PASS 才算数）：

| 旋钮 | 现值 | 上界判据 |
| --- | --- | --- |
| `DMI_NAV_LOW_NOP` | 6 | **4 → FAIL，读回全 0**（TMS 建立不够） |
| `DMI_NAV_HIGH_NOP` | 4 | 4 通过（配合 `NAV_LOW=8` 时的 4/4 曾 FAIL，说明卡的是 LOW） |
| `DMI_TCK_LOW_NOP` | 4 | **2 → FAIL，校验和 `0x11D9A168`**（TCK 低电平太窄） |
| `DMI_CAP_HIGH_NOP` | 1 | **0 → FAIL**（采样点不够；TDI 预置后已有 8 条指令垫底，补 1 拍正好） |
| `idle`（运行时 `hpm6800_riscv.py delay <n>`） | 8 | **6 只跑得动 6/50 轮就死**；≤4 立刻不应答 |

> ⚠️ 改这段汇编时最坑的一条：45M 汇编里 `andi t4, a4, JTDI_OFFSET` 看着能把
> "取位 + 移位"合成一条，但那只在 **TDI 流已经预先左移过 `JTDI_SHIFT`** 的代码里
> 成立。套到这里（`\in` 是原始请求寄存器、bit0 才是当前位）会去取
> `bit[JTDI_SHIFT]`，于是请求被整串移成 0（`op=NOP`）—— **基准照跑、计数器全干净、
> idcode/dtmcs 也还是对的，只有自检能抓到读回全 0**。这就是自检必须当门禁的原因。

### 复现清单

```bat
:: 1) 探针：编译 + DFU 升级 + 切 JTAG 模式
cd firmware\application_5301
python ..\..\script_test\hpm6800_flash_probe.py
python ..\..\script_test\hpm6800_probe.py set-mode 1

:: 2) 目标：烧狂发固件（**必须 ELF**）
python script_test\hpm6800_flash_target.py

:: 3) 引擎自检与基准
python script_test\hpm6800_riscv.py open
python script_test\hpm6800_selfcheck.py                         :: PASS 才算数
python script_test\hpm6800_riscv.py rbench 0x1200000 1024 50
python script_test\hpm6800_riscv.py wbench 0x1200000 1024 50    :: 见下面 ⚠
python script_test\sram_speed_hpm6800.py --size 65536 --regions axi   :: 主机侧口径

:: 4) RTT 交付率
python script_test\hpm6800_rtt_delivery.py COM5 5
python script_test\hpm6800_rtt_loss.py COM5 10
python script_test\hpm6800_rtt_diag.py COM5 3                   :: 出问题时读探针 RAM 定位
python script_test\hpm6800_cdc_check.py COM5                    :: 拆 CDC 那一跳

:: 5) 时序扫描（每点一次构建 + 烧写 + 自检，约 2 分钟）
powershell -File script_test\hpm6800_timing_sweep.ps1
```

> ⚠️ **写基准的地址就是目标自己的 RAM**：`0x1200000` 是狂发固件 `.bss` 的起点，
> `wbench` 会把目标正在用的变量整片覆盖，目标随后就不产数据了（现象是 RTT 桥
> poll 几万次全是空环、交付塌到 3 KB/s，而**读回校验和仍然是对的**，只有速率会
> 暴露它）。做完写基准确认要么换空闲 scratch 地址，要么重烧一次目标。

HID 侧接口：`CMD_RISCV`（**0x33** —— 原本是 0x32，因网页侧 SCOPE 占了 0x32 而让位；动作见
[`Custom HID Protocol.md`](firmware/application_5301/Custom%20HID%20Protocol.md)）；
RTT 桥切目标类型用 `CMD_RTT`（0x31）的 action 10。


## J-Scope 波形（探针侧 HSS 采样）

类 SEGGER J-Scope 的**变量示波器**：探针自己按固定周期用 SWD 读目标 RAM 里 1~8 个变量，
组 512 B 自描述包，从 interface 0 上那个**原本闲置的 bulk IN `0x83`**（SWO 端点，
`SWO_STREAM=0` 所以一直没人写过）推给主机。**目标固件一行都不用改** —— 变量地址来自
目标 `.elf` 的 DWARF。上位机是另一个仓库
[web-serial-rtt-tools](https://github.com/minichao9901/web-serial-rtt-tools) 的
「J-Scope 波形」页（WebUSB，**在线直接打开**：<https://minichao9901.github.io/web-serial-rtt-tools/>）。

控制面 HID `CMD_SCOPE 0x32`（形状照抄 0x31），数据面 `0x83`。协议与状态字见
[`Custom HID Protocol.md`](firmware/application_5301/Custom%20HID%20Protocol.md) 第 16 条。

### 实测（8 通道 f32/u32/u16/u8 混排，64 字节一帧 → 22 B/样本）

| 口径 | 结果 |
| --- | --- |
| **M0 标定 @45 MHz**（一个 span、24 B） | 13.49 µs/样本 → **74.2 kHz** |
| **M0 标定 @60 MHz** | 11.55 µs/样本 → **86.5 kHz** |
| 端到端 25 kHz 采集 | **99.7% 交付**（丢 268/78258，全是启动瞬态） |
| 端到端 50 kHz 采集 | 1.10 MB/s，96.8% 交付（**宿主读速限制**，见下） |

### 三个必须知道的实测结论

1. **靶子的主频决定一切，不是探针。** AHB-AP 每次读都要花目标侧几个 HCLK：同一份靶子固件
   跑 HSI 8 MHz 时块读封顶 **1.47 MB/s（0.68 µs/字节）**，而且 45 MHz 与 30 MHz 的读数
   **一模一样**（目标已饱和）；改成 HSE ×12 = **96 MHz** 后同一路径是 **3.37 MB/s
   （0.297 µs/字节）**，SWD 时钟才重新变成线性有效的旋钮。拿 8 MHz 的靶子量采样率，
   量到的是靶子的上限（≈46 kHz）。→ 项目里那份靶子固件已经改成 96 MHz：
   [`script_test/stm32f103_scope/`](script_test/stm32f103_scope)。
2. **热路径是"一次采样 = 按 span 块读"**：变量按地址排序，间隙 ≤ 阈值就并成一个 span。
   一次 span 块读的传输数是 **N+2**（TAR + prime + (N-1)×DRW + RDBUFF），这已是 AHB-AP
   的理论最小 —— `swd_host.c` 里 CSW 与 DP_SELECT 都带缓存，第二次起写 CSW 是**零传输**。
   真正能省的只有搬运：span 内变量首尾相接时，**span 的字节序与帧内布局逐字节相同**，
   于是零拷贝直读进包（有填充字节的结构体不能直读 —— 帧内紧凑排会把 `u_hi` 放到 18
   而它在内存里是 20，硬直读会写错位置，这个坑已经踩过并写进代码注释）。
3. **包缓冲的账不能"拿不到就退回 0 号"**。那样会往**在飞**的缓冲里写数据，再对它
   `usbd_ep_start_write` 会因端点忙而**静默不启动**（既不发也不回调），于是
   `s_if_count` 只增不减、缓冲永远还不回来 —— 实测 `txDone=912/1136` 而"无缓冲丢样本"
   却等于每包一次，打包率掉到 1/4 还丢 seq。现在拿不到就丢拍并如实计数，等回调还回来。

> ⚠️ **测交付率时主机侧读法同样决定结果**：`usb.read(N)` 会一直等到凑满 N 字节才返回，
> 读 16 KB = 32 个包 = 14 ms，这期间探针的包缓冲早被填满并开始丢拍 —— 看起来像固件丢数据。
> 另外**读线程必须在 START 之前起来**（启动后那段没人读的时间的账会全记在 dropped 上，
> 实测 25 kHz 下 dropped=3192 ≈ 120 ms × 25 kHz − 8 个缓冲）。同一个坑在 RTT 交付率
> 脚本里已经踩过一次，见 [`docs/hpm6800evk-jtag.md`](docs/hpm6800evk-jtag.md) §5.3。
>
> 还有第三层：**读线程里只搬字节、解析放到窗口之后**。早先在读线程里直接解析每个包
> （建 dict + 496 B 的 `bytes`），GIL 上与主线程抢，Python 一停顿超过缓冲深度就真丢数据，
> 而且账会算到固件头上（记成 `s_usb_drop`）。改掉之后单变量 5 µs 档的丢包从 3.3% 降到 1.9%。
> 更根本的是：**`s_dropped`（探针跳拍）与 `s_usb_drop`（包缓冲耗尽）必须分开看** ——
> 前者是探针 CPU 的账，后者是主机排空的账，两者要的解法完全不同（脚本现在两个都报）。

4. **每次 SWD 传输的成本已经量到底：`36.2 × 指令/bit + 184 周期`**（实测拟合，见下表）。
   60 MHz 档是 401 周期/次：其中 `47 bit × 6 指令 = 282` 是位翻转，**剩下 184 周期
   是每次传输的固定开销（占 46%）—— 在 ACK/转向相位、blob 的进出场与 FGPIO 总线延迟上，
   每次传输都逃不掉**。一个样本 = 9 次传输（6 字的 span：TAR + prime + 5×DRW + RDBUFF）
   ≈ 9 × 401 + 组帧 ≈ 3990 周期 = **11.09 µs → 90.2 kHz**。

   **各档实测**（`script_test/scope_hss_test.py bench`，单变量 u32 = 3 次传输 /
   8 通道 6 字 span = 9 次传输；MCHTMR 24 MHz）：

   | SWD 档 | 指令/bit | 单变量（3 次传输） | `g_pack`（9 次传输） | 每次传输边际 |
   |---|---|---|---|---|
   | **60M** | 6 | **4.503 µs → 222 kHz** | **11.193 µs → 89.3 kHz** | 1.115 µs（401 周期） |
   | 45M | 8 | 5.244 µs → 191 kHz | 13.092 µs → 76.4 kHz | 1.308 µs（471） |
   | 30M | 12 | 6.849 µs → 146 kHz | 17.140 µs → 58.3 kHz | 1.715 µs（617） |
   | 20M | 18 | 9.298 µs → 108 kHz | 23.213 µs → 43.1 kHz | 2.319 µs（835） |

   线性拟合 `T = 36.2 × k + 184`（k = 指令/bit）在 6/8/12/18 四点上误差 <1%。

   **⇒ blob 这条路的账算清楚了：每减 1 条指令/bit 只省 36 周期/次传输。**
   - **6 → 5 需要 funnel shift（`fsri`）**：数据相位现在是 `bexti` + `or` + `rori`
     三条（掩码到 bit0、合并、循环右移），换成 `bexti` + `fsri` 正好两条。但
     `fsri` 要 Zbb 的广义移位，**本工程用的 `riscv32-unknown-elf-gcc 11.1.0` 直接
     报 `unrecognized opcode`**（GCC 11 的 binutils 还没收这条），而且 Andes 核是否
     实现它也无从验证 —— 只能靠 `.insn` 手搓编码去赌，赌输就是采样循环里一条
     非法指令把探针挂死。**收益 +7%（单变量）/ +9%（6 字 span），不值这个风险。**
   - **6 → 4 结构上不可能**：自然位序合并至少 3 条（掩码、移位、并入），加上 2 条
     CLK 边沿 + 1 条采样 = 6 条已经是这个结构的floor。要 4 条就必须让"掩码 + 并入"
     合成一条，而那要求 SWDIO 落在 DI 寄存器的 **bit 0 或 bit 31**（好接 `fsri`）——
     EVKLite 上 SWDIO = PA07（bit 7），`FGPIO` 的 `DI[p].VALUE` 就是一个引脚一位的
     32 位寄存器，没有"单引脚视图"可以借。**结论：现在的 6 指令/bit 就是最优解。**
   - 真正能动的是**184 周期那个固定开销**和**传输次数**：单变量 3 次传输里有 2 次是
     纯固定开销，9 次的 6 字 span 里固定开销占 46%。

   > 试过把 9 次传输批成一条 CMSIS-DAP `DAP_TransferBlock`（指望省掉 `swd_host` 的 C
   > 调用链）：**实测反而慢 6.5%**（12.31 vs 11.55 µs）—— 那 184 周期不在调用链里。
   > 代码留在 `scope_sampler.c` 作对照，**不要启用**。
   >
   > 顺带纠正先前的猜测：块读的每字成本**不随块长摊薄**（1024 B 与 2048 B 都是
   > 1.18 µs/字）⇒ 它是"每次传输"而不是"每块"的开销。

   **每次采样 ≈ `(字数 + 3) × 401 周期` + 框架开销**。那 3 次固定传输
   （TAR 写 + prime 读 + RDBUFF 读）对 6 字的 span 就是 **34% 的时间** —— 小 span 尤其贵。
   **单变量 u32 更极端：3 次传输里 2 次是"取结果"，真正的读只有 1 次。**

   > 🚨 **AP 写比 AP 读贵得多：656 周期 vs 401**。从"去掉一次 TAR 写省下 1.82 µs、
   > 而一次读传输只要 1.115 µs"反推出来的 —— 多出来的 ~255 周期是 posted write 的
   > 完成延迟（写完之后下一次 AP 访问要等它落地）。所以**采样循环里每一次"写"都
   > 值得盯**：省掉一次写等于省掉 1.6 次读。

   所以**天花板由 span 字数决定**，把纯传输量出来就一目了然
   （`rtt_read_bytes` 循环，无组帧）：

   | span | 纯传输 | 上限 |
   |---|---|---|
   | 2 字（8 B） | 5.82 µs | 172 kHz |
   | **4 字（16 B）** | **7.96 µs** | **126 kHz** |
   | 5 字（20 B） | ~9.2 µs | ~109 kHz |
   | **6 字（24 B，g_pack 那份布局）** | **10.30 µs** | **97 kHz** ← 墙 |
   | 8 字（32 B） | 12.65 µs | 79 kHz |

   **单变量 u32 的实测** —— 分水岭在"抱住 TAR"那条快路径（见下节）：

   | | 结果 |
   |---|---|
   | M0 探针侧能力 @60 MHz，**旧**（3 次传输） | 4.503 µs/样本 → 222 kHz |
   | M0 探针侧能力 @60 MHz，**2 次传输** | 2.681 µs/样本 → 373 kHz |
   | M0 探针侧能力 @60 MHz，**1 次传输（流水线）** | **1.589 µs/样本 → 629 kHz** |
   | M0 @45 MHz（新） | 3.140 µs → 318.5 kHz（档位确实起作用） |
   | **实测零丢可持续（端到端）** | **251 kHz**（4 µs 周期 + 关掉 CDC 桥，丢 0.3%） |
   | 硬推 3 µs 周期 | 309 kHz，但丢 7.8%（其中探针侧约 3.7%）—— 拐点在 3 与 4 µs 之间 |
   | 同上（251 kHz 档）但 CDC 桥开着 | 242 kHz（丢 3.6%）—— **差距全在探针主循环**，见下节 |
   | 5 µs 周期（旧时代的"零丢"档） | 200.9 kHz，丢 0.3% |

   > ⚠️ **"单变量时档位不起作用"是个假象（已修）**：早先量到 1/20/45/60 MHz 读数
   > 一模一样，于是写成"时间全在固定开销上、位翻转不是瓶颈"。**真相是档位根本没换**：
   > `rtt_bridge_set_swd_clock()` 在桥那一侧链路没就绪时只把值**记下来**、不装载 blob，
   > 而采样器有自己一份 `s_swd_ready`；桥那份被 `rtt_bridge_stop()` / 主机碰 DAP
   > （`rtt_bridge_note_dap_activity()`）清掉后，采样器这份还是 1 ⇒ 既不重新初始化、
   > 也拿不到新档，**状态字里却报着新频率**。从慢档跳回快档时还会因为少了
   > `rtt_swd_init()` 里那段 20 MHz 斜坡而在第一次访问就 -4。
   > 现在采样器改用 `rtt_bridge_request_swd_clock()`（换挡一律走"下次重新初始化"），
   > 并且 `scope_link_ready()` 要同时看桥那份链路状态。修完 60 MHz 197.8 kHz、
   > 45 MHz 166.7 kHz —— 档位立刻能看出来了。
   >
   > **效率提示**：1 个 u32 = 3 次传输换 4 字节；**2 个相邻 u32 = 4 次传输换 8 字节**
   > （≈5.6 µs → 178 kHz 采两个通道）。多通道时"变量挨在一起"极其划算 ——
   > 每加一个相邻的 u32 只多 ~1.1 µs（22 个通道才到 24 字节）。
   **⇒ 6 字的 span 物理上到不了 100 kHz**（光传输就 9 × 401 = 3610 周期）。要 100 kHz+
   只能把被采样的变量排得更紧（少一个字就多 10%）。

   已经做掉的框架优化（86.6 → **90.2 kHz**）：
   - `swd_read_block4()`：span 天然 4 字节对齐、不跨页，跳过 `swd_read_memory()` 的头尾
     字节处理与分页循环，也跳过 `rtt_read_bytes()` 的分块循环；
   - 按变量宽度展开赋值代替 `memcpy()` —— 1/2/4 字节的 memcpy 会真的走一次函数调用，
     而一个样本有 8 个变量，实测这是框架开销的大头；
   - **零拷贝直读**：span 内变量首尾相接且 4 字节对齐时，直接把 SWD 读进包里该样本的槽位，
     省掉 `s_stage` 中转和一次 `memcpy` 调用（M0 4.546 → **4.478 µs**）；
   - 热路径上**去掉逐拍诊断计时**（`mchtmr_now()` 的 volatile 读在 20 万拍/秒这个量级上是实打实的开销）；
   - 周期换算成 MCHTMR tick **只在配置时做一次**，不再每拍做一次 64 位除法。

   > **试过但走不通的一条路（记下来免得再想）**：把 RDBUFF 读并进下一样本的流水
   > （指望 9 → 8 次传输）。**不成立** —— AP 的 DRW 读返回的是"上一次 **DRW 读**"的数据，
   > 而 RDBUFF 是 **DP** 读、不推进这条链。所以省掉 RDBUFF 读拿到的是**重复值**（上一次
   > DRW 的结果），不是缺的那个末字。**N+2 次传输是单次采样的下限。**
   >
   > 另外 `DAP_Data.clock_delay` 压到 0（`SCOPE_FLAG_DELAY0`）实测只有 0.2%、在噪声内
   > —— 高速档本来就设成 1，这条路也是死的。

### 单字快路径：**抱住 TAR，3 次传输压到 2 次（已做）**

上面把账算到"每次传输 401 周期、AP 写还要 656"这个粒度，于是**单变量 u32 的 3 次传输
里有 2 次纯属"取结果"**（TAR 写 + prime 读 + RDBUFF 读），而真正发起内存读的只有 1 次。
这三次的来历是 AHB-AP 的两条规矩：

1. **TAR 每次都要重写**，因为 CSW 里 `AddrInc=1`（自增），读完一个字之后 TAR 已经
   变成 `addr+4`。而 `swd_host.c` 原来只缓存了 CSW 与 DP_SELECT，**没有缓存 TAR**。
2. **AP 读是 posted 的**：DRW 读回来的是"上一次 DRW 读"的结果，所以每次采样都得
   配一次 RDBUFF 才能把当前值取出来。

**做法**（`swd_host.c`）：给单字 span 把 CSW 切成 `AddrInc=0`，并给 `swd_write_ap(AP_TAR)`
加缓存 —— 地址没变就整趟跳过。分两层，都在用：

| | 每次采样 | 探针侧上限 |
|---|---|---|
| 旧（3 次传输，含一次 AP 写） | 4.503 µs | 222 kHz |
| **① `swd_read_word_held()`：2 次传输**（DRW + RDBUFF） | 2.681 µs | 373 kHz |
| **② `swd_read_word_hold_prepare()` + `swd_read_word_pipe()`：1 次传输** | **1.589 µs** | **629 kHz（+183%）** |

②是把它当**流水线**用：每拍只发一次 DRW 读，而它返回的是**上一次** DRW 读的结果
（AP 读是 posted 的），于是把拿到的值**回填进上一拍自己的槽位** —— 时间戳、帧布局、
包边界全都不用动，只是写入晚了一拍。包里最后一拍的值由 `scope_pipe_flush()`
在推包前补一次读收回来（每 124 拍一次，摊到每拍 1/124 次传输）。

实测端到端（关掉 CDC 桥，单变量 u32）：

| 周期 | 端到端交付 | 丢包 | 谁的账 |
|---|---|---|---|
| **3 µs** | **~320–329 kHz** | 2.7–4.7% | **100% 主机读法**（探针侧只丢 13 拍 / 139 万） |
| 4 µs | 243–251 kHz | 0.3–3.3% | 同上 |
| 2 µs | 443–446 kHz | 11–12% | 探针侧 ~8–11%（拐点在 2 与 3 µs 之间） |

**本次会话：167 kHz（6 µs 周期）→ 333 kHz（3 µs 周期，探针侧零丢），约 2×。**

> ⚠️ **守卫是 `s_nspans == 1`，不是"所有 span 都是单字"**。两个远离的单字 span 交替读
> 时每拍都要重写 TAR，而这条路的 TAR 写走 `swd_write_ap`（写 + RDBUFF 收尾 = 2 次传输），
> 比 `swd_read_block` 的裸 TAR 写（1 次）还贵 —— 实测从 8.78 µs 变成 11.24 µs（**-31%**）。
> `--set two` 就是守这条守卫的用例（修好之后 8.78 µs）。
>
> ⚠️ **TAR/CSW 缓存的失效点**：AP 的 `AddrInc=1` 会把 TAR 带跑，所以每一处 DRW 访问
> （读或写、块读还是单字、成功还是失败）**之前**都要把 `tarp_ok` 置 0；
> 主机自己碰过 DAP 之后（`rtt_bridge_note_dap_activity()`）还要把 select/csw/tar
> 三个影子寄存器一起作废 —— 主机那条路走 `SWD_Read/SWD_Write`，完全绕过 swd_host。
> 方向要保守：多写一次 TAR 只是慢，判反了就是读到**别的地址**。
>
> ⚠️ **流水线的正确性靠三件事**：读失败时清空管线（那一拍的值会被写进上一拍的槽位，
> 而上一拍的值永远收不回来）；推包前必须 flush（**收不回来也照推** —— 那一拍的值是旧的，
> 但绝不能把"包已满"这个状态留着，否则下一拍的落点会算到帧区外面去）；
> START 时管线置空。验法是看 tick 斜率是否精确（50/100/500 µs 三个周期实测
> 0.5/1/5，且 `dropped = 0`），拍错一拍斜率立刻就不对了。

### 下一个杠杆：**打破 1 µs 的周期粒度**

采样现在只要 1.589 µs，而 `period_us` 是整数微秒 ⇒ 3 µs 与 2 µs 之间没有档位可用，
而拐点正好落在中间（3 µs 零丢、2 µs 丢 8–11%）。要吃到 2.3~2.8 µs 这种中间值，
得让协议支持亚微秒周期（加一个 `period_ns` 字段或"周期以 1/4 µs 为单位"的 flag），
并把时间轴一起改 —— 这需要网页侧配合。

**USB 侧**：250 kHz × 4 B ≈ 1.03 MB/s 还算安全；333 kHz 就是 1.37 MB/s，

  接近命令行 pyusb 读法的天花板（~1.7 MB/s），网页侧多条 transferIn 在飞才稳。

### 端到端 vs 探针能力：**差在主循环，不在 SWD**

M0 标定量的是**纯采样循环空转**，不含主循环其余部分、不含推包、不含 USB。
（下面这段是 CDC 桥那一轮的历史账，数字还是旧的 4.478 µs 采样；单字流水读之后
采样只要 1.589 µs，主循环那几百周期早就不是瓶颈了。）

```
5 µs 周期 = 1800 周期预算
一次采样  = 1612 周期（4.478 µs）   ← 与 SWD 时钟档位几乎无关
余量      =  188 周期               ← 主循环其余部分必须挤进这 188 周期
```

于是逐项砍主循环的非采样开销：

| 改动 | 5 µs 周期端到端 | 丢包 |
| --- | --- | --- |
| 起点（6 µs 周期，零丢） | 167 kHz | 0.3% |
| 5 µs 周期，什么都不改 | 170 kHz | 15.3% |
| **关掉 CDC/串口桥**（HID 0x34，见下） | 183.9 kHz | 8.4% |
| + 按键轮询分频（1 秒的按住时长不需要每轮读 GPIO+MCHTMR） | 186.5 kHz | 7.1% |
| + 周期 tick 预换算（省掉每拍一次 64 位除法） | **194.5 kHz** | 3.1% |
| + 读数脚本改成窗口后解析（见下"主机侧读法"） | **197.4 kHz** | **1.9%** |

剩下那 1.9% 里只有一小部分是探针跳拍：**`SCOPE_FLAG_DISCARD`（只采样不推 USB）
下 5 µs 周期是 1.0% 丢拍、6 µs 周期是 0.0%**。

> 这一段到此为止（5 µs 周期 197 kHz）。**后面单字快路径把一次采样从 4.478 µs 压到
> 2.681 µs，于是 4 µs 周期成了新的零丢档 → 251 kHz**；主循环那几百周期不再是瓶颈，
> 真正的限制变成了"周期只能是整数微秒"。见前面的「单字快路径」一节。

**CDC/串口桥开关（HID `0x34`，协议见 `Custom HID Protocol.md` 第 18 条）**
主循环每轮都要服务 CDC 桥：读一次 DMA 的 `DSTADDR`、两次 `disable_global_irq`、
三次环形缓冲查询 —— 正是上面说的那几百周期。现在有两种关法：

- **`SCOPE` flags bit5（`SCOPE_FLAG_CDC_OFF`，推荐）**：采样期间自动关、停采样自动恢复。
  网页只要在已有的 CONFIG 报文 flags 里加一位，不用自己管状态。只恢复"自己关过的那一次"，
  不会覆盖手动关掉的状态。
- **HID `0x34` 手动总开关**：`action=1` 带 0/1 设置，`action=0` 查状态（bit0 = 桥是开的）。
  给面板做显式勾选框用。状态不持久化，探针复位即恢复为开。

代价：暂停期间 **COM 口不通**（CDC 的 bulk OUT 被 NAK，主机自行重试），RTT-over-USB 也停。
采样数据走的是另一条 bulk IN `0x83`，与本开关无关；而且采样器与 RTT 桥本来就互斥
（`SCOPE` 的启动分支会先 `rtt_bridge_stop()`）。恢复时会丢掉暂停期间积压的串口数据并把
DMA 定位追平，不会灌一整圈陈旧字节给主机。

### 复测记录：F103ZE（512K flash / 64K RAM）+ 好线材

换到 ZE 之后**探针侧的数字和 C8 上完全一样** —— 说明瓶颈在探针 CPU，跟目标板/线材无关：

| 变量组 | span | M0 @60 MHz（2026-09-28 定稿） | 早期的 C8 数据 |
|---|---|---|---|
| 单个 u32 | 1 | **1.589 µs → 629.3 kHz**（单字流水读） | 4.536 µs → 220.5 kHz |
| `g_pack` 8 通道 | 1 | **11.193 µs → 89.3 kHz** | 11.092 µs → 90.2 kHz |
| `cross` 8 通道（跨 3 span） | 3 | 25.6 µs → **39.1 kHz** | 预测 25.7 µs ✓ |
| `mixed`（3 字 span + 单字 span） | 2 | 11.372 µs → 87.9 kHz（**不走**快路径） | — |
| `two`（两个远离的单字 span） | 2 | **8.780 µs → 113.9 kHz**（**不走**快路径；守卫写错会变 11.24 µs） | — |

**好线材的收益在"长跑稳定"上**：60 MHz 档连续跑 20 s，`swdErr = 0`（一次 SWD 读错都没有）。

> ⚠️ 早期那条"60 MHz 与 45 MHz 都是 1083 KB/s、都丢 4.7% ⇒ 跟 SWD 无关"的结论**别再用**：
> 那次两档读数一样是因为**档位压根没换**（见上一节的 bug），而不是"USB 背压盖过了 SWD"。
> 修完之后同样的对比是 60 MHz 197.8 kHz / 45 MHz 166.7 kHz，档位差 19%。

**探针与 USB 两段要分开看**（用 `SCOPE_FLAG_DISCARD` 把 USB 那段摘掉即可量出探针本体）：

| 变量组 | 周期 | 端到端（带 USB） | 丢包 | 其中 USB 缓冲耗尽 | **纯采样（DISCARD）** |
|---|---|---|---|---|---|
| 单个 u32 | **3 µs** | **~329 kHz** | 2.7% | **100%**（探针侧只丢 13 拍 / 139 万） | 346.7 kHz，丢 **0.001%** |
| 单个 u32 | 4 µs | 243~251 kHz | 0.3~3.3% | 100% | — |
| 单个 u32 | 2 µs | 443~446 kHz | 11~12% | 1~25% | 493.8 kHz，丢 **4.7%** |
| `g_pack` 8 通道 | 12 µs | 76.4 kHz | 8.8% | **100%** | 86.6 kHz，丢 **0.0%** |
| `cross` 3 span | 30 µs | 33.4 kHz | 0.2% | 100% | — |

即：**单变量那条路的探针本体已经贴着 629 kHz**（1 次传输/拍），3 µs 周期下探针侧
只丢 13 拍 / 139 万；端到端里的丢包全是命令行 pyusb 读法排空不及（读缓冲 64 KB 比
8 KB 好一截）。而 `pack`（每包只装 20 个样本、要 ~1.7 MB/s）曾经**卡在 USB 包率上**
（~3500 包/s ≈ 1.75 MB/s）—— 2026-10-02 定位到真因不是包率，而是 **DWC2 端点只认一笔
在飞、后推的包被静默拒绝并把缓冲占死**（二轮审查 N1，见顶部「最新进展」）：改成
"同一时刻只放一笔在飞 + 回调接踢"的发送队列后，pack 端到端 **75.7 → 82.3 kHz**
（丢 9.7% → 1.5%）、单变量 3 µs **316.8 → 334.5 kHz**（丢 5.5% → 0.2%），
纯探针侧 DISCARD 仍是 349 kHz ⇒ **热路径没有变化，涨的是原先被漏缓冲吃掉的那部分**。

> 🚨 **换板子要改链接脚本**：scope 例程原本只有 C8（64K/20K）的 ld。现在 `build.ps1` /
> `flash.ps1` 都带 `-Board ze|c8`，**默认 ze**、产物固定落在 `build\`（check.py 认这个路径）。
> ZE 与 C8 的变量地址**实测完全一致**（同一份链接布局），所以测试脚本里的地址不用改。
### RISC-V / JTAG 目标（HPM6800EVK）：同一套 J-Scope，只换传输后端

采样逻辑（计划、帧布局、组包、丢拍统计）与目标无关，**只有 4 个原语不同**（块读 /
抱住准备 / 流水读 / 链路初始化），所以做成了传输后端分派。RISC-V 侧走 Debug Module 的
**SBA**（系统总线访问），与 SWD 那边同构：

| | SWD/ARM | RISC-V/JTAG |
| --- | --- | --- |
| 单字快路径 | CSW 关自增 + 缓存 AP.TAR + posted DRW 流水（每拍 1 次传输） | SBA 抱在固定地址（`sbcs` 关自增）+ 每拍 **1 次 DMI 扫描** |
| 多字 span | AHB-AP 块读（自增） | SBA 块读（`sbautoincrement` + posted 扫描，一个词一次扫描） |

实测（HPM6800EVK / HPM6880，目标变量块取 `0x01240000` 非缓存区；探针 HPM5301EVKLite）：

| 配置 | 每样本 | 上限 | 对照（SWD @60M） |
| --- | --- | --- | --- |
| 单变量 u32（J-Scope 快路径：SBA 抱地址 + posted 扫描） | **3.15 µs**（09-30 修复周期性检查后；修复前 4.25 µs 且每 32 拍坏 1 个样本） | **317 kHz** | 1.588 µs / 629 kHz |
| 8 通道连续 u32（32 B，一个 span） | **36.1 µs**（09-30 回归；09-29 为 36.6 µs） | **27.7 kHz** | 11.19 µs / 89.3 kHz |

> 单字那条已经**贴到 DMI 扫描的时序下限**（一次 54 TCK 扫描 ≈ 2.7 µs，等于块读里
> 每个词的耗时），比"每拍 4 次扫描"的裸单字 SBA 路径（25.6 µs）快 **9 倍**。
> 多通道那条原先每次块读要白付 6 次扫描（重配 `SBCS` 4 次 + 收尾读回清错 2 次），
> 改成"配置变了才写、上一次失败过才清错"后 45.64 → 30.81 µs；**本轮又主动加回 2 次
> 扫描**（收尾回读 SBCS 查 sticky 错误，见下）→ 36.6 µs，用 +19% 换掉一类**静默故障**。（09-30：单字路径的检查改成流水保持式后
  不再付这笔账 —— 4.25 → 3.15 µs，每 32 拍毁样本的 bug 一并修掉，见顶部最新进展。）
> 整层顺带变快：1 KB 块读 1504.5 → **1518 KB/s**、块写 → **1551 KB/s**、
> RTT 交付 1389 → **1405 KB/s**（丢包校验仍是 0 丢 0 重）。
> 再往上就要动 DMI 扫描汇编（现在约 18 指令/bit ≈ 20 MHz TCK；SWD blob 是 6 指令/bit），
> 那是要过 `hpm6800_selfcheck.py` 门禁的深水区。

#### 🚨 SBA 的 `sbbusyerror` 会**静默**掐死后续所有访问（本轮定位并修掉）

只要有一次 SBA 访问落在 `sbbusy` 期间，DM 就把 `sbbusyerror` 置起来，之后**所有 SBA
访问都不再执行**：DMI op 照样回 `SUCCESS`，`sbdata0` 一直返回**上一次成功读到的那个
值** —— 表现就是"读到的值永远不变，而且不会自己恢复"。现场实测：J-Scope 稳定输出
恒定值（`0xBF19999A`），而同一时刻 OpenOCD 的 progbuf（经 CPU）读同一块区域**完全符合
契约**（`tick=0x7C7DD`、`p20=17` ⇒ `f_sin=0xBF4F1BBD` ✓ `f_tri=0xBF333333` ✓
`u_ramp=917=t%1000` ✓），OpenOCD 的 sysbus 读则直接报
`Failed to read memory via system bus` —— 同一个原因。

修法：块读收尾回读 SBCS（写 1 清 sticky），有错就让整块重读；单字流水路径每 32 拍
查一次、发现就地重挂地址；**写路径也查**（写同样会被静默吞掉，RTT 桥回写 RdOff 被吞
会导致下一块重复搬旧数据）；新增 `CMD_RISCV` action 9 `sbastat` 暴露 SBCS 实值与错误
计数（**正常情况下必须恒为 0**）。连续 200 次 32 B 块读 + 多次采样后计数仍为 0。

⚠️ 还有一种更糟、**只能复位目标**才能解的情况：访问**没有响应**的地址（未挂载/未上电
区域，例：HPM6800EVK 的 0x40000000 没有 SDRAM）会让总线事务永远不返回，`sbbusy` 一直
挂着 —— 它不是 sticky 位，清错 / TAP 复位 / `dmactive` 复位 DM / 干等**全都没用**
（均已实测），只有 ndmreset 或断电重上电才行。探针在这种状态下会**诚实报失败**
（读/写返回负值、scope 启动 `-4`），不会把旧值当数据报上去。

#### 🚨 两条 RISC-V 特有的固有属性（探针修不了，用之前必须知道）

1. **变量放在可缓存区 → SBA 读到旧值**（SBA 是总线主设备，绕过目标 D-cache；J-Link
   同理）。实测：变量放在 `0x01200000~0x0123FFFF`（写回缓存区）时探针读回**全 0**，
   而 OpenOCD 经 CPU 读到活值。HPM6800EVK 上 `0x01240000~` 是
   `MEM_TYPE_MEM_NON_CACHE_BUF`，变量放那里即可（`ATTR_PLACE_AT_NONCACHEABLE_BSS`）。
   备选是每拍 `l1c_dc_writeback`，但**地址和长度都要 64 B 对齐** —— 写错会挂 SDK
   断言 → `abort()` → `exit()` → `_exit()` → `ecall` → SDK 的 `syscall_handler` 是空
   实现、`mepc += 4` → 回到 `_exit+8` 的 `j .` **自旋停车**（实测靶子只跑 1 拍就停住，
   现场看起来像"探针读值冻结"，其实是靶子自己停了 —— 这一条耗掉了本次排查的大半时间）。
2. **一帧内多个字不是同一瞬间的（撕裂）**：读一个 32 B span 要 ~30 µs，期间目标可能
   更新到下一拍。验收判据因此允许"前 k 个字是 `tick`、后面是 `tick+1`"的**单调前缀
   撕裂**（实测 8.9%~26.6%，随机分布在各字之间），但任何第三种取值、任何非单调组合
   都判 FAIL；另加 **LFSR 逐拍链**校验（错位/串值/重排都过不去）。

**怎么用**：后端 = **全局目标类型**（HID `CMD_RTT` action 10，与 RTT 桥同一个开关 ⇒
**网页不改也能采 RISC-V**），或配置报文里 `flags bit6` 强制。**生效**后端在 DEF 包的
flags bit6 与状态字 0 的 bit1 里回报（丢弃模式没有 DEF 包，靠状态字那一位）。
⚠️ 目标类型是**粘**的：采过 RISC-V 再采 ARM 必须切回来；忘了切也不会直接失败 ——
后端拉不起来时采样器会换另一条路重试一次。⚠️ 切回 SWD 会**顺带**把探针端口模式改成
SWD-only，此后 OpenOCD 报 `CMSIS-DAP: JTAG not supported`，要 `set-mode 1` 切回来；
另外 RISC-V 引擎开着时会一直占着 TAP，用 OpenOCD 前先 `hpm6800_riscv.py stop`。

端到端验收（契约型靶子 `script_test/hpm6800evk_scope`，8 变量 32 B span）：
按节拍（200 µs 周期）**10320 帧全部字段与 `tick`/`tick+1` 相符、LFSR 链 10319/10319
逐拍正确、`dropped=4`、重同步 0**；满速 26.1 kHz 下 26880 帧同样全部相符。
靶子契约（每个字段都能由同一拍的 `g_tick` 反算）见 `src/main.c` 头部。

**加这个功能对 SWD 有没有影响？** 实测没有 —— 分派只在每个原语外面多一个可预测分支：

| SWD 复测（RISC-V 加固改动之后 / 基线） | 本轮 | 上一轮 | 基线 |
| --- | --- | --- | --- |
| DISCARD 3 µs @60M（纯探针侧） | **345.3 kHz，丢 3 拍** | 351.5 kHz / 13 | 348.4 kHz / 19 |
| M0 单字 @60M | **1.586 µs** | 1.583 | 1.548~1.588 µs |
| M0 pack（24 B span）@60M | **11.136 µs** | 11.148 | 11.193~11.234 µs |
| M0 `two` / `mixed` / `cross` | **8.920 / 11.566 / 25.794 µs** | 8.834 / 11.405 / 25.662 | 8.780 / 11.372 / 25.6 µs |
| 端到端 3 µs + CDC 关 | **321.4 kHz，丢 4.9%** | 320.7 kHz / 4.3% | 319.4 kHz / 4.7% |
| pack 端到端 12 µs（8 通道） | **76.2 kHz，重同步 0，u_hi 304832/304832 正确** | — | ~81 kHz（模型） |

全部落在 ±2% 的 run-to-run 噪声内、`swdErr=0` ⇒ **"RISC-V 后端对 SWD 没有可测影响"成立**。
内存代价：DLM **一个字节没涨**（127072 B / 130304，97.52%），FLASH 110096 B（12.11%）。

> **2026-09-29 追加：DLM 从 97.5% 降到 81.8%**（127072 → **106592 B**）。两块白拿的空间：
> ① SDK 的 `HEAP_SIZE`/`STACK_SIZE` 默认 0x4000 是抄 Segger 模板的（`cmake/application.cmake:307/312`），
> 而本工程**没有任何 malloc 调用**（唯一入口是 newlib stdio 的 `setvbuf`，1 KB 量级）⇒ 堆收到 **2 KB**（−14 KB）；
> ② USB 的 QHD/QTD 竞技场按"16 端点 × 8 QTD"算要 10 KB，而本工程只用 10 个端点、单笔最大 1 KB
> （一个 QTD 覆盖 16 KB）⇒ `USB_SOC_DCD_QTD_COUNT_EACH_ENDPOINT=2` 把它压到 **4 KB**（−6 KB）。
> 改完的复测：DFU 烧录、HID/配置子系统、raw DAP（1 KB 传输 + 混合读写 + 计数钳位）、
> scope bulk IN（1.7 MB/s、重同步 0、`u_hi` 303776/303776 正确）、SWD 标定（1.586/11.181 µs）、
> **VCOM 回环 115200~11.25 Mbps 全过（9 Mbps 起 975 KB/s 零丢包）** —— 全部无回归。
> 还能再挖的（未做）：**32 KB 的 AHB_SRAM（0xF0400000）一个字节没用**，`uartrx_ringbuffer`（32 KB）
> 是纯 CPU 环形、可整块搬过去；栈的 16 KB 也还没量过真实水位。详见本次分析记录。

> SWD 侧的代码路径**本次一行未改**（改动全在 `src/riscv/` 与 `script_test/`）；上面这轮是
> F103ZE 接上之后补测的（夹具先用 `stm32f103_scope/check.py` 验过：契约全过、时基
> 10014.7 Hz / +0.15%）。

```bat
:: 探针：切 SWD+JTAG 输出模式（RAM-only，掉电即失），再确认 6800 在不在
python script_test\hpm6800_probe.py set-mode 1
python script_test\hpm6800_riscv.py open

:: J-Scope over JTAG：单字 / 8 通道 / 端到端
python script_test\scope_hss_test.py bench --riscv --set one --addr 0x1240000 --iters 2000
python script_test\scope_hss_test.py bench --riscv --set one --base 0x1240000 --iters 1000
python script_test\scope_hss_test.py run   --riscv --set one --addr 0x1240024 --period 100 --secs 2

:: 切回 SWD/ARM（目标类型是粘的）
python script_test\scope_hss_test.py status --swd
```

### 复现

```bat
:: 靶子（STM32F103C8，96 MHz 超频，10 kHz 契约波形：正弦/三角/方波/锯齿/撕裂自检/50 kHz 混叠源）
cd script_test\stm32f103_scope
pwsh -File build.ps1
pwsh -File flash.ps1
python check.py                      :: 客观验收：halt → dump RAM → 逐项核对契约 + 反测时基

:: 探针侧（命令行验收，不等网页）
python script_test\scope_hss_test.py bench --clock 60000000     :: M0 标定（µs/样本 → 上限 kHz）
python script_test\scope_hss_test.py run --clock 60000000 --period 40 --secs 3

:: 上限在哪：单变量 u32、3 µs 周期、关掉 CDC 桥（flags bit5 = 0x20）⇒ ~329 kHz
python script_test\scope_hss_test.py run --set one   --clock 60000000 --period 3  --secs 4 --flags 0x20 --readsize 65536
:: 只量探针本体（不推 USB —— 把 USB 那段摘出去，丢包就全是探针 CPU 的账）
python script_test\scope_hss_test.py run --set one   --clock 60000000 --period 3  --secs 4 --flags 0x22
:: 守卫用例：单字流水读只在 **s_nspans == 1** 时启用，这两个配置都该走慢路径
python script_test\scope_hss_test.py bench --set mixed --clock 60000000 --iters 300   :: 3 字 span + 单字 span
python script_test\scope_hss_test.py bench --set two   --clock 60000000 --iters 300   :: 两个远离的单字 span
:: 多通道（每包 20 个样本；2026-10-02 起了发送队列之后不再漏缓冲，实测 82.3 kHz / 丢 1.5%）
python script_test\scope_hss_test.py run --set pack  --clock 60000000 --period 12 --secs 4 --flags 0x20
:: 手动总开关（网页面板那条命令）
python script_test\scope_hss_test.py status --bridge off     :: 之后记得 --bridge on 还回去
```

> ⚠️ **别拿 `rtt_bridge_sweep.py` 对着没有 RTT 控制块的目标跑**（比如现在这块跑 scope
> 例程的 F103ZE）：它的第 3 阶段会启动 RTT 桥并反复重扫，实测把探针主循环拖到不再应答
> HID，只能**拔插一次**才能恢复。纯 SWD 块读那两阶段（3301 KB/s）是可以跑的。


## USB→SPI/QSPI 转发桥

> 分支 `feature/usb-spi-bridge`。方案与全部实测记录：`docs/usb-spi-bridge-plan.md`；
> **网页侧实现说明**：`docs/web-handoff-spi-bridge.md`。协议真源：
> `firmware/application_5301/src/spi_bridge/spi_bridge_proto.h`。

把探针变成"USB 转 SPI/QSPI 主机"：主机通过 HID `0x35` 配置，通过 bulk `0x0B` 灌**有序帧流**
（一次 SPI 事务、CS/DC 翻转、延时、面板初始化步……），bulk `0x8B` 收回读数据与应答。
驱动 LCD/QSPI 屏时，网页把厂规初始化表一行编成一个 `STEP` 帧即可，屏的时序细节全在固件里。

**只有 HPM5301EVKLite 有这套排针**（`BOARD_HAS_SPI_BRIDGE = 1`）；akaLinkPro 原板置 0，
整个模块编成空实现，枚举与资源占用与改动前完全一致。

### 接线（J3 排针，全部在板上引出）

| 信号 | 引脚 | J3 脚 | | 信号 | 引脚 | J3 脚 |
|---|---|---|---|---|---|---|
| SPI2_CS0 | PB10 | 26 | | SPI2_MOSI / IO0 | PB13 | 28 |
| SPI2_SCLK | PB11 | 13 | | SPI2_MISO / IO1 | PB12 | 27 |
| **IO2**（quad） | PB14 | 10 | | **IO3**（quad） | PB15 | 8 |

辅助脚默认：`DC=PB11(J3.13)`、`RST=PB12(J3.27)`、`BL=PB13(J3.28)`、`CS_AUX=PB10(J3.26)`、
`TE`（输入）= 无（PB10 已归 SPI2 的 CS）。**验收/自测只需一根跳线**：`J3[28] ↔ J3[27]`（MOSI↔MISO）。

### 实测（2026-09-29，跳线回环）

| 项目 | 结果 |
|---|---|
| 回环 1/2/32/99/100/101/256/492 B（单线） | **16/16 PASS**；边界 3/255/257/491/492/493 也过 |
| SPI 模式 0/1/2/3（CPOL/CPHA 四组合） | 全部 PASS |
| SCLK 档位 | 20 / 40 / 60 / **75** MHz PASS（80/100 MHz 受跳线物理限制，待真从器件/LA 判） |
| 手动 CS + `CS_HOLD` 跨帧保持 | PASS（LA 实测三帧共用一个连续 CS 窗口） |
| 面板档线上格式（LA 逐位解 MOSI） | `spi_dcx`：`CE 5A A5` ✓；`qspi` 命令字位置 09-30 订正为 `02 00 F0 00 28`（原先 `00 00 F0` 只验了实现符合预期、未验面板要求，真屏未接） |
| 轮询 vs DMA | 都贴着 SPI 线速；DMA 固定开销 1~4.5 µs，40 MHz 约 200 B 交叉、75 MHz 约 128 B 交叉 |
| 资源 | FLASH 127384 B (14.01%)、**DLM 106592 B 零增长**、AHB_SRAM 24880 B |

### 回归验证：加了 SPI 桥没搞坏原有功能（2026-09-29，F103ZE 靶子）

SPI 桥改了 USB 描述符（新增接口 + 一对端点）与 EP0 请求缓冲，理论上会影响枚举路径。
实测逐个条件对下来：

| 场景 | RTT→CDC 交付率 | 丢包 |
|---|---|---|
| SPI 桥**关闭** | 2496.3 KB/s | 0 |
| SPI 桥关闭（复测） | 2497.7 KB/s | 0 |
| SPI 桥**使能**（空闲） | 2494.6 KB/s（−0.1%） | 0 |
| **一边整屏刷屏一边跑 RTT** | **2493.2 KB/s（−0.2%）** | **0** |
| 加 SPI 桥之前的基线 | 2527 KB/s | 0 |

- 全部落在基线的 **1.4% 以内且字节级零丢包**（`LOSSLESS: 0 gap, 0 lost, 0 duplicated`）。
- 并发场景是后台连刷 8 次整屏（每次 4.1~4.4 MB/s 走新端点 + SPI DMA），RTT 同时跑满 ——
  两条通道互不干扰，因为 SPI 桥的数据面是独立的 bulk 对，控制面只是 HID 里多一个 action。
- 同时顺带确认了原有各接口都在：**DAP**（OpenOCD 能 boost 目标、读 `RCC_CFGR`）、
  **CDC**（2.49 MB/s 交付）、**HID**（`CMD_RTT 0x31`）、**DFU Runtime**（今天用它刷了十几次）、
  **WebUSB**（界面正常枚举）。

> ⚠️ 一个已知的小 wart：`ENABLE 0` 关桥时**引脚保持现状**（不还原成默认复用）。
> 也就是用过 SPI 桥之后，`PB08`（它同时是 CDC 的 UART break 脚 `BOARD_APP_UART_BREAK_SIGNAL_PIN`）
> 会一直留在 SPI/GPIO 状态，直到重启。实测不影响 RTT/CDC 数据通路，但要用 UART break 的话
> 得先复位探针。要改成"关桥即还原引脚"可以再做。

### 用自检工具跑一遍

```powershell
cd script_test
python spi_bridge_test.py info            # 读配置/面板档/状态
python spi_bridge_test.py enable 1        # 使能（此时才配引脚）
python spi_bridge_test.py frames          # PING/DELAY/AUX_IN/CS 冒烟
python spi_bridge_test.py loop            # MOSI<->MISO 跳线回环全量验收
python spi_bridge_test.py bench           # 轮询 vs DMA 耗时对照
python spi_bridge_test.py pintest         # 回环失败时：先验跳线通不通
python spi_bridge_test.py dbg             # SPI 寄存器现场快照（卡在哪一步）
```

> 改这块代码前请先读 `docs/usb-spi-bridge-plan.md` §5.5–§5.7：那里记着四条**只有硬件才
> 暴露得出来**的硬约束（EP0 请求缓冲要装得下 MS OS 描述符集、SPI 模块时钟不许顶到
> 720 MHz、SCLK 焊盘必须带 `LOOP_BACK`、延时要有序帧流内生效）。


## USB→I2C 转发桥

把探针当 **USB 转 I2C 主机**用：HID `CMD_I2C` **0x36**，**只走 HID 一条通路、不占 DMA**。
上位机是网页版「USB→I2C」标签页（总线扫描 / 命令表 / 脚本 / 实时值四个 tab，
<https://minichao9901.github.io/web-serial-rtt-tools/>）。

### 接线

| 信号 | 引脚 | J3 位置 |
| --- | --- | --- |
| **SDA** | `PA28` | J3.21 |
| **SCL** | `PA29` | J3.19 |

这是 J3 上唯一一对引出来的硬件 I2C 脚（I2C3）。记得**共地**；上拉按从器件的实际情况加。

### 协议与能力

- **一次事务 = 一条 HID 报文**：子地址 + 写数据 + **repeated START** + 读，一口气跑完；
  写 ≤ **51 B** / 读 ≤ **54 B**。
- **主机轮询取结果**，不在中断里碰外设寄存器：事务登记在 HID 中断、执行在主循环
  （与 `CMD_RISCV` 同款模型）。
- **三档速率**：100 kHz / 400 kHz / 1 MHz。
- 状态字里除了**本命令的结果码**（bit24..31），还有 **SDA/SCL 的真实线电平**（bit3/4）——
  总线被谁拉死一眼可见。
- 配套三个诊断：**总线扫描**（0x08..0x77）、**引脚自检**（在真实事务中采线电平，证明脚
  确实在驱动总线）、**总线恢复**（`RESET` = 清计数器 + 9 个 SCL 脉冲）。

### 实测（2026-10-02，AT24Cxx EEPROM）

| 项 | 结果 |
| --- | --- |
| 总线扫描 | 扫到 **0x50** |
| 页写 → 回读 | **逐字节一致** |
| 54 B 块读 @ 100 kHz | **5243 µs** |
| 54 B 块读 @ 400 kHz | **1309 µs**（档位 ×4、耗时 ÷4，线性） |
| `eeprom` 门禁连跑 10 遍 | **10/10 通过，探针健在** |

### 用自检工具跑一遍

```powershell
cd script_test
python i2c_bridge_test.py info                 # 探针支持 0x36 吗、当前档位与状态
python i2c_bridge_test.py scan                 # 扫总线找从机
python i2c_bridge_test.py pintest              # 总线/引脚自检
python i2c_bridge_test.py eeprom               # AT24C02：页写 → 回读校验（数据面门禁）
python i2c_bridge_test.py eeprom --dev 0x50 --n 8
1..10 | % { python i2c_bridge_test.py eeprom } # 挂死类问题要连跑
python i2c_bridge_test.py rd 0x50 0x00 54      # 直接读 54 B
```

> 🚨 **改这块代码前必读** [`docs/usb-i2c-bridge-plan.md`](docs/usb-i2c-bridge-plan.md) §8：
> 曾经「**收到第一条 0x36 就把整个探针挂死**」—— 枚举正常、EP0 能读描述符，但 HID 一个
> 命令不回、CDC 打不开，**只能拔插**。真因是 `ib_status_word()` 无条件读 `IB_I2C->STATUS`，
> 而 `init_board_clock()` 里**没有 `clock_i2c3`** ⇒ 给"没开时钟的 IP"发一次 AHB 读，
> 事务可能永远不完成 ⇒ CPU 停在 USB 中断里。现在的规矩：上电即
> `clock_add_to_group(clock_i2c3, 0)`，**未使能时一个寄存器都不读**。
> 同处还记着三个"探针哑了"的假象（浏览器 WebHID 抢应答、HID 报文长度字段写 0 被丢、
> Windows 侧管道卡住只能拔插）。


## 文档

| 文档 | 内容 |
| --- | --- |
| [`docs/hpm6800evk-jtag.md`](docs/hpm6800evk-jtag.md) | **HPM6800EVK（HPM6880，RISC-V）用本探针调 JTAG 的完整记录**：接线坑、启动头真相、DMI/SBA 引擎与专用汇编、三个 DTM 时序坑、RTT 交付率与跨后端极性 bug、TCK 频率上限 |
| [`docs/HPM5301EVKLite_port.md`](docs/HPM5301EVKLite_port.md) | EVKLite 移植说明：引脚映射、构建、烧录、自调试、验证清单 |
| [`docs/HANDOVER-evklite-20260927.md`](docs/HANDOVER-evklite-20260927.md) | 移植过程交接记录（含 CDC 回环故障的根因与修复） |
| [`firmware/application_5301/Custom HID Protocol.md`](firmware/application_5301/Custom%20HID%20Protocol.md) | HID 配置协议（0x31 RTT / 0x32 SCOPE / 0x33 RISCV / 0x34 BRIDGE / 0x35 SPI / 0x36 I2C / 0x39 SPI→USB） |
| [`docs/web-handoff-riscv-scope.md`](docs/web-handoff-riscv-scope.md) | **给网页侧的最小改动说明**：波形页采 RISC-V 需要的三个新位、哪些 SWD 专属控件该藏、按后端分档的采样率提示 |
| [`docs/代码审查报告.md`](docs/代码审查报告.md) | 两轮代码审查全文 + 逐条处置结论（修复/上板验证/判定不修的理由） |
| [`docs/web-handoff-spi-bridge.md`](docs/web-handoff-spi-bridge.md) | **USB→SPI/QSPI 桥的主机侧实现说明**：HID 0x35 全部动作、bulk 帧格式与流程、面板初始化表怎么搬、排坑清单、实测性能 |
| [`docs/usb-spi-bridge-plan.md`](docs/usb-spi-bridge-plan.md) | USB→SPI/QSPI 桥方案 + P1~P4 上板实测记录（含四条硬件硬约束与四个真坑） |
| [`docs/web-handoff-i2c-bridge.md`](docs/web-handoff-i2c-bridge.md) | **USB→I2C 桥的主机侧实现说明**：HID 0x36 全部动作、事务与状态字格式、引脚与三档速率、排坑清单、AT24Cxx 实测 |
| [`docs/usb-i2c-bridge-plan.md`](docs/usb-i2c-bridge-plan.md) | USB→I2C 桥方案 + 上板实测与「第一条命令就整机挂死」结案（含三个"探针哑了"的假象） |
| [`script_test/stm32f103_scope/`](script_test/stm32f103_scope) | J-Scope 的靶子固件（F103C8/ZE，96 MHz，10 kHz 契约波形）+ `check.py` 客观验收 |
| [`firmware/application_5301/Flash_Memory_Map.md`](firmware/application_5301/Flash_Memory_Map.md) | Flash 布局 |
| [`firmware/application_5301/Firmware_Integrity_Plan.md`](firmware/application_5301/Firmware_Integrity_Plan.md) | 固件头/CRC 校验设计 |
| [`.opencode/skills/akalinkpro-firmware/SKILL.md`](.opencode/skills/akalinkpro-firmware/SKILL.md) | 构建 / 烧录 / 调试技能说明 |
| [`docs/HPM5301EVKLite_UG_V1.1.pdf`](docs/HPM5301EVKLite_UG_V1.1.pdf) | 先楫官方 EVKLite 用户手册（J3/J5 引脚定义出处） |

## License

本项目采用 **Apache License 2.0** 许可，详见根目录 [`LICENSE`](LICENSE)。

```
Copyright (c) 2026 akaInstruments

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0
```

## 第三方组件

本项目在源码与构建中使用/参考了以下第三方组件，其版权归各自作者所有，
并按其各自的许可证分发（这些组件的原始许可证声明请见对应文件/目录）：

| 组件 | 用途 | 许可证 |
| --- | --- | --- |
| [HPMicro HPM SDK](https://github.com/hpmicro/hpm_sdk) | HPM5301 SoC/驱动/启动/链接脚本 | BSD-3-Clause |
| [ARM CMSIS-DAP](https://github.com/ARM-software/CMSIS-DAP) | CMSIS-DAP 命令引擎（`src/dap/`） | Apache-2.0 |
| [CherryUSB](https://github.com/cherry-embedded/CherryUSB) | USB 设备协议栈 | Apache-2.0 |
| [CherryRB](https://github.com/cherry-embedded/CherryRB) | 无锁环形缓冲区 | Apache-2.0 |
| [EasyFlash](https://github.com/armink/EasyFlash) | 参数持久化（`src/easyflash/`） | MIT |

> 若再分发二进制，请一并保留上述组件的版权与许可证声明。
