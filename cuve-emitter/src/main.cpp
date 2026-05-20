#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <math.h>
#include "auth.h"
#include "wdt.h"
#include "lora_board.h"

#ifndef LORA_PSK
#error "LORA_PSK undefined: cp secrets.example.ini secrets.ini then fill in."
#endif

#if WITH_OLED
#include <Wire.h>
#include <U8g2lib.h>
#endif

#ifndef NODE_ID
#define NODE_ID "cuve"
#endif

// Default TX cadence used until the gateway has pushed a value (which is then
// persisted in NVS). EU868 reminder: ~50 ms airtime per packet at SF7/BW125,
// regulatory limit 1% (= 1 packet/5s). Hourly cadence is fine; 1 s only OK
// for indoor private use.
#ifndef TX_INTERVAL_S_DEFAULT
#define TX_INTERVAL_S_DEFAULT 60
#endif

// Bounds enforced on incoming config (must match the gateway HA slider range).
#ifndef TX_INTERVAL_S_MIN
#define TX_INTERVAL_S_MIN 5
#endif
#ifndef TX_INTERVAL_S_MAX
#define TX_INTERVAL_S_MAX 3600
#endif

// Re-ask the gateway for config every ~1h, plus on every fresh power-up.
#ifndef CFG_REFRESH_S
#define CFG_REFRESH_S 3600
#endif
// RX window after sending a cfg_req. Gateway reply is ~250-400 ms over the air,
// so 2 s leaves comfortable slack.
#ifndef CFG_RX_WINDOW_MS
#define CFG_RX_WINDOW_MS 2000
#endif

#ifndef WITH_DEEP_SLEEP
#define WITH_DEEP_SLEEP 0
#endif

#ifndef OLED_BOOT_DISPLAY_MS
#define OLED_BOOT_DISPLAY_MS 10000
#endif

// SR04M-2 in mode 0 (TTL pulse): RX=TRIG (sensor input), TX=ECHO (output).
// Datasheet: VCC 3.0-5.5V. Powered at 3.3V => echo at 3.3V => safe for ESP32.
// Wiring: sensor RX -> ESP32 IO4 (trig out), sensor TX -> ESP32 IO25 (echo in).
#ifndef US_TRIG_PIN
#define US_TRIG_PIN 4
#endif
#ifndef US_ECHO_PIN
#define US_ECHO_PIN 25
#endif
#ifndef US_TIMEOUT_US
#define US_TIMEOUT_US 40000UL
#endif
// 100us: comfortable margin vs datasheet spec ">10uS". Helps reliability when
// the TRIG pulse is driven at 3.3V (ESP32) on a sensor with VCC=5V.
#ifndef US_TRIG_PULSE_US
#define US_TRIG_PULSE_US 100
#endif
#ifndef US_STALE_MS
#define US_STALE_MS 30000UL
#endif

// DS18B20 5m stainless steel waterproof probe (Aideepen). 1-Wire with external 4.7k pull-up.
// 11-bit -> 0.125 deg C / ~375 ms conversion. Async read to avoid blocking.
#ifndef TEMP_PIN
#define TEMP_PIN 13
#endif
#ifndef TEMP_RESOLUTION_BITS
#define TEMP_RESOLUTION_BITS 11
#endif

#if WITH_OLED
// OLED SSD1306 0.96" on LILYGO T3 V1.6.1.
// Reset pin handled manually in oledInit() to avoid a hang in the static
// constructor (seen on some variants of the T3 V1.6.1).
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
#ifndef OLED_I2C_ADDR
#define OLED_I2C_ADDR 0x3C
#endif
static bool oledPresent = false;
#endif

// RTC memory survives deep sleep, resets on full power cycle. The reset is
// what lets the gateway's "seq < 100 and lastSeq > 10000" reboot heuristic
// re-arm the anti-replay counter.
RTC_DATA_ATTR static uint32_t rtcSeq = 0;
// Initialized > CFG_REFRESH_S so the very first packet after power-up carries
// cfg_req=1 and the emitter pulls fresh settings.
RTC_DATA_ATTR static uint32_t rtcCfgAccumS = 0xFFFFFFFFu;

