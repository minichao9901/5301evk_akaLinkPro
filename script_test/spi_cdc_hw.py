"""F103ZE SPI1 -> probe SPI2 slave DMA -> CDC integrity / throughput matrix.
Requires the built/flashed stm32f103_spi_tx/fw.elf and current probe APP.
py -u script_test/spi_cdc_hw.py --dividers 3,2,1 --seconds 5 --json build/spi-cdc.json
Clock divisors are F1 BR encodings: 3=4.5MHz, 2=9MHz, 1=18MHz at 72MHz CPU.
"""
import argparse, json, re, struct, subprocess, threading, time
from pathlib import Path
import serial
from serial.tools.list_ports import comports
from spi_bridge_test import Hid

ROOT=Path(__file__).resolve().parents[1]
OCD=Path('E:/Share/env-windows/xpack-openocd-0.12.0-6')
ARM=Path('E:/Share/env-windows/tools/gnu_gcc/arm_gcc/mingw/bin')
CMD=0x39
def status(h,action=0,args=()):
    raw=h.xfer(CMD,[action,*args],tmo=2)
    if not raw or raw[3]!=action: raise RuntimeError('SPI CDC HID timeout/unsupported firmware')
    w=struct.unpack('<13I',bytes(raw[4:56]))
    if w[0]!=0x31435053: raise RuntimeError('missing SPC1 capability')
    return dict(zip(['magic','flags','rc','config','buffer','received','forwarded','dropped','fifo_overflows','pending','cdc_queued','generation','dma_errors'],w))
def settled(h):
    for _ in range(100):
        s=status(h)
        if not s['flags']&4 and s['rc']!=0xffffff9c:return s
        time.sleep(.02)
    raise RuntimeError('SPI CDC start/stop did not settle')
def symbols():
    out=subprocess.check_output([str(ARM/'arm-none-eabi-nm.exe'),str(ROOT/'script_test/stm32f103_spi_tx/fw.elf')],text=True)
    return {line.split()[2]:int(line.split()[0],16) for line in out.splitlines() if len(line.split())==3}
def target(sym,run,divider=1,pattern=1,mode=0,lsb=0):
    writes=[f'mww 0x{sym["g_run"]:x} 0','sleep 10']
    if run:writes += [f'mww 0x{sym["g_div"]:x} {divider}',f'mww 0x{sym["g_pattern"]:x} {pattern}',f'mww 0x{sym["g_mode"]:x} {mode}',f'mww 0x{sym["g_lsb"]:x} {lsb}',f'mww 0x{sym["g_run"]:x} 1','sleep 10']
    reads=[f'echo "METRIC {k} [read_memory 0x{sym[k]:x} 32 1]"' for k in ['g_core_hz','g_spi_hz','g_refill_late']]
    command=['init',*writes,*reads,'shutdown']
    result=subprocess.run([str(OCD/'bin/openocd.exe'),'-s',str(OCD/'openocd/scripts'),'-f','interface/cmsis-dap.cfg','-c','cmsis-dap backend usb_bulk','-f','target/stm32f1x.cfg','-c','adapter speed 1000','-c','gdb port disabled','-c','tcl port disabled','-c','telnet port disabled','-c','; '.join(command)],capture_output=True,text=True,timeout=20)
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    output=result.stdout+result.stderr
    (ROOT/'build').mkdir(exist_ok=True)
    (ROOT/'build/spi-cdc-target.log').write_text(output,encoding='utf-8')
    values={}
    for key in ['g_core_hz','g_spi_hz','g_refill_late']:
        match=re.search(r'METRIC '+key+r'\s+(0x[0-9a-fA-F]+|\d+)\b',output)
        if not match:raise RuntimeError('Missing target metric '+key+'\n'+output)
        values[key]=int(match[1],0)
    return values
