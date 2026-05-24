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

#ifdef FLOWERCARE_MAC
#include <NimBLEDevice.h>
// Arduino 3.x releases BTDM memory in initArduino() unless btInUse() returns
// true. NimBLE-Arduino 1.4.x doesn't set _btLibraryInUse, so override here.
extern "C" bool btInUse() { return true; }
#endif

#ifndef DEBUG_US
#  ifndef LORA_PSK
#    error "LORA_PSK undefined: cp secrets.example.ini secrets.ini then fill in."
#  endif
#else
#  define LORA_PSK ""
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

// Minimum time from boot to first sensor read. Lets the rail and sensors
// stabilize after a deep-sleep wake transient. Configurable from HA.
#ifndef BOOT_DELAY_MS_DEFAULT
#define BOOT_DELAY_MS_DEFAULT 1000
#endif
#ifndef BOOT_DELAY_MS_MIN
#define BOOT_DELAY_MS_MIN 0
#endif
#ifndef BOOT_DELAY_MS_MAX
#define BOOT_DELAY_MS_MAX 5000
#endif

// Ultrasonic read attempts per cycle. Configurable from HA.
#ifndef US_RETRIES_DEFAULT
#define US_RETRIES_DEFAULT 3
#endif
#ifndef US_RETRIES_MIN
#define US_RETRIES_MIN 1
#endif
#ifndef US_RETRIES_MAX
#define US_RETRIES_MAX 10
#endif

// DS18B20 conversion attempts per cycle. Configurable from HA.
#ifndef TEMP_RETRIES_DEFAULT
#define TEMP_RETRIES_DEFAULT 2
#endif
#ifndef TEMP_RETRIES_MIN
#define TEMP_RETRIES_MIN 1
#endif
#ifndef TEMP_RETRIES_MAX
#define TEMP_RETRIES_MAX 5
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

#ifndef OLED_DISPLAY_MS_DEFAULT
#define OLED_DISPLAY_MS_DEFAULT 10000
#endif
#ifndef OLED_DISPLAY_MS_MIN
#define OLED_DISPLAY_MS_MIN 0
#endif
#ifndef OLED_DISPLAY_MS_MAX
#define OLED_DISPLAY_MS_MAX 30000
#endif

// HC-SR04 waterproof variant. VCC=5V; ECHO outputs 5V => voltage divider
// 10k+20k required on ECHO before ESP32 GPIO (brings 5V down to ~3.3V).
// Wiring: TRIG -> ESP32 IO4, ECHO -> divider -> ESP32 IO25.
#ifndef US_TRIG_PIN
#define US_TRIG_PIN 4
#endif
#ifndef US_ECHO_PIN
#define US_ECHO_PIN 25
#endif
#ifndef US_TIMEOUT_US
#define US_TIMEOUT_US 40000UL
#endif
// 100us: comfortable margin vs datasheet minimum of 10us.
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

#ifdef FLOWERCARE_MAC
// Timeout for a single BLE connect attempt. Keep short so a missing
// Flower Care doesn't add more than this many seconds to each wake cycle.
#ifndef FLOWERCARE_CONNECT_TIMEOUT_S
#define FLOWERCARE_CONNECT_TIMEOUT_S 5
#endif
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
static float oledVbat = NAN;
#ifdef FLOWERCARE_MAC
static float   oledSoilTempC = NAN;
static int     oledSoilMoist = -1;
static int32_t oledSoilLight = -1;
static int     oledSoilCond  = -1;
#endif
#endif

// RTC memory survives deep sleep, resets on full power cycle. The reset is
// what lets the gateway's "seq < 100 and lastSeq > 10000" reboot heuristic
// re-arm the anti-replay counter.
RTC_DATA_ATTR static uint32_t rtcSeq = 0;
// Initialized > CFG_REFRESH_S so the very first packet after power-up carries
// cfg_req=1 and the emitter pulls fresh settings.
RTC_DATA_ATTR static uint32_t rtcCfgAccumS = 0xFFFFFFFFu;

static Preferences prefs;
static uint32_t txIntervalS   = TX_INTERVAL_S_DEFAULT;
static uint32_t bootDelayMs   = BOOT_DELAY_MS_DEFAULT;
static uint32_t oledDisplayMs = OLED_DISPLAY_MS_DEFAULT;
static uint8_t  usRetries     = US_RETRIES_DEFAULT;
static uint8_t  tempRetries   = TEMP_RETRIES_DEFAULT;