static Preferences prefs;
static uint32_t txIntervalS = TX_INTERVAL_S_DEFAULT;

static bool loraReady = false;
static float lastValidDistanceCm = NAN;
static uint32_t lastValidDistanceMs = 0;

static OneWire oneWire(TEMP_PIN);
static DallasTemperature tempSensor(&oneWire);
static bool tempReady = false;
static uint32_t tempRequestMs = 0;

static void loadSettings() {
  prefs.begin("emitter", true);
  txIntervalS = prefs.getUInt("tx_int_s", TX_INTERVAL_S_DEFAULT);
  prefs.end();
  if (txIntervalS < TX_INTERVAL_S_MIN || txIntervalS > TX_INTERVAL_S_MAX) {
    txIntervalS = TX_INTERVAL_S_DEFAULT;
  }
  Serial.printf("[emitter] settings loaded: tx_interval_s=%u\n",
                static_cast<unsigned>(txIntervalS));
}

static void saveSettings() {
  prefs.begin("emitter", false);
  prefs.putUInt("tx_int_s", txIntervalS);
  prefs.end();
  Serial.printf("[emitter] settings saved: tx_interval_s=%u\n",
                static_cast<unsigned>(txIntervalS));
}

static void loraInit() {
  int16_t s = loraBegin();
  if (s != RADIOLIB_ERR_NONE) {
    Serial.printf("[emitter] LoRa init failed: %d\n", s);
    return;
  }
  loraRadio.startReceive();
  loraReady = true;
}

#if WITH_OLED
static void oledInit() {
  // No software pulse on OLED_RST: touching GPIO 16 on this T3 V1.6.1
  // variant causes a chip reset. The hardware RC at power-on is enough.
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.beginTransmission(OLED_I2C_ADDR);
  oledPresent = (Wire.endTransmission() == 0);
  if (!oledPresent) {
    Serial.printf("[emitter] OLED absent at 0x%02X, skipping\n", OLED_I2C_ADDR);
    return;
  }
  oled.begin();
  oled.setContrast(255);
  Serial.println("[emitter] OLED ready");
}

static void oledRender(float currentDist, float currentTemp, uint32_t txSeq) {
  if (!oledPresent) return;

  bool fresh = !isnan(currentDist);
  bool stale = false;
  float shown = currentDist;
  if (!fresh) {
    if (!isnan(lastValidDistanceCm) &&
        (millis() - lastValidDistanceMs) < US_STALE_MS) {
      shown = lastValidDistanceCm;
      stale = true;
    }
  }

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "Cuve emitter");
  oled.drawStr(74, 9, loraReady ? "LoRa OK" : "LoRa ERR");
  oled.drawHLine(0, 12, 128);

  oled.setFont(u8g2_font_logisoso24_tn);
  char num[12];
  if (isnan(shown)) {
    oled.drawStr(8, 44, "----");
  } else {
    snprintf(num, sizeof(num), "%.1f", shown);
    oled.drawStr(8, 44, num);
  }
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(102, 44, "cm");
  if (stale) oled.drawStr(118, 9, "?");

  oled.drawHLine(0, 50, 128);
  char foot[40];
  if (isnan(currentTemp)) {
    snprintf(foot, sizeof(foot), "TX #%lu  %us",
             static_cast<unsigned long>(txSeq),
             static_cast<unsigned>(txIntervalS));
  } else {
    snprintf(foot, sizeof(foot), "TX #%lu %.1fC %us",
             static_cast<unsigned long>(txSeq), currentTemp,
             static_cast<unsigned>(txIntervalS));
  }
  oled.drawStr(0, 62, foot);

  oled.sendBuffer();
}
#endif // WITH_OLED

static void sensorInit() {
  pinMode(US_TRIG_PIN, OUTPUT);
  pinMode(US_ECHO_PIN, INPUT);
  digitalWrite(US_TRIG_PIN, LOW);
}

