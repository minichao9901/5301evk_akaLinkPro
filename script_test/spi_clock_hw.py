"""Verify fixed SPI2 clock and legacy configuration compatibility over HID.
No target firmware writes. Run with other probe sessions stopped.
"""
import argparse
import json
import struct
import time
from pathlib import Path
from spi_bridge_test import Hid
from spi_cdc_h743_hw import retire_master


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--json', default='build/spi-clock.json')
    a = p.parse_args()
    h = Hid()
    saved = h.cfg_get()
    rows = []
    try:
        assert struct.unpack_from('<I', saved, 24)[0] == 240000000
        retire_master(h)
        for request, expected in [(0, 20000000), (10000000, 10000000),
                                  (20000000, 20000000), (40000000, 40000000),
                                  (60000000, 60000000), (75000000, 60000000),
                                  (100000000, 60000000)]:
            cfg = bytearray(saved)
            struct.pack_into('<I', cfg, 0, request)
            struct.pack_into('<I', cfg, 24, 120000000)  # Legacy override must be ignored.
            h.cfg_set(cfg)
            got = h.cfg_get()
            assert struct.unpack_from('<I', got, 24)[0] == 240000000
            h.enable(1)
            time.sleep(.15)
            debug, status = h.dbg(), h.status()
            assert debug[2] == 240000000, debug
            assert debug[3] == status['sclk'] == expected, (debug, status)
            assert status['status'] & 1, status
            rows.append({'requested_sclk_hz': request, 'actual_sclk_hz': expected,
                         'legacy_module_hint_hz': 120000000,
                         'module_clock_hz': debug[2], 'cfg_hex': got.hex()})
            retire_master(h)
        bad = bytearray(saved)
        struct.pack_into('<I', bad, 0, 479999)
        previous = h.cfg_get()
        rejected = h.cfg_set(bad)
        assert (rejected >> 8) & 255 == 4, rejected
        assert h.cfg_get() == previous
        path = Path(a.json)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({'rows': rows, 'low_rate_rejected': True}, indent=2), encoding='utf-8')
        print(json.dumps(rows), flush=True)
    finally:
        try:
            retire_master(h)
            h.cfg_set(saved)
        finally:
            h.close()


if __name__ == '__main__':
    main()
