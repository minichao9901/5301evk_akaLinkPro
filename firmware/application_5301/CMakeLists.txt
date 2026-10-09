# Copyright (c) 2021 HPMicro
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.13)

set(APP_NAME akaLinkPro_App)

set(BOARD_SEARCH_PATH ${CMAKE_CURRENT_SOURCE_DIR}/boards)

# Prevent SDK from forcing -u _printf_float/-u _scanf_float (saves ~31KB)
set(HPM_SDK_LD_NO_NANO_SPECS 1)

# Enable CherryUSB device stack. CDC ACM + HID + MSC classes are needed for the
# CMSIS-DAP composite enumeration (CDC and custom-HID + MSC interfaces).
set(CONFIG_HPM_SPI 1)
set(CONFIG_DMA_MGR 1)
set(CONFIG_CHERRYRB 1)
set(CONFIG_CHERRYUSB 1)
set(CONFIG_CHERRYUSB_DEVICE 1)
set(CONFIG_USB_DEVICE 1)
set(CONFIG_USB_DEVICE_CDC_ACM 1)
set(CONFIG_USB_DEVICE_HID 1)
set(CONFIG_USB_DEVICE_MSC 1)
# NOTE: Do NOT set CONFIG_USB_DEVICE_DFU here.
# The APP only needs DFU runtime (usbd_core), not DFU mode (hpm_dfu_port.c).
# dap_main.c handles DFU DETACH/GETSTATUS/GETSTATE manually as a runtime iface.

# Custom linker script for the APP (DFU) layout: it reserves the first 256 bytes
# of the APP region for the integrity/version header so the code entry moves to
# 0x80020100 (signature stays at 0x80020000). Must be set before find_package.
# Always on: both the classic flash_dfu type (older SDKs) and the flash_xip
# emulation below (HPM SDK 1.11+ removed flash_dfu) link through this script.
set(CUSTOM_GCC_LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linker/flash_dfu_app.ld)

# ---------------------------------------------------------------- DLM 空间 ----
# DLM 只有 128 KB（0x80300 起），曾经用到 127072 B = 97.5%。这两块是白拿的：
#
# ① 堆 16 KB → 2 KB：SDK 的 HEAP_SIZE/STACK_SIZE 默认 0x4000 是抄 Segger 的模板值
#    （cmake/application.cmake:307/312），不是按本工程算的。本工程**没有任何
#    malloc/calloc/realloc 调用**（cherryusb 只有 host 栈和 RTOS OSAL 用 malloc，
#    都没链进来），堆的唯一入口是 newlib stdio 的 setvbuf/__sfmoreglue（stdout
#    缓冲，BUFSIZ=1 KB 量级），留 2 KB 已是一倍余量。
#    ⚠️ 必须在 find_package 之前设：application.cmake 用 `if(NOT HEAP_SIZE)` 兜底。
#    栈暂不动（真实用量还没量过，见 README 的 DLM 账）。
set(HEAP_SIZE 0x800)      # 2 KB

# ② USB 的 QHD/QTD 竞技场 10 KB → 4 KB：`dcd_data_t` 的大小由
#    USB_SOC_DCD_QTD_COUNT_EACH_ENDPOINT 决定（qtd 池 = 16 端点 × 2 方向 × 该值 ×
#    32 B；默认 8 → 8192 B）。SDK 自己的约束是"单笔传输需要的 QTD 数 ≤ 该值"，而
#    一个 QTD 覆盖 16 KB（hpm_usb_device.c:262 与 xfer_len = 0x4000 的分块）——
#    本工程单笔最大 1 KB（DAP_XFER_SIZE；scope 包 512 B、MSC 512 B/块），所以 2
#    已经是 32 KB 的单笔上限、共享池 64 项，余量很大。
#    ⚠️ 两个坑：一是**必须走 CMAKE_C_FLAGS**——SDK 的库是在 find_package 里
#    add_subdirectory 出来的，sdk_compile_definitions() 之后的调用传不进那些已创建的
#    目标，而结构体布局 app 与 SDK 必须一致（不一致就是 ABI 错乱）；二是它必须在
#    find_package 之前设好。
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DUSB_SOC_DCD_QTD_COUNT_EACH_ENDPOINT=2")