static float readDistanceCm() {
  digitalWrite(US_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(US_TRIG_PIN, HIGH);
  delayMicroseconds(US_TRIG_PULSE_US);
  digitalWrite(US_TRIG_PIN, LOW);
  unsigned long us = pulseIn(US_ECHO_PIN, HIGH, US_TIMEOUT_US);
  if (us == 0) return NAN;
  return us / 58.0f;
}

static void tempInit() {
  tempSensor.begin();
  if (tempSensor.getDeviceCount() == 0) {
    Serial.println("[emitter] DS18B20 not found");
    return;
  }
  tempSensor.setResolution(TEMP_RESOLUTION_BITS);
  tempSensor.setWaitForConversion(false);
  tempSensor.requestTemperatures();
  tempRequestMs = millis();
  tempReady = true;
  Serial.printf("[emitter] DS18B20 ready (%d-bit)\n", TEMP_RESOLUTION_BITS);
}

// Async read: returns the result of the previous conversion, kicks off a new one.
// Filters:
// - DEVICE_DISCONNECTED_C: dead bus (cable unplugged, sensor missing).
// - 85.0: power-on reset value, before the 1st conversion. Impossible for
//   garden water, so we treat it as a read bug.
static float readWaterTempC() {
  if (!tempReady) return NAN;
  if ((millis() - tempRequestMs) < 400) return NAN;
  float t = tempSensor.getTempCByIndex(0);
  tempSensor.requestTemperatures();
  tempRequestMs = millis();
  if (t == DEVICE_DISCONNECTED_C || t >= 85.0f) return NAN;
  return t;
}

// Listen for a gateway config response addressed to us, echoing reqSeq.
// Returns true if a valid, on-target ACK arrived (txIntervalS may have been
// updated and persisted as a side effect).
static bool tryReceiveConfig(uint32_t reqSeq) {
  loraRadio.startReceive();
  uint32_t deadline = millis() + CFG_RX_WINDOW_MS;
  while ((int32_t)(deadline - millis()) > 0) {
    if (!loraRxFlag) {
      delay(5);
      continue;
    }
    loraRxFlag = false;
    char buf[200];
    size_t pktLen = loraReadPacket(buf, sizeof(buf));
    if (pktLen == 0) continue;
    int jsonLen = authVerifyMac(buf, static_cast<int>(pktLen),
                                reinterpret_cast<const uint8_t*>(LORA_PSK),
                                strlen(LORA_PSK));
    if (jsonLen < 0) {
      Serial.printf("[emitter] cfg RX: HMAC invalid len=%u\n",
                    static_cast<unsigned>(pktLen));
      continue;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, jsonLen);
    if (err) continue;
    const char* to = doc["to"] | "";
    if (strcmp(to, NODE_ID) != 0) continue;
    if (doc["ack"].as<uint32_t>() != reqSeq) continue;
    if (!doc["cfg"].is<JsonObject>()) continue;
    JsonObject cfg = doc["cfg"].as<JsonObject>();
    bool changed = false;
    if (cfg["tx_interval_s"].is<unsigned int>() ||
        cfg["tx_interval_s"].is<int>()) {
      uint32_t v = cfg["tx_interval_s"].as<uint32_t>();
      if (v >= TX_INTERVAL_S_MIN && v <= TX_INTERVAL_S_MAX && v != txIntervalS) {
        Serial.printf("[emitter] tx_interval_s %u -> %u\n",
                      static_cast<unsigned>(txIntervalS),
                      static_cast<unsigned>(v));
        txIntervalS = v;
        changed = true;
      }
    }
    if (changed) saveSettings();
    Serial.printf("[emitter] cfg ACK seq=%u tx_interval_s=%u\n",
                  static_cast<unsigned>(reqSeq),
                  static_cast<unsigned>(txIntervalS));
    return true;
  }
  Serial.printf("[emitter] cfg ACK timeout for seq=%u\n",
                static_cast<unsigned>(reqSeq));
  return false;
}

static void sendSample() {
  float dist = readDistanceCm();
  if (!isnan(dist)) {
    lastValidDistanceCm = dist;
    lastValidDistanceMs = millis();
  }
  float waterTemp = readWaterTempC();
  float vbat = readVbatVolts();
  uint32_t s = rtcSeq++;
  bool wantCfg = rtcCfgAccumS >= CFG_REFRESH_S;

  JsonDocument doc;
  doc["node"] = NODE_ID;
  doc["seq"]  = s;
  if (isnan(dist)) {
    doc["tank_cm"] = nullptr;
  } else {
    doc["tank_cm"] = roundf(dist * 10.0f) / 10.0f;
  }
  if (isnan(waterTemp)) {
    doc["water_temp_c"] = nullptr;
  } else {
    doc["water_temp_c"] = roundf(waterTemp * 10.0f) / 10.0f;
  }
  doc["vbat"] = roundf(vbat * 100.0f) / 100.0f;
  if (wantCfg) doc["cfg_req"] = 1;

  char buf[200];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  n = authAppendMac(buf, n, sizeof(buf),
                    reinterpret_cast<const uint8_t*>(LORA_PSK),
                    strlen(LORA_PSK));
  buf[n] = '\0';

  bool txOk = false;
  if (loraReady) {
    txOk = (loraTx(reinterpret_cast<const uint8_t*>(buf), n) == RADIOLIB_ERR_NONE);
  }

  Serial.printf("[emitter] TX seq=%lu cfg_req=%d bytes=%u lora=%d tx_ok=%d payload=%s\n",
                static_cast<unsigned long>(s),
                wantCfg ? 1 : 0,
                static_cast<unsigned>(n),
                loraReady ? 1 : 0,
                txOk ? 1 : 0,
                buf);

  if (wantCfg && loraReady) {
    if (tryReceiveConfig(s)) {
      rtcCfgAccumS = 0;
    }
    // On timeout: keep accumS over threshold so next cycle retries.
  } else {
    rtcCfgAccumS += txIntervalS;
  }

#if WITH_OLED
  oledRender(dist, waterTemp, s);
#endif
}

static void enterDeepSleep() {
  uint64_t us = static_cast<uint64_t>(txIntervalS) * 1000000ULL;
  Serial.printf("[emitter] deep sleep %us\n",
                static_cast<unsigned>(txIntervalS));
  Serial.flush();
  // Park the SX1276 in STDBY before yanking power: empties FIFOs and avoids
  // a half-finished TX leaking into the next boot's first packet.
  if (loraReady) loraRadio.sleep();
  esp_sleep_enable_timer_wakeup(us);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  watchdogInit();

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool firstBoot = (cause == ESP_SLEEP_WAKEUP_UNDEFINED);
  Serial.printf("[emitter] node=%s band=%lu Hz wake_cause=%d first_boot=%d\n",
                NODE_ID,
                static_cast<unsigned long>(LORA_BAND),
                static_cast<int>(cause),
                firstBoot ? 1 : 0);

  loadSettings();

#if WITH_OLED
  oledInit();
#endif
  sensorInit();
  tempInit();
  loraInit();
#if WITH_OLED
  oledRender(NAN, NAN, rtcSeq);
#endif

  int oledStatus = 0;
#if WITH_OLED
  oledStatus = oledPresent ? 1 : 0;
#endif
  Serial.printf("[emitter] ready (lora=%d) tx_interval_s=%u oled=%d\n",
                loraReady ? 1 : 0,
                static_cast<unsigned>(txIntervalS),
                oledStatus);

#if WITH_DEEP_SLEEP
  sendSample();
#if WITH_OLED
  if (oledPresent) {
    delay(OLED_BOOT_DISPLAY_MS);
  }
#endif
  enterDeepSleep();
#endif
}

void loop() {
#if WITH_DEEP_SLEEP
  // Unreachable: setup() ends in deep sleep, which restarts via setup().
  enterDeepSleep();
#else
  watchdogFeed();
  sendSample();
  // Chunked sleep so the watchdog stays fed even at long txIntervalS.
  uint32_t remaining = static_cast<uint32_t>(txIntervalS) * 1000UL;
  while (remaining > 0) {
    uint32_t chunk = remaining > 1000 ? 1000 : remaining;
    delay(chunk);
    watchdogFeed();
    remaining -= chunk;
  }
#endif
}
