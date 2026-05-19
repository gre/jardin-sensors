"""
Pre-upload bootloader entry for DX-LR30 (STM32F103 + CH340C).

CH340C wiring (LR20-SP schematic, assumed same on LR30):
  DTR# -> NRST   (pyserial dtr=True  -> DTR# LOW  -> NRST LOW  = reset held)
  RTS# -> BOOT0  (pyserial rts=False -> RTS# HIGH -> BOOT0 HIGH = bootloader)

This script pulses NRST via DTR while holding BOOT0 HIGH via RTS, so no
manual RST press is needed. stm32flash then runs with its own -i entry
sequence as a second attempt in case the port open briefly glitches the lines.
"""

import time
import serial

Import("env")


def enter_bootloader(source, target, env):
    port = env.subst("$UPLOAD_PORT")
    print(f"\n>>> Auto-entering bootloader on {port}...")
    try:
        ser = serial.Serial(port, 115200, timeout=1,
                            dsrdtr=False, rtscts=False)
        # DX-LR30 has NPN transistors inverting DTR/RTS before NRST/BOOT0.
        # Polarity (confirmed by olek87 on stm32duino/Arduino_Core_STM32#2777):
        #   dtr=False -> NRST LOW (reset)    dtr=True  -> NRST HIGH (run)
        #   rts=True  -> BOOT0 HIGH (loader) rts=False -> BOOT0 LOW (firmware)
        ser.rts = True    # BOOT0 HIGH (bootloader mode)
        ser.dtr = False   # NRST LOW  (hold in reset)
        time.sleep(0.1)
        ser.dtr = True    # NRST HIGH -> enters bootloader!
        time.sleep(0.3)   # Wait for STM32 bootloader to start (typ. ~100 ms)
        ser.close()
        time.sleep(0.1)
        print(">>> Bootloader ready, flashing...\n")
    except Exception as e:
        print(f">>> Warning: {e}\n")
        print(">>> If flash fails: hold KEY, press+release RST, then re-run.\n")


env.AddPreAction("upload", enter_bootloader)
