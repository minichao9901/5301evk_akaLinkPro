"""Read-only HID 0x18 UART clock diagnostics. Does not reconfigure the probe."""
import argparse, hid, json, struct
p=argparse.ArgumentParser();p.add_argument('--serial');a=p.parse_args()
infos=[i for i in hid.enumerate(0x0d28,0x0204) if i.get('usage_page')==0xff00 and (not a.serial or i.get('serial_number')==a.serial)]
if len(infos)!=1:raise SystemExit('Select exactly one probe with --serial')
d=hid.device();d.open_path(infos[0]['path'])
try:
 d.write([1,1,0x18]+[0]*61);r=bytes(d.read(64,2000))
 if len(r)<35 or r[:3]!=bytes([2,33,0x18]):raise SystemExit('Firmware does not support UART diagnostics')
 words=struct.unpack_from('<8I',r,3)
 if words[0]!=1:raise SystemExit('Unknown diagnostics version')
 names=['version','clockHz','requestedBaud','appliedBaud','osr','maxBaud','initStatus','clockRegister']
 print(json.dumps(dict(zip(names,words))))
finally:d.close()
