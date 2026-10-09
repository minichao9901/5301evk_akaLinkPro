"""Link the production script with populated optional RAM sections and check startup ranges.
Requires the SDK RISC-V GCC and pyelftools; no hardware is accessed.
"""
from pathlib import Path
import argparse, subprocess, tempfile
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--gcc', default='E:/sdk_env_v1.11.0/toolchains/rv32imac_zicsr_zifencei_multilib_b_ext-win/bin/riscv32-unknown-elf-gcc.exe')
args = parser.parse_args()
SCRIPT = ROOT / 'firmware/application_5301/linker/flash_dfu_app.ld'
SOURCE = r'''
.section .dfu_signature,"a"
.word 0x48504d21
.section .start,"ax"
.global _start
_start:
.option push
.option norelax
la gp, __global_pointer$
.option pop
la a0, small_zero
lw a1, small_init
j _start
.section .rodata,"a"
.word __ramfunc_start__, __ramfunc_end__, __tdata_start__, __tdata_end__
.section .vector_table,"a"
.word _start, _start
.section .fast,"ax"
nop
.section .data,"aw"
.global ordinary_init
ordinary_init: .word 0x11223344
.space 8192
.section .init_array,"aw"
.word _start
.section .sdata,"aw"
.global small_init
small_init: .word 0x13579bdf
.space 508
.section .sbss,"aw",@nobits
.global small_zero
small_zero: .space 512
.section .bss,"aw",@nobits
.global ordinary_zero
ordinary_zero: .space 8192
.section .tdata,"awT",@progbits
.word 0x2468ace0
.section .tbss,"awT",@nobits
.space 16
.section .noncacheable.init,"aw"
.word 0x55667788
.section .fast_ram.init,"aw"
.word 0x99aabbcc
.section .noncacheable.bss,"aw",@nobits
.space 32
.section .fast_ram.bss,"aw",@nobits
.space 32
'''

def link(directory, extra=''):
    src, output = directory / 'fixture.S', directory / 'fixture.elf'
    src.write_text(SOURCE + extra, encoding='utf-8')
    command = [args.gcc, '-march=rv32imac_zicsr_zifencei', '-mabi=ilp32', '-nostdlib',
               '-Wl,-T,' + str(SCRIPT), '-Wl,--defsym=_stack_size=0x4000',
               '-Wl,--defsym=_heap_size=0x800', '-Wl,--defsym=_flash_size=0xfe000',
               str(src), '-o', str(output)]
    return subprocess.run(command, capture_output=True, text=True), output

with tempfile.TemporaryDirectory(prefix='akalink-gp-') as temp:
    directory = Path(temp)
    result, path = link(directory)
    assert result.returncode == 0, result.stderr
    with path.open('rb') as file:
        elf = ELFFile(file)
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        sections = {s.name: s for s in elf.iter_sections()}
        gp = symbols['__global_pointer$']
        assert symbols['_start'] == 0x80020100
        assert 0 <= symbols['__sbss_start__'] - symbols['__sdata_end__'] < 8
        assert symbols['__sdata_start__'] >= gp - 2048
        assert symbols['__sbss_end__'] <= gp + 2048
        assert symbols['ordinary_zero'] >= symbols['__sbss_end__']
        assert sections['.bss']['sh_type'] == 'SHT_NOBITS'
        # Emulate the SDK's clear-then-copy startup from actual ELF LMAs.
        ram, flash = {}, {}
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_LOAD':
                for i, byte in enumerate(segment.data()):
                    flash[segment['p_paddr'] + i] = byte
        for begin, end in [('__bss_start__', '__bss_end__'),
                           ('__noncacheable_bss_start__', '__noncacheable_bss_end__'),
                           ('__fast_ram_bss_start__', '__fast_ram_bss_end__')]:
            for address in range(symbols[begin], symbols[end]):
                ram[address] = 0
        for section, start, end, load in [
            ('.vectors', '__vector_ram_start__', '__vector_ram_end__', '__vector_load_addr__'),
            ('.data', '__data_start__', '__data_end__', '__data_load_addr__'),
            ('.fast', '__ramfunc_start__', '__ramfunc_end__', '__fast_load_addr__'),
            ('.tdata', '__tdata_start__', '__tdata_end__', '__tdata_load_addr__'),
            ('.noncacheable.init', '__noncacheable_init_start__', '__noncacheable_init_end__', '__noncacheable_init_load_addr__'),
            ('.fast_ram.init', '__fast_ram_init_start__', '__fast_ram_init_end__', '__fast_ram_init_load_addr__')]:
            content = sections[section].data()
            assert len(content) == symbols[end] - symbols[start]
            for i in range(len(content)):
                address = symbols[start] + i
                assert address not in ram, f'copy/zero ranges overlap at {address:#x}'
                ram[address] = flash[symbols[load] + i]
            assert bytes(ram[symbols[start] + i] for i in range(len(content))) == content
        for name, size in [('small_zero', 512), ('ordinary_zero', 8192)]:
            assert bytes(ram[symbols[name] + i] for i in range(size)) == bytes(size)
        assert bytes(ram[symbols['small_init'] + i] for i in range(4)) == bytes.fromhex('df9b5713')
        print(f'PASS: populated TLS/init RAM, copy/zero isolation, adjacent small data, DFU entry; gp={gp:#x}')
    result, _ = link(directory, '\n.section .sbss,"aw",@nobits\n.space 4096\n')
    assert result.returncode != 0 and 'sdata/sbss exceed' in result.stderr, result.stderr
    print('PASS: overflowing small-data window rejected by the production linker')
