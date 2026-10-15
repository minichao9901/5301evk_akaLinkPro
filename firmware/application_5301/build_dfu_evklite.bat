@echo off
@cd /d "%~dp0"
@REM ---------------------------------------------------------------------------
@REM Application build for HPM5301EVKLite (BOARD=hpm5301evklite).
@REM
@REM Same memory layout as the akaLinkPro build (flash_dfu, APP at 0x80020000,
@REM entry 0x80020100). Output goes to ./build_dfu_evklite so the akaLinkPro
@REM artifacts in ./build_dfu are not touched.
@REM
@REM Toolchain paths point at the local sdk_env install (E:\sdk_env_v1.11.0);
@REM override by setting HPM_SDK_ENV_DIR before running.
@REM ---------------------------------------------------------------------------
@if not defined HPM_SDK_ENV_DIR set "HPM_SDK_ENV_DIR=E:\sdk_env_v1.11.0"

@set "HPM_TOOLCHAIN_PATH=%HPM_SDK_ENV_DIR%"
@set "PATH=%HPM_TOOLCHAIN_PATH%\tools\python3;%HPM_TOOLCHAIN_PATH%\tools\cmake\bin;%HPM_TOOLCHAIN_PATH%\tools\ninja;%PATH%"
@set "HPM_SDK_BASE=%HPM_TOOLCHAIN_PATH%\hpm_sdk"
@set "GNURISCV_TOOLCHAIN_PATH=%HPM_TOOLCHAIN_PATH%\toolchains\rv32imac_zicsr_zifencei_multilib_b_ext-win"
@set "HPM_SDK_TOOLCHAIN_VARIANT=gcc"
@set "BOARD=hpm5301evklite"
@REM HPM SDK 1.11+ removed flash_dfu: flash_xip + the custom flash_dfu_app.ld
@REM (plus -DFLASH_XIP=0 -DFLASH_DFU=1 set in CMakeLists.txt) reproduces the
@REM same DFU layout (APP at 0x80020000, entry 0x80020100).
@set "HPM_BUILD_TYPE=flash_xip"

@cmake -G Ninja -DBOARD=%BOARD% -DHPM_BUILD_TYPE=%HPM_BUILD_TYPE% -DCMAKE_BUILD_TYPE=debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -B=./build_dfu_evklite -S .
@if errorlevel 1 exit /b %errorlevel%
@cmake --build ./build_dfu_evklite
@if errorlevel 1 exit /b %errorlevel%

@echo.
@echo [build_dfu_evklite] APP image (flash this): build_dfu_evklite\output\akaLinkPro_App_pack.hex / _pack.bin (linked at 0x80020000, entry 0x80020100)
@echo [build_dfu_evklite] raw (no header): build_dfu_evklite\output\akaLinkPro_App.hex / .bin
