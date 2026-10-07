# akaLinkPro build & flash shortcuts (Windows)
#
# Targets (BOARD=hpm5301evklite):
#   make build      - build bootloader + APP
#   make build-boot - build DFU bootloader only  (build_xip_evklite)
#   make build-app  - build APP only             (build_dfu_evklite)
#   make flash      - J-Link first flash: bootloader + APP in one session
#   make flash-app  - J-Link flash APP only (bootloader preserved)
#   make dfu        - dfu-util download of the APP (needs dfu-util in PATH)
#   make reset-usb  - force a USB re-enumeration of the probe
#   make clean      - remove generated EVKLite output, preserve checked-in ELF snapshots
#
# Hardware test targets (ST/EVKLite target board must be wired up):
#   make sram-test  - STM32F103 SRAM read/write benchmark over CMSIS-DAP+OpenOCD
#   make rtt-test   - STM32F103 SEGGER RTT throughput test (probe -> host)
#   make uart-echo  - EVKLite CDC loopback check   (short J3.8 <-> J3.10 first)
#   make uart-loop  - EVKLite CDC loopback sweep, 9600 .. 10 Mbps
#
# USB -> SPI/QSPI panel targets (see docs/spi-bridge-wiring.md):
#   make panel      - ★ one-shot: reset + init + colour bars on the AXS15352 panel
#   make spi-loop   - SPI bridge loopback acceptance (jumper J3[19] <-> J3[21])
#
# Variables:
#   PYTHON  - interpreter for the test scripts (needs pyserial for uart-*)
#   COM     - CDC port of the probe, e.g. COM7
#   OPENOCD_EXE / OPENOCD_SCRIPTS / HPM_SDK_ENV_DIR - tool locations
#
# The recipes delegate to the maintained .bat scripts, which carry the
# toolchain paths (E:\sdk_env_v1.11.0, overridable via HPM_SDK_ENV_DIR).
# J-Link install dir can be overridden with JLINK_EXE.

SHELL      := cmd.exe
.SHELLFLAGS := /c

APP_DIR  = firmware\application_5301
BOOT_DIR = firmware\bootloader_dfu

PYTHON ?= python
COM    ?= COM52
# 回归目标默认**不传串口**：脚本按 VID/PID 自动探测探针的 CDC 口（会随 USB 口变，
# 本机实测 COM5 → COM43）。要强制指定时：make regression-swd COMREG=COM7
COMREG ?=
# 回归脚本要 hidapi/pyusb/pyserial —— 这台机器上只有 Python 3.13（py 启动器）装了
PYHID  ?= py
REGDIR = build\regression
COM_ARG = $(if $(COMREG),--port $(COMREG))

.PHONY: all help build build-boot build-app flash flash-app dfu reset-usb clean \
        sram-test rtt-test rtt-max rtt-link uart-echo uart-loop \
        regression-swd regression-swd-bg regression-swd-log regression-swd-jsonl \
        regression-riscv regression-riscv-bg regression-riscv-log regression-riscv-jsonl \
        panel panel-red panel-green panel-blue panel-gradient panel-checker panel-le \
        spi-loop spi-frames spi-pintest spi-dbg spi-bench spi-info test-host test-jtag-host
all: help

# Production C regression models; these targets do not touch attached boards.
test-jtag-host:
	$(PYTHON) script_test/riscv_jtag_host_test.py

test-host: test-jtag-host
	$(PYTHON) script_test/service_gate_host_test.py
	$(PYTHON) script_test/analog_host_test.py
	$(PYTHON) script_test/adc_stream_host_test.py
	$(PYTHON) script_test/adc_cache_host_test.py
	$(PYTHON) script_test/bus_periodic_host_test.py
	$(PYTHON) script_test/scope_host_test.py
	$(PYTHON) script_test/rtt_stop_host_test.py
	$(PYTHON) script_test/spi_drain_host_test.py
	$(PYTHON) script_test/spi_adc_owner_host_test.py
	$(PYTHON) script_test/spi_cdc_host_test.py
	$(PYTHON) script_test/spi_cdc_service_host_test.py
	$(PYTHON) script_test/cdc_source_host_test.py
	$(PYTHON) script_test/target_switch_host_test.py
	$(PYTHON) script_test/test_scope_hss_test.py

