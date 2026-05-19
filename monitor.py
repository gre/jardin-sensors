#!/usr/bin/env python3
"""Interleaved serial monitor for gateway + actuator."""
import serial, threading, time

PORTS = {
    "GW":  "/dev/cu.wchusbserial58F00048451",
    "ACT": "/dev/cu.wchusbserial110",
}
BAUD = 115200

def reader(name, port):
    try:
        ser = serial.Serial(port, BAUD, timeout=1)
    except Exception as e:
        print(f"[{name}] OPEN FAILED: {e}", flush=True)
        return
    buf = b""
    while True:
        try:
            data = ser.read(256)
        except Exception as e:
            print(f"[{name}] READ ERROR: {e}", flush=True)
            break
        if not data:
            continue
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = time.time()
            hms = time.strftime("%H:%M:%S", time.localtime(t))
            ms  = int(t * 1000) % 1000
            print(f"[{hms}.{ms:03d}] [{name}] {line.decode(errors='replace').rstrip()}", flush=True)

for name, port in PORTS.items():
    threading.Thread(target=reader, args=(name, port), daemon=True).start()

print(f"Monitoring {list(PORTS.keys())} — Ctrl-C to quit", flush=True)
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    pass