find_package(hpm-sdk REQUIRED HINTS $ENV{HPM_SDK_BASE})
project(akaLinkPro_App)

# compile opt
sdk_compile_options(-O3)

# linker opt
# sdk_ld_options(-flto)

# Use nano libc (saves ~31KB)
sdk_ld_options("--specs=nano.specs")

# CherryUSB config — pick up local usb_config.h
sdk_compile_definitions(-D__INCLUDE_CHERRYUSB_CONFIG_H__)

# Board-specific JTAG bit-bang assembly pin mapping.
# hpm5301evklite wires the DAP target port to the J5 header: TCK=PA06,
# TMS=PA07, TDI=PA05, TDO=PA04 (defaults are the akaLinkPro mapping).
# The SWD blobs for this mapping are pre-generated and selected by
# BOARD_SWD_BLOB_EVKLITE in boards/hpm5301evklite/board.h.
if(BOARD STREQUAL "hpm5301evklite")
    sdk_compile_definitions(
        -DJTAG_PIN_JTCK_SHIFT=6
        -DJTAG_PIN_JTMS_SHIFT=7
        -DJTAG_PIN_JTDI_SHIFT=5
        -DJTAG_PIN_JTDO_SHIFT=4
    )
endif()

# Default to the user-selected 240 MHz profile (existing PLL0CLK0 / 3).
# HPM5300 DS Rev0.11 permits 100 MHz; 180/200/240 exceed that rating.
set(UART2_CLOCK_MHZ "" CACHE STRING "CDC UART input clock: 80/100/180/200/240 MHz")
if(NOT UART2_CLOCK_MHZ STREQUAL "")
    if(NOT UART2_CLOCK_MHZ MATCHES "^(80|100|180|200|240)$")
        message(FATAL_ERROR "UART2_CLOCK_MHZ must be 80, 100, 180, 200 or 240")
    endif()
    sdk_compile_definitions(-DUART2_CLOCK_MHZ=${UART2_CLOCK_MHZ})
endif()

# Reserve the first 128K of flash for the bootloader when using the DFU layout
# (flash_dfu => APP linked at 0x80020000). A flash_xip build links at
# 0x80000000 and must not reserve anything.
#
# Also shrink _flash_size by 8K (0x2000) so the APP flash region ends at
# 0x800FE000: the last two 4K sectors (0x800FE000/0x800FF000) are reserved for
# the parameter store and must never be claimed by APP code/data.
if(NOT HPM_BUILD_TYPE STREQUAL "flash_xip")
    sdk_linker_global_symbols("_dfu_bl_length=0x20000" "_flash_size=0xFE000")
endif()

# HPM SDK 1.11+ removed the flash_dfu build type. The evklite build scripts
# pass -DHPM_BUILD_TYPE=flash_xip together with the custom linker script
# above; override the SDK's FLASH_XIP=1 (no .nor_cfg_option in the APP image)
# and define FLASH_DFU=1 so the .dfu_signature lands at 0x80020000.
if(HPM_BUILD_TYPE STREQUAL "flash_xip")
    sdk_linker_global_symbols("_dfu_bl_length=0x20000" "_flash_size=0xFE000")
    sdk_compile_definitions(-DFLASH_XIP=0 -DFLASH_DFU=1)
endif()

# sdk_compile_definitions(-DCONFIG_NDEBUG_CONSOLE=1)

sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/dfu)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/usb)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/dap)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/api)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/led)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/rtt)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/riscv)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/scope)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/spi_bridge)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/spi_cdc)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/i2c_bridge)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/bus_periodic)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/swd_host)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/drv)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/easyflash/inc)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/easyflash/port)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/dap/SW_DP)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/dap/JTAG_DP)