# Requires HPM_SDK_BASE (or sibling hpm-sdk-reference); no hardware access.
.PHONY: test-usb-descriptors
test-usb-descriptors:
	$(PYTHON) script_test/usb_adc_descriptor_test.py

build: build-boot build-app

build-boot:
	@echo [make] building bootloader ...
	@cd /d $(BOOT_DIR) && build_xip_evklite.bat

build-app:
	@echo [make] building APP ...
	@cd /d $(APP_DIR) && build_dfu_evklite.bat

flash:
	@echo [make] J-Link first flash: bootloader + APP ...
	@cd /d $(APP_DIR) && flash_jlink_evklite_full.bat

flash-app:
	@echo [make] J-Link flash APP (bootloader preserved) ...
	@cd /d $(APP_DIR) && flash_jlink_evklite.bat

dfu:
	@echo [make] dfu-util download of the APP ...
	@cd /d $(APP_DIR) && program_evklite.bat

reset-usb:
	powershell -NoProfile -ExecutionPolicy Bypass -File script_test\reset_akalink_usb.ps1

# --- hardware tests ---------------------------------------------------------
# STM32F103 SRAM read/write speed: OpenOCD load_image/dump_image of 20 KB at
# 0x20000000 across SWD clocks 1..60 MHz, byte-compared every run.
# The target is normalised to 64 MHz first: an STM32F1 left at its reset
# default (HSI 8 MHz) caps the throughput near 1.4 MB/s whatever the SWD clock
# is. Use `python script_test\sram_speed_test.py --no-boost` to keep it as-is.
sram-test:
	@echo [make] STM32F103 SRAM read/write benchmark (CMSIS-DAP + OpenOCD) ...
	$(PYTHON) script_test\sram_speed_test.py

# SEGGER RTT throughput from the bundled STM32F103 test firmware.
# The target clock is normalised to 64 MHz first: an STM32F1 out of reset runs
# at 8 MHz and its RTT write loop then caps the stream at ~277 KB/s whatever
# the SWD clock is.
rtt-test:
	@echo [make] STM32F103 SEGGER RTT throughput test (OpenOCD rtt server) ...
	$(PYTHON) script_test\rtt_speed_test.py

# Ceiling measurement: the poll loop runs inside OpenOCD (no telnet round
# trips), 32-bit reads in bounded chunks. ~1.1 MB/s at 60 MHz.
rtt-max:
	@echo [make] RTT drain ceiling (poll loop inside OpenOCD) ...
	$(PYTHON) script_test\rtt_drain_bench.py

# SWD read reliability while the target RUNS (AP reads fail at high clocks).
rtt-link:
	@echo [make] SWD link matrix while the target runs ...
	$(OPENOCD_EXE) -s "$(OPENOCD_SCRIPTS)" -f script_test\openocd_stm32f1_swd.cfg \
	  -c "gdb port disabled" -c "tcl port disabled" -c "telnet port disabled" \
	  -c "init" -c "reset run" -c "source script_test/rtt_link_matrix.tcl" -c "shutdown"

# EVKLite CDC loopback (short J3.8 = UART_TXD to J3.10 = UART_RXD first).
uart-echo:
	@echo [make] CDC loopback check on $(COM) ...
	$(PYTHON) script_test\evk_echo.py $(COM) 115200 make-echo

uart-loop:
	@echo [make] CDC loopback sweep on $(COM) ...
	$(PYTHON) script_test\uart_loopback_common.py $(COM)

