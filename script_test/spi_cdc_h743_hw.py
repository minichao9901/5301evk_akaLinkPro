"""H743 SPI1 -> probe SPI2 -> CDC sweep. Flash fixture first; restore backup afterwards.
Rates are nominal HSI/PLL clocks. Payload BER and lost frames are separate metrics.
"""
import argparse, json, re, struct, subprocess, threading, time
from pathlib import Path
import serial
import usb.core, usb.util
from serial.tools.list_ports import comports
from spi_cdc_hw import ROOT, OCD, ARM, status, settled
from spi_bridge_test import Hid
METRICS=['g_run','g_core_hz','g_spi_hz','g_clock_err','g_refill_late','g_dma_errors','g_blocks','g_sequence','g_spi_sr']
def retire_master(h):
    h.enable(0);time.sleep(.1)
    dev=usb.core.find(idVendor=0x0d28,idProduct=0x0204)
    # An armed OUT DMA survives disable until its completion callback. A ZLP
    # retires it without resetting USB or losing the configured module clock.
    try:dev.write(0x0b,b'',timeout=500)
    except usb.core.USBTimeoutError:pass
    finally:usb.util.dispose_resources(dev)
    time.sleep(.1)
def symbols():
    out=subprocess.check_output([str(ARM/'arm-none-eabi-nm.exe'),str(ROOT/'script_test/stm32h743_spi_tx/fw.elf')],text=True)
    return {a[2]:int(a[0],16) for line in out.splitlines() if len(a:=line.split())==3}
def target(sym,run,mhz=10):
    commands=[f'mww 0x{sym["g_run"]:x} 0','sleep 20']
    if run:commands += [f'mww 0x{sym["g_mhz"]:x} {mhz}',f'mww 0x{sym["g_run"]:x} 1','sleep 20']
    commands += [f'echo "METRIC {k} [read_memory 0x{sym[k]:x} 32 1]"' for k in METRICS]
    result=subprocess.run([str(OCD/'bin/openocd.exe'),'-s',str(OCD/'openocd/scripts'),'-f','interface/cmsis-dap.cfg','-c','cmsis-dap backend usb_bulk','-f','target/stm32h7x.cfg','-c','adapter speed 1000','-c','gdb port disabled','-c','tcl port disabled','-c','telnet port disabled','-c','; '.join(['init',*commands,'shutdown'])],capture_output=True,text=True,timeout=25)
    out=result.stdout+result.stderr
    if result.returncode:raise RuntimeError(out)
    values={}
    for k in METRICS:
        m=re.search(r'METRIC '+k+r'\s+(0x[0-9a-fA-F]+|\d+)\b',out)
        if not m:raise RuntimeError('Missing metric '+k+'\n'+out)
        values[k]=int(m[1],0)
    return values
class Reader:
    payloads=[bytes(((s+17*i)^0x5a)&255 for i in range(8,64)) for s in range(256)]
    def __init__(self,port):
        self.port=port;self.stop=False;self.error=None
        self.bytes=self.frames=self.gaps=self.reverse=self.bad_frames=self.bit_errors=self.compared_bits=self.unaligned_bytes=0
        self.last=None;self.tail=bytearray();self.sample=bytearray()
        self.thread=threading.Thread(target=self.read,daemon=True);self.thread.start()
    def read(self):
        try:
            while not self.stop:
                b=self.port.read(min(262144,max(1,self.port.in_waiting)))
                if not b:continue
                self.bytes+=len(b)
                if len(self.sample)<128:self.sample.extend(b[:128-len(self.sample)])
                self.feed(b)
        except Exception as e:self.error=repr(e)
    def feed(self,b):
        self.tail.extend(b);head=0;a=self.tail
        while len(a)-head>=64:
            if a[head:head+4]!=b'SPIC':
                pos=a.find(b'SPIC',head+1)
                if pos<0:pos=len(a)-3
                self.unaligned_bytes+=pos-head;head=pos;continue
            seq=struct.unpack_from('<I',a,head+4)[0]
            payload=a[head+8:head+64];expected=self.payloads[seq&255]
            # Only aligned, monotonic headers supply a trustworthy BER denominator.
            gap=0 if self.last is None else (seq-self.last-1)&0xffffffff
            if gap>=0x80000000 or gap>1000000:
                self.reverse+=1;self.unaligned_bytes+=1;head+=1;continue
            if payload!=expected:
                self.bad_frames+=1
                self.bit_errors+=sum((x^y).bit_count() for x,y in zip(payload,expected))
            self.compared_bits+=448;self.gaps+=gap;self.last=seq;self.frames+=1;head+=64
        if head:del a[:head]
    def snapshot(self):return {k:getattr(self,k) for k in ['bytes','frames','gaps','reverse','bad_frames','bit_errors','compared_bits','unaligned_bytes']}
    def close(self):self.stop=True;self.thread.join(2)