static bool loraReady = false;
static float lastValidDistanceCm = NAN;
static uint32_t lastValidDistanceMs = 0;

static OneWire oneWire(TEMP_PIN);
static DallasTemperature tempSensor(&oneWire);
static bool tempReady = false;
static uint32_t tempRequestMs = 0;

static void loadSettings() {
  prefs.begin("emitter", true);
  txIntervalS   = prefs.getUInt("tx_int_s",    TX_INTERVAL_S_DEFAULT);
  bootDelayMs   = prefs.getUInt("boot_dly_ms", BOOT_DELAY_MS_DEFAULT);
  oledDisplayMs = prefs.getUInt("oled_disp_ms", OLED_DISPLAY_MS_DEFAULT);
  usRetries     = prefs.getUChar("us_retries",   US_RETRIES_DEFAULT);
  tempRetries   = prefs.getUChar("temp_retries", TEMP_RETRIES_DEFAULT);
  prefs.end();
  if (txIntervalS < TX_INTERVAL_S_MIN || txIntervalS > TX_INTERVAL_S_MAX)
    txIntervalS = TX_INTERVAL_S_DEFAULT;
  if (bootDelayMs > BOOT_DELAY_MS_MAX)
    bootDelayMs = BOOT_DELAY_MS_DEFAULT;
  if (oledDisplayMs > OLED_DISPLAY_MS_MAX)
    oledDisplayMs = OLED_DISPLAY_MS_DEFAULT;
  if (usRetries < US_RETRIES_MIN || usRetries > US_RETRIES_MAX)
    usRetries = US_RETRIES_DEFAULT;
  if (tempRetries < TEMP_RETRIES_MIN || tempRetries > TEMP_RETRIES_MAX)
    tempRetries = TEMP_RETRIES_DEFAULT;
  Serial.printf("[emitter] settings loaded: tx_int=%u boot_dly=%u oled_ms=%u us_ret=%u temp_ret=%u\n",
                static_cast<unsigned>(txIntervalS),
                static_cast<unsigned>(bootDelayMs),
                static_cast<unsigned>(oledDisplayMs),
                static_cast<unsigned>(usRetries),
                static_cast<unsigned>(tempRetries));
}

static void saveSettings() {
  prefs.begin("emitter", false);
  prefs.putUInt("tx_int_s",      txIntervalS);
  prefs.putUInt("boot_dly_ms",   bootDelayMs);
  prefs.putUInt("oled_disp_ms",  oledDisplayMs);
  prefs.putUChar("us_retries",   usRetries);
  prefs.putUChar("temp_retries", tempRetries);
  prefs.end();
  Serial.printf("[emitter] settings saved: tx_int=%u boot_dly=%u oled_ms=%u us_ret=%u temp_ret=%u\n",
                static_cast<unsigned>(txIntervalS),
                static_cast<unsigned>(bootDelayMs),
                static_cast<unsigned>(oledDisplayMs),
                static_cast<unsigned>(usRetries),
                static_cast<unsigned>(tempRetries));
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
  uint32_t now = millis();
  bool blink = (now / 500) % 2;

  bool fresh = !isnan(currentDist);
  bool stale = false;
  float shown = currentDist;
  if (!fresh) {
    if (!isnan(lastValidDistanceCm) &&
        (now - lastValidDistanceMs) < US_STALE_MS) {
      shown = lastValidDistanceCm;
      stale = true;
    }
  }

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);