# --- USB -> SPI/QSPI bridge -------------------------------------------------
# Wiring: docs/spi-bridge-wiring.md   Protocol: docs/web-handoff-spi-bridge.md
# The panel is the TianMa 2P01 / AXS15352 (240x296, 4-wire SPI + DC):
#   SCLK=J3[23] MOSI=J3[19] CS=J3[24] DC=J3[13] RST=J3[27] BL=J3[28] TE=J3[26]
# Loopback tests need a jumper between J3[19] (MOSI) and J3[21] (MISO).
panel:  ## ★ 一键刷屏：复位 + 初始化 + 彩色条
	@echo [make] panel: reset + init + colour bars (TianMa 2P01 / AXS15352) ...
	$(PYTHON) tools\panel_show.py

panel-red:
	$(PYTHON) tools\panel_show.py -p solid -c red

panel-green:
	$(PYTHON) tools\panel_show.py -p solid -c green

panel-blue:
	$(PYTHON) tools\panel_show.py -p solid -c blue

panel-gradient:
	$(PYTHON) tools\panel_show.py -p gradient

panel-checker:
	$(PYTHON) tools\panel_show.py -p checker

# Same bars but with the other RGB565 byte order - flip between the two when
# red and blue look swapped (the canonical setting is MADCTL=0x00 + big endian).
panel-le:
	$(PYTHON) tools\panel_show.py --littleendian

spi-loop:  ## SPI 桥回环全量验收（要 J3[19]<->J3[21] 跳线）
	@echo [make] SPI bridge loopback sweep (jumper J3[19] <-> J3[21]) ...
	cd script_test && $(PYTHON) spi_bridge_test.py loop

spi-frames:
	cd script_test && $(PYTHON) spi_bridge_test.py frames

spi-pintest:  ## 回环失败时先跑它：验 MOSI<->MISO 跳线通断 + pad 输入通路
	cd script_test && $(PYTHON) spi_bridge_test.py pintest

spi-dbg:
	cd script_test && $(PYTHON) spi_bridge_test.py dbg

spi-bench:
	cd script_test && $(PYTHON) spi_bridge_test.py bench

spi-info:
	cd script_test && $(PYTHON) spi_bridge_test.py info

# --- ADC / SPI shared-buffer ownership --------------------------------------
.PHONY: adc-owner-hw
adc-owner-hw:  ## ADC 与 SPI/QSPI 共享缓冲：互斥及原生传输退场门禁
	$(PYTHON) script_test/spi_adc_owner_hw_test.py

# --- one-click regression ---------------------------------------------------
# SWD  = F103ZE（SRAM 测速 / RTT 20·45·60M 交付率+零丢 / HSS 单·多变量 bench+run+拟合）
# RISCV = HPM6800EVK（selfcheck+块基准+sbastat / RTT 交付+零丢 / HSS 完整性+bench+契约）
# 每阶段/每频率档结果实时进 build\regression\<family>.{log,jsonl}；吞吐 < 基线 80%
# 或错误/丢失非 0 ⇒ 立即 ERROR 退出；--budget 硬超时防卡死。COM 由 COM= 传入。
regression-swd:
	@echo [make] SWD regression (F103ZE) - results stream to $(REGDIR)\swd.{log,jsonl}
	$(PYHID) script_test\regression_swd.py $(COM_ARG)

regression-swd-bg:
	@if not exist $(REGDIR) mkdir $(REGDIR)
	@powershell -NoProfile -Command "Start-Process -WindowStyle Hidden -FilePath '$(PYHID)' -ArgumentList 'script_test\regression_swd.py' -RedirectStandardOutput '$(REGDIR)\swd.log' -RedirectStandardError '$(REGDIR)\swd.err' -PassThru | ForEach-Object { 'PID=' + $$_.Id }"
	@echo [make] watch: make regression-swd-log   /   make regression-swd-jsonl

regression-swd-log:
	@powershell -NoProfile -Command "if (Test-Path '$(REGDIR)\swd.log') { Get-Content '$(REGDIR)\swd.log' -Tail 40 -Encoding UTF8 } else { 'no log yet' }"

