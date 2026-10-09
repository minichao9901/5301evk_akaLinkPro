# SWO 接收时钟会话与 240 MHz 默认模式

2026-10-09，用户明确要求 UART 默认 240 MHz、测试并支持 30 Mbaud。应用默认使用 PLL0CLK0 720 MHz /3，CPU 保持 720/2=360 MHz，SPI2 启用后保持已有 240 MHz 配置。只改 UART 分频不会改 CPU；修改共享 PLL0 倍频/小数参数则会影响消费者。本实现不调 PLL0。

CMake `UART2_CLOCK_MHZ` 可覆盖 80/100/180/200/240，未指定时为 240。240 MHz 是用户指定的超规格模式；此前核查的 HPM5300 DS Rev0.11 UART 额定输入为 100 MHz，本机有限测试不代表全系列额定能力。

## HID 0x19 协议

沿用自定义 HID OUT report 1、IN report 2。请求布局为 `[report=1, length=10, cmd=0x19, action, arg:u32LE, mode:u32LE]`，补齐 64 字节。响应为 `[report=2, length=58, cmd=0x19, action, words:14×u32LE]`；WebHID 已去掉 report ID，解析时注意偏移差异。

| action | arg / mode | 用途 |
|---|---|---|
| 0 | 忽略 | 读取会话状态 |
| 1 | baud / mode | 准备；返回 token，主循环执行切换 |
| 2 | token | 释放并恢复；无会话时幂等 |
| 3 | token | 续期；客户端每秒发送，超时 5 秒 |
| 4 | 页 0–2 | 每页 12 个 SYSCTL CLOCK，共 36 个 |
| 5 | 忽略 | 根时钟及 CPU/PLL1 快照 |
| 6 | 忽略 | UART RX 累计错误计数 |

mode=0 固定原 UART 源/分频；1 自动选已有源与分频；2 必要时允许 PLL1 精细调频。准备后必须轮询到 rc=0、token 匹配且会话活动，再配置目标 trace。rc=1 是排队中，不能作为切换成功。

普通响应 words：0 协议版本（1），1 有符号 rc，2 token，3 活动模式+1（0 表示无会话），4 请求 baud，5 实际 baud，6 UART Hz，7 SYSCTL 分频，8 MUX，9 OSR，10 UART divisor，11 PLL1 Hz，12 剩余租约 ms，13 阻塞节点（0xffffffff 无）。CPU 阻塞编号 36。

action 4 的 words[2..13] 为页内寄存器；action 5 的 words[2..9] 为八个实际标称根频率、[10] CPU CLOCK 原值、[11..13] PLL1 MFI/MFN/MFD。action 6 的 words[2..6] 分别为 overrun、framing、parity、break、ring 丢弃字节，其余字段不是诊断计数。

rc：-1 参数不合法；-2 忙/错误 token/CDC 非 UART；-3 无法在 0.5% 内匹配；-4 共享时钟无法迁移；-5 节点切换失败；-6 PLL 等待超时；-7 恢复失败；-8 UART 实际分频回读失败。负错误不得启用目标 trace。

## 切换与恢复

枚举八源、SYSCTL 分频 1–256、偶数 OSR 8–30、UART divisor 1–65535，UART 输入不超过 240 MHz、请求不超过 30 Mbaud。PLL1 精细模式限标称 400–1000 MHz，验证晶振参考、默认 MFD、无扩频和 CLK0 输出分频。它表示软件搜索范围，不代替芯片数据手册的电气许可。

PLL1 活动消费者先迁往保持原频率的稳定源；CPU 使用 PLL1 时拒绝。XIP 必须保护，必要时降低到最近的稳定 PLL0 频率，停止恢复。所有 PLL/XIP 切换、有界等待和恢复执行于 ILM，临界区屏蔽中断。PLL 关闭时 BUSY 清零即可稳定，开启时还须 RESPONSE，遵循 SDK 等待条件。

UART 打开期间，租约只接受相同 baud 和 8N1，直接应用计划 OSCR/DLL/DLM 并读回验证。退出恢复之前的 UART 请求速率。USB reset/disconnect 排队恢复；无续期 5 秒恢复。正常记录外主循环提前返回，不增加计时/关中断开销。心跳与超时使用原子时间快照，避免 ISR 更新导致 unsigned 差值误判。

RX 错误记录 UART LSR 标志的观察次数，可能低于受影响字节数；ring 丢弃字节记录写入短缺。主机记录开始后的基线差值。未实现 DMA 错误计数，不输出假的 DMA 零值。

## 验收

镜像 SHA256：`660d9c60f6490eec28c4647be51d8c1f98b7b1fca67ad700aedf47b2b47247bc`，编译时间 2026/10/09 11:04:54，HPM5301 探针 SN `B4444F2110DDDAFE44800BBB0AE800E0`。

F103CB HSE/PLL 60 MHz、PC /128、纯 PC、30 Mbaud 连续 6 秒：12,916,276 字节、2,424,768 PC，畸形包/overflow/未知 PC/UART 已检测错误均为零。目标 24 MHz、PC /64、24 Mbaud 也通过。停止后恢复探针默认 240 MHz、CPU 360 MHz、目标 HSE 72 MHz。

`script_test/swo_clock_lease.py --serial <SN> --json <路径>` 验证固定 30/24 Mbaud、已有源 18 Mbaud、PLL1=920 MHz /4 /10 的 23 Mbaud；150 次续期、错 token、30 Mbaud+1 拒绝、5.3 秒超时恢复全部节点和 PLL 参数。SPI2 已启用为 240 MHz 时 CPU/SPI 模块时钟保持不变；没有重测外部 SPI 数据吞吐。

机器报告见 [clock240-acceptance.json](clock240-acceptance.json)。Bootloader 未修改，未烧录；应用使用打包镜像并保留运行，禁止恢复历史 80/200 MHz 默认镜像。
