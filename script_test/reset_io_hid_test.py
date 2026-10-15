"""Read and exercise the EVKLite dual-reset electrical-mode HID command.

Usage:
  python script_test/reset_io_hid_test.py                 # query only
  python script_test/reset_io_hid_test.py --exercise      # test push-pull/open-drain and restore original mode

The exercise changes PA27/J3[23] electrical mode, but does not assert reset or
write target memory. PA08/J5[3] remains open-drain in both modes.
"""
import argparse
import sys
import time

import hid

VID = 0x0D28
PID = 0x0204
USAGE_PAGE = 0xFF00
CMD_RESET_IO = 0x3A
ACT_GET = 0
ACT_SET_NRST_MODE = 1


def transfer(device, action, mode=0):
    req = [1, 3, CMD_RESET_IO, action, mode] + [0] * 59
    device.write(req)
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        res = device.read(64, timeout_ms=100)
        if not res:
            continue
        if len(res) > 2 and res[2] == CMD_RESET_IO:
            rc = res[3] if res[3] < 128 else res[3] - 256
            if rc:
                raise RuntimeError("RESET_IO rc=%d" % rc)
            if len(res) < 6:
                raise RuntimeError("short RESET_IO response: %r" % (res,))
            return {"openDrain": bool(res[4]), "capabilities": res[5]}
        if len(res) > 2 and res[2] == 0:
            raise RuntimeError("probe firmware does not support HID 0x3A")
    raise TimeoutError("no HID 0x3A response")


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exercise", action="store_true", help="toggle push-pull/open-drain and restore the original mode")
    args = parser.parse_args()

    candidates = [d for d in hid.enumerate(VID, PID) if d.get("usage_page") == USAGE_PAGE]
    if len(candidates) != 1:
        raise RuntimeError("expected one custom HID interface, found %d" % len(candidates))
    device = hid.device()
    device.open_path(candidates[0]["path"])
    device.set_nonblocking(1)
    original = None
    try:
        state = transfer(device, ACT_GET)
        original = state["openDrain"]
        caps = state["capabilities"]
        if (caps & 7) != 7:
            raise RuntimeError("dual-reset and drive-mode capabilities missing: 0x%02X" % caps)
        print("GET: mode=%s capabilities=0x%02X" % ("open-drain" if original else "push-pull", caps))
        if args.exercise:
            for mode in (False, True):
                state = transfer(device, ACT_SET_NRST_MODE, int(mode))
                readback = transfer(device, ACT_GET)
                if state["openDrain"] != mode or readback["openDrain"] != mode:
                    raise RuntimeError("mode readback mismatch for %s" % ("open-drain" if mode else "push-pull"))
                print("SET/GET: %s PASS" % ("open-drain" if mode else "push-pull"))
    finally:
        if args.exercise and original is not None:
            try:
                transfer(device, ACT_SET_NRST_MODE, int(original))
                restored = transfer(device, ACT_GET)
                if restored["openDrain"] != original:
                    raise RuntimeError("failed to restore original drive mode")
                print("RESTORE: %s PASS" % ("open-drain" if original else "push-pull"))
            finally:
                device.close()
        else:
            device.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
