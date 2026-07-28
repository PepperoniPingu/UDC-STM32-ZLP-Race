#!/usr/bin/env python3
"""Continuously read the PoC's bulk IN endpoint and detect the wedge.

Usage: sudo ./read_bulk.py  (or add a udev rule for VID:PID 2fe3:f00d)

WEDGED - the endpoint stopped delivering entirely (HAL state torn by an
         enqueue during the ZLP window; endpoint NAKs forever).

Healthy output is a once-per-second rate line. Exits 1 on WEDGED.
"""
import sys
import time

import usb.core
import usb.util

VID, PID = 0x2FE3, 0xF00D
EP_IN = 0x81
READ_TIMEOUT_MS = 2000


def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit("PoC device 2fe3:f00d not found")

    if dev.is_kernel_driver_active(0):
        dev.detach_kernel_driver(0)
    usb.util.claim_interface(dev, 0)

    total = 0
    transfers = 0
    t0 = time.time()
    last_report = t0

    print("reading bulk IN 0x81 ... Ctrl+C to stop")
    while True:
        try:
            data = dev.read(EP_IN, 4096, timeout=READ_TIMEOUT_MS)
        except usb.core.USBTimeoutError:
            dt = time.time() - t0
            print(f"WEDGED: no data for {READ_TIMEOUT_MS} ms after "
                  f"{transfers} transfers ({total} bytes), "
                  f"at t={dt:.1f}s")
            sys.exit(1)

        total += len(data)
        transfers += 1

        now = time.time()
        if now - last_report >= 1.0:
            rate = total / (now - t0) / 1e6
            print(f"t={now - t0:6.1f}s  transfers={transfers}  "
                  f"total={total / 1e6:.2f} MB  avg={rate:.2f} MB/s",
                  flush=True)
            last_report = now


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
