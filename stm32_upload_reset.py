"""
Pre-upload bootloader entry for DX-LR30 (STM32F103 + CH340C).

CH340C wiring (verified from LR20-SP schematic, same on LR30):
  DTR# -> NRST   (pyserial dtr=False -> DTR# LOW  -> NRST LOW  = reset)
  RTS# -> BOOT0  (pyserial rts=True  -> RTS# LOW  -> BOOT0 HIGH = bootloader)
  (NPN transistors on the board invert the DTR/RTS signals before NRST/BOOT0)

The CH340 re-enumerates with a new port suffix on every USB reconnect, so the
port in platformio.ini goes stale. This script auto-detects the CH340 by
VID:PID (1A86:7523) and updates UPLOAD_PORT before stm32flash runs.

stm32flash -i alone is unreliable for this board: the GPIO sequence executes
but the bootloader handshake fails intermittently. Opening the port from
Python and toggling DTR/RTS directly is stable.
"""

import time
import serial
import serial.tools.list_ports

Import("env")

CH340_VID = 0x1A86
CH340_PID = 0x7523


def find_ch340_port():
    # Prefer wchusbserial* (WCH driver) over usbserial* (Apple CDC).
    # stm32flash works reliably with the WCH driver port.
    matches = [p.device for p in serial.tools.list_ports.comports()
               if p.vid == CH340_VID and p.pid == CH340_PID]
    wch = [p for p in matches if "wch" in p.lower()]
    return (wch or matches or [None])[0]


def enter_bootloader(source, target, env):
    import os
    port = env.subst("$UPLOAD_PORT")

    # Auto-detect only when the configured port is missing (stale after reconnect).
    if not os.path.exists(port):
        detected = find_ch340_port()
        if detected:
            print(f"\n>>> CH340 detected at {detected} (configured: {port} missing), using detected port")
            port = detected
            env.Replace(UPLOAD_PORT=port)

    print(f"\n>>> Auto-entering bootloader on {port}...")
    try:
        ser = serial.Serial(port, 115200, timeout=1, dsrdtr=False, rtscts=False)
        # DX-LR30 polarity (NPN transistors invert DTR/RTS before NRST/BOOT0):
        #   rts=True  -> BOOT0 HIGH (bootloader mode)
        #   dtr=False -> NRST LOW   (hold in reset)
        #   dtr=True  -> NRST HIGH  (release reset -> enters bootloader)
        ser.rts = True    # BOOT0 HIGH
        ser.dtr = False   # NRST LOW (hold in reset)
        time.sleep(0.1)
        ser.dtr = True    # NRST HIGH -> STM32 boots into bootloader
        time.sleep(0.3)   # Wait for bootloader UART init (~100 ms typ.)
        ser.close()
        time.sleep(0.1)
        print(">>> Bootloader ready, flashing...\n")
    except Exception as e:
        print(f">>> Warning: {e}")
        print(">>> Manual fallback: hold BOOT0, press+release RESET, then re-run.\n")


env.AddPreAction("upload", enter_bootloader)
