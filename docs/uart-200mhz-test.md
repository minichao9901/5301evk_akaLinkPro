# UART 默认 200 MHz 与 SWO 网页验收（2026-10-09）

本页为早期 200 MHz 验收记录。用户后续已把默认改为 240 MHz、上限 30 Mbaud，当前实现和结果见 [SWO 时钟会话](swo-clock-lease.md)。

按用户最终要求，CDC UART 默认使用 **PLL1CLK0 800 MHz / 4 = 200 MHz**，配置最高波特率 **25 Mbps**；已重新构建烧录并保持运行（APP 编译时间 `2026/10/09 07:46:41`）。代码位于独立 `codex/uart-200mhz` 分支，主 checkout 未改动。

## 时钟配置

用户图中的 800 MHz 是 PLL1CLK0（源选择 4），PLL0CLK1 默认 600 MHz。仅切换 UART 功能时钟，不重新设置共享 PLL，不修改 CPU/XPI 时钟。每次有效采样后 HID 0x18 读回输入 200,000,000 Hz、SYSCTL MUX=4、DIV 编码=3（实际四分频）、初始化状态 0。

[用户手册表 16](https://www.hpmicro.com/Public/Uploads/uploadfile/files/20250417/HPM5300UMV10.pdf) 列出时钟源。[数据手册 Rev0.11 表 17，PDF 第 44 页](https://www.hpmicro.com/Public/Uploads/uploadfile/files/20250205/HPM5300DSV011.pdf) 的 UART 最大输入值为 100 MHz；200 MHz 是用户选择的超规格默认模式。源码已纠正旧注释里「80 MHz 是手册最大值」的错误。内部时钟占空比没有测量，不能由收包测试证明 50%。

配置公式 `baud = fUART / (divisor × OSR)`。200 MHz / OSR8 / divisor1 = 25 Mbps；OSR10 / divisor1 = 20 Mbps。其他请求可能被钳制/取整，实测结果不随波特率单调变化。

CMake 参数 `UART2_CLOCK_MHZ` 可指定 80/100/180/200，不指定时源码默认 **200**。保留旧 `UART2_OVERCLOCK=1` 的 180 MHz 兼容定义。已构建 80 MHz 与默认 200 MHz；100/180 配置尚未板上验收。

## 板上结果

使用 STM32F103CB PB3 SWO → HPM5301 EVKLite probe PB09 / J3[3] VCOM RX 和 SWD/GND（更正此前 PB07 标注），复杂 F103 测试 ELF 与源码在配套 Web 分支 `codex/swo-pc-trace`。

| 发端 / 请求 | 接收端取整配置 | 结果 |
| ---: | ---: | --- |
| 10 Mbps | 10 Mbps | 通过 |
| 14 Mbps | 14.285714 Mbps | 通过 |
| 16 Mbps | 16.666666 Mbps | 通过，有收发偏差 |
| **20 Mbps** | **20 Mbps** | 精确匹配；三组 3 秒/256 周期通过，约 1.19 MB/s |
| 22 Mbps | 20 Mbps | 失配，未通过 |
| **24 Mbps** | **25 Mbps** | 三组有效低负载 5 秒、三组 3 秒/256 周期、三组 2 秒/192 周期通过；无溢出重复吞吐约 1.90–1.92 MB/s |
| 28/32 Mbps | 25 Mbps 限幅 | 失配，未通过 |

24 Mbps/192 周期三组数据量 3,810,569 / 3,829,786 / 3,836,948 字节，ITM overflow、malformed、truncated、unmapped 与阶段核对错误均为 0，GNU 函数/源码行核对通过。进一步提高到 128 周期/1 秒出现 22 个 ITM 溢出，2.307 MB 数据不计作无损通过。

当前 STM32 HSI/PLL 测试配置未提供精确的 25 Mbps 发端；24→25 的接收容差结果不能代替精确 25 Mbps 验收或全系列保证。原始 SWO 吞吐包含 PC、时间戳、睡眠和 ITM，不是应用净载荷。

三轮 200 MHz 探测共 27 次：19 次通过、4 次数据失配/溢出失败、4 次配置前启动时钟读回拒绝。最近默认固件配套新网页四组自动识别 48/40 MHz、25 Mbps 请求（目标实际 24）/20 Mbps 精确匹配均通过，页面计数与独立分析一致。页面对无效 RCC 最多做三次只读连接重试，不复位、暂停或改目标时钟。

## 构建及只读诊断

在已有 HPM SDK 环境中使用 EVKLite 构建参数：

```powershell
cmake -G Ninja -DBOARD=hpm5301evklite -DHPM_BUILD_TYPE=flash_xip -DCMAKE_BUILD_TYPE=debug -B firmware/application_5301/build_uart200 -S firmware/application_5301
cmake --build firmware/application_5301/build_uart200
python script_test/uart_clock_diag.py --serial <SN>
```

使用打包镜像 `build_uart200/output/akaLinkPro_App_pack.bin`。同步更新已发布 `build_dfu_evklite/output/akaLinkPro_App.elf` 快照为此次默认固件。

HID 0x18 响应为八个小端 u32：协议版本、实际输入时钟、主机请求波特率、取整后交给 SDK 的波特率、硬件 OSR、编译上限、最后初始化状态、原始 CLOCK 寄存器。[协议文档](../firmware/application_5301/Custom%20HID%20Protocol.md#uart-只读诊断0x18) 描述完整布局。初始化失败会返回应用波特率 0，且清除 SDK 可能留下的 DLAB，不再把失败配置标记为成功。诊断不读取 DLL/DLM，避免 DLAB=0 时误读 RX 数据；低速档的应用值不等于实际线路频率测量。

## 配套网页

普通串口候选及 SWO 页面最高 25 Mbps。录制时识别 F103/F1 Cortex-M3 CPUID/DEV_ID、RCC 与 DWT 能力，配置 DEMCR、ITM/DWT PC 采样、PB3 与 TPIU NRZ/波特率。HSI 和 HSI/2 PLL 主频自动计算；HSE 需填写实际外部晶振值。其他芯片明确提示尚未适配。

完整 `.c` / `.txt` 导出在后台重新解码全部原始记录，保留每个 PC/函数/源码行、ITM/异常/睡眠和显式缺口，不受页面展开上限或筛选影响。.c 仅供阅读，包含原源码行供 VS Code 高亮，不能视为可编译或完整指令还原的程序。260,000 PC 的超过页面上限测试以及 22 项页面检查通过。长导出支持按块直接写文件；未提供源码明确保留地址/位置。

## 备份、历史恢复与当前状态

[机器报告](uart-200mhz-acceptance.json) 保留各次结果与状态变化。首次实验前完整原 APP 909,312 字节读两次一致、CRC 正确，代码与原构建包完全相同。首轮结束曾全量恢复并读回一致，SHA256 `4aa1f2e9d9e848fb0f9c8f646834a35e9be3a461528311f1b97a629e67f95ef7`；这些是历史证据。随后用户要求保留 200 MHz，已重新烧录默认版本，**当前不恢复旧探针 APP**。

目标测试结束仍恢复用户原 STM32 128 KiB Flash、全量读回一致，SHA256 `5c54ac4301861d8827a31bbaf2d995f6fd2e2b58a845e706a4d367d352adce40`。常规网页录制不烧录目标，停止时还原 trace 快照。Bootloader、EasyFlash 与两仓库主目录均未改动。原始备份/录制/日志留在忽略 tmp 目录。
