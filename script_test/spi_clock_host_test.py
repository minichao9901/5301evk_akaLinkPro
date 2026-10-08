"""Compile production clock selection; verify divider limits and failed clock setup."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
text=(root/'firmware/application_5301/src/spi_bridge/spi_bridge.c').read_text(encoding='utf-8')
a=text.index('uint32_t spi_bridge_configure_clock(void)\n{')
b=text.index('/* TX DMA',a)
source=r'''
#include <stdint.h>
#include <assert.h>
#include "spi_bridge_proto.h"
#define SB_SPI_CLK_NAME 1
#define SB_SPI ((void*)1)
#define SB_DEF_SCLK_HZ 20000000U
#define SB_MAX_SCLK_HZ 100000000U
#define clock_source_pll0_clk0 1
#define clk_src_pll0_clk0 1
#define status_success 0
typedef struct {struct {uint32_t clk_src_freq_in_hz,sclk_freq_in_hz;}master_config;}spi_timing_config_t;
static uint32_t src=720000000,actual=240000000,s_module_clk,sets,fail;
static uint32_t get_frequency_for_source(int s){assert(s==1);return src;}
static int clock_set_source_divider(int c,int s,uint32_t d){assert(c==1&&s==1&&src/d==240000000);sets++;return fail==1;}
static void clock_add_to_group(int c,int g){assert(c==1&&g==0);}
static uint32_t clock_get_frequency(int c){assert(c==1);return actual;}
static void spi_master_get_default_timing_config(spi_timing_config_t *c){c->master_config.clk_src_freq_in_hz=c->master_config.sclk_freq_in_hz=0;}
static int spi_master_timing_init(void *p,spi_timing_config_t *c){
 assert(p==SB_SPI);uint32_t m=c->master_config.clk_src_freq_in_hz,h=c->master_config.sclk_freq_in_hz;
 assert(m==240000000&&h&&m%h==0&&!(m/h&1)&&m/h<=510);return fail==2;
}
'''+text[a:b]+r'''
int main(void){
 assert(sb_pick_sclk(0)==20000000);
 assert(sb_pick_sclk(20000000)==20000000&&sb_pick_sclk(40000000)==40000000&&sb_pick_sclk(60000000)==60000000);
 assert(sb_pick_sclk(75000000)==60000000&&sb_pick_sclk(100000000)==60000000);
 assert(sb_pick_sclk(500000)==500000&&sb_pick_sclk(480000)==480000&&sb_pick_sclk(479999)==0);
 for(uint32_t hz=480000;hz<=100000000;hz+=170003){uint32_t r=sb_pick_sclk(hz);assert(r&&r<=hz);}
 uint32_t before=sets;src=600000000;assert(!spi_bridge_configure_clock()&&sets==before);
 src=720000000;fail=1;assert(!sb_pick_sclk(20000000));fail=0;actual=120000000;assert(!sb_pick_sclk(20000000));
 actual=240000000;fail=2;assert(!sb_pick_sclk(20000000));return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.c').write_text(source,encoding='utf-8')
    subprocess.run([os.environ.get('CC','gcc'),'-std=c11','-Wall','-Werror','-I',str(root/'firmware/application_5301/src/spi_bridge'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('SPI fixed 240MHz: safe source selection, SCK bounds and clock/SDK failures PASS')
