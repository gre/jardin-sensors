#include <Arduino.h>
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

#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 1
#endif
#ifndef RELAY1_ACTIVE_LOW
#define RELAY1_ACTIVE_LOW RELAY_ACTIVE_LOW
#endif
#ifndef RELAY2_ACTIVE_LOW
#define RELAY2_ACTIVE_LOW RELAY_ACTIVE_LOW
#endif

static Preferences prefs;
static bool loraReady = false;
static uint32_t txSeq = 0;
static uint32_t lastTxMs = 0;
static int relay1State = 0;
static int relay2State = 0;
static bool g_bootRestoreSent = false;
static uint32_t g_lastCmdSeq = 0;
static uint32_t g_ledOffMs = 0;
static uint32_t g_loraDownMs = 0;
static uint32_t g_loraRetryMs = 0;
static char g_powerOnBehavior[12] = "previous";

#if WITH_OLED
#ifdef OLED_PORTRAIT
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R1, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
#else
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
#endif
#ifndef OLED_I2C_ADDR
#define OLED_I2C_ADDR 0x3C
#endif
static bool oledPresent = false;
static uint32_t lastCmdMs = 0;
#endif

static inline int relayLevel(int state, bool activeLow) {
  return activeLow ? (state ? LOW : HIGH) : (state ? HIGH : LOW);
}

static void relayApply() {
  digitalWrite(RELAY1_PIN, relayLevel(relay1State, RELAY1_ACTIVE_LOW));
  digitalWrite(RELAY2_PIN, relayLevel(relay2State, RELAY2_ACTIVE_LOW));
}

static void relayInit() {
  // Pre-set output latch to safe level before enabling the driver.
  // The latch is independent of direction; when pinMode enables the driver
  // it comes up at the pre-written level with no glitch.
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
  g_lastCmdSeq = prefs.getUInt("cs", 0);
  size_t n = prefs.getString("pob", g_powerOnBehavior, sizeof(g_powerOnBehavior));
  prefs.end();
  if (n == 0 ||
      (strcmp(g_powerOnBehavior, "previous") != 0 &&
       strcmp(g_powerOnBehavior, "on") != 0 &&
       strcmp(g_powerOnBehavior, "off") != 0 &&
       strcmp(g_powerOnBehavior, "toggle") != 0)) {
    strcpy(g_powerOnBehavior, "previous");
  }

  int r1 = relay1State, r2 = relay2State;
  if (strcmp(g_powerOnBehavior, "on") == 0)       { r1 = 1; r2 = 1; }
  else if (strcmp(g_powerOnBehavior, "off") == 0)  { r1 = 0; r2 = 0; }
  else if (strcmp(g_powerOnBehavior, "toggle") == 0) { r1 ^= 1; r2 ^= 1; }

  if (r1 != relay1State || r2 != relay2State) {
    relay1State = r1;
    relay2State = r2;
    prefs.begin(NODE_ID, false);
    prefs.putInt("r1", relay1State);
    prefs.putInt("r2", relay2State);
    prefs.end();
  }

  Serial.printf("[" NODE_ID "] relay state loaded: relay1=%d relay2=%d lastCmdSeq=%lu pob=%s\n",
                relay1State, relay2State, (unsigned long)g_lastCmdSeq, g_powerOnBehavior);
}

static void loraInit() {
  g_loraRetryMs = millis();
  int16_t s = loraBegin();
  if (s != RADIOLIB_ERR_NONE) {
    Serial.printf("[" NODE_ID "] LoRa init failed: %d\n", s);
    return;
  }
  loraRadio.startReceive();
  loraReady = true;
  g_loraDownMs = 0;
}

#if WITH_OLED
static void oledInit() {
#ifdef OLED_VEXT_PIN
  // Heltec V3 / ARCELI: OLED panel power is gated by Vext (active-low).
  pinMode(OLED_VEXT_PIN, OUTPUT);
  digitalWrite(OLED_VEXT_PIN, LOW);
  delay(50);
#endif
#ifdef OLED_RST_PULSE_PIN
  // Heltec V3: no pull-up on OLED RST, display stays in reset until driven HIGH.
  pinMode(OLED_RST_PULSE_PIN, OUTPUT);
  digitalWrite(OLED_RST_PULSE_PIN, LOW);
  delay(50);
  digitalWrite(OLED_RST_PULSE_PIN, HIGH);
  delay(50);
#endif
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.beginTransmission(OLED_I2C_ADDR);
  oledPresent = (Wire.endTransmission() == 0);
  if (!oledPresent) {
    Serial.printf("[" NODE_ID "] OLED absent at 0x%02X\n", OLED_I2C_ADDR);
    return;
  }
  oled.begin();
  Wire.begin(OLED_SDA, OLED_SCL);
  oled.setContrast(255);
  Serial.println("[" NODE_ID "] OLED ready");
}

