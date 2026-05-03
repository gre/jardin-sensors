# Connected Garden Project – LoRa & Home Assistant

> **Status**: deployed, full LoRa + MQTT + HA + auth pipeline working end-to-end.
> **License**: [GPL v3](#license) · **Project**: personal

## Goal

Monitoring and alerting system for the **water level of the garden tank**:

- tank level reported via ultrasonic sensor
- "needs refill" notification
- centralized integration into **Home Assistant**

## Constraints

- Distance house ↔ garden: ~100 m
- House Wi-Fi insufficient on the garden side → LoRa radio link
- Outdoor environment (humidity, interference, frost)
- **Mains** powered on both sides
- Reliability and simplicity required

## Philosophy: **emitter dumb, gateway smart**

Structural rule of the project, to follow for any new sensor added later.

- **The emitter** (garden side, in a waterproof box, hard to access) only
  does **raw measurement**: pin toggling, pulseIn, physical filtering
  (median). It serializes raw values into JSON and sends them over LoRa. No
  semantics, no calibration.
- **The gateway** (house side, accessible and reflashable) carries all the
  **semantic intelligence**: calibration, derivation of business values
  (`tank_pct`), enrichment (`rssi`, `snr`), MQTT publish and HA discovery.
- **Why**: recalibrating the tank or changing a threshold should never
  require opening the waterproof box in the garden.

```
                EMITTER                       GATEWAY                      HOME ASSISTANT
              ┌─────────┐                   ┌──────────┐                   ┌─────────────┐
              │   raw   │                   │   raw    │                   │   raw +     │
   [Sensor]───┤ values  │──── LoRa ─────────┤    +     │── MQTT (retain)──>│  derived    │
              │  + meta │  point-to-point   │ derived  │   + discovery     │  entities   │
              └─────────┘                   └──────────┘                   └─────────────┘
              physical                      calibration                    automation
              filtering                     geometry                       notifications
              tx interval                   link tracking                  long-term storage
```

## Overall architecture

Both nodes run on **the same board** (LILYGO LoRa32 T3 V1.6.1); only the
firmware differs. The role is selected by the PlatformIO env (`emitter` or
`gateway`).

```
        [GARDEN]                              [HOUSE]
   ┌──────────────────────────┐          ┌──────────────────────────┐
   │  EMITTER                 │          │  GATEWAY                 │
   │  LILYGO LoRa32 T3 V1.6.1 │  LoRa    │  LILYGO LoRa32 T3 V1.6.1 │
   │  ──────────────────────  │ 868.1MHz │  ──────────────────────  │
   │  • SR04M-2 -> tank_cm    │ ───────> │  • Decode JSON           │
   │  • LoRa TX every 5s      │  ~100 m  │  • Derive tank_pct       │
   │                          │  point-  │  • Track availability    │
   │  "dumb sensor"           │  to-pt   │  • MQTT publish + LWT    │
   │                          │          │  • HA auto-discovery     │
   │                          │          │  "smart hub"             │
   └──────────────────────────┘          └────────────┬─────────────┘
                                                      │ Wi-Fi + MQTT
                                                      v
                                         ┌──────────────────────────┐
                                         │  Home Assistant          │
                                         │  • auto discovery        │
                                         │  • notifications         │
                                         │  • long-term graphs      │
                                         └──────────────────────────┘
```

## Per-measurement data flow

```
   SR04M-2 ----trig/echo----> [median 5x] -----> tank_cm (float, raw distance)
                                                  │
                                                  ▼  (LoRa JSON)
                                            ┌─────────────┐
                                            │   GATEWAY   │
                                            │  augment:   │
                                            │  rssi, snr  │   tank_cm still
                                            │  tank_pct = │   published in
                                            │   geometric │   parallel as
                                            │   mapping   │   "diagnostic" for HA
                                            └──────┬──────┘
                                                   │
                                                   ▼
                                           tank_pct (% filled, 0..100)
```

## JSON fields schema

| Field | Source | Type | Unit | HA category | Meaning |
|---|---|---|---|---|---|
| `node` | emitter | string | – | – | emitter identifier |
| `seq` | emitter | int | – | – | packet counter |
| `tank_cm` | emitter | float | cm | diagnostic | raw ultrasonic distance |
| `water_temp_c` | emitter | float | °C | primary | water temperature (DS18B20, factory-calibrated) |
| `rssi` | gateway | int | dBm | diagnostic | LoRa reception quality |
| `snr` | gateway | float | dB | diagnostic | LoRa signal-to-noise ratio |
| `tank_pct` | **gateway derived** | int | % | primary | tank fill level |

> Note: `water_temp_c` is sent directly in °C by the emitter without gateway-side derivation. The DS18B20 is factory-calibrated to ±0.5 °C, no deployment-specific parameter. The "raw vs derived" rule applies to analog sensors or sensors that need deployment calibration (tank %).

## Tank geometry

The sensor is fixed under the tank cover, head facing down. It measures the
**distance between itself and the water surface** (`tank_cm`). The fuller
the tank, the smaller this distance.

```
   ┌──────────────────────┐  ◄── tank ceiling (cover with hole)
   │                      │
   │      ▼ SR04M-2       │  ◄── sensor head fixed to cover,
   │      ║║              │      measurement cone facing down
   │      ║║   ↓          │
   │      ║║   │          │
   │   acoustic wave      │
   │      │               │
   │      ▼               │
   │  ~~~~~~~~~~~~~~      │  ◄── water surface
   │                      │
   │       water          │
   │                      │
   └──────────────────────┘  ◄── tank bottom

   tank_cm = distance sensor -> water surface (read by sensor)
   TANK_FULL_DISTANCE_CM  = sensor offset alone (full tank, water near cover)
   TANK_EMPTY_DISTANCE_CM = tank depth + offset (empty tank, bottom visible)
```

> Mind the **dead zone** of the SR04M-2 (~25 cm): if `TANK_FULL_DISTANCE_CM < 25`, the full-tank state will translate to erratic readings. Better to over-measure (sensor 25-30 cm above the "full" level) than razor-thin.

## Calibration

Calibration is **not hardcoded**: the gateway exposes two HA `number`
entities (range 0-200 cm) that drive the thresholds at runtime, persisted
via MQTT retain.

| HA entity (number) | MQTT topic | Range | Meaning |
|---|---|---|---|
| `Tank empty distance` | `jardin/config/tank_empty_cm` | 0-200 cm | sensor distance when tank is empty |
| `Tank full distance` | `jardin/config/tank_full_cm` | 0-200 cm | sensor distance when tank is full |

`tank_pct = clamp(map(tank_cm, tank_empty_cm, tank_full_cm, 0, 100), 0, 100)`

At boot, the gateway loads values from `[env:gateway]/build_flags`
(`TANK_EMPTY_DISTANCE_CM=80`, `TANK_FULL_DISTANCE_CM=5`), then HA overrides
them via the retained messages on subscribe.

In-service calibration procedure:
1. Connect the emitter in the garden, watch `tank_cm` in HA (diagnostic entity)
2. Empty tank → note the value, push it into the HA `Tank empty distance` slider
3. Full tank → same in `Tank full distance`
4. No reflash: values are retained on the broker, persist across gateway reboot

If `tank_empty == tank_full` (bad calibration), the `tank_pct` field is
omitted from the payload (no div/0, the HA entity goes `unavailable`).

### "null = full" rule + debounce

The ultrasonic sensor is mounted **at the top of the tank**. When it is
nearly full, the water surface goes **below the dead zone (~25 cm)** of the
SR04M-2 → no echo → `tank_cm: null`. This missing echo is **interpreted as
full tank** by the gateway, but with a **debounce** to avoid flicker when a
single ping is missed in an otherwise valid stream:

- **Isolated null** in a stream of recent valid measurements (`<TANK_NULL_GRACE_MS=3000` ms) → the gateway substitutes the last known value (per node)
- **Sustained null** beyond 3 s → switches to 100% (truly full tank)
- **Valid measurement** (`tank_cm` non-null) → memorized as the new reference for debounce, and pct computed directly

Consequence:
- `tank_pct` is **always defined** when the emitter is transmitting (never "unavailable" while the node is online)
- `tank_cm` (diagnostic) stays `null` when the measurement is missing, which lets you distinguish "really full" (null) from a real distance reading
- If the emitter goes down completely (offline), `tank_pct` AND `tank_cm` go unavailable via the MQTT `availability` mechanism (do not confuse with null tank_cm)

## LoRa link security

LoRa point-to-point has **no native security** (no encryption, no
authentication, public sync word). Forge / replay / sniff attacks are
trivial by default. The project implements **truncated HMAC-SHA256** +
**anti-replay** at the application layer.

### HMAC-SHA256 (auth + integrity)

- Pre-shared key `LORA_PSK` (string), defined in `secrets.ini` section `[lora]`, **identical between emitter and gateway**
- On-air payload format: `<json>|<hex16>` where `hex16` = first 8 bytes of `HMAC-SHA256(key, json)` in hex
- Emitter: automatic MAC append on every TX (`include/auth.h`, `authAppendMac`)
- Gateway: const-time verification before any processing (`authVerifyMac`). Any packet without a valid MAC is dropped with `[gateway] HMAC invalid, drop ...`
- HMAC implementation: `mbedtls/md.h` (bundled in arduino-esp32, no external lib)
- Guarantees: **an attacker without the key can neither forge nor modify packets**. The MAC truncated to 64 bits gives 2⁶⁴ possibilities, brute-force unfeasible.

### Anti-replay (per node monotonic seq)

- Emitter: `seq` field that increments on each packet (uint32_t counter since boot)
- Gateway: `NodeState` stores `lastSeq` per node, accepts a packet only if `seq > lastSeq`
- Emitter reboot tolerance: if `seq < 100` and `lastSeq > 10000`, we assume a reset and re-arm the counter (the "attacker replays after gateway reboot" scenario is already mitigated by the HMAC, which prevents forging)
- Logged drops: `[gateway] replay/reorder drop node=cuve seq=42 (last=43)`

### What it does NOT cover

- **Confidentiality**: the JSON is in clear over the air, sniffable. To hide values, AES-128 would have to be added on top (not implemented)
- **Radio DoS**: an attacker can spam the frequency to block reception. Mitigation = RF level (not applicable in software here)

For a garden water-level sensor, auth + integrity + anti-replay is largely
enough.

## MQTT topics

```
homeassistant/                          (HA discovery prefix)
└── sensor/jardin-<node>/<key>/config   (one per sensor x node)

jardin/                                 (MQTT_BASE_TOPIC)
├── <node>/state                        (full JSON payload, retain=true)
├── <node>/availability                 (online | offline, retain=true)
├── gateway/availability                (online | offline, broker LWT)
└── config/                             (runtime config, retain=true)
    ├── tank_empty_cm
    └── tank_full_cm

homeassistant/                          (HA discovery prefix)
├── sensor/jardin-<node>/<key>/config   (one per sensor x emitter)
└── number/jardin-gateway-<key>/config  (runtime config sliders)
```

Example payload for `jardin/cuve/state` after augmentation by the gateway:

```json
{
  "node": "cuve",
  "seq": 42,
  "tank_cm": 47.3,
  "water_temp_c": 18.5,
  "rssi": -75,
  "snr": 9.5,
  "tank_pct": 43
}
```

What goes **over the air** (LoRa) before the gateway parses, validates and
augments:

```
{"node":"cuve","seq":42,"tank_cm":47.3,"water_temp_c":18.5}|6d17ea007dd04cf8
                                                           ^^^^^^^^^^^^^^^^^^
                                                           HMAC-SHA256 truncated (8 hex bytes)
```

`rssi`, `snr`, `tank_pct` are added on the gateway side (never transmitted
over the air).

## Gateway-side node lifecycle

```
   [LoRa RX]
       │
       ▼
   parse JSON ───err──> log + drop
       │
       ▼
   findNode(node) ───null──> publishDiscovery() ───fail──> retry next packet
       │                              │
       │                              ▼
       │                        registerNode()
       │                              │
       ▼                              ▼
   markSeen ──not online──> publishAvailability("online")
       │
       ▼
   augmentDerived (tank_pct)
       │
       ▼
   publish <base>/<node>/state retain=true
```

`checkNodeTimeouts()` runs in `loop()`: any `online` node that has not sent
since `NODE_TIMEOUT_MS` (3 min default) flips to `offline`.

`softWatchdog()` reboots the gateway via `ESP.restart()` if Wi-Fi or MQTT
have been down for more than 5 min.

## Hardware

### Boards

**2x LILYGO LoRa32 T3 V1.6.1**, identical (silkscreen `T3_V1.6.1 20210104`).

- ESP32-PICO-D4 + LILYGO LORA32 module (SX1276, 868/915 MHz)
- CH9102F USB-UART, micro-USB port, ON/OFF switch, JST battery
- SMA + IPEX antenna connector
- Built-in OLED SSD1306 0.96" (I2C: SDA=21, SCL=22, RST=16)
- One board → emitter (garden), the other → gateway (house). Role is chosen via the PlatformIO env, not via hardware.

> Note: vendor listings may still mention "TTGO" or refs like `2ASYE-T3-V1-6-1` / `XY241015`. Same product (TTGO = old LILYGO sub-branding).

### Sensors

**1. SR04M-2** (JSN-SR04T family) — waterproof ultrasonic for distance.

- Waterproof transducer head at the end of a ~2 m cable, **control board to keep dry**
- Dead zone ~25 cm, range ~25-450 cm
- **5 V** powered, echo signals at **5 V** → voltage divider mandatory to ESP32 (3.3 V max)
- Pin header silkscreen: `RST | 5V | RX | TX | GND` (+ `SWIM` test pad)
- In **mode 0** (factory default): `RX` = TRIG (sensor input from MCU), `TX` = ECHO (sensor output to MCU). Same behavior as a standard HC-SR04 / JSN-SR04T.
- **22 µF electrolytic decoupling cap** (or 100 nF + 22 µF in parallel) between VCC and GND **directly on the sensor PCB**: required to absorb the transducer's current peaks. Without the cap, the sensor becomes unstable after a few pings.

**2. DS18B20** (5 m stainless steel waterproof probe, Aideepen) — water temperature.

- 1-Wire digital, factory-calibrated ±0.5 °C
- Range -55 to +125 °C (the tank will be between 0 and 30 °C in practice)
- 5 m factory-sealed waterproof cable → goes directly into the tank water
- Wires: Red=VDD (3.3V), Black=GND, Yellow=DATA
- **4.7 kΩ pull-up mandatory** between DATA and VDD (the ESP32 internal pull-up is too weak for 1-Wire)

### Emitter wiring

LILYGO T3 V1.6.1 ↔ sensors pinout (defaults `platformio.ini`):

```
   LILYGO T3 V1.6.1 (emitter)              SR04M-2 (ultrasonic)
   ──────────────────────────              ─────────────────────
            5V o─────────────────────────o 5V (VCC)
           GND o─────────────────────────o GND
                                          o RX  (= TRIG, mode 0)
           IO4 o─────────────────────────┘
                                          
          IO25 o◄──┐                       
                   │                       
                   ├──[ R1 = 10k ]─o TX    (= ECHO, mode 0, 5V signal)
                   │
                  [ R2 = 20k ]
                   │
                  GND
                  
   + 22 uF (electro) decoupling capacitor BETWEEN VCC AND GND PINS
     of the SR04M-2 PCB, leads as short as possible. Without the cap,
     the sensor becomes unstable after a few pings.

   5V -> 3.3V voltage divider (echo):
     V_out = V_in * R2 / (R1 + R2) = 5 * 20 / 30 = 3.33V


   LILYGO T3 V1.6.1 (emitter)              DS18B20 (water temp)
   ──────────────────────────              ────────────────────
          3.3V o───────────┬─────────────o Red    (VDD)
                           │
                       [ 4.7k ]            <- 1-Wire pull-up mandatory
                           │
          IO13 o───────────┴─────────────o Yellow (DATA)
           GND o─────────────────────────o Black  (GND)
```

LoRa pinout internal to the board (already wired on the PCB, exposed via
`build_flags`):
`SCK=5  MISO=19  MOSI=27  SS=18  RST=23  DIO0=26`

OLED pinout internal (I2C):
`SDA=21  SCL=22  RST=16`

### OLED display

OLED controlled by `WITH_OLED=1` in `platformio.ini` (both envs). If the
board does not have a soldered OLED, set to `0` or omit — the code I2C-probes
at 0x3C and disables the display cleanly.

**Emitter**: every cycle (1 s):

```
┌────────────────────────────┐
│ Cuve emitter      LoRa OK  │
│────────────────────────────│
│                            │
│   47.3              cm     │   ◄── ultrasonic distance (large)
│                            │
│────────────────────────────│
│ TX #42      18.5C          │   ◄── seq + water temp if available
└────────────────────────────┘
```

- Failed distance → `----`, OR last known value with `?` if <30 s
- No DS18B20 (disconnected/init failure) → temp omitted from footer
- LoRa init failed → `LoRa ERR` top right, TX skipped

**Gateway**: refresh every 500 ms:

```
┌────────────────────────────┐
│ Gateway     W:OK M:OK      │
│────────────────────────────│
│                            │
│   87 %       18.5C         │   ◄── tank_pct (large) + temp
│                            │
│────────────────────────────│
│ cuve 2s -75dBm             │   ◄── node + age last RX + RSSI
└────────────────────────────┘
```

- No RX yet since boot → `no RX yet`
- WiFi/MQTT down → `W:--` or `M:--` at the top
- `tank_pct` not yet received → `----`

During deployment: the gateway OLED is valuable to manually check that
packets are arriving without having to look at HA.

### Power and enclosure

- **Mains** powered on the garden side (USB) and house side
- IP65 enclosure + cable glands on the garden side (transducer cable + USB power passthrough)
- STLs to design and print (`hardware/3d/`)

## Communication

### Radio

- **LoRa point-to-point** (no LoRaWAN)
- Frequency: **868.1 MHz**
- CRC enabled
- Short messages, ~5 s interval at first (configurable via `TX_INTERVAL_MS`)

## Home Assistant integration

- Native MQTT auto-discovery: entities appear in HA on the 1st packet from a new emitter
- **Primary entities** (foreground):
  - `tank_pct` — % filled (0-100)
  - `water_temp_c` — water temperature (°C, DS18B20 if connected)
- **Diagnostic entities** (grouped in the HA "Diagnostic" section):
  - `tank_cm` — raw ultrasonic distance (may be `null` when tank is full, see null=full rule)
  - `rssi`, `snr` — LoRa link quality
- **Config entities** under the "Jardin Gateway" device:
  - `Tank empty distance` (number, 0-200 cm)
  - `Tank full distance` (number, 0-200 cm)
- **Availability**:
  - Per-node: `jardin/<node>/availability` (offline if no packet for 3 min)
  - Gateway: MQTT LWT on `jardin/gateway/availability`
  - If the emitter dies → all node entities go unavailable
  - If the DS18B20 is missing but the emitter works → only `water_temp_c` goes unavailable, the rest keeps going
- **Automations to create on the HA side**:
  - "Tank low" notification on `tank_pct < 20`
  - "Tank full" notification on `tank_pct >= 100` (transition)
  - "Emitter offline for X min" alert
  - Optional: temperature alert (frost `<2°C`, indicative overheat `>30°C`)

## Repo structure

```
.
├── README.md
├── CLAUDE.md           rules for Claude Code
├── LICENSE             GPL v3
├── platformio.ini      two envs: emitter, gateway
├── secrets.example.ini template to copy to secrets.ini (gitignore)
├── include/
│   └── auth.h          HMAC-SHA256 + verify (header-only, shared emitter/gateway)
├── cuve-emitter/src/   firmware sensor node (garden, tank ultrasonic + temp)
├── gateway/src/        firmware gateway (house)
└── hardware/           wiring schematics, enclosure STLs (TODO)
```

## Setup

```bash
pip install platformio

# 1. Configure secrets (WiFi, MQTT, shared LoRa key)
cp secrets.example.ini secrets.ini

# 2. Generate a random key for LoRa auth, then edit secrets.ini
openssl rand -hex 32
# paste this value into the [lora] section of secrets.ini, between the \"...\"

# 3. Also edit the [wifi] and [mqtt] sections with your real credentials

# Tank emitter
pio run -e cuve-emitter
pio run -e cuve-emitter -t upload
pio device monitor -e cuve-emitter

# Gateway (the same [lora] key must be present, otherwise packets get dropped)
pio run -e gateway
pio run -e gateway -t upload
pio device monitor -e gateway
```

Without a complete `secrets.ini`, the build fails with clear messages
(`#error WIFI_SSID undefined`, `#error MQTT_HOST undefined`, `#error
LORA_PSK undefined`).

> Important: **emitter and gateway must share the same `LORA_PSK`**, otherwise the gateway will drop every packet (`HMAC invalid, drop ...` on the serial monitor). If you change the key, reflash both.

## Roadmap

### Software (done)
- [x] `emitter` skeleton (JSN-SR04T as JSON LoRa every 1s)
- [x] `gateway` skeleton (LoRa RX → serial log)
- [x] Wi-Fi + MQTT on the gateway
- [x] MQTT discovery for Home Assistant (diagnostic vs primary entities)
- [x] Soft watchdog (reboot if Wi-Fi/MQTT down > 5 min)
- [x] Link loss detection (`NODE_TIMEOUT_MS`, availability)
- [x] raw (emitter) / derived (gateway) split
- [x] Contextual OLED on emitter AND gateway (with graceful degradation if absent)
- [x] DS18B20 stainless steel waterproof probe (1-Wire async, factory-calibrated, °C direct)
- [x] Runtime calibration via HA `number` entities (TANK_EMPTY/FULL_DISTANCE_CM)
- [x] "null = full" rule + 3 s debounce to avoid flicker
- [x] **HMAC-SHA256 + anti-replay seq** on the LoRa link (auth + integrity)

### Software (possibly later)
- [ ] AES-128 on the LoRa payload for confidentiality (HMAC alone does not encrypt)
- [ ] Interrupt-driven LoRa RX instead of polling (if needed after testing)
- [ ] Time smoothing on the gateway side (moving average over the last N valid `tank_cm`)
- [ ] HA `number` for thermal offset if drift observed vs reference

### Hardware
- [x] Emitter wiring documented (LILYGO T3 V1.6.1 + SR04M-2 + voltage divider + 22 µF)
- [x] DS18B20 wiring documented (1-Wire + 4.7 kΩ pull-up)
- [ ] 3D enclosure for emitter (IP65, cable glands for power + sensor exits)
- [ ] 3D enclosure for gateway (indoor)
- [ ] House ↔ garden range tests

### Doc
- [ ] Photos of the final build
- [ ] Tank calibration (cm ↔ liters depending on geometry)

## License

[GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html) — see `LICENSE`.
