"""Real SPI->CDC closed receiver, resource exclusion and clean recovery."""
import json,struct,time
import serial
from serial.tools.list_ports import comports
from spi_cdc_hw import ROOT,Hid,status,settled,target,symbols,Reader
sym=symbols();h=Hid();ser=None;reader=None
result={}
try:
    target(sym,False);h.xfer(0x31,[0]);status(h,2);settled(h)
    port=next(i.device for i in comports() if i.vid==0xd28 and i.pid==0x204)
    # Configure the CDC once, then deliberately leave the receiver closed.
    with serial.Serial(port,115200,timeout=.05):pass
    status(h,1,[0,0]);assert settled(h)['flags']&2
    r=h.xfer(0x35,[1,1]);result['master_rejected']=list(r[4:8])
    # SB_E_BUSY=8 in bits 8..15 of native SPI status.
    assert ((struct.unpack('<I',bytes(r[4:8]))[0]>>8)&255)==8
    target(sym,True);time.sleep(.5);blocked=status(h);result['closed']=blocked
    assert blocked['received']>100000 and blocked['dropped']>0 and blocked['pending']<=8192
    assert blocked['flags']&2 and not blocked['fifo_overflows'] and not blocked['dma_errors']
    ser=serial.Serial(port,115200,timeout=.05);reader=Reader(ser);time.sleep(.5)
    resumed=status(h);result['resumed']=resumed
    assert resumed['forwarded']>blocked['forwarded'] and not resumed['fifo_overflows'] and not resumed['dma_errors']
    target(sym,False);time.sleep(.1);status(h,2);settled(h);reader.close();reader=None;ser.reset_input_buffer()
    # This fresh generation must be exact after the intentionally lossy run.
    reader=Reader(ser);status(h,1,[0,0]);settled(h);target(sym,True);time.sleep(2)
    clean=status(h);target(sym,False);time.sleep(.1);status(h,2);end=settled(h);reader.close();result['clean_host']=reader.snapshot();result['clean_status']=clean;result['end']=end
    assert reader.bytes>4000000 and not reader.error and not reader.gaps and not reader.bad and not reader.reversed
    assert not clean['dropped'] and not clean['fifo_overflows'] and not clean['dma_errors'] and not end['flags']&6
    (ROOT/'build/spi-cdc-backpressure.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2));print('SPI CDC closed-host backpressure, master exclusion, late CDC open and clean restart PASS')
finally:
    try:target(sym,False)
    finally:
        try:status(h,2);settled(h)
        finally:
            if reader:reader.close()
            if ser:ser.close()
            h.close()
