# SPI转发：SPI 从机接收转 CDC

本功能把外部 SPI 主机发送的 **8 位原始字节流**送到现有 CDC ACM 串口。网页「SPI转发」位于「RTT转发」之后，并提供引脚分配图。没有新增 USB 接口、CDC 串口或样本缓冲。

2026-10-08 起，启动转发时主动设置并核验 **SPI2 模块时钟 240 MHz**，与 SPI/QSPI 主机共用同一配置函数，不再继承前一个功能留下的时钟。该内部时钟不是外部 SCK。配置失败以 -4 退出并释放资源。H743 复测见 [固定 240 MHz 验证](validation/2026-10-08-spi-fixed240.md)。

## 接线和使用

HPM5301EVKLite 复用原 SPI2 引脚。已实测的 F103ZE 接线如下；两板共地、使用 3.3 V 逻辑电平。

| F103ZE SPI1 | 探针 SPI2 | EVKLite J3 |
| --- | --- | --- |
| PA4，CS 输出 | PB10，CS 输入 | 26 |
| PA5，SCK 输出 | PB11，SCK 输入 | 13 |
| PA7，MOSI 输出 | PB13，MOSI 输入 | 28 |
| PA6，MISO 输入 | PB12，MISO | 27；只接收时可不接 |

先在 SPI→USB 页连接探针、选择 Mode 0…3 与 MSB/LSB，打开探针 CDC 接收口，点击启动转发，再启动外部主机。时钟来自外部主机，CDC 的波特率设置不限制传输速度。主机应先建立空闲 SCK，再拉低 CS；停止时先释放 CS，再关闭 SPI，避免 CPOL=1 时多出一个时钟边沿。

CDC 是无分帧的原始流，不添加 CS 标记、时间戳或长度头；停止会丢弃尚未搬入 CDC 的 SPI 数据，并计入丢弃字节数。已经搬入 CDC 的旧字节仍会发完。如果需要报文边界、序号或校验，请由发送端加入。

网页与 RTT 转发并列提供接收、清空、暂停、ASCII/HEX/ANSI、速率统计和文件记录。它们使用同一个串口会话。UART、RTT、SPI 三种 CDC 数据源互斥；切换数据源先停止原功能，保持接收串口打开。SPI 从机与 SPI/QSPI 主机、点屏、ADC 互斥；SPI 引脚与 SWD 独立，因此可同时使用 SWD 调试。高流量自动暂停显示；计数和已启动的文件记录继续。完整二进制记录应关闭「记录带时间戳」。

## 调度与存储

- `service_gate_t spi_cdc_gate` 存放权威运行/请求/故障字节。主循环按门控调用 `spi_cdc_poll()`；空闲仅检查标志。
- HID 只排队 START/STOP。硬件初始化、停止、错误恢复和字节搬运全部在主循环执行。
- 从 SPI 主机借用 AHB SRAM 中的 **16 KiB OUT 环**作为无限循环 RX DMA。借用前检查 SPI/ADC 原生 IN/OUT 在途状态；DMA 停止、资源释放后才归还缓冲及引脚。
- DMA IRQ 只累计完成圈数或登记故障。主循环读取圈数、待处理 TC 与实时 DMA DSTADDR，处理圈尾/重载窗口和 32 位回绕。
- 每轮只搬入口时已有的数据，最多 **4096 B**；新到字节留给下一轮，避免连续输入占住主循环。每次关中断复制最多 **1024 B**，写入现有 **32 KiB `g_uartrx`**。CDC 缓冲满时不覆盖 USB 在途数据。
- DMA 未消费量超过半圈时丢弃最旧字节，保留最新 8192 B，并累计 `dropped`；没有 SPI 硬件流控，主机持续快于接收端时不能保证无损。FIFO 溢出与 DMA 故障另行计数。
- USB reset 取消待启动并排队停止。DMA 故障同样停止，归还资源。切换 CDC 数据源保留现有 USB 在途缓冲，返回 UART 后在主循环重放最后的串口参数；重复 SET_LINE_CODING 不会重置正在发送的 endpoint。

