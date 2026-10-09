"""Clock lease acceptance; no flashing or target writes. Restores every probe clock."""
import argparse, hid, struct, time, json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--serial',required=True);p.add_argument('--json');a=p.parse_args()
devices=[i for i in hid.enumerate(0x0d28,0x0204) if i.get('usage_page')==0xff00 and i.get('serial_number')==a.serial]
assert len(devices)==1,'Select exactly one probe'
d=hid.device();d.open_path(devices[0]['path']);owned=0
def command(action,arg=0,mode=0):
    data=bytes([1,10,0x19,action])+struct.pack('<II',arg,mode)
    d.write(data+bytes(64-len(data)));r=bytes(d.read(64,2000))
    assert r[:4]==bytes([2,58,0x19,action]),r
    return list(struct.unpack_from('<14I',r,4))
def settled():
    for _ in range(50):
        time.sleep(.02);s=command(0)
        if s[1]!=1:return s
    raise AssertionError('Clock transition timeout')
def pages():return [command(4,n)[2:] for n in range(3)]
def node_rate(pages,sources,node):
    clock=pages[node//12][node%12]
    return sources[2+((clock>>8)&7)]//((clock&255)+1)
report={'serial':a.serial,'rows':[]}
try:
    before=pages();sources=command(5);assert not command(0)[2],'Stop the active SWO session first'
    report.update(before=before,sources=sources)
    for mode,baud in [(0,30000000),(0,24000000),(1,18000000),(2,23000000)]:
        first=command(1,baud,mode);owned=first[2];s=settled()
        assert s[1]==0 and s[5]==baud,s
        during=pages();during_sources=command(5);assert during_sources[10]==sources[10],'CPU clock changed'
        assert node_rate(during,during_sources,19)==node_rate(before,sources,19),'SPI2 frequency changed'
        if baud==30000000:
            for _ in range(150):
                time.sleep(.04);h=command(3,owned);assert h[1]==0 and h[2]==owned,h
        assert command(3,owned+1)[1]==0xfffffffe,'Wrong token accepted'
        command(2,owned);assert settled()[1]==0;owned=0
        assert pages()==before,'Clock registers not restored';assert command(5)==sources,'PLL / CPU not restored'
        report['rows'].append({'mode':mode,'baud':baud,'status':s,'restoredAllClocks':True})
    first=command(1,23000000,2);owned=first[2];assert settled()[1]==0
    time.sleep(5.3);assert command(0)[2]==0;owned=0;assert pages()==before;assert command(5)==sources
    first=command(1,30000001,2);owned=first[2];s=settled();assert s[1]==0xffffffff and not s[2];owned=0
    command(2);assert command(0)[1]==0
    report.update(passed=True,heartbeat150Checks=True,timeoutRestored=True,cpuUnchanged=True,spiClockUnchanged=True)
finally:
    if owned:
        command(2,owned);s=settled();assert s[1]==0,s
    d.close()
    if a.json:Path(a.json).write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report))