class Reader:
    payloads=[bytes(((s+17*i)^0x5a)&255 for i in range(8,64)) for s in range(256)]
    def __init__(self,port,hello=False):
        self.port=port;self.hello=hello;self.stop=False;self.bytes=self.frames=self.gaps=self.bad=self.reversed=0
        self.buffer=bytearray();self.sample=bytearray();self.last=None;self.error=None
        self.thread=threading.Thread(target=self.read,daemon=True);self.thread.start()
    def read(self):
        try:
            while not self.stop:
                b=self.port.read(min(65536,max(1,self.port.in_waiting)))
                if not b:continue
                self.bytes+=len(b)
                if len(self.sample)<128:self.sample.extend(b[:128-len(self.sample)])
                if self.hello:
                    pattern=b'hello world!\r\n'
                    if self.last is None:
                        self.last=pattern.find(b[:1])
                        if self.last<0:self.last=0
                    expected=(pattern*((len(b)+self.last)//len(pattern)+1))[self.last:self.last+len(b)]
                    self.bad+=sum(x!=y for x,y in zip(b,expected))
                    self.last=(self.last+len(b))%len(pattern)
                    continue
                self.buffer.extend(b);head=0
                while len(self.buffer)-head>=64:
                    if self.buffer[head:head+4]!=b'SPIC':
                        pos=self.buffer.find(b'SPIC',head+1)
                        if pos<0:head=len(self.buffer)-3;self.bad+=1;break
                        head=pos;self.bad+=1;continue
                    sequence=struct.unpack_from('<I',self.buffer,head+4)[0]
                    if self.buffer[head+8:head+64]!=self.payloads[sequence&255]:self.bad+=1;head+=1;continue
                    if self.last is not None:
                        gap=(sequence-self.last-1)&0xffffffff
                        if gap<0x80000000:self.gaps+=gap
                        else:self.reversed+=1
                    self.last=sequence;self.frames+=1;head+=64
                if head:del self.buffer[:head]
        except Exception as error:self.error=repr(error)
    def snapshot(self):return {k:getattr(self,k) for k in ['bytes','frames','gaps','bad','reversed']}
    def close(self):self.stop=True;self.thread.join(1)
def main():
    p=argparse.ArgumentParser();p.add_argument('--seconds',type=float,default=5);p.add_argument('--dividers',default='3,2,1');p.add_argument('--modes',default='0');p.add_argument('--orders',default='msb');p.add_argument('--hello',action='store_true');p.add_argument('--port');p.add_argument('--json',default='build/spi-cdc.json');a=p.parse_args()
    dividers=list(map(int,a.dividers.split(',')));modes=list(map(int,a.modes.split(',')));orders=a.orders.split(',')
    if a.seconds<=0 or any(d<0 or d>7 for d in dividers) or any(m<0 or m>3 for m in modes) or any(o not in ['msb','lsb'] for o in orders):p.error('invalid duration, divider, mode or order')
    port=a.port or next(i.device for i in comports() if i.vid==0x0d28 and i.pid==0x0204)
    sym=symbols();h=Hid();results=[]
    try:
        target(sym,False);h.xfer(0x31,[0]);status(h,2);settled(h)
        with serial.Serial(port,115200,timeout=.05) as ser:
            for divider,mode,order in ((d,m,o) for d in dividers for m in modes for o in orders):
                target(sym,False);status(h,2);settled(h);ser.reset_input_buffer()
                reader=Reader(ser,a.hello)
                try:
                    status(h,1,[mode,int(order=='lsb')]);s=settled(h)
                    if s['rc']!=0 or not s['flags']&2:raise RuntimeError('start failed '+str(s))
                    info=target(sym,True,divider,0 if a.hello else 1,mode,int(order=='lsb'))
                    time.sleep(.5);initial=reader.snapshot();before=status(h);t0=time.perf_counter()
                    time.sleep(a.seconds);elapsed=time.perf_counter()-t0;last=reader.snapshot();after=status(h)
                    result={'divider':divider,'mode':mode,'order':order,'seconds':elapsed,'target':info,'host':{k:last[k]-initial[k] for k in last},'before':before,'after':after,'sample_hex':reader.sample.hex(),'reader_error':reader.error}
                    result['MBps']=result['host']['bytes']/elapsed/1e6
                finally:
                    try:final_info=target(sym,False)
                    finally:
                        try:time.sleep(.05);status(h,2);stopped=settled(h)
                        finally:reader.close()
                result['target_after']=final_info
                result['stopped']=stopped
                results.append(result);print(json.dumps(result),flush=True)
    finally:h.close()
    path=ROOT/a.json;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(results,indent=2),encoding='utf-8')
    if not results or any(r['host']['bytes']==0 or r['reader_error'] or
        any(r['host'][k] for k in ['gaps','bad','reversed']) or
        any(r['after'][k] for k in ['dropped','fifo_overflows','dma_errors']) or
        r['target_after']['g_refill_late'] or r['stopped']['flags']&6
        for r in results):raise RuntimeError('SPI integrity, loss, sender refill or STOP check failed; see '+str(path))
if __name__=='__main__':main()
