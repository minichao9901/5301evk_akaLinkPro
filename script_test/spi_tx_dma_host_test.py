"""Compile production DMA setup; verify width units, tail safety and failed setup.
Hardware byte-order validation is recorded in the SPI image throughput report.
"""
from pathlib import Path
import os, re, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
text = (root / 'firmware/application_5301/src/spi_bridge/spi_bridge.c').read_text(encoding='utf-8')
start = text.index('static hpm_stat_t sb_dma_tx_start(', text.index('static void sb_dma_init(void)\n{'))
end = text.index('/* 等 DMA', start)
setup = text[start:end].replace('(uint32_t)tx', '(uintptr_t)tx')
selection = re.search(r'    merge = \(uint8_t\).*?;', text).group()
source = r'''
#include <stdint.h>
#include <assert.h>
typedef int hpm_stat_t;
#define status_success 0
#define status_fail 1
#define DMA_MGR_TRANSFER_WIDTH_WORD 2
#define DMA_MGR_TRANSFER_WIDTH_BYTE 0
static int s_dma, calls, fail_at, started;
static unsigned src_width, dst_width, units;
static uintptr_t address;
static int step(void) { return ++calls == fail_at ? status_fail : status_success; }
static int dma_mgr_set_chn_src_width(const int *r,unsigned w) { assert(r==&s_dma);src_width=w;return step(); }
static int dma_mgr_set_chn_dst_width(const int *r,unsigned w) { assert(r==&s_dma);dst_width=w;return step(); }
static int dma_mgr_set_chn_src_addr(const int *r,uintptr_t a) { assert(r==&s_dma);address=a;return step(); }
static int dma_mgr_set_chn_transize(const int *r,unsigned n) { assert(r==&s_dma);units=n;return step(); }
static int dma_mgr_enable_channel(const int *r) { assert(r==&s_dma);started++;return step(); }
''' + setup + r'''
static unsigned eligible(const uint8_t *tx,uint32_t wlen,uint32_t rlen,unsigned use_dma) {
 uint8_t merge;
''' + selection + r'''
 return merge;
}
int main(void) {
 uint32_t data[128]={0};
 for(unsigned len=1;len<=492;len++) for(unsigned offset=0;offset<4;offset++) {
  const uint8_t *tx=(uint8_t*)data+offset;
  unsigned merge=eligible(tx,len,0,1);
  if(merge) assert(offset==0 && len%4==0);
  if(offset==0 && len%4==0) assert(merge);
  assert(!eligible(tx,len,len,1)); /* Full duplex/RX must retain byte reads. */
  assert(!eligible(tx,len,0,0)); /* Polling/NO_DMA never packs FIFO words. */
  calls=started=fail_at=0;
  assert(sb_dma_tx_start(tx,len,merge)==status_success && started==1);
  assert(src_width==dst_width && address==(uintptr_t)tx);
  assert(units*(1U<<src_width)==len); /* Exact bytes: no truncated or oversized tails. */
 }
 /* SDK failure must stop configuration immediately, without starting DMA on
  * partly updated source/destination widths, address or transfer count. */
 for(int stage=1;stage<=5;stage++) {
  calls=started=0;fail_at=stage;
  assert(sb_dma_tx_start((uint8_t*)data,492,1)==status_fail);
  assert(calls==stage && started==(stage==5));
 }
 fail_at=calls=started=0;
 assert(sb_dma_tx_start((uint8_t*)data,492,1)==status_success && units==123 && src_width==2);
 calls=started=0;
 assert(sb_dma_tx_start((uint8_t*)data+1,101,0)==status_success && units==101 && src_width==0);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(source, encoding='utf-8')
    subprocess.run([os.environ.get('CC','gcc'),'-std=c11','-Wall','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('SPI TX DMA: 1968 length/alignment cases, exact byte counts, RX/polling fallback, SDK failure containment PASS')
