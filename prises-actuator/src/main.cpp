#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "auth.h"
#include "wdt.h"
#include "lora_board.h"

#if WITH_OLED
#include <Wire.h>
#include <U8g2lib.h>
#endif

#ifndef LORA_PSK
#error "LORA_PSK undefined: cp secrets.example.ini secrets.ini then fill in."
#endif

#ifndef NODE_ID
#define NODE_ID "prises"
#endif

#ifndef TX_INTERVAL_S
#define TX_INTERVAL_S 60
#endif

#ifndef RELAY1_PIN
#define RELAY1_PIN 32
#endif
#ifndef RELAY2_PIN
#define RELAY2_PIN 33
#endif

// Set to 0 for active-HIGH relay modules.
#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 1
#endif

static Preferences prefs;
static bool loraReady = false;
static uint32_t txSeq = 0;
static int relay1State = 0;
static int relay2State = 0;

#if WITH_OLED
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
#ifndef OLED_I2C_ADDR
#define OLED_I2C_ADDR 0x3C
#endif
static bool oledPresent = false;
static uint32_t lastCmdMs = 0;
#endif

static inline int relayLevel(int state) {
#if RELAY_ACTIVE_LOW
  return state ? LOW : HIGH;
#else
  return state ? HIGH : LOW;
#endif
}

static void relayApply() {
  digitalWrite(RELAY1_PIN, relayLevel(relay1State));
  digitalWrite(RELAY2_PIN, relayLevel(relay2State));
}

static void relayInit() {
  // Drive pins to safe level before enabling output drivers (boot glitch prevention).
  relayApply();
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
}

static void setRelays(int r1, int r2) {
  r1 = constrain(r1, 0, 1);
  r2 = constrain(r2, 0, 1);
  if (r1 == relay1State && r2 == relay2State) return;
  relay1State = r1;
  relay2State = r2;
  relayApply();
  prefs.begin(NODE_ID, false);
  prefs.putInt("r1", relay1State);
  prefs.putInt("r2", relay2State);
  prefs.end();
  Serial.printf("[" NODE_ID "] relay1=%d relay2=%d\n", relay1State, relay2State);
}

static void loadRelayState() {
  prefs.begin(NODE_ID, true);
  relay1State = constrain(prefs.getInt("r1", 0), 0, 1);
  relay2State = constrain(prefs.getInt("r2", 0), 0, 1);
  prefs.end();
  Serial.printf("[" NODE_ID "] relay state loaded: relay1=%d relay2=%d\n",
                relay1State, relay2State);
}

static void loraInit() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_BAND)) {
    Serial.println("[" NODE_ID "] LoRa init failed");
    return;
  }
  loraConfigureRadio();
  loraReady = true;
}

#if WITH_OLED
static void oledInit() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.beginTransmission(OLED_I2C_ADDR);
  oledPresent = (Wire.endTransmission() == 0);
  if (!oledPresent) {
    Serial.printf("[" NODE_ID "] OLED absent at 0x%02X\n", OLED_I2C_ADDR);
    return;
  }
  oled.begin();
  oled.setContrast(255);
  Serial.println("[" NODE_ID "] OLED ready");
}

static void oledRender() {
  if (!oledPresent) return;
  uint32_t now = millis();

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "Prises actuateur");
  oled.drawStr(92, 9, loraReady ? "LoRa OK" : "LoRa ERR");
  oled.drawHLine(0, 12, 128);

  char r1buf[16], r2buf[16];
  snprintf(r1buf, sizeof(r1buf), "Relay1: %s", relay1State ? "ON " : "OFF");
  snprintf(r2buf, sizeof(r2buf), "Relay2: %s", relay2State ? "ON " : "OFF");
  oled.drawStr(0, 28, r1buf);
  oled.drawStr(0, 40, r2buf);

  oled.drawHLine(0, 50, 128);
  char foot[40];
  if (lastCmdMs == 0) {
    snprintf(foot, sizeof(foot), "TX #%lu  no cmd yet",
             static_cast<unsigned long>(txSeq));
  } else {
    uint32_t ageS = (now - lastCmdMs) / 1000;
    snprintf(foot, sizeof(foot), "TX #%lu  cmd %lus ago",
             static_cast<unsigned long>(txSeq),
             static_cast<unsigned long>(ageS));
  }
  oled.drawStr(0, 62, foot);
  oled.sendBuffer();
}
#endif

static void sendHeartbeat() {
  float vbat = readVbatVolts();

  JsonDocument doc;
  doc["node"]   = NODE_ID;
  doc["seq"]    = txSeq++;
  doc["relay1"] = relay1State;
  doc["relay2"] = relay2State;
  doc["vbat"]   = roundf(vbat * 100.0f) / 100.0f;

  char buf[200];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  n = authAppendMac(buf, n, sizeof(buf),
                    reinterpret_cast<const uint8_t*>(LORA_PSK),
                    strlen(LORA_PSK));

  bool txOk = false;
  if (loraReady) {
    LoRa.beginPacket();
    LoRa.write(reinterpret_cast<const uint8_t*>(buf), n);
    txOk = (LoRa.endPacket() == 1);
  }
  Serial.printf("[" NODE_ID "] TX seq=%lu relay1=%d relay2=%d bytes=%u ok=%d\n",
                static_cast<unsigned long>(txSeq - 1),
                relay1State, relay2State,
                static_cast<unsigned>(n), txOk ? 1 : 0);
}

static void handleLoRaPacket() {
  int sz = LoRa.parsePacket();
  if (sz <= 0) return;

  char buf[200];
  int len = 0;
  while (LoRa.available() && len < (int)sizeof(buf) - 1) {
    buf[len++] = static_cast<char>(LoRa.read());
  }

  int jsonLen = authVerifyMac(buf, len,
                              reinterpret_cast<const uint8_t*>(LORA_PSK),
                              strlen(LORA_PSK));
  if (jsonLen < 0) {
    Serial.printf("[" NODE_ID "] HMAC invalid, drop %d bytes\n", len);
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, buf, jsonLen)) return;

  const char* to = doc["to"] | "";
  if (strcmp(to, NODE_ID) != 0) return;

  int r1 = doc["relay1"].is<int>() ? doc["relay1"].as<int>() : relay1State;
  int r2 = doc["relay2"].is<int>() ? doc["relay2"].as<int>() : relay2State;
  setRelays(r1, r2);

#if WITH_OLED
  lastCmdMs = millis();
  oledRender();
#endif

  sendHeartbeat();
}

void setup() {
  Serial.begin(115200);
  watchdogInit();
  Serial.printf("[" NODE_ID "] band=%lu Hz\n",
                static_cast<unsigned long>(LORA_BAND));

  loadRelayState();
  relayInit();

#if WITH_OLED
  oledInit();
  oledRender();
#endif

  loraInit();
  Serial.printf("[" NODE_ID "] ready lora=%d relay1=%d relay2=%d tx_interval_s=%d\n",
                loraReady ? 1 : 0, relay1State, relay2State, TX_INTERVAL_S);
}

void loop() {
  esp_task_wdt_reset();
  handleLoRaPacket();

  static uint32_t lastTxMs = 0;
  uint32_t now = millis();
  if (now - lastTxMs >= static_cast<uint32_t>(TX_INTERVAL_S) * 1000UL) {
    lastTxMs = now;
    sendHeartbeat();
#if WITH_OLED
    oledRender();
#endif
  }
  delay(5);
}
