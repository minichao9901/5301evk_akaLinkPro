# BMP 发图吞吐量：SPI 字节 DMA 是 QSPI 的主要限制

日期：2026-10-08。探针：HPM5301EVKLite，akaLinkPro，序号 B4444F2110DDDAFE44800BBB0AE800E0，USB HS。浏览器：本机 Chrome 153，实际 WebUSB 页面。按用户要求先测 AXS15352，再测 ST77916；没有接屏，验证发送通路及探针执行，不声称验证了显示效果、排针电气质量或屏端最高时钟。

基线代码为 akaLinkPro `7da281f`，Web 为 `06c1b55`。保留基线固件后烧录优化固件，再恢复基线重测相同整屏 BMP，最后重新烧录优化固件并长测。以下 MB/s 均按 1,000,000 字节计算；历史文档中有 MiB/s（1,048,576 字节），两种单位不能混用。

## 结果

页面实际载入 24-bit BMP，经过现有解码、RGB565 转换、帧打包、WebUSB 调度及响应匹配。关闭局部更新，每张图完整发送。AXS15352 为 240×296、142,080 字节像素；ST77916 为 360×360、259,200 字节像素。每组预热 2 张，再测 20 张；OUT 并发保持现有默认 8。

| 屏协议 | 实际 SCLK | USB 批量 | 优化前 MB/s | 优化后 MB/s | 优化后平均发送耗时 |
|---|---:|---:|---:|---:|---:|
| AXS15352，单线 SPI＋DC | 40 MHz | 8 KiB | 4.41 | 4.42 | 32.17 ms |
| AXS15352，单线 SPI＋DC | 60 MHz | 8 KiB | 5.81 | 6.14 | 23.13 ms |
| AXS15352，单线 SPI＋DC | 60 MHz | 32 KiB | 5.90 | 6.18 | 22.99 ms |
| ST77916，四线 QSPI | 40 MHz | 8 KiB | 6.75 | 13.22 | 19.61 ms |
| ST77916，四线 QSPI | 60 MHz | 8 KiB | 7.09 | 15.38 | 16.85 ms |
| ST77916，四线 QSPI | 60 MHz | 32 KiB | 7.14 | 16.36 | 15.85 ms |

同一时钟、批量、分辨率下，QSPI 提升约 2.2–2.3 倍；单线 SPI 提升约 5%。所有组的 `framesErr`、`outOverrun`、`inDrop` 增量为 0。每张实际执行 292 / 529 个帧，分别发送 142,091 / 259,208 字节（像素加开窗命令参数），与期望逐项一致。原始记录见 [baseline-full.json](baseline-full.json)、[word-dma-full.json](word-dma-full.json)。

60 MHz 下，每款屏分别以 8 / 32 KiB 批量连续发 100 张，总计 400 张整屏 BMP，合计 164,200 个执行帧，错误、OUT 溢出、IN 丢弃均为 0。单线约 6.13–6.16 MB/s；QSPI 约 12.50 / 15.00 MB/s。长测出现浏览器调度与 CPU 波动，不能把短测峰值 16.36 MB/s 当作保证值。见 [final-soak.json](final-soak.json)。

## 定位证据与修改

原代码每次 DMA 仅向 SPI DATA 寄存器写 1 字节。提高四线 SCLK，从 40 到 60 MHz，整屏吞吐量只从约 6.7 到 7.1 MB/s。USB 主机攒批已生效（整屏 32 KiB 批量仅 10 次提交），继续增大批量不足以解除这个限制。

HPM5300 SDK 的 SPI 寄存器说明定义了 `TRANSFMT.DATAMERGE`：数据单元仍为 8 bit，一次 DATA 寄存器访问可拆成四个字节。修改 `spi_bridge.c`，在 TX-only DMA、源地址 4 字节对齐且长度是 4 的倍数时启用合并，DMA 源和目的访问宽度改为 word。DMA `TRANSIZE` 按源宽度计数，设为字节数 / 4；SPI 传输计数仍是原字节数。

轮询、RX、全双工、非对齐源地址、非整字尾包均使用原字节路径。每笔事务重新选择合并状态和 DMA 宽度，避免上一笔的设置污染下一笔。没有改变 USB 帧协议、CS 保持规则、QSPI 首片命令规则、主循环处理预算或 USB 接收缓冲所有权。

只改此项，QSPI 由约 7.1 提升到 15–16 MB/s，USB-only 对照仍约 18–19 MB/s。这是支持字节 DMA 为主要限制的硬件对照证据；不是仅根据代码猜测。

## 剩余限制与测量口径

