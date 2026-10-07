"""Production CDC producer handoff/reconfigure keeps in-flight USB storage."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
text=(root/'firmware/application_5301/src/usb/cdc_interface.c').read_text(encoding='utf-8')
def section(start,end):return text[text.index(start):text.index(end,text.index(start))]
source=r'''
#include <stdint.h>
#include <assert.h>
#define CSR_MSTATUS_MIE_MASK 8
#define CDC_SOURCE_UART 0
#define CDC_SOURCE_RTT 1
#define CDC_SOURCE_SPI 2
#define UART_RX_DMA_RESOURCE_INDEX 0
typedef struct {void *base;} dma_resource_t;
static dma_resource_t dma_resource_pools[1]={{(void*)1}};
static unsigned irq,configs,disable,restart,written=27;
static volatile uint8_t s_cdc_src_rtt;
static uint32_t rb_write_pos;
static uint32_t disable_global_irq(unsigned mask){(void)mask;assert(!irq);irq=1;return 0;}
static void restore_global_irq(unsigned lock){(void)lock;assert(irq);irq=0;}
static uint32_t uartx_rx_written(void){assert(irq);return written;}
static void chry_dap_usb2uart_request_config(void){assert(irq);configs++;}
static void dma_mgr_disable_channel(dma_resource_t *r){assert(irq&&r->base);disable++;}
static void uartx_rx_dma_start(void){assert(irq);restart++;}
// Deliberately no chry_ringbuffer_reset(): touching CDC storage fails compilation.
'''+section('uint8_t uartx_get_cdc_source(void)','ATTR_PLACE_AT_NONCACHEABLE_BSS_WITH_ALIGNMENT')+section('static void uartx_rx_dma_restart(void)','/* Pick the achievable UART2 baud')+r'''
int main(void){
 uartx_set_cdc_source(CDC_SOURCE_SPI);assert(uartx_get_cdc_source()==2&&!configs);
 uartx_rx_dma_restart();assert(!rb_write_pos&&restart==1&&disable==1);
 uartx_set_cdc_source(CDC_SOURCE_UART);assert(rb_write_pos==27&&configs==1);
 uartx_rx_dma_restart();assert(restart==2&&!rb_write_pos);
 uartx_set_cdc_source(CDC_SOURCE_UART);assert(configs==1);
 uartx_set_cdc_source(CDC_SOURCE_RTT);written=16383;uartx_set_cdc_source(CDC_SOURCE_UART);assert(rb_write_pos==16383&&configs==2);
 dma_resource_pools[0].base=0;uartx_rx_dma_restart();assert(restart==2);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    p=Path(folder);(p/'test.c').write_text(source,encoding='utf-8')
    subprocess.run([os.getenv('CC','gcc'),'-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('CDC production handoff: UART/RTT/SPI, latest line-coding reapply, separate DMA restart, no USB in-flight ring reset PASS')