#ifdef FLOWERCARE_MAC
  // Compact 5-row layout (128x64):
  //   y=9:  CUVE          <interval>
  //   y=34: <24pt dist>          cm
  //   y=46: W:<water>C  <vbat>V
  //   y=56: T:<soil>    H:<moist>%
  //   y=63: L:<lux>     C:<cond>
  oled.drawStr(0, 9, "CUVE");
  if (!loraReady && blink) {
    oled.setFont(u8g2_font_open_iconic_all_1x_t);
    oled.drawGlyph(30, 9, 0x118);
    oled.setFont(u8g2_font_6x10_tf);
  }
  {
    char ibuf[8];
    snprintf(ibuf, sizeof(ibuf), "%us", static_cast<unsigned>(txIntervalS));
    oled.drawStr(128 - static_cast<uint8_t>(strlen(ibuf) * 6), 9, ibuf);
  }

  oled.setFont(u8g2_font_logisoso16_tn);
  if (isnan(shown)) {
    oled.drawStr(8, 26, "----");
  } else if (!stale || blink) {
    char num[12];
    snprintf(num, sizeof(num), "%.1f", shown);
    oled.drawStr(8, 26, num);
  }
  oled.setFont(u8g2_font_6x10_tf);
  if (!isnan(shown) && (!stale || blink)) {
    oled.drawStr(72, 26, "cm");
  }

  // Water temp + vbat
  if (!isnan(currentTemp)) {
    char buf[12];
    snprintf(buf, sizeof(buf), "W:%.1f\xb0""C", currentTemp);
    oled.drawStr(0, 40, buf);
  } else {
    oled.drawStr(0, 40, "W:---");
  }
  if (!isnan(oledVbat)) {
    bool vbatLow = oledVbat < 3.5f;
    if (!vbatLow || blink) {
      char buf[8];
      snprintf(buf, sizeof(buf), "%.2fV", oledVbat);
      oled.drawStr(66, 40, buf);
    }
  }

  // Soil temp + moisture
  if (!isnan(oledSoilTempC)) {
    char buf[10];
    snprintf(buf, sizeof(buf), "T:%.1f", oledSoilTempC);
    oled.drawStr(0, 52, buf);
  } else {
    oled.drawStr(0, 52, "T:---");
  }
  if (oledSoilMoist >= 0) {
    char buf[8];
    snprintf(buf, sizeof(buf), "H:%d%%", oledSoilMoist);
    oled.drawStr(66, 52, buf);
  } else {
    oled.drawStr(66, 52, "H:--");
  }

  // Soil light + conductivity
  if (oledSoilLight >= 0) {
    char buf[12];
    snprintf(buf, sizeof(buf), "L:%ld", (long)oledSoilLight);
    oled.drawStr(0, 63, buf);
  } else {
    oled.drawStr(0, 63, "L:---");
  }
  if (oledSoilCond >= 0) {
    char buf[8];
    snprintf(buf, sizeof(buf), "C:%d", oledSoilCond);
    oled.drawStr(66, 63, buf);
  } else {
    oled.drawStr(66, 63, "C:--");
  }

#else
  // Standard layout: header y=10, big number y=44, footer y=62
  oled.drawStr(0, 10, "CUVE");
  if (!loraReady && blink) {
    oled.setFont(u8g2_font_open_iconic_all_1x_t);
    oled.drawGlyph(114, 10, 0x118);
    oled.setFont(u8g2_font_6x10_tf);
  }

  oled.setFont(u8g2_font_logisoso24_tn);
  if (isnan(shown)) {
    oled.drawStr(8, 44, "----");
  } else if (!stale || blink) {
    char num[12];
    snprintf(num, sizeof(num), "%.1f", shown);
    oled.drawStr(8, 44, num);
  }
  oled.setFont(u8g2_font_6x10_tf);
  if (!isnan(shown) && (!stale || blink)) {
    oled.drawStr(102, 44, "cm");
  }

  if (!isnan(currentTemp)) {
    char buf[10];
    snprintf(buf, sizeof(buf), "%.1f\xb0""C", currentTemp);
    oled.drawStr(0, 62, buf);
  } else {
    oled.drawStr(0, 62, "---");
  }
  if (!isnan(oledVbat)) {
    bool vbatLow = oledVbat < 3.5f;
    if (!vbatLow || blink) {
      char buf[8];
      snprintf(buf, sizeof(buf), "%.2fV", oledVbat);
      oled.drawStr(44, 62, buf);
    }
  }
  {
    char buf[8];
    snprintf(buf, sizeof(buf), "%us", static_cast<unsigned>(txIntervalS));
    oled.drawStr(128 - static_cast<uint8_t>(strlen(buf) * 6), 62, buf);
  }
#endif

  oled.sendBuffer();
}
#endif // WITH_OLED

static void sensorInit() {
  pinMode(US_TRIG_PIN, OUTPUT);
  pinMode(US_ECHO_PIN, INPUT);
  digitalWrite(US_TRIG_PIN, LOW);
}

