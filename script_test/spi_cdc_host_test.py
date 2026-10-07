"""Production RX ring math: DMA wrap/late TC, loss accounting, CDC backpressure.
No attached hardware used; shared buffer claim/gate has separate C regression.
"""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
src=r'''
#include <assert.h>
#include <stdint.h>
#include "spi_cdc_ring.h"
int main(void) {
 const uint32_t n=16384;
 assert(spi_cdc_position(0,0,42,n,0)==42);
 assert(spi_cdc_position(0,0,n,n,n-1)==n); /* destination advanced before TC */
 assert(spi_cdc_position(0,1,0,n,n-1)==n); /* TC pending, IRQ not serviced */
 assert(spi_cdc_position(1,0,n,n,n-1)==n); /* serviced TC, reload in progress */
 assert(spi_cdc_position(1,0,7,n,n)==n+7);
 assert(spi_cdc_position(1,0,n,n,2*n-1)==2*n);
 assert(spi_cdc_position(1,1,0,n,2*n-1)==2*n);
 assert(spi_cdc_position(2,0,n,n,2*n-1)==2*n);
 assert(spi_cdc_position(262143,1,3,n,0xfffffff0U)==3); /* 32bit rollover */
 uint32_t read=0,drop=0,forward=0;
 /* Advancing producer, variable CDC availability and varying copy budgets. */
 for(uint32_t writer=0;writer<1000000;writer+=137) {
   drop+=spi_cdc_trim(writer,&read,n);
   uint32_t free=(writer/137)%5==0?0:511;
   uint32_t bytes=spi_cdc_chunk(writer,read,n,free,233);
   assert(bytes<=free && bytes<=233 && bytes<=writer-read);
   assert(bytes<=n-read%n);
   read+=bytes;forward+=bytes;
   assert(read==drop+forward && writer-read<=n/2);
 }
 assert(spi_cdc_chunk(200,199,n,500,4096)==1); /* partial final byte */
 assert(spi_cdc_chunk(n+8,n-3,n,500,4096)==3); /* no span crosses DMA end */
 assert(spi_cdc_chunk(5000,0,n,10000,4096)==1024); /* bounded IRQ/copy time */
 assert(spi_cdc_chunk(10,0,n,0,4096)==0); /* USB buffer never overwritten */
 read=0;assert(spi_cdc_trim(n+40,&read,n)==n/2+40);
 assert(read==n/2+40);
 read=0xffffff00U;drop=spi_cdc_trim(100,&read,n);assert(!drop);
 assert(spi_cdc_chunk(100,read,n,1000,4096)==256);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    p=Path(folder);(p/'test.c').write_text(src)
    subprocess.run([os.getenv('CC','gcc'),'-std=c11','-Wall','-Wextra','-Werror','-I',str(root/'firmware/application_5301/src/spi_cdc'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('SPI CDC production math: DMA edge/IRQ epochs, wrap, partial tails, CDC backpressure, bounded chunks and exact loss accounting PASS')
