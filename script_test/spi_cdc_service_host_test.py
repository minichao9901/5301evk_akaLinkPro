"""Compile the production service with SDK stubs; exercise DMA/CDC lifecycle.
Ring arithmetic and SPI/ADC buffer ownership also have separate regressions.
"""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
header=r'''
#ifndef TEST_HW_H
#define TEST_HW_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define CSR_MSTATUS_MIE_MASK 8
#define status_success 0
#define clock_spi2 0
#define IOC_PAD_PB10 10
#define IOC_PAD_PB13 13
#define GPIO_GET_PORT_INDEX(p) 0
#define GPIO_GET_PIN_INDEX(p) (p)
#define HPM_GPIO0 ((void*)1)
#define HPM_DMA_SRC_SPI2_RX 17
#define DMA_MGR_TRANSFER_WIDTH_BYTE 0
#define DMA_MGR_HANDSHAKE_MODE_HANDSHAKE 1
#define DMA_MGR_HANDSHAKE_MODE_NORMAL 0
#define DMA_MGR_ADDRESS_CONTROL_FIXED 0
#define DMA_MGR_ADDRESS_CONTROL_INCREMENT 1
#define DMA_MGR_CHANNEL_PRIORITY_HIGH 3
#define DMA_MGR_INTERRUPT_MASK_ALL 7
#define DMA_MGR_INTERRUPT_MASK_TC 1
#define DMA_MGR_INTERRUPT_MASK_ERROR 2
#define spi_sclk_high_idle 1
#define spi_sclk_low_idle 0
#define spi_sclk_sampling_even_clk_edges 1
#define spi_sclk_sampling_odd_clk_edges 0
#define spi_trans_write_read_together 1
#define spi_single_io_mode 0
#define spi_rx_fifo_overflow_int 4
#define CDC_SOURCE_UART 0
#define CDC_SOURCE_RTT 1
#define CDC_SOURCE_SPI 2
typedef struct {uint32_t INTTCSTS;struct {uint32_t DSTADDR;} CHCTRL[1];} DMA_Type;
typedef struct {DMA_Type *base;uint32_t channel;} dma_resource_t;
typedef struct {struct {uint32_t addr_len_in_bytes;}master_config;struct {uint32_t data_len_in_bits,lsb,cpol,cpha;}common_config;}spi_format_config_t;
typedef struct {struct {bool slave_data_only;}slave_config;struct {int trans_mode,data_phase_fmt;}common_config;}spi_control_config_t;
typedef struct {int src_width,dst_width,src_mode,dst_mode,src_addr_ctrl,dst_addr_ctrl;uint32_t src_addr,dst_addr,size_in_byte;bool en_dmamux,en_infiniteloop;int dmamux_src,priority,interrupt_mask;}dma_mgr_chn_conf_t;
static DMA_Type fake_dma;
static struct {uint32_t DATA;} fake_spi;
static struct {struct {uint32_t FUNC_CTL;}PAD[32];} fake_ioc;
#define HPM_SPI2 (&fake_spi)
#define HPM_IOC (&fake_ioc)
static uint8_t ring[16384],source,claimed,other_owner;
static unsigned fail_stage,releases,parks,enabled,dma_active,produced,fifo_status;
static unsigned arrival_on_write;
static void produce(unsigned n);
static void (*tc_cb)(DMA_Type*,uint32_t,void*),(*error_cb)(DMA_Type*,uint32_t,void*);
static struct {uint32_t used;uint8_t data[32768];}g_uartrx;
static uint32_t disable_global_irq(uint32_t m){(void)m;return 0;}
static void restore_global_irq(uint32_t v){(void)v;}
static uint8_t uartx_get_cdc_source(void){return source;}
static void uartx_set_cdc_source(uint8_t v){source=v;}
static void chry_dap_usb2uart_set_enabled(uint8_t v){enabled=v;}
static uint8_t spi_bridge_slave_claim(uint8_t **p,uint32_t *size){if(claimed||other_owner)return 0;claimed=1;*p=ring;*size=sizeof(ring);return 1;}
static void spi_bridge_slave_release(void){assert(!dma_active);claimed=0;}
static void spi_disable_rx_dma(void *p){(void)p;}
static void spi_enable_rx_dma(void *p){(void)p;assert(dma_active);}
static void dma_mgr_disable_channel(dma_resource_t *r){(void)r;dma_active=0;}
static void dma_mgr_release_resource(dma_resource_t *r){assert(!dma_active&&r->base);releases++;}
static void gpio_set_pin_input(void *p,int port,int pin){(void)p;(void)port;assert(pin>=10&&pin<=13);parks++;}
static void clock_add_to_group(int clock,int group){(void)clock;(void)group;}
static void init_spi2_bridge_pins(unsigned aux,unsigned cs){assert(aux==0&&cs==1);}
static void spi_slave_get_default_format_config(spi_format_config_t *c){memset(c,0,sizeof(*c));}
static void spi_format_init(void *p,spi_format_config_t *c){(void)p;assert(c->common_config.data_len_in_bits==8);}
static void spi_slave_get_default_control_config(spi_control_config_t *c){memset(c,0,sizeof(*c));}
static int spi_control_init(void *p,spi_control_config_t *c,unsigned n,unsigned m){(void)p;assert(c->slave_config.slave_data_only&&n==1&&m==1);return fail_stage==1;}
static int dma_mgr_request_resource(dma_resource_t *r){if(fail_stage==2)return 1;r->base=&fake_dma;r->channel=0;return 0;}
static void dma_mgr_get_default_chn_config(dma_mgr_chn_conf_t *c){memset(c,0,sizeof(*c));}
static int dma_mgr_setup_channel(dma_resource_t *r,dma_mgr_chn_conf_t *c){assert(c->en_infiniteloop&&c->size_in_byte==sizeof(ring)&&c->src_addr_ctrl==0&&c->dst_addr_ctrl==1);produced=0;r->base->INTTCSTS=0;r->base->CHCTRL[0].DSTADDR=c->dst_addr;return fail_stage==3;}
static void dma_mgr_install_chn_tc_callback(dma_resource_t *r,void(*cb)(DMA_Type*,uint32_t,void*),void *d){(void)r;(void)d;tc_cb=cb;}
static void dma_mgr_install_chn_error_callback(dma_resource_t *r,void(*cb)(DMA_Type*,uint32_t,void*),void *d){(void)r;(void)d;error_cb=cb;}
static void dma_mgr_enable_chn_irq(dma_resource_t *r,unsigned mask){(void)r;assert(mask==3);}
static void dma_mgr_enable_dma_irq_with_priority(dma_resource_t *r,unsigned p){(void)r;assert(p==1);}
static void dma_mgr_enable_channel(dma_resource_t *r){assert(r->base);dma_active=1;}
static uint32_t chry_ringbuffer_get_free(void *p){(void)p;return sizeof(g_uartrx.data)-g_uartrx.used;}
static uint32_t chry_ringbuffer_get_used(void *p){(void)p;return g_uartrx.used;}
static void chry_ringbuffer_write(void *p,const uint8_t *b,uint32_t n){(void)p;assert(n<=1024&&n<=chry_ringbuffer_get_free(p));memcpy(g_uartrx.data+g_uartrx.used,b,n);g_uartrx.used+=n;if(arrival_on_write)produce(arrival_on_write);}
static uint32_t spi_get_interrupt_status(void *p){(void)p;return fifo_status;}
static void spi_clear_interrupt_status(void *p,uint32_t bits){(void)p;fifo_status&=~bits;}
#endif
'''
test=r'''
#include "spi_cdc.c"
static uint8_t req[64],res[64];
static int32_t cmd(unsigned action){req[3]=action;spi_cdc_hid(req,res);assert(res[1]==54&&res[2]==0x39&&res[3]==action);int32_t rc;memcpy(&rc,res+12,4);return rc;}
static void produce(unsigned n){assert(dma_active);for(unsigned i=0;i<n;i++){ring[produced%sizeof(ring)]=(uint8_t)produced;produced++;if(!(produced%sizeof(ring)))tc_cb(&fake_dma,0,0);}fake_dma.CHCTRL[0].DSTADDR=(uint32_t)(uintptr_t)ring+produced%sizeof(ring);}
int main(void){
 assert(!spi_cdc_needs_service());assert(cmd(0)==0);
 req[4]=4;assert(cmd(1)==-5&&!spi_cdc_needs_service());req[4]=0;
#if BOARD_HAS_SPI_BRIDGE
 for(unsigned stage=1;stage<=3;stage++){
  fail_stage=stage;assert(cmd(1)==-100&&spi_cdc_needs_service());spi_cdc_poll();
  assert(cmd(0)==(stage==1?-4:-3));assert(!claimed&&!dma_active&&!spi_cdc_needs_service());
 }
 fail_stage=0;source=CDC_SOURCE_RTT;cmd(1);spi_cdc_poll();assert(cmd(0)==-2&&source==CDC_SOURCE_RTT&&!claimed);source=0;
 other_owner=1;cmd(1);spi_cdc_poll();assert(cmd(0)==-2&&!claimed);other_owner=0;
 // Both requests can arrive in USB IRQs before main services either.
 cmd(1);cmd(2);spi_cdc_poll();assert(!spi_cdc_running()&&!claimed&&!spi_cdc_needs_service());
 cmd(1);spi_cdc_usb_reset();spi_cdc_poll();assert(!spi_cdc_running()&&!claimed&&!spi_cdc_needs_service());
 cmd(1);spi_cdc_poll();assert(spi_cdc_running()&&claimed&&source==2&&enabled);
 // Short chunks and circular wrap preserve every byte, with a bounded poll.
 for(unsigned k=0;k<100;k++){
  produce(257);spi_cdc_poll();assert(g_uartrx.used==257);
  for(unsigned i=0;i<257;i++)assert(g_uartrx.data[i]==(uint8_t)(k*257+i));g_uartrx.used=0;
 }
 assert(s_received==25700&&s_forwarded==25700&&!s_dropped);
 // DMA keeps receiving while copying. Yield after the entry snapshot rather
 // than chasing incoming bytes until the 4KiB budget has been filled.
 produce(100);arrival_on_write=50;spi_cdc_poll();arrival_on_write=0;
 assert(g_uartrx.used==100&&s_received==25800&&produced==25850);
 g_uartrx.used=0;spi_cdc_poll();assert(g_uartrx.used==50&&s_read==25850);g_uartrx.used=0;
 // USB owns this entire ring: never overwrite its in-flight bytes.
 g_uartrx.used=sizeof(g_uartrx.data);memset(g_uartrx.data,0xa5,sizeof(g_uartrx.data));produce(20000);spi_cdc_poll();
 assert(s_received-s_read==8192&&s_dropped==11808);
 for(unsigned i=0;i<sizeof(g_uartrx.data);i++)assert(g_uartrx.data[i]==0xa5);
 g_uartrx.used=0;spi_cdc_poll();assert(g_uartrx.used==4096&&s_received-s_read==4096);g_uartrx.used=0;spi_cdc_poll();assert(g_uartrx.used==4096&&s_received==s_read);
 fifo_status=4;spi_cdc_poll();assert(s_overflow==1&&!fifo_status);
 unsigned kept=g_uartrx.used;cmd(2);assert(spi_cdc_running());spi_cdc_poll();assert(!spi_cdc_running()&&!claimed&&!dma_active&&source==0&&g_uartrx.used==kept);
 cmd(1);spi_cdc_poll();assert(!s_received&&!s_dropped);error_cb(&fake_dma,0,0);spi_cdc_poll();assert(cmd(0)==-3&&s_errors==1&&!claimed&&!dma_active&&!spi_cdc_needs_service());
 cmd(1);spi_cdc_poll();produce(13);spi_cdc_usb_reset();spi_cdc_poll();assert(!claimed&&!dma_active&&!spi_cdc_needs_service()&&s_dropped==13);
 assert(parks>=16&&releases>=4);
#else
 cmd(1);spi_cdc_poll();assert(cmd(0)==-1&&!spi_cdc_needs_service());cmd(2);spi_cdc_poll();assert(cmd(0)==0);
#endif
 return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    p=Path(folder)
    (p/'test_hw.h').write_text(header)
    for name in ['board.h','spi_bridge.h','usb_composite.h','cdc_interface.h','hpm_interrupt.h','hpm_spi_drv.h','hpm_dma_mgr.h','hpm_dmav2_drv.h','hpm_dmamux_src.h','hpm_clock_drv.h','hpm_gpio_drv.h','pinmux.h']:
        (p/name).write_text('#include "test_hw.h"\n')
    (p/'test.c').write_text(test)
    for supported in [0,1]:
        exe=p/f'test{supported}'
        subprocess.run([os.getenv('CC','gcc'),'-std=c11','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable','-Wno-pointer-to-int-cast','-Wno-misleading-indentation',f'-DBOARD_HAS_SPI_BRIDGE={supported}',
            '-I',str(p),'-I',str(root/'firmware/application_5301/src/spi_cdc'),'-I',str(root/'firmware/application_5301/src/api'),str(p/'test.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
print('SPI CDC production service: deferred START/STOP/reset, failure cleanup, CDC owner conflict, DMA wrap/content, bounded copying, in-flight USB protection, overflow/fault/restart and unsupported boards PASS')
