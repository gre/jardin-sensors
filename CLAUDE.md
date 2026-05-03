# CLAUDE.md

Notes for Claude Code working on this repo.

## Project context

Connected garden, LoRa point-to-point + Home Assistant via MQTT.

- 2x **LILYGO LoRa32 T3 V1.6.1** boards, identical (ESP32-PICO-D4, CH9102F,
  LILYGO LORA32 868/915MHz module, OLED SSD1306 0.96"). One in the garden
  (emitter), one in the house (gateway). No hardware difference between the
  two roles.
- **Sensors on emitter side**:
  - SR04M-2 (JSN-SR04T family): mode 0 by default, RX/TX = TRIG/ECHO. Echo
    is 5V => voltage divider 10k+20k to ESP32. 22 uF cap on VCC mandatory.
  - DS18B20 stainless steel waterproof probe 5m, 1-Wire, factory-calibrated.
    4.7k pull-up required between DATA and VDD.
- **LILYGO T3 V1.6.1 OLED quirk**: do not software-pulse GPIO 16 (OLED_RST).
  On some variants, touching GPIO 16 resets the chip. The U8g2 constructor
  must use `U8X8_PIN_NONE` for the reset pin.
- Radio: 868.1 MHz, lib `sandeepmistry/LoRa`
- **LoRa auth**: HMAC-SHA256 truncated to 8 bytes on each packet, anti-replay
  via monotonic `seq` per node. Pre-shared key `LORA_PSK` in `secrets.ini`
  section `[lora]`, identical on emitter and gateway. Helpers in
  `include/auth.h` (header-only via mbedtls).
- Status: deployed, full LoRa + MQTT + HA + auth pipeline working end-to-end.

## Approach

- Read existing files before writing. Don't re-read unless changed.
- Thorough in reasoning, concise in output.
- Skip files over 100KB unless required.
- No sycophantic openers or closing fluff.
- No emojis or em-dashes.
- Do not guess APIs, versions, flags, commit SHAs, or package names. Verify by reading code or docs before asserting.

## Layout

```
cuve-emitter/src/main.cpp   firmware sensor node (garden)
gateway/src/main.cpp   firmware gateway (house): LoRa RX -> MQTT
include/auth.h         HMAC-SHA256 + verify (header-only, shared emitter/gw)
hardware/              wiring schematics, enclosure STLs (TODO)
platformio.ini         two envs: emitter, gateway
secrets.example.ini    template (gitignore: secrets.ini)
```

Shared code: `include/` with inline-only headers. PlatformIO automatically
adds `include/` to the search path. Keep this header-only to avoid issues
with build_src_filter (which explicitly targets `cuve-emitter/src/` or
`gateway/src/` per env).

## Build

```
pio run -e cuve-emitter
pio run -e cuve-emitter -t upload
pio device monitor -e cuve-emitter

pio run -e gateway
pio run -e gateway -t upload
pio device monitor -e gateway
```

## Architecture: emitter dumb, gateway smart

Structural rule of the project, to follow for any new sensor or field.

- **Emitter (garden)**: physical layer only. Reads sensors, applies physical
  filtering (median, average, debounce), sends **raw values** in the LoRa
  JSON. No semantics, no deployment-specific calibration. Emitter macros =
  pins, sample counts, intervals.
- **Gateway (house)**: semantic layer. Receives the raw JSON, adds derived
  values (`tank_pct` from `tank_cm`), augments with `rssi`/`snr`, publishes
  to MQTT with HA discovery. Gateway macros = calibration
  (`TANK_EMPTY_DISTANCE_CM`, `TANK_FULL_DISTANCE_CM`), deployment geometry,
  thresholds.
- **Why**: the emitter box is outdoors and waterproof; recalibrating the
  tank should not require opening it. The gateway is mains-powered at home,
  trivially reflashable.
- **Raw vs derived fields in HA**: raw fields (`tank_cm`, `rssi`, `snr`) are
  marked `entity_category: diagnostic` to stay in the background in HA.
  Derived fields (`tank_pct`) are the primary entities.

If a new sensor is added later, the pattern stays: emitter sends raw units,
gateway derives whatever depends on the deployment.

Special case: **factory-calibrated digital sensors** (DS18B20, BME280, etc.)
already output the SI quantity without deployment calibration. So the
emitter can send the final value directly (`water_temp_c`). The "raw vs
derived" rule only applies when there is deployment-specific math.

## "null = full tank" rule

The ultrasonic sensor is mounted at the very top of the tank. When it is
nearly full, the water surface goes below the SR04M-2 dead zone (~25 cm) =>
no echo => `tank_cm: null`. The gateway interprets `null` as `0 cm` =
**100% full** in `augmentDerived`. Consequence: `tank_pct` is always defined
when the node is online; only the offline-availability (3 min without
packet) makes `tank_pct` go unavailable.

Do not confuse `tank_cm: null` (tank full) with `availability: offline`
(emitter completely lost).

## Conventions

- No comments explaining "what" the code does; names are enough. Only
  comment the "why" when it is non-obvious (workaround, hard constraint,
  subtle invariant).
- LoRa pinout overridable via `build_flags` in `platformio.ini`
  (`LORA_SCK`, `LORA_MISO`, `LORA_MOSI`, `LORA_SS`, `LORA_RST`, `LORA_DIO0`).
  Defaults: LILYGO/TTGO LoRa32 v2.x family, to confirm on both real boards
  before claiming "it works".
- `LORA_BAND=868100000UL` (Europe). Do not change without a regulatory
  reason.
- ArduinoJson v7: use `JsonDocument` (not `StaticJsonDocument`, deprecated).

## clangd diagnostics

The local LSP does not have the Arduino/ESP32 includes until `pio run` has
been executed. Errors like `'Arduino.h' file not found` or `LORA_SS
undeclared` are expected outside of build. Verify with `pio run -e <env>`
rather than reacting to clangd diagnostics.
