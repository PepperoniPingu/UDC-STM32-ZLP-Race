# udc_stm32 endpoint busy-flag race - proof of concept

Minimal Zephyr application that reproduces a race condition in `drivers/usb/udc/udc_stm32.c` on STM32 OTG
controllers. It permanently wedges a bulk IN endpoint.

Found on an STM32N6570-DK streaming UVC/H.264 (stream froze after ~2 s), but the bug should be generic to
any usbd-next class that enqueues IN transfers from a context that is not synchronized with USB completion events.
Present in the v4.4.0 release and in upstream `main` as of
[`ae55454cc`](https://github.com/zephyrproject-rtos/zephyr/commit/ae55454cc37c8ad8eec23b66469d3a59f46c09f9)
(2026-07-30).

## Root cause

The driver serializes hardware access per endpoint with a software busy flag. `udc_stm32_tx()` refuses to call
`hal_udc_set_endpoint_transmit()` (`HAL_PCD_EP_Transmit()` in v4.4.0 and earlier) while it is set. The flag itself is
undocumented. `udc_common.h` says only "Checks if the endpoint is busy". But the HAL supports one transfer at a time
per endpoint and this flag is the only thing gating concurrent transmit calls.

`handle_msg_data_in()` runs on every IN stage completion. But it clears the flag unconditionally at function entry.
Before it knows whether the transfer is actually finished. In most cases this works since the packet is actually
completed. But it becomes a problem when sending the ZLP. The ZLP is a zero length packet that terminates a bulk
transfer whose length is an exact multiple of the packet size. Without it the host keeps waiting for more data. The
OTG core cannot know a terminator is needed. So the driver sends the ZLP manually as a second
`hal_udc_set_endpoint_transmit()`. That second arming step creates a window when a concurrent thread will think packet
transmission is done, while it is in fact not. So this concurrent thread can submit a packet through `udc_stm32_tx()`
and not be blocked from arming the hardware again. After this I don't understand the exact workings except it freezes
the endpoint.

## Fix

`fix.patch` applies to the pinned `main` commit. Clear busy only when the hardware is provably idle, i.e. keep the
endpoint marked busy through the ZLP.

## Reproduction strategy

- Every bulk IN transfer is `2 * MPS` bytes with the ZLP flag set, so every completion takes the vulnerable branch.
- Up to 4 buffers are kept outstanding so the endpoint is re-armed back to back.
- A producer thread enqueues with random 0-200 us jitter, decoupled from completion events, so enqueues sample all
  transfer phases.

With a host reading continuously, an unpatched driver wedges almost immediately: typically within the first few
hundred transfers, i.e. tens of milliseconds of traffic. The device logs
`WEDGED: bulk IN produced no completions for 3 s`, and the host reader exits once its 2 s read timeout expires.

## Building, loading and running the PoC

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install west

west init -l manifest
west update
pip install -r zephyr/scripts/requirements-base.txt

make build          # west build -b stm32n6570_dk/stm32n657xx/sb .
```

Verified with west 1.5.0 and Zephyr SDK 1.0.1. Any board whose `zephyr_udc0` is an STM32 OTG instance should work.

### Loading over SWD

Set switches BOOT0=1 and BOOT1=1. Then run:
```sh
make flash-swd
```

### Host reader

This needs raw USB access. Either run the reader with sudo or install a udev rule, e.g.
`/etc/udev/rules.d/60-zlprace.rules`:
```
SUBSYSTEM=="usb", ATTRS{idVendor}=="2fe3", ATTRS{idProduct}=="f00d", TAG+="uaccess"
```

Then start it with
```sh
make read
```


## Verifying the fix

Apply `fix.patch` to the workspace Zephyr, rebuild and reload:

```sh
patch -p1 -d zephyr < fix.patch
make flash-swd read
```

## Measured results

STM32N6570-DK at high speed, 2026-07-28, on `main` at `c25aaee66`, whose `drivers/usb/udc/udc_stm32.c` is
byte-identical to the pinned commit:

- Unpatched: wedges within the first few hundred transfers of the reader attaching -
  `WEDGED: no data for 2000 ms after 319 transfers (325120 bytes), at t=2.1s`. Repeat runs land anywhere between ~270
  and ~1550 transfers; the last 2 s of the reported time is the host read timeout
- With `fix.patch`: sustained ~5000 transfers/s (~5.1 MB/s, every transfer ZLP-terminated) for the full test window,
  most recently 319529 ZLP transfers over 64 s, zero stalls.

Note: the device-side `WEDGED` log line simply means "no completions for 3 s while enabled" - it also fires when no host
reader is attached. The authoritative check is the host reader: it exits with `WEDGED: ...` on a genuine stall.

## Related upstream work

- [zephyrproject-rtos/zephyr#75129](https://github.com/zephyrproject-rtos/zephyr/pull/75129) (merged 2024-07-01) added
  the ZLP re-arm branch that this race lives in.
