#include <Arduino.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include "auth.h"
#include "wdt.h"
#include "lora_board.h"

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
#define RELAY1_PIN PB0
#endif
#ifndef RELAY2_PIN
#define RELAY2_PIN PB1
#endif

#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 1
#endif
#ifndef RELAY1_ACTIVE_LOW
#define RELAY1_ACTIVE_LOW RELAY_ACTIVE_LOW
#endif
#ifndef RELAY2_ACTIVE_LOW
#define RELAY2_ACTIVE_LOW RELAY_ACTIVE_LOW
#endif

// EEPROM layout: 1 byte per relay.
#define EEPROM_RELAY1_ADDR 0
#define EEPROM_RELAY2_ADDR 1

static bool loraReady = false;
static uint32_t txSeq = 0;
static int relay1State = 0;
static int relay2State = 0;
static uint32_t lastTxMs = 0;

static inline int relayLevel(int state, bool activeLow) {
  return activeLow ? (state ? LOW : HIGH) : (state ? HIGH : LOW);
}

static void relayApply() {
  digitalWrite(RELAY1_PIN, relayLevel(relay1State, RELAY1_ACTIVE_LOW));
  digitalWrite(RELAY2_PIN, relayLevel(relay2State, RELAY2_ACTIVE_LOW));
}

static void relayInit() {
  // Drive pins to safe level before enabling output drivers.
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
  EEPROM.write(EEPROM_RELAY1_ADDR, static_cast<uint8_t>(relay1State));
  EEPROM.write(EEPROM_RELAY2_ADDR, static_cast<uint8_t>(relay2State));
  Serial.printf("[" NODE_ID "] relay1=%d relay2=%d\n", relay1State, relay2State);
}

static void loadRelayState() {
  uint8_t v1 = EEPROM.read(EEPROM_RELAY1_ADDR);
  uint8_t v2 = EEPROM.read(EEPROM_RELAY2_ADDR);
  relay1State = (v1 <= 1) ? static_cast<int>(v1) : 0;
  relay2State = (v2 <= 1) ? static_cast<int>(v2) : 0;
  Serial.printf("[" NODE_ID "] relay state loaded: relay1=%d relay2=%d\n",
                relay1State, relay2State);
}

static void loraInit() {
  int16_t s = loraBegin();
  if (s != RADIOLIB_ERR_NONE) {
    Serial.printf("[" NODE_ID "] LoRa init failed: %d\n", s);
    return;
  }
  loraReady = true;
  loraRadio.startReceive();
  Serial.printf("[" NODE_ID "] loraReady=1 t=%lums\n", (unsigned long)millis());
}

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
    txOk = (loraTx(reinterpret_cast<const uint8_t*>(buf), n) == RADIOLIB_ERR_NONE);
    loraRxFlag = false;  // clear any flag latched during TX before re-entering RX
  }
  Serial.printf("[" NODE_ID "] TX seq=%lu relay1=%d relay2=%d bytes=%u ok=%d\n",
                static_cast<unsigned long>(txSeq - 1),
                relay1State, relay2State,
                static_cast<unsigned>(n), txOk ? 1 : 0);
}

static void handleLoRaPacket() {
  if (!loraRxFlag) return;
  loraRxFlag = false;

  char buf[200];
  size_t pktLen = loraReadPacket(buf, sizeof(buf));
  if (pktLen == 0) return;

  int jsonLen = authVerifyMac(buf, static_cast<int>(pktLen),
                              reinterpret_cast<const uint8_t*>(LORA_PSK),
                              strlen(LORA_PSK));
  if (jsonLen < 0) {
    Serial.printf("[" NODE_ID "] HMAC invalid, drop %u bytes\n",
                  static_cast<unsigned>(pktLen));
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, buf, jsonLen)) return;

  const char* to = doc["to"] | "";
  if (strcmp(to, NODE_ID) != 0) return;

  int r1 = doc["relay1"].is<int>() ? doc["relay1"].as<int>() : relay1State;
  int r2 = doc["relay2"].is<int>() ? doc["relay2"].as<int>() : relay2State;
  setRelays(r1, r2);

  sendHeartbeat();
  lastTxMs = millis();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  watchdogInit();
  Serial.printf("[" NODE_ID "] band=%lu Hz\n",
                static_cast<unsigned long>(LORA_BAND));

  loraDisableJtag();
  EEPROM.begin();
  loadRelayState();
  relayInit();
  loraInit();

  Serial.printf("[" NODE_ID "] ready lora=%d relay1=%d relay2=%d tx_interval_s=%d\n",
                loraReady ? 1 : 0, relay1State, relay2State, TX_INTERVAL_S);
  sendHeartbeat();
}

void loop() {
  watchdogFeed();
  handleLoRaPacket();

  uint32_t now = millis();

  if (now - lastTxMs >= static_cast<uint32_t>(TX_INTERVAL_S) * 1000UL) {
    lastTxMs = now;
    sendHeartbeat();
  }
  delay(5);
}
