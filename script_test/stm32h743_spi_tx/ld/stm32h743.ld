/* STM32H743（flash 版）：向量表/代码在 flash，**被采样的变量全部在 AXI SRAM**，栈在 DTCM。
 *
 * 🚨 内存布局是 H7 上最容易踩的地方：H7 的 RAM 是**散的** ——
 *    ITCM        0x00000000   64KB    内核私有总线
 *    DTCM        0x20000000  128KB    内核私有总线、零等待、**不需要开任何时钟**
 *    AXI SRAM    0x24000000  512KB    ← 本固件把 .data/.bss 放这里
 *    AHB SRAM    0x30000000  等      D2/D3 域（不用）
 *
 * 为什么不能只图省事把变量放 DTCM：**H7 的 ITCM/DTCM 是内核私有总线，外部调试器走 AHB-AP
 * 读不到**（本仓库 tools/target-firmware/stm32h743_rtt_speed/README.md 与 H7B0 的 RESULTS.md
 * 都写明）。J-Scope 靶子的全部意义就是"让探针把变量读出来"，所以 .data/.bss 必须落在 AXI SRAM。
 * 证据：`arm-none-eabi-nm build\fw.elf` → 所有 g_* 都是 0x24xxxxxx。
 *
 * 栈留在 DTCM：探针不需要读栈，而 DTCM 零等待、最省事。
 * （AXI SRAM 上电即可写：RCC_AHB3ENR 里根本没有 AXI SRAM 的时钟使能位，不需要开时钟。）
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
  DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K     /* 只放栈 */
  AXI   (rwx) : ORIGIN = 0x24000000, LENGTH = 512K     /* .data/.bss：探针读得到 */
}

_estack = ORIGIN(DTCM) + LENGTH(DTCM);

SECTIONS
{
  .isr_vector : {
    KEEP(*(.isr_vector))
  } > FLASH

  .text : {
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  _sidata = LOADADDR(.data);

  .data : {
    . = ALIGN(4);
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > AXI AT> FLASH

  .bss : {
    . = ALIGN(4);
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > AXI

  . = ALIGN(8);
  _end = .;
  PROVIDE(end = .);

  /* 明确报错：被采样的数据一旦溢出到别处，说明布局假设已经不成立 */
  ASSERT(_ebss <= ORIGIN(AXI) + LENGTH(AXI), "AXI SRAM overflow")
}
