"""Compile production state/init/ownership/completions; model NOLOAD and native DMA."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
# 这两个源文件里有中文注释：必须显式 utf-8，否则在默认编码不是 UTF-8 的机器上
# （Windows 的 GBK）直接 UnicodeDecodeError，整条 host 回归从这一条断掉。
text = (root / 'firmware/application_5301/src/spi_bridge/spi_bridge.c').read_text(encoding='utf-8')
main = (root / 'firmware/application_5301/src/main.c').read_text(encoding='utf-8')
assert main.index('spi_bridge_init();') < main.index('chry_dap_init('), 'initialize before USB callbacks'
def section(start, end, after=text.index("/* ============================== 状态")):
    a = text.index(start, after)
    return text[a:text.index(end, a)]
state = section('typedef struct\n{', '/* Shared buffers', text.index('/* ============================== 状态'))
helper = section('uint8_t spi_bridge_adc_flags(void)\n{', '/* ============================== 小工具')
init = section('void spi_bridge_init(void)', 'void spi_bridge_usb_ready', text.index('static void sb_set_enabled'))
out_done = section('void spi_bridge_out_done(uint32_t nbytes)', '/* ============================== IN 环')
in_done = section('void spi_bridge_in_done(uint32_t nbytes)', '/* ============================== 面板档')
source = '''#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "spi_bridge_proto.h"
#include "service_gate.h"
#define ATTR_PLACE_AT_WITH_ALIGNMENT(...)
#define SB_OUT_SLOTS 32U
#define SB_IN_SLOTS 16U
#define SB_DEF_DMA_THRESHOLD 100U
typedef uint32_t dma_resource_t;
typedef uint32_t spi_format_config_t;
static service_gate_t spi_bridge_gate;
static uint8_t s_out_buf[SB_OUT_SLOTS][SB_PKT_SIZE],s_in_buf[SB_IN_SLOTS][SB_PKT_SIZE];
static void sb_out_kick(void){}
static void sb_in_kick(void){}
''' + state + helper + init + out_done + in_done + '''
int main(void){
 uint32_t *capture=0;uint8_t *transmit=0;
 /* NOLOAD is not BSS: poison every field, not just hw_req. */
 memset(&s_st,0xA5,sizeof(s_st));memset(&spi_bridge_gate,0xA5,sizeof(spi_bridge_gate));
 spi_bridge_init();assert(!spi_bridge_adc_flags());assert(!service_gate_pending(&spi_bridge_gate));
 assert(!s_hw_req && !s_st.dma && !s_st.spi_need_reset && !s_st.format);
 assert(s_cfg.bits==8 && s_cfg.flags==SB_CFG_F_CLEAR_ON_ENABLE);
 assert(spi_bridge_adc_claim(&capture,&transmit));
 assert(capture==(uint32_t*)&s_out_buf[0][0] && transmit==&s_in_buf[0][0]);
 assert(!spi_bridge_adc_claim(&capture,&transmit));spi_bridge_adc_release();
'''
# Every rejected claim preserves native bookkeeping and the whole gate/state.
flags = ['s_enabled', 's_pkt_active', 's_adc_owner', 's_slave_owner', 's_out_inflight', 's_in_inflight',
         's_out_used', 's_in_used', 's_hw_req', 's_reset_req', 's_usb_reset_req',
         's_abort_req', 's_drain_reads', 's_cs_asserted']
for flag in flags:
    source += f'''{flag}=1;{{sb_state_t before=s_st;service_gate_t gate=spi_bridge_gate;
 assert(spi_bridge_adc_flags());assert(!spi_bridge_adc_claim(&capture,&transmit));
 assert(!memcmp(&before,&s_st,sizeof(s_st)));assert(!memcmp(&gate,&spi_bridge_gate,sizeof(gate)));}}{flag}=0;
'''
source += '''
 uint8_t *slave_rx=0;uint32_t slave_size=0;
 assert(spi_bridge_slave_claim(&slave_rx,&slave_size));
 assert(slave_rx==&s_out_buf[0][0] && slave_size==sizeof(s_out_buf));
 assert(!spi_bridge_adc_claim(&capture,&transmit));assert(!spi_bridge_slave_claim(&slave_rx,&slave_size));
 spi_bridge_slave_release();
 assert(spi_bridge_adc_claim(&capture,&transmit));assert(!spi_bridge_slave_claim(&slave_rx,&slave_size));spi_bridge_adc_release();
 /* Both native transfers still own their buffers after the bridge is disabled.
  * Only real completions release them; generations never change on a claim. */
 s_out_inflight=s_in_inflight=1;s_in_used=1;
 s_out_gen=s_out_armed_gen=7;s_in_gen=s_in_armed_gen=9;
 assert(!spi_bridge_adc_claim(&capture,&transmit));
 spi_bridge_out_done(0);assert(!s_out_inflight && s_in_inflight);
 assert(!spi_bridge_adc_claim(&capture,&transmit));
 spi_bridge_in_done(8);assert(!s_in_inflight && !s_in_used);
 assert(spi_bridge_adc_claim(&capture,&transmit));assert(s_out_gen==7 && s_in_gen==9);
 spi_bridge_adc_release();
 /* Explicit re-init must also clear a prior ownership/service state. */
 spi_bridge_init();assert(!spi_bridge_adc_flags());assert(!service_gate_pending(&spi_bridge_gate));
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(source, encoding='utf-8')   # 生成物里含抽取出来的中文注释，必须显式 utf-8
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root/'firmware/application_5301/src/spi_bridge'),
                    '-I', str(root/'firmware/application_5301/src/api'),
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('SPI/ADC: poisoned NOLOAD initialization, complete gate reset, failed claim preserves DMA, delayed IN/OUT completion fence PASS')