static float readDistanceCm() {
  for (int attempt = 0; attempt < usRetries; attempt++) {
    if (attempt > 0) delay(20);
    digitalWrite(US_TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(US_TRIG_PIN, HIGH);
    delayMicroseconds(US_TRIG_PULSE_US);
    digitalWrite(US_TRIG_PIN, LOW);
    unsigned long us = pulseIn(US_ECHO_PIN, HIGH, US_TIMEOUT_US);
    if (us > 0) return us / 58.0f;
  }
  return NAN;
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
  // Retry loop catches the 85 °C power-on sentinel and DEVICE_DISCONNECTED
  // transients on the first read after a deep-sleep wake. Each attempt blocks
  // for the remainder of the 11-bit conversion window (~375 ms).
  for (int attempt = 0; attempt < tempRetries; attempt++) {
    uint32_t elapsed = millis() - tempRequestMs;
    if (elapsed < 400) delay(400 - elapsed);
    float t = tempSensor.getTempCByIndex(0);
    tempSensor.requestTemperatures();
    tempRequestMs = millis();
    if (t != DEVICE_DISCONNECTED_C && t < 85.0f) return t;
  }
  return NAN;
}

template<typename T>
static bool applyCfgField(JsonObject& cfg, const char* key,
                           T& var, T lo, T hi) {
  if (!cfg[key].is<unsigned int>() && !cfg[key].is<int>()) return false;
  T v = cfg[key].as<T>();
  if (v < lo || v > hi || v == var) return false;
  Serial.printf("[emitter] %s %u -> %u\n", key,
                static_cast<unsigned>(var), static_cast<unsigned>(v));
  var = v;
  return true;
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
    changed |= applyCfgField<uint32_t>(cfg, "tx_interval_s",
                                        txIntervalS, TX_INTERVAL_S_MIN,  TX_INTERVAL_S_MAX);
    changed |= applyCfgField<uint32_t>(cfg, "boot_delay_ms",
                                        bootDelayMs,  BOOT_DELAY_MS_MIN,  BOOT_DELAY_MS_MAX);
    changed |= applyCfgField<uint8_t> (cfg, "us_retries",
                                        usRetries,    US_RETRIES_MIN,     US_RETRIES_MAX);
    changed |= applyCfgField<uint8_t> (cfg, "temp_retries",
                                        tempRetries,    TEMP_RETRIES_MIN,     TEMP_RETRIES_MAX);
    changed |= applyCfgField<uint32_t>(cfg, "oled_display_ms",
                                        oledDisplayMs,  OLED_DISPLAY_MS_MIN,  OLED_DISPLAY_MS_MAX);
    if (changed) saveSettings();
    Serial.printf("[emitter] cfg ACK seq=%u\n", static_cast<unsigned>(reqSeq));
    return true;
  }
  Serial.printf("[emitter] cfg ACK timeout for seq=%u\n",
                static_cast<unsigned>(reqSeq));
  return false;
}

#ifdef FLOWERCARE_MAC
struct FlowerCareData {
  float    soilTempC;
  uint8_t  soilMoisture;
  uint32_t lightLux;
  int16_t  conductivity;
  uint8_t  battery;
  bool     valid;
};

static FlowerCareData readFlowerCare() {
  FlowerCareData out = {NAN, 0, 0, 0, 0, false};
  NimBLEClient* client = NimBLEDevice::createClient();
  if (!client) {
    Serial.println("[emitter] FlowerCare: createClient failed");
    return out;
  }
  client->setConnectTimeout(FLOWERCARE_CONNECT_TIMEOUT_S);
  if (!client->connect(NimBLEAddress(FLOWERCARE_MAC, 0))) {
    Serial.println("[emitter] FlowerCare: connect failed");
    NimBLEDevice::deleteClient(client);
    return out;
  }
  do {
    NimBLERemoteService* svc = client->getService("00001204-0000-1000-8000-00805f9b34fb");
    if (!svc) { Serial.println("[emitter] FlowerCare: service not found"); break; }
    NimBLERemoteCharacteristic* cmd  = svc->getCharacteristic("00001a00-0000-1000-8000-00805f9b34fb");
    NimBLERemoteCharacteristic* data = svc->getCharacteristic("00001a01-0000-1000-8000-00805f9b34fb");
    NimBLERemoteCharacteristic* batC = svc->getCharacteristic("00001a02-0000-1000-8000-00805f9b34fb");
    if (!cmd || !data) { Serial.println("[emitter] FlowerCare: char not found"); break; }
    uint8_t wake[] = {0xA0, 0x1F};
    if (!cmd->writeValue(wake, sizeof(wake), true)) {
      Serial.println("[emitter] FlowerCare: write failed");
      break;
    }
    delay(300);
    std::string raw = data->readValue();
    if (raw.length() < 10) {
      Serial.printf("[emitter] FlowerCare: short read %u bytes\n", (unsigned)raw.length());
      client->disconnect();
      break;
    }
    const uint8_t* d = reinterpret_cast<const uint8_t*>(raw.data());
    int16_t rawT;
    memcpy(&rawT, d, 2);     out.soilTempC    = rawT / 10.0f;
    memcpy(&out.lightLux, d + 3, 4);
                              out.soilMoisture = d[7];
    memcpy(&out.conductivity, d + 8, 2);
    if (batC) {
      std::string b = batC->readValue();
      if (!b.empty()) out.battery = (uint8_t)b[0];
    }
    out.valid = true;
    Serial.printf("[emitter] FlowerCare T:%.1f H:%u L:%lu C:%d BAT:%u\n",
      out.soilTempC, (unsigned)out.soilMoisture, (unsigned long)out.lightLux,
      (int)out.conductivity, (unsigned)out.battery);
  } while (false);

  client->disconnect();
  NimBLEDevice::deleteClient(client);
  return out;
}
#endif // FLOWERCARE_MAC