#ifdef OLED_PORTRAIT
static void oledRenderRelay(uint8_t labelY, uint8_t boxTopY, int n, int state) {
  char lbl[9];
  snprintf(lbl, sizeof(lbl), "Prise %d", n);
  uint8_t lw = oled.getStrWidth(lbl);
  oled.drawStr((64 - lw) / 2, labelY, lbl);
  // Box: 36px wide centered in 64px canvas (x=14), 20px tall
  if (state) {
    oled.drawBox(14, boxTopY, 36, 20);
    oled.setDrawColor(0);
    oled.drawStr(26, boxTopY + 14, "ON");   // "ON" centered in 36px
    oled.setDrawColor(1);
  } else {
    oled.drawFrame(14, boxTopY,     36, 20);  // outer border
    oled.drawFrame(15, boxTopY + 1, 34, 18);  // inner border → 2px thick
    oled.drawStr(23, boxTopY + 14, "OFF");    // "OFF" centered in 36px
  }
}
#else
static void oledRenderRelay(uint8_t baseline, int n, int state) {
  char lbl[9];
  snprintf(lbl, sizeof(lbl), "Relay %d", n);
  oled.drawStr(0, baseline, lbl);
  uint8_t bx = 100, by = baseline - 9;
  if (state) {
    oled.drawBox(bx, by, 26, 10);
    oled.setDrawColor(0);
    oled.drawStr(bx + 2, baseline, " ON");
    oled.setDrawColor(1);
  } else {
    oled.drawFrame(bx, by, 26, 10);
    oled.drawStr(bx + 2, baseline, "OFF");
  }
}
#endif

