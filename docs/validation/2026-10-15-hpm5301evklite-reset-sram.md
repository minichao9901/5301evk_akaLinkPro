# HPM5301EVKLite 双路复位与 SRAM 验收

验证编号：`20261015-2046-hpm5301-reset-sram`

受测基线：`07f2bb44eda55205d9846df1ae592dc3ffab7291`，提交前工作树含本次改动。

证据目录：`E:/web-etm-trace-coordination/artifacts/hpm5301evklite-reset-sram/20261015-2046-hpm5301-reset-sram/`

本次将 J3[23]/PA27 配为第二路目标 nRESET，并保留 J5[3]/PA08 原有复位路径。PA27 始终开漏；PA08 默认开漏，可通过 HID `0x3A` 在开漏与推挽之间切换。释放共享 nRESET 网络时先释放 PA27，再释放 PA08；断开 DAP 端口时也会先释放两路复位。PA27 从 SPI 桥辅助脚候选中移除。

固件以 `BOARD=hpm5301evklite` 和 HPM SDK 1.11.0 构建，`build_dfu_evklite.bat` 完成，链接占用 FLASH 162,352 B。最终打包镜像为 192,504 B，SHA256：`9105c42242a4849d5d3862b0b1acf718bbb39009135d98ad4ecefbca399b64fc`；跟踪的 `akaLinkPro_App.elf` SHA256：`ca80a0979ec197696768b10fc049f2f9cdbf8872bbde910573469135cf5257f2`。

通过 `script_test/reset_io_hid_test.py --exercise` 实测 HID 读回能力位 `0x03`，并分别切换推挽、开漏后读回通过，最后恢复开漏。DFU 下载完整镜像后，`dfu-util` 在请求离开 DFU 的收尾阶段返回 `LIBUSB_ERROR_PIPE`（退出码 74）；设备随后重新枚举为 akaLinkPro 应用，HID 身份查询读到构建时间 `2026/10/15 20:57:46`，并通过新 `0x3A` 命令，因此固件写入已由运行态确认。DFU 工具日志保存在证据目录。

H7B0 通过 akaLinkPro SWD 实测：目标 CPUID `0x411fc271`。开漏和推挽两种模式各执行一次 CMSIS-DAP nRESET 脉冲，目标均重新应答；随后在 `0x24080000` 对 20、32、64、128 KiB 各写读三轮并逐字节校验，均通过。PyOCD 测得约 0.08 MB/s 读、0.09 MB/s 写（1 MHz SWD）；测试结束后 CPU 恢复运行。测试区内容按要求未备份或恢复。复位期间没有单独用示波器区分两根线的波形，实测结论是双路固件已加载、复位命令可使已接线的 H7B0 复位。

完整构建、DFU、HID、H7B0 测试日志及输入镜像在证据目录。硬件验收脚本为该目录中的 `h7b0-hardware-acceptance.py`；可重用的 HID 模式测试脚本已加入 `script_test/reset_io_hid_test.py`。