sdk_app_src(src/main.c)
sdk_app_src(src/dfu/hpm_dfu_trigger.c)
sdk_app_src(src/usb/usb_composite.c)
sdk_app_src(src/usb/hid_interface.c)
sdk_app_src(src/usb/cdc_interface.c)
sdk_app_src(src/usb/swo_rx_clock.c)
sdk_app_src(src/usb/msc_interface.c)
sdk_app_src(src/api/api_param.c)
sdk_app_src(src/led/led_state.c)
sdk_app_src(src/rtt/rtt_bridge.c)
sdk_app_src(src/scope/scope_sampler.c)
sdk_app_src(src/spi_bridge/spi_bridge.c)
sdk_app_src(src/spi_cdc/spi_cdc.c)
sdk_app_src(src/i2c_bridge/i2c_bridge.c)
sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/src/analog_bridge)
sdk_app_src(src/analog_bridge/analog_bridge.c)
sdk_app_src(src/analog_bridge/adc_stream.c)
sdk_app_src(src/analog_bridge/adc_dma_hw.c)
sdk_app_src(src/bus_periodic/bus_periodic.c)
sdk_app_src(src/bus_periodic/bus_periodic_timer.c)
sdk_app_src(src/riscv/riscv_jtag.c)
sdk_app_src(src/riscv/riscv_svc.c)
sdk_app_src(src/swd_host/swd_host.c)
sdk_app_src(src/swd_host/swd_host_port.c)
sdk_app_src(src/drv/drv_flash.c)
sdk_app_src(src/easyflash/src/easyflash.c)
sdk_app_src(src/easyflash/src/ef_env.c)
sdk_app_src(src/easyflash/src/ef_utils.c)
sdk_app_src(src/easyflash/port/ef_port.c)

sdk_app_src(src/dap/DAP.c)
sdk_app_src(src/dap/SWO.c)
sdk_app_src(src/dap/SW_DP/SW_SPEED_CALC.c)
sdk_app_src(src/dap/SW_DP/SW_DP.c)
sdk_app_src(src/dap/SW_DP/SW_DP_GPIO_Slow.c)
sdk_app_src(src/dap/SW_DP/SW_DP_GPIO_ASM_20M.S)
sdk_app_src(src/dap/SW_DP/SW_DP_GPIO_ASM_30M.S)
sdk_app_src(src/dap/SW_DP/SW_DP_GPIO_ASM_36M.S)
sdk_app_src(src/dap/SW_DP/SW_DP_GPIO_ASM_45M.S)
sdk_app_src(src/dap/SW_DP/SW_DP_GPIO_ASM_60M.S)
sdk_app_src(src/dap/SW_DP/SW_DP_GPIO_ASM_SLOW.S)
sdk_app_src(src/dap/JTAG_DP/JTAG_DP.c)
sdk_app_src(src/dap/JTAG_DP/JTAG_DP_GPIO_Slow.c)
sdk_app_src(src/dap/JTAG_DP/JTAG_DP_GPIO_ASM_45M.S)
sdk_app_src(src/dap/JTAG_DP/JTAG_DP_GPIO_ASM_DMI.S)

