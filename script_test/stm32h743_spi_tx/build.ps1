$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$arm='E:/Share/env-windows/tools/gnu_gcc/arm_gcc/mingw/bin'
New-Item -ItemType Directory -Force "$root/build" | Out-Null
& "$arm/arm-none-eabi-gcc.exe" '-mcpu=cortex-m7' '-mthumb' '-mfpu=fpv5-d16' '-mfloat-abi=hard' '-O3' '-g3' '-gdwarf-4' '-ffreestanding' '-fno-builtin' '-ffunction-sections' '-fdata-sections' '-Wall' '-Wextra' '-nostdlib' "-T$root/ld/stm32h743.ld" '-Wl,--gc-sections' "-Wl,-Map=$root/build/fw.map" "$root/src/main.c" "$root/src/startup.c" -o "$root/fw.elf"
if ($LASTEXITCODE) {throw 'Build failed'}
& "$arm/arm-none-eabi-objcopy.exe" -O binary "$root/fw.elf" "$root/build/fw.bin"
& "$arm/arm-none-eabi-size.exe" "$root/fw.elf"
