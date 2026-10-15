# PA27 复位模式修正与实板验收

验证编号：`20261015-2145-pa27-correction-accepted-280mhz`。

Tested-Revision: pre-commit-worktree  
Tested-Base: 3b292e06bae4446cfd740c5ad16e93c3ff491b44  
Tested-Dirty: true  
Validation-Result: passed（复位功能）；高速 SWD 上限未验收。

## 接线和行为

- HPM5301EVKLite J3[23]/PA27 接 H7B0 NRST，可切换开漏／推挽，探针启动时默认开漏。
- J5[3]/PA08 接目标 TRST，固定开漏。两路仍随 CMSIS-DAP nRESET 联动输出；PA08 接 TRST 时仅影响 JTAG TAP，不代替 NRST。
- J5[15] 接的是探针自身 RESET_N 输入，不是可由应用驱动的目标复位输出。
- HID `0x3A` 的能力位新增 bit2，标识“PA27 可切换、PA08 固定开漏”。旧 bit2=0 的固件含义相反，配套网页禁用该开关，要求升级。
- 设置模式保留当前输出锁存状态；复位释放、端口重新连接时，先释放固定开漏路线，再释放可能推挽的路线。预装释放电平后才使能输出，避免启动低脉冲。
- PA27 继续从 SPI 辅助 GPIO 中保留；网页对应选项和引脚图也同步禁用。

## Review 结论及修正

1. 原模式切换作用于 PA08、PA27 固定开漏，和用户要求相反：已修正板级主／副输出映射及变量和文档名称。
2. 原模式设置会主动释放复位：已改为保留锁存位，不因切换电气方式而结束一次复位。
3. HID 未校验设置长度／模式范围：已拒绝非法请求，并保留原设置。
4. 重新连接端口时存在输出使能／预置电平的顺序风险：已先释放开漏路线并预装电平，再使能输出。
5. 构建脚本可能在编译失败后仍返回成功：已传播配置和构建错误，并允许从其他目录调用。

`PIN_nRESET_IN` 当前报告探针输出锁存位，不能据此宣称目标实际重启。未用示波器分别验收两路电气波形；PA08 固定开漏来自板级与 PAD 配置审查，HID 回读确认的是运行时设置。

## 更正上一次复位结论

2026-10-15 的前一份报告以复位命令后仍能读 CPUID 作为主要依据，这只能说明通信恢复，不能证明系统复位，更不能证明接 TRST 的 PA08 可以独立复位芯片。该项旧结论以本报告为准；原始报告和日志保留，不改写旧证据。

这次使用真实 Chrome 页面，通过 WebHID 切换两种模式，再用页面全局复位按钮的业务入口发送 CMSIS-DAP 硬复位。目标在复位前运行；预先启用 Reset Vector Catch 后，两种模式均得到：

- PC=`0x080002c8`，等于新 280 MHz 目标固件复位向量去掉 Thumb 位后的 Reset_Handler。
- DHCSR=`0x00030003`，S_HALT 置位；DFSR=`0x0b`，VCATCH 置位。
- 清除旧复位标志后，RCC_RSR=`0x00400000`，PINRSTF 置位。

因此，按已确认的 PA27→NRST 接线，真实系统复位通过。该证据不把 PA08→TRST 宣称为独立系统复位路径。测试恢复 DEMCR 并让目标继续运行；随后连续三次网页全局复位通过。

## 构建与验证证据

- 固件工作目录：`E:/Share/github/akaLinkPro/firmware/application_5301`；命令：`cmd /c build_dfu_evklite.bat`。HPM SDK 1.11.0，构建通过；既有宏重定义、EasyFlash 和 RWX 链接警告保留。
- HID：在仓库根目录执行 `python script_test/reset_io_hid_test.py --exercise`，推挽／开漏／恢复默认回读通过，能力位 `0x07`。
- 网页硬件脚本：`E:/web-serial-rtt-tools/tools/selftest/reset-sram-hw.mjs`，在网页仓库运行 `node tools/selftest/reset-sram-hw.mjs E:/web-etm-trace-coordination/artifacts/hpm5301evklite-reset-sram/20261015-2145-pa27-correction/accepted-280mhz`。
- 页面：`http://127.0.0.1:8899/index.html?modules=bundle#flash`，真实 USB／HID，用户完成两次浏览器接口授权，无模拟探针。
- 固件构建时间回读：`2026/10/15 22:33:58`。打包镜像 192,464 B，SHA256 `b1574157213df8e8d9bf80208719cf48214b2d3b4c8d2281e1edd27589ac7f87`；跟踪 ELF SHA256 `fe0c3c54fba004fee9c32f4d6984df6b3f887668e3fd85479b1200f83466239f`。
- DFU 完整下载后在 leave 阶段仍出现已知 `LIBUSB_ERROR_PIPE`，运行态版本和 HID 功能已再次核实，未把 dfu-util 的退出码直接当成成功证据。

持久证据根目录：`E:/web-etm-trace-coordination/artifacts/hpm5301evklite-reset-sram/20261015-2145-pa27-correction/`。最终原始报告：`accepted-280mhz/web-hardware.json`，SHA256 `f8ad35db1d54511a84245e7593c8a6e213d093ac891cf8e5f7adaf9ca6dcbea0`。构建、DFU、HID 和运行身份分别见 `build-accepted.log`、`dfu-accepted.log`、`hid-accepted.log`、`probe-accepted-identity.log`；证据索引为 `accepted-evidence-sha256.json`。大体积证据在该本机目录，未随 Git 上传。

## 性能范围

SRAM 测速已改用网页批量命令与最多四笔 USB 流水，并逐轮校验。当前 H7B0 280 MHz 下四种长度均通过，失败的高 SWD 档会记录并降档重测。高档在旧探针镜像中也出现错误；未据此证明具体电气原因，也未宣称达到历史约 3 MB/s。用户决定次日换板测上限，本次停止继续追查性能极限。