static void sendSample() {
  float dist = readDistanceCm();
  if (!isnan(dist)) {
    lastValidDistanceCm = dist;
    lastValidDistanceMs = millis();
  }
  float waterTemp = readWaterTempC();
#ifdef FLOWERCARE_MAC
  FlowerCareData fc = readFlowerCare();
#endif
  float vbat = readVbatVolts();
  uint32_t s = rtcSeq++;
  // Always request config for the first 5 packets (covers missed ACK on boot 0).
  // s < 5: retry cfg_req for the first 5 cycles after power-on, but only
  // while rtcCfgAccumS != 0 (i.e., no ACK received yet this power cycle).
  bool wantCfg = (rtcCfgAccumS >= CFG_REFRESH_S) || (s < 5 && rtcCfgAccumS != 0);

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
#ifdef FLOWERCARE_MAC
  if (fc.valid) {
    doc["soil_temp_c"]   = roundf(fc.soilTempC * 10.0f) / 10.0f;
    doc["soil_moisture"] = fc.soilMoisture;
    doc["soil_light"]    = fc.lightLux;
    doc["soil_cond"]     = fc.conductivity;
    doc["soil_bat"]      = fc.battery;
  } else {
    doc["soil_temp_c"]   = nullptr;
    doc["soil_moisture"] = nullptr;
    doc["soil_light"]    = nullptr;
    doc["soil_cond"]     = nullptr;
    doc["soil_bat"]      = nullptr;
  }
#endif
  if (wantCfg) doc["cfg_req"] = 1;

  char buf[256];
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
  oledVbat = vbat;
#ifdef FLOWERCARE_MAC
  oledSoilTempC = fc.valid ? fc.soilTempC        : NAN;
  oledSoilMoist = fc.valid ? (int)fc.soilMoisture : -1;
  oledSoilLight = fc.valid ? (int32_t)fc.lightLux : -1;
  oledSoilCond  = fc.valid ? (int)fc.conductivity  : -1;
#endif
  oledRender(dist, waterTemp, s);
#endif
}

static void enterDeepSleep() {
  uint64_t us = static_cast<uint64_t>(txIntervalS) * 1000000ULL;
  Serial.printf("[emitter] deep sleep %us\n",
                static_cast<unsigned>(txIntervalS));
  Serial.flush();
  digitalWrite(US_TRIG_PIN, LOW);
#if WITH_OLED
  // SSD1306 stays powered on the 3.3V rail during deep sleep; explicit power-off
  // prevents the screen from staying lit the whole sleep interval.
  if (oledPresent) oled.setPowerSave(1);
#endif
  // Park the SX1276 in STDBY before yanking power: empties FIFOs and avoids
  // a half-finished TX leaking into the next boot's first packet.
  if (loraReady) loraRadio.sleep();
#ifdef FLOWERCARE_MAC
  NimBLEDevice::deinit(true);
#endif
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

#ifdef DEBUG_US
  Serial.println("[debug] US continuous mode — reading every 100ms");
  while (true) {
    float d = readDistanceCm();
    if (isnan(d)) Serial.println("[US] ---");
    else          Serial.printf("[US] %.1f cm\n", d);
#if WITH_OLED
    oledRender(d, NAN, 0);
#endif
    delay(100);
  }
#endif

  tempInit();
#ifdef FLOWERCARE_MAC
  NimBLEDevice::init("");
#endif
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
  {
    // Ensure bootDelayMs ms have elapsed since power-on before the first
    // sensor read; accounts for time already spent in init.
    uint32_t elapsed = millis();
    if (elapsed < bootDelayMs) delay(bootDelayMs - elapsed);
  }
  sendSample();
#if WITH_OLED
  if (oledPresent) {
    delay(oledDisplayMs);
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