def main():
    p=argparse.ArgumentParser();p.add_argument('--mhz',default='5,10,20,30,40,50,60,70,80,90,100,110,120,133');p.add_argument('--seconds',type=float,default=3);p.add_argument('--module-mhz',type=int,default=0);p.add_argument('--json',default='build/h743-spi-cdc.json');a=p.parse_args()
    rates=list(map(int,a.mhz.split(',')))
    if a.seconds<=0 or not rates or any(not 1<=n<=133 for n in rates) or not 0<=a.module_mhz<=240:
        p.error('duration must be positive, SPI 1..133 MHz, module clock 0..240 MHz')
    path=ROOT/a.json;path.parent.mkdir(parents=True,exist_ok=True)
    h=Hid();sym=symbols();cfg=h.cfg_get();results=[];clock_info=None
    try:
        target(sym,False);h.xfer(0x31,[0]);status(h,2);settled(h);retire_master(h)
        if a.module_mhz:
            new=bytearray(cfg);struct.pack_into('<I',new,0,10000000);struct.pack_into('<I',new,24,a.module_mhz*1000000)
            h.cfg_set(bytes(new));h.enable(1);time.sleep(.15);clock_info=h.dbg();retire_master(h)
        port=next(i.device for i in comports() if i.vid==0x0d28 and i.pid==0x0204)
        with serial.Serial(port,115200,timeout=.01) as ser:
            for mhz in rates:
                target(sym,False);status(h,2);settled(h);ser.reset_input_buffer();reader=Reader(ser)
                try:
                    status(h,1,[0,0]);started=settled(h)
                    if started['rc'] or not started['flags']&2:raise RuntimeError(str(started))
                    info=target(sym,True,mhz)
                    if info['g_clock_err'] or not info['g_run']:raise RuntimeError(str(info))
                    time.sleep(.3);initial=reader.snapshot();before=status(h);t0=time.perf_counter()
                    time.sleep(a.seconds);elapsed=time.perf_counter()-t0;last=reader.snapshot();after=status(h)
                    result={'mhz_requested':mhz,'seconds':elapsed,'module_clock_debug':clock_info,'target':info,'host':{k:last[k]-initial[k] for k in last},'before':before,'after':after}
                finally:
                    try:final=target(sym,False)
                    finally:
                        time.sleep(.05);status(h,2);stopped=settled(h);reader.close()
                result.update(target_after=final,stopped=stopped,reader_error=reader.error,sample_hex=reader.sample.hex())
                result['MBps']=result['host']['bytes']/elapsed/1e6
                result['payload_BER']=result['host']['bit_errors']/result['host']['compared_bits'] if result['host']['compared_bits'] else None
                result['probe_window']={k:(after[k]-before[k])&0xffffffff for k in ['received','forwarded','dropped','fifo_overflows','dma_errors']}
                result['clean']=bool(result['host']['bytes'] and not reader.error and not any(result['host'][k] for k in ['gaps','reverse','bad_frames','unaligned_bytes']) and not any(result['probe_window'][k] for k in ['dropped','fifo_overflows','dma_errors']) and not final['g_refill_late'] and not final['g_dma_errors'])
                results.append(result);path.write_text(json.dumps(results,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
    finally:
        try:target(sym,False);status(h,2);settled(h)
        finally:
            if a.module_mhz:
                h.cfg_set(cfg);h.enable(1);time.sleep(.15);retire_master(h)
            h.close()
if __name__=='__main__':main()