60 MHz 的数据线速：单线 7.5 MB/s，四线 30 MB/s。单线优化后约达到线速的 82%，继续优化的绝对空间较小；QSPI 仍有余量。

4096 个完整 512 字节 PING 槽、不执行 SPI，以末帧响应确认实际解析完成：8 / 32 KiB 批量约 18–19 MB/s；每次只提交 512 字节则约 8.4–8.6 MB/s。这个结果测量的是当前桥的 USB 接收、512 字节槽解析与主循环通路，**不是 USB HS 总带宽上限，也不是 CDC 的测速**。目前仍每收 512 字节触发一次完成与重新武装，SPI 每片仍单独设置控制器并等待完成，这些是后续可优化的部分。

完整时钟/批量矩阵见 [baseline-clock-batch.json](baseline-clock-batch.json)、[word-dma-clock-batch.json](word-dma-clock-batch.json)：20 / 40 / 60 / 75 MHz，512 / 8192 / 32768 字节批量。该早期矩阵统一使用 240×296 图像，ST77916 发送的是同尺寸窗口，不能当作 360×360 整屏结果。75 MHz 无屏测试成功也不代表真屏能接受此时钟。

页面 `lastRun.ms` 从转换完像素后开始，包含打包、USB 与最后响应，未包含 BMP 首次解码、像素转换、预览和发送后的 HID 状态查询。另记录 `wallMs`，覆盖整个 `sendImage()`。例如 60 MHz、32 KiB 短测：AXS 传输 6.18 MB/s、完整操作 4.53 MB/s；ST 传输 16.36 MB/s、完整操作 9.33 MB/s。ST 的逐张完整操作约 27.8 ms，而传输约 15.85 ms。CPU 独立分段数据和 USB 时间均保存在原始报告，分段时间存在重叠，不能机械相加。

32 KiB 批量在 QSPI 长测中比 8 KiB 更有利，用户可选择该档；单线受线速限制，改变批量收益较小。本轮保留页面默认值，避免把单次发送结果直接当作所有动画场景的最佳值。

## 完整性与回归

- 生产 DMA 设置主机测试：1–492 字节 × 4 种对齐，共 1968 组，验证精确搬运字节数、RX/轮询回退、word→byte 切换、SDK 设置失败有界退出；纳入 `make test-host`。
- F103 临时运行 RAM 中的 GPIO 接收程序，500 kHz 接收真实 SPI 输出。长度 4、100、101、104、108、492、103、488；104 字节帧前插入一个 1 字节 PING 使源地址非对齐。总计 1500 字节全部相同，零错误。源程序及编译 ELF 为 [capture.c](capture.c)、[capture.elf](capture.elf)，结果为 [wire-actual.bin](wire-actual.bin)、[wire-expected.bin](wire-expected.bin)。从 PA5 采 SCLK、PA7 采 MOSI，原 RAM 保存后恢复并复位运行原 Flash；没有烧写目标 Flash。
- SPI PING / 2 ms DELAY / CS 冒烟通过；未配置 TE 的 AUX_IN 返回预期 GPIO 状态，收尾清零状态。首次未 ENABLE 的冒烟调用超时，按原脚本前置条件启用后通过。
- SPI/ADC 共享缓冲硬件检查 13 项通过。ADC 真页面有限 / 环回 / 连续采集 37 项通过，含 2 MSa/s，见 `adc-regression.json`。
- `make test-host`、Web `make test-offline` 全部通过；Web 语法检查包含 296 个模块。

探索线路捕获时，20 kHz 的 4 字节事务触发 SPI SDK 固定轮询次数超时；恢复原固件后同样失败，改用 500 kHz 完成上述字节校验。这属于原有低速等待限制，不用这组失败数据推断高速吞吐量。

最终 APP ELF SHA256：`72e21b69e9951289a9f890a613b26c4dfe3582206b136a76ee9f7f9d882190b4`。已重新编译并烧录优化固件；编译 ELF 随代码提交。

## 复现

Web 工程中运行（探针空闲、浏览器已授权、8899 服务和 9333 CDP 浏览器已启动）：

```powershell
node tools/dev/spi-image-rate-hw.mjs --out=tmp/spi-image-rate.json --rounds=20 --clocks=40,60 --batches=8192,32768
```

脚本通过实际页面的 `pickImage()` / `sendImage()` 发图，读取探针累计计数，核对完整帧数、像素数和错误；末帧响应确认实际执行。结束时恢复探针原配置和档位、关闭桥并释放设备句柄。`--full-panel=false` 可重现早期同尺寸窗口矩阵。
