param()
$ErrorActionPreference = 'Stop'
$gcc = (Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue).Source
if (!$gcc) { $gcc = 'E:\Share\env-windows\tools\gnu_gcc\arm_gcc\mingw\bin\arm-none-eabi-gcc.exe' }
$out = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$elf = Join-Path $PSScriptRoot 'fw.elf'
$args = @('-mcpu=cortex-m3','-mthumb','-O2','-g3','-gdwarf-4','-Wall','-Wextra',
  '-ffunction-sections','-fdata-sections','-nostartfiles','-specs=nano.specs','-specs=nosys.specs',
  '-Wl,--gc-sections',"-Wl,-Map=$out/fw.map", "-T$PSScriptRoot/../stm32f103_scope/ld/stm32f103ze.ld",
  "$PSScriptRoot/src/main.c", "$PSScriptRoot/../stm32f103_scope/src/startup.c", '-o', $elf)
& $gcc @args
if ($LASTEXITCODE) { throw 'SPI TX build failed' }
& (Join-Path (Split-Path $gcc) 'arm-none-eabi-objcopy.exe') -O binary $elf "$out/fw.bin"
& (Join-Path (Split-Path $gcc) 'arm-none-eabi-size.exe') $elf