#ifdef OLED_PORTRAIT
static void oledRenderBootFrame(uint8_t arcs) {
  if (!oledPresent) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oledRenderRelay(16, 23, 1, relay1State);
  oledRenderRelay(56, 63, 2, relay2State);
  oled.drawDisc(32, 104, 2);
  if (arcs >= 1) oled.drawCircle(32, 104, 6,  U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  if (arcs >= 2) oled.drawCircle(32, 104, 10, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  if (arcs >= 3) oled.drawCircle(32, 104, 14, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  uint8_t bw = oled.getStrWidth("JARDIN");
  oled.drawStr((64 - bw) / 2, 120, "JARDIN");
  oled.sendBuffer();
}
#endif

static void oledRender() {
  if (!oledPresent) return;
  uint32_t now = millis();

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);

#ifdef OLED_PORTRAIT
  // Portrait canvas: 64px wide x 128px tall
  // Relays: label + wide box, centered
  // Relays: label y, then 7px gap, then box (20px tall). Canvas 64×128.
  // Prise 1: label y=16, box y=23..43. Prise 2: label y=56, box y=63..83.
  oledRenderRelay(16, 23, 1, relay1State);
  oledRenderRelay(56, 63, 2, relay2State);

  // Connection status (y=83..128 = 45px available, centered)
  char buf[18];
  uint8_t bw;
  if (loraReady) {
    // WiFi-style arcs: upper semicircles + center dot, origin (32, 104)
    oled.drawDisc(32, 104, 2);
    oled.drawCircle(32, 104, 6,  U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    oled.drawCircle(32, 104, 10, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    oled.drawCircle(32, 104, 14, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    bw = oled.getStrWidth("JARDIN");
    oled.drawStr((64 - bw) / 2, 120, "JARDIN");
  } else {
    // X cross (cx=32, cy=92, r=7) instead of arcs
    oled.drawLine(25, 85, 39, 99);
    oled.drawLine(39, 85, 25, 99);
    bw = oled.getStrWidth("JARDIN");
    oled.drawStr((64 - bw) / 2, 112, "JARDIN");
    uint32_t dnS = (now - g_loraDownMs) / 1000;
    if (dnS < 60) snprintf(buf, sizeof(buf), "dn %lus", (unsigned long)dnS);
    else          snprintf(buf, sizeof(buf), "dn %lum", (unsigned long)(dnS / 60));
    bw = oled.getStrWidth(buf);
    oled.drawStr((64 - bw) / 2, 124, buf);
  }
#else
  // Landscape canvas: 128px wide x 64px tall
  oled.drawStr(0, 9, "Prises");
  if (loraReady) {
    oled.drawStr(74, 9, "LoRa");
    oled.drawBox(104, 0, 9, 9);
  } else {
    oled.drawStr(74, 9, "LoRa ERR");
  }
  oled.drawHLine(0, 11, 128);

  oledRenderRelay(27, 1, relay1State);
  oledRenderRelay(41, 2, relay2State);

  oled.drawHLine(0, 51, 128);

  char foot[32];
  if (lastCmdMs == 0) {
    snprintf(foot, sizeof(foot), "#%lu  no cmd yet",
             static_cast<unsigned long>(txSeq));
  } else {
    uint32_t ageS = (now - lastCmdMs) / 1000;
    snprintf(foot, sizeof(foot), "#%lu  cmd %lus ago",
             static_cast<unsigned long>(txSeq),
             static_cast<unsigned long>(ageS));
  }
  oled.drawStr(0, 62, foot);
#endif

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
  if (!g_bootRestoreSent) {
    doc["restore_req"] = 1;
    g_bootRestoreSent = true;
  }

  char buf[200];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  n = authAppendMac(buf, n, sizeof(buf),
                    reinterpret_cast<const uint8_t*>(LORA_PSK),
                    strlen(LORA_PSK));

  bool txOk = false;
  if (loraReady) {
    txOk = (loraTx(reinterpret_cast<const uint8_t*>(buf), n) == RADIOLIB_ERR_NONE);
    if (!txOk) {
      loraReady = false;
      if (g_loraDownMs == 0) g_loraDownMs = millis();
    }
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

  if (doc["cs"].is<uint32_t>()) {
    uint32_t cs = doc["cs"].as<uint32_t>();
    if (cs <= g_lastCmdSeq) {
      Serial.printf("[" NODE_ID "] replay drop cs=%lu last=%lu\n",
                    (unsigned long)cs, (unsigned long)g_lastCmdSeq);
      return;
    }
    g_lastCmdSeq = cs;
    prefs.begin(NODE_ID, false);
    prefs.putUInt("cs", g_lastCmdSeq);
    prefs.end();
  }

  if (doc["power_on"].is<const char*>()) {
    const char* pob = doc["power_on"].as<const char*>();
    if ((strcmp(pob, "previous") == 0 || strcmp(pob, "on") == 0 ||
         strcmp(pob, "off") == 0 || strcmp(pob, "toggle") == 0) &&
        strcmp(pob, g_powerOnBehavior) != 0) {
      strncpy(g_powerOnBehavior, pob, sizeof(g_powerOnBehavior) - 1);
      g_powerOnBehavior[sizeof(g_powerOnBehavior) - 1] = 0;
      prefs.begin(NODE_ID, false);
      prefs.putString("pob", g_powerOnBehavior);
      prefs.end();
      Serial.printf("[" NODE_ID "] pob updated=%s\n", g_powerOnBehavior);
    }
  }

  int r1 = doc["relay1"].is<int>() ? doc["relay1"].as<int>() : relay1State;
  int r2 = doc["relay2"].is<int>() ? doc["relay2"].as<int>() : relay2State;
  setRelays(r1, r2);

#ifdef LED_PIN
  g_ledOffMs = millis() + 200;
  digitalWrite(LED_PIN, HIGH);
#endif

#if WITH_OLED
  lastCmdMs = millis();
  oledRender();
#endif

  sendHeartbeat();
  lastTxMs = millis();
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
#ifdef OLED_PORTRAIT
  for (uint8_t c = 0; c < 2; c++)
    for (uint8_t a = 0; a <= 3; a++) { oledRenderBootFrame(a); delay(200); }
#else
  oledRender();
#endif
#endif

  g_loraDownMs = millis();
  loraInit();

#if WITH_OLED
  oledRender();
#endif
#ifdef LED_PIN
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
#endif
  Serial.printf("[" NODE_ID "] ready lora=%d relay1=%d relay2=%d tx_interval_s=%d\n",
                loraReady ? 1 : 0, relay1State, relay2State, TX_INTERVAL_S);
  sendHeartbeat();
  lastTxMs = millis();
}

void loop() {
  watchdogFeed();
#ifdef LED_PIN
  if (g_ledOffMs && millis() >= g_ledOffMs) {
    g_ledOffMs = 0;
    digitalWrite(LED_PIN, LOW);
  }
#endif
  handleLoRaPacket();

  uint32_t now = millis();
  if (!loraReady && now - g_loraRetryMs >= 30000UL) {
    loraInit();
    if (loraReady) {
      g_bootRestoreSent = false;
      sendHeartbeat();
      lastTxMs = millis();
    }
#if WITH_OLED
    oledRender();
#endif
  }
  if (now - lastTxMs >= static_cast<uint32_t>(TX_INTERVAL_S) * 1000UL) {
    lastTxMs = now;
    sendHeartbeat();
#if WITH_OLED
    oledRender();
#endif
  }
  delay(5);
}