参考工程 `ch32v305_dap_wi_spi_speedup/CherryDAP/spi2usb.c` 的循环 DMA 接收思路。其 WCH DMA 半满中断直接启动独立 CDC endpoint 的实现未照搬；这里沿用本工程主循环服务门控、共享缓冲和现有 CDC 的架构。

## HID 0x39

完整 HID report 为 64 B。Report ID 占 byte 0；WebHID 将它单独传递，payload 从完整 report byte 1 开始。

| 完整 report 字节 | 请求 |
| --- | --- |
| 1 | 数据长度：STATUS/STOP 为 2，START 为 4 |
| 2 | `0x39` |
| 3 | action：0 STATUS，1 START，2 STOP |
| 4 | START 的 SPI mode：0…3 |
| 5 | START 的 LSB：0 MSB first，1 LSB first |

响应 byte 1 为 54，byte 2 为 0x39，byte 3 回显 action。byte 4 起是 13 个小端 u32：

| word | 内容 |
| --- | --- |
| 0 | `0x31435053`，SPC1 能力标记 |
| 1 | flags：bit0 支持，bit1 运行，bit2 请求/配置处理中 |
| 2 | 有符号返回码：0 成功，-100 待处理，-1 板型不支持，-2 缓冲或 CDC 占用，-3 DMA 失败，-4 SPI 初始化失败，-5 参数/动作非法 |
| 3 | mode 位 0…1，LSB 位 8 |
| 4 | 借用的循环缓冲大小 |
| 5…8 | 接收、已转入 CDC、丢弃字节、FIFO 溢出次数 |
| 9…10 | 尚未转入 CDC 字节、CDC 待发送字节 |
| 11 | 接受的 START generation；拒绝 START 不增加 |
| 12 | DMA 故障次数 |

START/STOP 响应表示请求入队，必须轮询 STATUS 到 pending=0，再判断返回码和运行位。每次成功 START 清零统计；字节统计按 u32 模 2³² 回绕。RTT 启动在 CDC 已被 SPI 占用时返回 -15，防止两个生产者写同一个环。

## 可复现测试

测试发送端在 `script_test/stm32f103_spi_tx/`，编译产物 `fw.elf` 随源码提交。默认上电运行但不发送（`g_run=0`）。通过 SWD 写 `g_run=1` 开始；`g_div` 为 F1 的 BR 编码（1/2/3 对应 18/9/4.5 MHz，CPU=72 MHz）；`g_pattern=0` 连续发送 `hello world!\r\n`，1 发送 64 B 帧（SPIC + u32 序号 + 可校验载荷）。`g_mode`、`g_lsb` 选择模式与位顺序。DMA 使用 4 KiB 双半区，`g_refill_late` 记录发送端未及时补缓冲。

```powershell
& script_test/stm32f103_spi_tx/build.ps1
# 通过 OpenOCD program ... verify reset exit 烧录上述 ELF
py -u script_test/spi_cdc_hw.py --dividers 3,2,1 --seconds 10
py -u script_test/spi_cdc_hw.py --hello --dividers 1 --seconds 5
py -u script_test/spi_cdc_hw.py --dividers 2 --modes 0,1,2,3 --orders msb,lsb --seconds 2
py -u script_test/spi_cdc_backpressure_hw.py
make test-host
```

硬件脚本需要 pyserial、hidapi、ARM GCC 和 OpenOCD；当前工具路径见脚本。测试结束停止发送与转发，释放 COM/HID。网页实测脚本在 Web 仓库 `tools/selftest/spi-cdc-hw.mjs`，运行前须给测试浏览器授权探针。

实测结果与原始数据见 [2026-10-08 F103ZE 验收](validation/2026-10-08-spi-cdc.md)。当前最高验证档位为 18 MHz，网页实收约 **2.250 MB/s**；这是该档位实测结果，不是探针极限。

并发回归见 [SPI 加入后的回归与瓶颈](validation/2026-10-08-spi-cdc-regression.md)。未开启 SPI 时没有测出 RTT/JScope 明显降速；18 MHz SPI 与 HSS 同时运行时，单变量 50 kHz、八变量 10 kHz 的稳定窗口没有跳拍，极限速率仍共享探针执行时间。标定/初始化期间可能覆盖丢弃 SPI 数据，完整计数必须与稳定窗口分开评估。