# DMI 扫描的时序旋钮。三个都已经实测到最小值，**别再往下减**（判据见
# docs/hpm6800evk-jtag.md §4 与 script_test/README.md 的回归基线表）：
#
#   DMI_CAP_HIGH_NOP   移位相位高电平里额外的 nop。TDI 预置方案下，上升沿到采样
#                      之间本来就有 3(累加)+3(TDI 预置) 条指令垫着，所以这里只需
#                      补 1 拍；它的作用仍是覆盖电平转换往返 + 目标 TCK->TDO 延迟，
#                      太小会让每个响应晚一位。
#   DMI_TCK_LOW_NOP    移位相位**低电平**保持拍数。TDI 已在高相位摆好，这里只受
#                      目标最小 TCK 低电平宽度约束，所以远小于 DMI_NAV_LOW_NOP。
#   DMI_NAV_*_NOP      导航/idle 时钟的低/高电平保持。低电平那 8 拍**不能省**：
#                      TMS/TDI 与 TCK 拉低在同一次 DO_VAL 写里，少了会采到旧 TMS。
#
# 扫描用：set DMI_TCK_LOW_NOP=2 && build（见 script_test/hpm6800_timing_sweep.ps1）
set(DMI_CAP_HIGH_NOP "1"  CACHE STRING "JTAG DMI shift-phase extra high-phase nops")
set(DMI_TCK_LOW_NOP  "4"  CACHE STRING "JTAG DMI shift-phase TCK-low nop count")
set(DMI_BIT40_HIGH_NOP "8" CACHE STRING "JTAG DMI bit40 sample delay nops")
set(DMI_NAV_LOW_NOP  "6"  CACHE STRING "JTAG DMI nav/idle low-phase nop count")
set(DMI_NAV_HIGH_NOP "4"  CACHE STRING "JTAG DMI nav/idle high-phase nop count")
foreach(_knob DMI_CAP_HIGH_NOP DMI_TCK_LOW_NOP DMI_BIT40_HIGH_NOP DMI_NAV_LOW_NOP DMI_NAV_HIGH_NOP)
    if(DEFINED ENV{${_knob}})
        set(${_knob} "$ENV{${_knob}}" CACHE STRING "overridden from the environment" FORCE)
    endif()
endforeach()
sdk_compile_definitions(-DDMI_CAP_HIGH_NOP=${DMI_CAP_HIGH_NOP})
sdk_compile_definitions(-DDMI_TCK_LOW_NOP=${DMI_TCK_LOW_NOP})
sdk_compile_definitions(-DDMI_BIT40_HIGH_NOP=${DMI_BIT40_HIGH_NOP})
sdk_compile_definitions(-DDMI_NAV_LOW_NOP=${DMI_NAV_LOW_NOP})
sdk_compile_definitions(-DDMI_NAV_HIGH_NOP=${DMI_NAV_HIGH_NOP})

# generate_ide_projects()

# Also emit an Intel HEX image (absolute addresses) so the application can be
# flashed directly with J-Link (loadfile), independent of the DFU bootloader.
set(ELF ${CMAKE_BINARY_DIR}/output/${APP_NAME}.elf)
set(HEX ${CMAKE_BINARY_DIR}/output/${APP_NAME}.hex)

add_custom_command(OUTPUT ${HEX}
    COMMAND ${CMAKE_OBJCOPY} -O ihex -S ${ELF} ${HEX}
    DEPENDS ${ELF}
    COMMENT "Generating ${APP_NAME}.hex"
)
add_custom_target(hex_artifacts ALL DEPENDS ${HEX})

# ---------------------------------------------------------------------------
# Post-build pack: inject the APP integrity/version header (length, CRC32,
# version, build time, description) into the raw .bin and produce the packed
# .bin (dfu-util) and .hex (J-Link).
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_EXECUTABLE)
    set(Python3_EXECUTABLE "$ENV{HPM_SDK_BASE}/../tools/python3/python.exe")
endif()

set(FW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(APP_RAW_BIN ${CMAKE_BINARY_DIR}/output/${APP_NAME}.bin)
set(APP_PACK_BIN ${CMAKE_BINARY_DIR}/output/${APP_NAME}_pack.bin)
set(APP_PACK_HEX ${CMAKE_BINARY_DIR}/output/${APP_NAME}_pack.hex)

add_custom_command(TARGET ${APP_ELF_NAME} POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${FW_ROOT}/tools/pack.py app
            --bin ${APP_RAW_BIN}
            --out-bin ${APP_PACK_BIN}
            --out-hex ${APP_PACK_HEX}
            --version-json ${FW_ROOT}/version.json
    COMMENT "Packing APP image (header + CRC32 + version info)"
)
