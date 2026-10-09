# 2026-10-09 UART / SWO 主线合并验证

主线从 `5d1e9af` 快进至 `0842761`，合入 `codex/uart-200mhz` 的两个提交，无冲突。包含默认 UART 240 MHz／最高 30 Mbaud、可恢复 SWO 接收时钟租约、接收错误诊断及此前验收资料。

在主目录执行 `make build-app test-host`，APP 构建及全部主机测试通过。覆盖 JTAG/RISC-V、RTT/JScope 启停与缓冲、SPI DMA／ADC 所有权／CDC 转发／时钟，以及目标切换等生产 C 路径。未使用实物探针、未烧录。

确认构建缓存 `UART2_CLOCK_MHZ` 为空，代码默认仍是 PLL0CLK0 720 MHz /3 = 240 MHz、最高 30 Mbaud；无旧 180／200 MHz 编译覆盖。

| 项目 | 结果 |
|---|---|
| FLASH | 166,928 B / 888 KB |
| ILM | 31,704 B / 128 KB |
| DLM | 114,784 B / 130,304 B |
| 升级包长度 | 196,504 B |
| 代码长度 | 196,248 B |
| 签名 / 头版本 | HPM! / 1 |
| CRC32 | `c8d881ce`，重新计算一致 |
| 升级包 SHA256 | `821c53c1e18e14e427509e5415888269ac3ac10df633abdd64979d067f9a5ee1` |

主目录重新生成的 `firmware/application_5301/build_dfu_evklite/output/akaLinkPro_App.elf` 一同保存；打包镜像在同目录。本次构建镜像与历史实物验收镜像分别记录，未覆盖历史验收报告中的 SHA256。

完整日志和包校验报告在 `build/merge-20261009`。原 worktree 的 408 个被忽略文件（50,926,293 字节）已复制到 `E:/worktree-archives/2026-10-09-merge/firmware-uart` 并逐文件校验 SHA256；主目录预先存在的 `.zcodeignore` 未修改。
