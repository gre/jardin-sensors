#include <Arduino.h>
#include <SPI.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include <math.h>
#include "auth.h"

#ifndef LORA_PSK
#error "LORA_PSK undefined: cp secrets.example.ini secrets.ini then fill in."
#endif

#if WITH_OLED
#include <Wire.h>
#include <U8g2lib.h>
#endif

#ifndef NODE_ID
#define NODE_ID "jardin"
#endif

// 1s to stay near real-time on OLED + HA. Note EU868 duty-cycle: ~50 ms
// airtime per packet at SF7/BW125. On 868.1 MHz the regulatory limit is
// 1% (= 1 packet/5s). We are at ~5%, OK for private indoor use.
#ifndef TX_INTERVAL_MS
#define TX_INTERVAL_MS 1000
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

static uint32_t seq = 0;
static bool loraReady = false;
static float lastValidDistanceCm = NAN;
static uint32_t lastValidDistanceMs = 0;

static OneWire oneWire(TEMP_PIN);
static DallasTemperature tempSensor(&oneWire);
static bool tempReady = false;

static void loraInit() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_BAND)) {
    Serial.println("[emitter] LoRa init failed");
    return;
  }
  LoRa.enableCrc();
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
  char foot[32];
  if (isnan(currentTemp)) {
    snprintf(foot, sizeof(foot), "TX #%lu", static_cast<unsigned long>(txSeq));
  } else {
    snprintf(foot, sizeof(foot), "TX #%lu   %.1fC",
             static_cast<unsigned long>(txSeq), currentTemp);
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
  float t = tempSensor.getTempCByIndex(0);
  tempSensor.requestTemperatures();
  if (t == DEVICE_DISCONNECTED_C || t >= 85.0f) return NAN;
  return t;
}

static void sendSample() {
  float dist = readDistanceCm();
  if (!isnan(dist)) {
    lastValidDistanceCm = dist;
    lastValidDistanceMs = millis();
  }
  float waterTemp = readWaterTempC();
  uint32_t s = seq++;

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

  char buf[200];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  n = authAppendMac(buf, n, sizeof(buf),
                    reinterpret_cast<const uint8_t*>(LORA_PSK),
                    strlen(LORA_PSK));
  buf[n] = '\0';  // null terminator for the printf below (LoRa.write does not need one)

  if (loraReady) {
    LoRa.beginPacket();
    LoRa.write(reinterpret_cast<const uint8_t*>(buf), n);
    LoRa.endPacket();
  }

  Serial.printf("[emitter] TX seq=%lu bytes=%u lora=%d payload=%s\n",
                static_cast<unsigned long>(s),
                static_cast<unsigned>(n),
                loraReady ? 1 : 0,
                buf);

#if WITH_OLED
  oledRender(dist, waterTemp, s);
#endif
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {}

  Serial.printf("[emitter] node=%s band=%lu Hz\n",
                NODE_ID,
                static_cast<unsigned long>(LORA_BAND));
#if WITH_OLED
  oledInit();
#endif
  sensorInit();
  tempInit();
  loraInit();
#if WITH_OLED
  oledRender(NAN, NAN, 0);
#endif
  Serial.printf("[emitter] ready (lora=%d)\n", loraReady ? 1 : 0);
}

void loop() {
  sendSample();
  delay(TX_INTERVAL_MS);
}