regression-swd-jsonl:
	@powershell -NoProfile -Command "if (Test-Path '$(REGDIR)\swd.jsonl') { Get-Content '$(REGDIR)\swd.jsonl' -Encoding UTF8 } else { 'no jsonl yet' }"

regression-riscv:
	@echo [make] RISC-V regression (HPM6800EVK) - results stream to $(REGDIR)\riscv.{log,jsonl}
	$(PYHID) script_test\regression_riscv.py $(COM_ARG)

regression-riscv-bg:
	@if not exist $(REGDIR) mkdir $(REGDIR)
	@powershell -NoProfile -Command "Start-Process -WindowStyle Hidden -FilePath '$(PYHID)' -ArgumentList 'script_test\regression_riscv.py' -RedirectStandardOutput '$(REGDIR)\riscv.log' -RedirectStandardError '$(REGDIR)\riscv.err' -PassThru | ForEach-Object { 'PID=' + $$_.Id }"
	@echo [make] watch: make regression-riscv-log   /   make regression-riscv-jsonl

regression-riscv-log:
	@powershell -NoProfile -Command "if (Test-Path '$(REGDIR)\riscv.log') { Get-Content '$(REGDIR)\riscv.log' -Tail 40 -Encoding UTF8 } else { 'no log yet' }"

regression-riscv-jsonl:
	@powershell -NoProfile -Command "if (Test-Path '$(REGDIR)\riscv.jsonl') { Get-Content '$(REGDIR)\riscv.jsonl' -Encoding UTF8 } else { 'no jsonl yet' }"

clean:
	@echo [make] cleaning generated evklite output ...
	powershell -NoProfile -ExecutionPolicy Bypass -File tools/clean-evklite-build.ps1

help:
	@echo akaLinkPro targets:
	@echo   make build       build bootloader + APP
	@echo   make build-boot  build DFU bootloader only
	@echo   make build-app   build APP only
	@echo   make flash       J-Link first flash (bootloader + APP)
	@echo   make flash-app   J-Link flash APP only
	@echo   make dfu         dfu-util download of the APP
	@echo   make reset-usb   force USB re-enumeration of the probe
	@echo   make sram-test   STM32F103 SRAM read/write speed test (OpenOCD)
	@echo   make rtt-test    STM32F103 SEGGER RTT throughput (rtt server)
	@echo   make rtt-max     RTT drain ceiling (poll loop inside OpenOCD)
	@echo   make rtt-link    SWD read reliability while the target runs
	@echo   make uart-echo   CDC loopback check (COM=COMx)
	@echo   make uart-loop   CDC loopback sweep 9600..10M (COM=COMx)
	@echo.
	@echo   USB -^> SPI/QSPI bridge (panel = TianMa 2P01 / AXS15352):
	@echo   make panel       ★ one-shot: reset + init + colour bars
	@echo   make panel-red   solid red      make panel-green   solid green
	@echo   make panel-blue  solid blue     make panel-gradient gradient
	@echo   make panel-checker checkerboard make panel-le      bars, other byte order
	@echo   make spi-loop    loopback sweep (jumper J3[19] to J3[21])
	@echo   make spi-frames  PING/DELAY/AUX_IN/CS smoke test
	@echo   make spi-pintest jumper continuity check (run this first on failure)
	@echo   make spi-dbg     SPI register snapshot
	@echo   make spi-bench   poll vs DMA timing
	@echo   make spi-info    config / profile / status
	@echo.
	@echo   One-click regression (results stream to build\regression\*.log+jsonl):
	@echo   make regression-swd       F103ZE: SRAM + RTT(20/45/60M) + HSS bench/run/fit
	@echo   make regression-swd-bg    same, background   make regression-swd-log to watch
	@echo   make regression-riscv     6800EVK: engine + RTT + HSS integrity/contract/fit
	@echo   make regression-riscv-bg  same, background   make regression-riscv-log to watch
	@echo   (needs PYHID interpreter with hidapi; floor 80 pct, FAIL = early exit)
	@echo   make clean       remove generated EVKLite output (checked-in ELF snapshots kept)
