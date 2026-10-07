"""Compile production inline gates and exercise every authoritative wake flag."""
from pathlib import Path
import os, re, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
src=root/'firmware/application_5301/src'
modules={'api/api_param':['s_save_pending'],
 'riscv/riscv_svc':['s_pending'],
 'rtt/rtt_bridge':['s_running','s_start_pending','s_stop_pending','s_raw_pending','s_bench_pending'],
 'scope/scope_sampler':['s_running','s_start_req','s_usb_reset_req','s_bench_req'],
 'spi_bridge/spi_bridge':['s_enabled','s_reset_req','s_usb_reset_req','s_abort_req','s_drain_reads','s_adc_owner','s_slave_owner'],
 'spi_cdc/spi_cdc':['s_running','s_start_req','s_stop_req','s_reset_req','s_fault','s_configuring']}
unit='#include <assert.h>\n'
for path in modules:
 name=Path(path).name
 unit+=f'#include "{name}.h"\nservice_gate_t {name}_gate;\n'
unit+='int main(void){\n'
for path,flags in modules.items():
 name=Path(path).name
 code=(src/(path+'.c')).read_text(encoding='utf-8')
 unit+=f'assert(!{name}_needs_service());\n'
 for flag in flags:
  match=re.search(r'#define '+flag+r' \('+name+r'_gate.flag\[(\d+)\]\)',code)
  assert match, (name,flag)
  n=match[1]
  # Pending flag must survive repeated gate reads until service consumes it.
  unit+=f'{name}_gate.flag[{n}]=1;assert({name}_needs_service());assert({name}_needs_service());'
  unit+=f'{name}_gate.flag[{n}]=0;assert(!{name}_needs_service());\n'
 # A new IRQ event after an idle check remains visible on next iteration.
 unit+=f'assert(!{name}_needs_service());{name}_gate.flag[0]=1;assert({name}_needs_service());{name}_gate.flag[0]=0;\n'
unit+='return 0;}\n'
with tempfile.TemporaryDirectory() as folder:
 p=Path(folder);(p/'test.c').write_text(unit)
 subprocess.run([os.getenv('CC','gcc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',
  *[f'-I{src / Path(path).parent}' for path in modules],str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
main=(src/'main.c').read_text(encoding='utf-8')
for path in modules:
 name=Path(path).name
 assert re.search(r'if \('+name+r'_needs_service\(\)\)\s*\{\s*'+name+r'_poll\(\);',main),name
print('Production idle gates: all running/control/reset/drain/ADC flags, non-consuming reads, late wake and main-loop wiring PASS')
