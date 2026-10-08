# akaLinkPro

**开源 USB 高速调试探针 · ARM / RISC-V · 浏览器工作台**

[在线工作台](https://minichao9901.github.io/web-serial-rtt-tools/) · [Web 源码](https://github.com/minichao9901/web-serial-rtt-tools) · [快速开始](#快速开始) · [实测性能](#实测性能) · [Apache-2.0](LICENSE)

akaLinkPro 基于 HPM5301，将目标调试、固件烧录、串口、RTT、变量采样和常用总线测试集成到一条 USB 连接。既可作为 CMSIS-DAP 探针配合 OpenOCD 等桌面工具，也可搭配纯静态 Web 工作台，在浏览器中完成日常嵌入式开发。

本仓库包含探针应用固件、DFU Bootloader、板级适配与测试夹具。完整产品由本仓库和 [web-serial-rtt-tools](https://github.com/minichao9901/web-serial-rtt-tools) 共同组成。

## 为什么选择 akaLinkPro

- **一条 USB 连接，多种开发工具。** 调试、串口日志、RTT、变量波形、SPI/QSPI、I2C 和 ADC 共用一个探针。
- **浏览器直接操作硬件。** WebUSB / WebHID / Web Serial 通路无需安装专用上位机；打开工作台、授权设备即可使用。
- **把实时工作交给探针。** RTT 轮询、HSS 周期采样和硬件 ADC 采集在探针端执行，浏览器负责控制、显示与记录。
- **ARM 与 RISC-V 双通路。** ARM 目标走 SWD/JTAG；RISC-V 目标使用 JTAG、DMI 与 SBA，已在 HPM6800EVK 验证。
- **开源且可复现。** 固件、网页、协议和测试夹具开放，性能数字附带目标条件和验收记录。

## 功能概览

| 能力 | 典型用途 |
| --- | --- |
| CMSIS-DAP 调试 | ARM / RISC-V 目标连接、寄存器与内存访问；配合 Web 进行源码调试、断点、单步与回溯 |
| 固件烧录 | 配合 Web flashloader 完成擦除、写入、校验和复位；也可使用桌面调试工具 |
| 串口与 RTT | USB CDC 串口；探针端 RTT→CDC 转发，减少主机逐次读内存的开销 |
| J-Scope / HSS | 从目标 RAM 周期读取 1–8 个变量，支持不同宽度、结构体成员及数组元素，无需加入采样协议代码 |
| USB→SPI/QSPI | 单/双/四线事务、命令与地址相位；外设寄存器、NOR Flash、LCD 初始化与发图 |
| SPI转发 | 外部 SPI 主机 → 探针从机 DMA → USB CDC，用于高速数据接收 |
| USB→I2C | 100 kHz / 400 kHz / 1 MHz，总线扫描、寄存器读写、重复起始及恢复 |
| USB ADC 示波器 | 定时器硬件触发、DMA 采集；8/10/12/16 位、有限或连续记录，最高请求 2 MSa/s |
| 固件升级与配置 | DFU / 虚拟 U 盘升级，固件头与 CRC32 校验，板载 Flash 保存配置 |

SPI/QSPI、I2C 和高速 ADC 当前面向 **HPM5301EVKLite** 板型。HPM5301 无物理 DAC；Web 的 DAC 页提供波形预览、数据导出和未来硬件接口，当前不输出模拟信号。

**查看工作台界面：** [J-Scope、烧录器、RTT 转发、源码调试器、SPI/QSPI 发图与 ADC 示波器](https://github.com/minichao9901/web-serial-rtt-tools#界面预览)。Web 主页提供当前页面截图，并标明模拟目标、虚拟信号和烧录准备场景；下方性能表引用独立板上验收结果。

## 实测性能

下面是代表性板上结果。目标时钟、接线、变量布局和主机负载会影响速率；峰值采样率与无损采样率分别报告。

| 通路 | 条件与结果 | 证据 |
| --- | --- | --- |
| ARM SRAM 传输 | F103ZE，SWD 60 MHz 档；OpenOCD 纯传输写 **3384 KiB/s**、读 **2928 KiB/s** | [SWD 历史基线](docs/development-history.md#最新进展2026-09-30) |
| RTT→CDC | F103ZE，SWD 60 MHz 档；探针侧交付约 **2954 KiB/s** | [RTT 实测记录](docs/development-history.md#最新进展2026-09-30) |
| RISC-V SRAM / RTT | HPM6800EVK：SRAM 读/写约 **1504 / 1512 KiB/s**；RTT 约 **1385 KiB/s** | [JTAG 验证](docs/hpm6800evk-jtag.md) |
| HSS 单变量 / SWD | F103CB @72 MHz、SWD 60 MHz：名义 400 kHz 实得约 **399.97 kHz**，调度跳拍约 **0.008%**；500 kHz 档实得约 **496 kHz**，跳拍约 **0.8%** | [采样优化与边界](https://github.com/minichao9901/web-serial-rtt-tools/blob/main/docs/validation/2026-10-07-f103cb-hss-optimization.md) |
| HSS / RISC-V | HPM6800EVK：单 u32 名义 200 kHz 实得约 **200 kHz**、跳拍约 **0.00275%**；8 个连续 u32 在 **25 kHz** 窗口内跳拍与 USB 丢样均为 0 | [HPM HSS 复测](docs/validation/2026-10-07-hpm6800-hss-rate.md) |
| SPI / QSPI 发图 | 实际 SCK 60 MHz、32 KiB 批次：单线 SPI **6.29 MB/s**，四线 QSPI **16.77 MB/s** | [固定 240 MHz 实测](docs/validation/2026-10-08-spi-fixed240.md) |
| SPI转发→CDC | H743 标称 SCK 67 MHz，原生 CDC 接收 **8.41 MB/s**；15 秒稳定窗口完整性检查通过 | [转发与 Web 缓冲对照](docs/validation/2026-10-08-spi-fixed240.md) |
| ADC 采集 | EVKLite，8/10/12/16 位在 **1 / 2 MSa/s** 各测 30 秒，计数一致、无溢出 | [采集实现与验收入口](docs/adc-bulk-stream.md) |

性能口径：KiB/s 按 2¹⁰ B/s（历史脚本标作 KB/s），MB/s 按 10⁶ B/s。SPI/QSPI 数字为像素有效载荷的传输速率，测试未接屏，不代表实屏帧率；ADC 结果验证时基、传输和计数，不代表模拟精度或 ENOB。SPI转发在 Web 默认 4 KiB 接收缓冲下，高速档仍可能出现背压丢字节；原生 CDC 峰值不能直接当作网页无损保证。

**当前 SPI2 内部模块时钟固定 240 MHz。** 主机对外 SCK 仍按偶数分频：10/20/40/60 MHz 精确生成，75/100 MHz 请求实际为 60 MHz；SPI转发从机的外部 SCK 来自发送板。

## 快速开始

### 使用探针

1. 准备 akaLinkPro 或已烧入对应固件的 HPM5301EVKLite，按 [板级接线说明](docs/HPM5301EVKLite_port.md) 连接目标。
2. 用 USB 连接电脑，打开 [在线工作台](https://minichao9901.github.io/web-serial-rtt-tools/)，使用桌面版 Chrome / Edge。
3. 选择调试器、RTT、J-Scope 或总线页面，在浏览器设备选择框中授权相应 HID / USB / 串口接口。
4. 调试或变量采样时载入与板上固件匹配的 ELF。**载入 ELF 只提供符号和地址，不会更新目标固件。**

RTT 输出要求目标已集成 RTT 并运行；HSS 从目标内存直接采样，缓存中的变量需要考虑访问一致性。各页面会协调共用资源，SPI/QSPI 主机、SPI转发和高速 ADC 不能同时占用相同引脚与缓冲。

### 构建与升级

Windows 构建环境：HPM SDK 1.11.0、配套 RISC-V GCC 和 GNU Make。默认 SDK 环境目录为 `E:\sdk_env_v1.11.0`，可通过 `HPM_SDK_ENV_DIR` 调整。以下命令默认构建 EVKLite 版本：

```powershell
make build        # Bootloader + APP
make build-app    # 仅更新 APP 产物
make test-host    # 无硬件的固件逻辑回归
```

首次烧录使用外部 J-Link 和 `make flash` 安装 Bootloader 与 APP。已安装 Bootloader 后，运行时长按 USER 键约 1 秒进入 DFU，将 **带头的** `akaLinkPro_App_pack.bin` 拖入虚拟盘 `AKALINKPRO` 升级。

| 产物 | 路径 |
| --- | --- |
| APP ELF / 调试符号 | [`akaLinkPro_App.elf`](firmware/application_5301/build_dfu_evklite/output/akaLinkPro_App.elf) |
| DFU 升级包 | `firmware/application_5301/build_dfu_evklite/output/akaLinkPro_App_pack.bin` |
| 应用源码 | [`firmware/application_5301/`](firmware/application_5301) |
| Bootloader 源码 | [`firmware/bootloader_dfu/`](firmware/bootloader_dfu) |

详细烧录、救砖和目标侧接线见 [EVKLite 移植说明](docs/HPM5301EVKLite_port.md)。

## 硬件与兼容性

| 板型 | 调试 / RTT / HSS | 串口 | SPI/QSPI、I2C、高速 ADC |
| --- | --- | --- | --- |
| akaLinkPro 原板 | 支持 | UART2，PA08 / PA09 | 当前板型未开放这些扩展 |
| HPM5301EVKLite | 支持，目标调试口 J5 | UART2，PB08 / PB09，J3[5] / J3[3] | 支持，按页面引脚图接线 |

- 已验证目标包括 STM32F103CB / F103ZE、STM32H743 和 HPM6800EVK；具体功能与速率以对应测试记录为准。
- 原板与 EVKLite 共用应用源码，由板级特性宏区分引脚与外设能力；固件应匹配实际板型。
- 浏览器工作台支持通用 CMSIS-DAP v2 的基础功能；探针端 RTT 转发、HSS 与总线扩展使用 akaLinkPro 专用协议。

## 关键里程碑

| 时间 | 成果 |
| --- | --- |
| 2026-09 | EVKLite 移植与 DFU 升级打通，探针端 RTT→CDC 桥完成高速交付验证 |
| 2026-09 | HPM6800EVK 的 RISC-V JTAG / DMI / SBA 通路落地，加入 HSS 变量采样与 SPI/QSPI |
| 2026-10-02 | I2C 扫描、读写和错误恢复完成板上验证 |
| 2026-10-06 | ADC 硬件触发与 USB 流水线完成 1 / 2 MSa/s 持续采集测试 |
| 2026-10-07 | SWD 与 RISC-V 高速采样优化，建立双仓库基线标签 [`milestone-2026-10-07`](https://github.com/minichao9901/5301evk_akaLinkPro/tree/milestone-2026-10-07) |
| 2026-10-08 | SPI转发完成 H743 高频与完整性测试，SPI2 模块统一固定 240 MHz |

完整研发过程、历史测量与问题定因见 [开发与调试记录](docs/development-history.md)。里程碑表示阶段成果，具体测试条件与未覆盖场景保留在验收文档中。

## 文档与开发入口

| 入口 | 内容 |
| --- | --- |
| [Web 工程](https://github.com/minichao9901/web-serial-rtt-tools) | 页面功能、设备授权、本地运行和兼容性 |
| [EVKLite 板级文档](docs/HPM5301EVKLite_port.md) | 引脚、构建、升级和自调试 |
| [SPI/QSPI 接线](docs/spi-bridge-wiring.md) / [SPI转发](docs/spi-cdc.md) | 外设与高速接收使用说明 |
| [I2C 协议与接线](docs/web-handoff-i2c-bridge.md) | I2C 控制与事务约定 |
| [ADC 数据流](docs/adc-bulk-stream.md) | 硬件触发、采集流程和性能范围 |
| [自定义 HID 协议](firmware/application_5301/Custom%20HID%20Protocol.md) | 探针扩展功能的控制接口 |
| [测试脚本](script_test) / [Makefile](Makefile) | 模型回归、目标夹具和板上复现 |
| [历史调试记录](docs/development-history.md) | 原 README 完整归档、优化过程与排障证据 |

## 许可证

本项目自有代码使用 [Apache License 2.0](LICENSE)。HPM SDK、CMSIS-DAP、CherryUSB、CherryRB、EasyFlash 等第三方组件遵循各自许可证；再分发时保留对应版权声明，组件清单见 [历史文档](docs/development-history.md#第三方组件)。
