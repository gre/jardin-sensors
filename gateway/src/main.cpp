#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "auth.h"
#include "wdt.h"
#include "lora_board.h"

#if WITH_OLED
#include <Wire.h>
#include <U8g2lib.h>
#endif

#ifndef WIFI_SSID
#error "WIFI_SSID undefined: cp secrets.example.ini secrets.ini then fill in."
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD undefined: cp secrets.example.ini secrets.ini then fill in."
#endif
#ifndef MQTT_HOST
#error "MQTT_HOST undefined: cp secrets.example.ini secrets.ini then fill in."
#endif
#ifndef LORA_PSK
#error "LORA_PSK undefined: cp secrets.example.ini secrets.ini then fill in."
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID "jardin-gateway"
#endif
#ifndef MQTT_BASE_TOPIC
#define MQTT_BASE_TOPIC "jardin"
#endif
#ifndef MQTT_RECONNECT_BACKOFF_MS
#define MQTT_RECONNECT_BACKOFF_MS 5000
#endif
#ifndef HA_DISCOVERY_PREFIX
#define HA_DISCOVERY_PREFIX "homeassistant"
#endif
#ifndef HA_DEVICE_NAME
#define HA_DEVICE_NAME "Cuve"
#endif
#ifndef HA_ACTUATOR_DEVICE_NAME
#define HA_ACTUATOR_DEVICE_NAME "Prises"
#endif
#ifndef HA_DEVICE_MODEL
// Emitter model (the HA device), not the gateway.
#define HA_DEVICE_MODEL "TTGO LoRa32"
#endif
#ifndef HA_DEVICE_MANUFACTURER
#define HA_DEVICE_MANUFACTURER "DIY"
#endif
#ifndef HA_MAX_NODES
#define HA_MAX_NODES 4
#endif
#ifndef NODE_TIMEOUT_MS
#define NODE_TIMEOUT_MS 180000UL
#endif
#ifndef WIFI_DOWN_REBOOT_MS
#define WIFI_DOWN_REBOOT_MS 300000UL
#endif
#ifndef MQTT_DOWN_REBOOT_MS
#define MQTT_DOWN_REBOOT_MS 300000UL
#endif
// Covers the case where the actuator's heartbeat is missed because the gateway
// was still in TX when the actuator responded.
#ifndef RELAY_CMD_RETRY_MS
#define RELAY_CMD_RETRY_MS 8000UL
#endif
// Debounce for "null = full tank": an isolated null in a valid stream is
// replaced by the last known value while it is recent. A null sustained
// longer truly switches to 100%.
#ifndef TANK_NULL_GRACE_MS
#define TANK_NULL_GRACE_MS 3000UL
#endif

// Gateway-side calibration. Defaults at boot, overridden at runtime by HA
// via MQTT (topics jardin/config/tank_*_cm) with range 0..TANK_DISTANCE_MAX_CM.
#ifndef TANK_EMPTY_DISTANCE_CM
#define TANK_EMPTY_DISTANCE_CM 80
#endif
#ifndef TANK_FULL_DISTANCE_CM
#define TANK_FULL_DISTANCE_CM 5
#endif
#ifndef TANK_DISTANCE_MAX_CM
#define TANK_DISTANCE_MAX_CM 200
#endif

static_assert(TANK_EMPTY_DISTANCE_CM > TANK_FULL_DISTANCE_CM,
              "Default TANK_EMPTY_DISTANCE_CM must be > TANK_FULL_DISTANCE_CM.");

// Bounds enforced when an emitter receives this value. Must match
// cuve-emitter's TX_INTERVAL_S_{MIN,MAX}.
#ifndef CUVE_TX_INTERVAL_S_DEFAULT
#define CUVE_TX_INTERVAL_S_DEFAULT 60
#endif
#ifndef CUVE_TX_INTERVAL_S_MIN
#define CUVE_TX_INTERVAL_S_MIN 5
#endif
#ifndef CUVE_TX_INTERVAL_S_MAX
#define CUVE_TX_INTERVAL_S_MAX 3600
#endif

// Persist anti-replay state per node every N packets, to bound flash wear.
// Worst case after a gateway crash: an attacker can replay up to (this many)
// past packets within their freshness window. 25 @ 60-s cadence = 25 min.
#ifndef SEQ_PERSIST_EVERY
#define SEQ_PERSIST_EVERY 25
#endif

static int g_tankEmptyCm     = TANK_EMPTY_DISTANCE_CM;
static int g_tankFullCm      = TANK_FULL_DISTANCE_CM;
static int g_cuveTxIntervalS = CUVE_TX_INTERVAL_S_DEFAULT;

#ifndef ACTUATOR_NODE_ID
#define ACTUATOR_NODE_ID "prises"
#endif

// Desired relay state for the relay actuator. -1 = not yet commanded by HA
// (avoids overriding the actuator's NVS state on gateway reboot).
static int g_relay1Desired = -1;
static int g_relay2Desired = -1;
static bool g_relayCommandPending = false;
static uint32_t g_lastRelayTxMs = 0;
static int g_relay1Actual = -1;  // -1 = not yet heard from the actuator
static int g_relay2Actual = -1;
static int g_relay1Target = -1;  // last HA-commanded state, NVS-persisted
static int g_relay2Target = -1;

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

#if WITH_OLED
// Constructor with no reset pin (touching GPIO 16 on this T3 V1.6.1 variant
// causes a chip reset). The hardware RC at power-on is enough.
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
#ifndef OLED_I2C_ADDR
#define OLED_I2C_ADDR 0x3C
#endif
#ifndef OLED_REFRESH_MS
#define OLED_REFRESH_MS 500
#endif
static bool oledPresent = false;

// Snapshot of the last received packet, to refresh the OLED.
static String lastNodeName;
static int lastTankPct = -1;
static float lastTankCm = NAN;
static float lastWaterTempC = NAN;
static int lastRssi = 0;
static float lastSnr = 0.0f;
static uint32_t lastRxMs = 0;
#endif

struct HaSensor {
  const char* key;
  const char* name;
  const char* deviceClass;
  const char* unit;
  const char* stateClass;
  const char* entityCategory;
};

// Order: primary then diagnostic, for readability on the HA side.
// nullptr = optional field omitted from the discovery payload.
static const HaSensor HA_SENSORS[] = {
  {"tank_pct",     "Tank fill",         nullptr,           "%",   "measurement", nullptr},
  {"water_temp_c", "Water temperature", "temperature",     "°C",  "measurement", nullptr},
  {"tank_cm",      "Tank distance",     "distance",        "cm",  "measurement", "diagnostic"},
  {"vbat",         "Battery voltage",   "voltage",         "V",   "measurement", "diagnostic"},
  {"rssi",         "LoRa RSSI",         "signal_strength", "dBm", "measurement", "diagnostic"},
  {"snr",          "LoRa SNR",          nullptr,           "dB",  "measurement", "diagnostic"},
};
constexpr size_t HA_SENSORS_N = sizeof(HA_SENSORS) / sizeof(HA_SENSORS[0]);

// Relay actuator: switches (primary) + diagnostic sensors.
struct HaSwitch {
  const char* key;
  const char* name;
};
static const HaSwitch ACTUATOR_SWITCHES[] = {
  {"relay1", "Prise 1"},
  {"relay2", "Prise 2"},
};
constexpr size_t ACTUATOR_SWITCHES_N = sizeof(ACTUATOR_SWITCHES) / sizeof(ACTUATOR_SWITCHES[0]);

static const HaSensor ACTUATOR_SENSORS[] = {
  {"vbat", "Battery voltage", "voltage",         "V",   "measurement", "diagnostic"},
  {"rssi", "LoRa RSSI",       "signal_strength", "dBm", "measurement", "diagnostic"},
  {"snr",  "LoRa SNR",        nullptr,           "dB",  "measurement", "diagnostic"},
};
constexpr size_t ACTUATOR_SENSORS_N = sizeof(ACTUATOR_SENSORS) / sizeof(ACTUATOR_SENSORS[0]);

struct NodeState {
  String name;
  uint32_t lastSeenMs;
  bool online;
  float lastValidTankCm;     // NAN until we have ever seen a valid measurement
  uint32_t lastValidTankCmMs;
  bool seenSeq;
  uint32_t lastSeq;
  uint32_t persistedSeq;     // last seq written to NVS (for dedup)
};

static NodeState nodes[HA_MAX_NODES];
static uint8_t nodesCount = 0;
static Preferences seqPrefs;

static NodeState* findNode(const char* name) {
  for (uint8_t i = 0; i < nodesCount; ++i) {
    if (nodes[i].name == name) return &nodes[i];
  }
  return nullptr;
}

// NVS keys are limited to 15 chars. Truncate the node name and prefix it; the
// hash distinguishes longer names that share a prefix.
static void seqKeyForNode(const char* name, char* out, size_t outLen) {
  uint32_t h = 2166136261u;
  for (const char* p = name; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
  snprintf(out, outLen, "s_%.6s_%04x", name, static_cast<unsigned>(h & 0xFFFF));
}

static NodeState* registerNode(const char* name) {
  if (nodesCount >= HA_MAX_NODES) {
    Serial.printf("[gateway] node table full (max=%d), cannot register %s\n",
                  HA_MAX_NODES, name);
    return nullptr;
  }
  NodeState& n = nodes[nodesCount++];
  n.name = name;
  n.lastSeenMs = millis();
  n.online = false;
  n.lastValidTankCm = NAN;
  n.lastValidTankCmMs = 0;

  char key[16];
  seqKeyForNode(name, key, sizeof(key));
  seqPrefs.begin("gw-seq", true);
  uint32_t saved = seqPrefs.getUInt(key, 0);
  seqPrefs.end();
  n.lastSeq      = saved;
  n.persistedSeq = saved;
  n.seenSeq      = (saved > 0);
  Serial.printf("[gateway] node=%s registered, seq restored from NVS=%u\n",
                name, static_cast<unsigned>(saved));
  return &n;
}

static void persistSeqIfDue(NodeState& st) {
  if (st.lastSeq < st.persistedSeq + SEQ_PERSIST_EVERY) return;
  char key[16];
  seqKeyForNode(st.name.c_str(), key, sizeof(key));
  seqPrefs.begin("gw-seq", false);
  seqPrefs.putUInt(key, st.lastSeq);
  seqPrefs.end();
  st.persistedSeq = st.lastSeq;
}

static void buildStateTopic(const char* node, char* out, size_t outLen) {
  snprintf(out, outLen, "%s/%s/state", MQTT_BASE_TOPIC, node);
}

static void buildAvailabilityTopic(const char* node, char* out, size_t outLen) {
  snprintf(out, outLen, "%s/%s/availability", MQTT_BASE_TOPIC, node);
}

static void buildConfigTopic(const char* key, char* out, size_t outLen) {
  snprintf(out, outLen, "%s/config/%s", MQTT_BASE_TOPIC, key);
}

static void buildCommandTopic(const char* node, const char* key, char* out, size_t outLen) {
  snprintf(out, outLen, "%s/%s/%s/set", MQTT_BASE_TOPIC, node, key);
}

struct GwConfig {
  const char* key;
  const char* name;
  const char* unit;
  int min;
  int max;
  int* target;
};

static const GwConfig GW_CONFIG[] = {
  {"tank_empty_cm",      "Tank empty distance", "cm", 0, TANK_DISTANCE_MAX_CM, &g_tankEmptyCm},
  {"tank_full_cm",       "Tank full distance",  "cm", 0, TANK_DISTANCE_MAX_CM, &g_tankFullCm},
  {"cuve_tx_interval_s", "Cuve TX interval",    "s",
   CUVE_TX_INTERVAL_S_MIN, CUVE_TX_INTERVAL_S_MAX, &g_cuveTxIntervalS},
};
constexpr size_t GW_CONFIG_N = sizeof(GW_CONFIG) / sizeof(GW_CONFIG[0]);

// Send relay state to the relay actuator over LoRa. Pass -1 for a relay to
// omit it from the JSON; the actuator keeps the existing state for absent fields.
static void sendRelayCommand(const char* node, int relay1, int relay2) {
  JsonDocument doc;
  doc["to"] = node;
  if (relay1 >= 0) doc["relay1"] = relay1;
  if (relay2 >= 0) doc["relay2"] = relay2;

  char buf[128];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  n = authAppendMac(buf, n, sizeof(buf),
                    reinterpret_cast<const uint8_t*>(LORA_PSK),
                    strlen(LORA_PSK));

  bool txOk = (loraTx(reinterpret_cast<const uint8_t*>(buf), n) == RADIOLIB_ERR_NONE);
  Serial.printf("[gateway] relay cmd to=%s relay1=%d relay2=%d bytes=%u ok=%d\n",
                node, relay1, relay2, static_cast<unsigned>(n), txOk ? 1 : 0);
}

static bool publishSensorDiscoveries(const char* node, const char* deviceName,
                                     const HaSensor* sensors, size_t count) {
  char stateTopic[96];
  buildStateTopic(node, stateTopic, sizeof(stateTopic));
  char availTopic[96];
  buildAvailabilityTopic(node, availTopic, sizeof(availTopic));
  char nodeId[64];
  snprintf(nodeId, sizeof(nodeId), "%s-%s", MQTT_BASE_TOPIC, node);

  bool allOk = true;
  for (size_t i = 0; i < count; ++i) {
    const HaSensor& s = sensors[i];
    JsonDocument doc;
    doc["name"] = s.name;
    char unique[80];
    snprintf(unique, sizeof(unique), "%s-%s", nodeId, s.key);
    doc["unique_id"]   = unique;
    doc["state_topic"] = stateTopic;
    char valueTpl[48];
    snprintf(valueTpl, sizeof(valueTpl), "{{ value_json.%s }}", s.key);
    doc["value_template"] = valueTpl;
    if (s.deviceClass)    doc["device_class"]        = s.deviceClass;
    if (s.unit)           doc["unit_of_measurement"] = s.unit;
    if (s.stateClass)     doc["state_class"]         = s.stateClass;
    if (s.entityCategory) doc["entity_category"]     = s.entityCategory;
    doc["availability_topic"]    = availTopic;
    doc["payload_available"]     = "online";
    doc["payload_not_available"] = "offline";
    doc["expire_after"] = (NODE_TIMEOUT_MS / 1000UL) + 30UL;

    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"].add(nodeId);
    device["name"]         = deviceName;
    device["model"]        = HA_DEVICE_MODEL;
    device["manufacturer"] = HA_DEVICE_MANUFACTURER;

    char topic[160];
    snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config",
             HA_DISCOVERY_PREFIX, nodeId, s.key);

    char buf[512];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    bool ok = mqtt.publish(topic, reinterpret_cast<const uint8_t*>(buf), n, true);
    if (!ok) allOk = false;
    Serial.printf("[gateway] HA sensor %s len=%u ok=%d\n",
                  topic, static_cast<unsigned>(n), ok);
  }
  return allOk;
}

static bool publishActuatorDiscovery(const char* node) {
  char stateTopic[96];
  buildStateTopic(node, stateTopic, sizeof(stateTopic));
  char availTopic[96];
  buildAvailabilityTopic(node, availTopic, sizeof(availTopic));
  char nodeId[64];
  snprintf(nodeId, sizeof(nodeId), "%s-%s", MQTT_BASE_TOPIC, node);

  bool allOk = true;
  for (size_t i = 0; i < ACTUATOR_SWITCHES_N; ++i) {
    const HaSwitch& sw = ACTUATOR_SWITCHES[i];
    JsonDocument doc;
    doc["name"] = sw.name;
    char unique[80];
    snprintf(unique, sizeof(unique), "%s-%s", nodeId, sw.key);
    doc["unique_id"]   = unique;
    doc["state_topic"] = stateTopic;
    char valueTpl[48];
    snprintf(valueTpl, sizeof(valueTpl), "{{ value_json.%s }}", sw.key);
    doc["value_template"] = valueTpl;
    doc["state_on"]    = "1";
    doc["state_off"]   = "0";
    doc["payload_on"]  = "1";
    doc["payload_off"] = "0";
    char cmdTopic[96];
    buildCommandTopic(node, sw.key, cmdTopic, sizeof(cmdTopic));
    doc["command_topic"]         = cmdTopic;
    doc["availability_topic"]    = availTopic;
    doc["payload_available"]     = "online";
    doc["payload_not_available"] = "offline";
    doc["expire_after"] = (NODE_TIMEOUT_MS / 1000UL) + 30UL;

    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"].add(nodeId);
    device["name"]         = HA_ACTUATOR_DEVICE_NAME;
    device["model"]        = HA_DEVICE_MODEL;
    device["manufacturer"] = HA_DEVICE_MANUFACTURER;

    char topic[160];
    snprintf(topic, sizeof(topic), "%s/switch/%s/config",
             HA_DISCOVERY_PREFIX, unique);

    char buf[600];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    bool ok = mqtt.publish(topic, reinterpret_cast<const uint8_t*>(buf), n, true);
    if (!ok) allOk = false;
    Serial.printf("[gateway] HA actuator switch %s len=%u ok=%d\n",
                  topic, static_cast<unsigned>(n), ok);
  }
  allOk &= publishSensorDiscoveries(node, HA_ACTUATOR_DEVICE_NAME, ACTUATOR_SENSORS, ACTUATOR_SENSORS_N);
  return allOk;
}

static void publishRelayOptimistic() {
  if (!mqtt.connected()) return;
  int r1 = g_relay1Desired >= 0 ? g_relay1Desired : g_relay1Actual;
  int r2 = g_relay2Desired >= 0 ? g_relay2Desired : g_relay2Actual;
  if (r1 < 0 || r2 < 0) return;
  JsonDocument doc;
  doc["relay1"] = r1;
  doc["relay2"] = r2;
  char topic[96];
  buildStateTopic(ACTUATOR_NODE_ID, topic, sizeof(topic));
  char buf[64];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topic, reinterpret_cast<const uint8_t*>(buf), n, true);
  Serial.printf("[gateway] relay optimistic relay1=%d relay2=%d\n", r1, r2);
}

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char buf[16];
  if (len == 0 || len >= sizeof(buf)) return;
  memcpy(buf, payload, len);
  buf[len] = 0;

  for (size_t i = 0; i < GW_CONFIG_N; ++i) {
    const GwConfig& c = GW_CONFIG[i];
    char expected[64];
    buildConfigTopic(c.key, expected, sizeof(expected));
    if (strcmp(topic, expected) != 0) continue;
    int v = atoi(buf);
    if (v < c.min || v > c.max) {
      Serial.printf("[gateway] config %s out of range [%d..%d]: %s\n",
                    c.key, c.min, c.max, buf);
      return;
    }
    *c.target = v;
    Serial.printf("[gateway] config %s = %d\n", c.key, v);
    return;
  }

  static const struct { const char* topic; int* desired; const char* key; } k_relays[] = {
    {MQTT_BASE_TOPIC "/" ACTUATOR_NODE_ID "/relay1/set", &g_relay1Desired, "relay1"},
    {MQTT_BASE_TOPIC "/" ACTUATOR_NODE_ID "/relay2/set", &g_relay2Desired, "relay2"},
  };
  for (size_t i = 0; i < sizeof(k_relays) / sizeof(k_relays[0]); ++i) {
    if (strcmp(topic, k_relays[i].topic) != 0) continue;
    int v = constrain(atoi(buf), 0, 1);
    *k_relays[i].desired = v;
    int& target = (i == 0) ? g_relay1Target : g_relay2Target;
    target = v;
    Preferences p;
    p.begin("gw-relay", false);
    p.putInt(i == 0 ? "r1t" : "r2t", v);
    p.end();
    g_relayCommandPending = true;
    publishRelayOptimistic();
    Serial.printf("[gateway] relay %s desired=%d target=%d\n", k_relays[i].key, v, v);
    return;
  }
}

static bool publishAvailability(const char* node, bool online) {
  if (!mqtt.connected()) return false;
  char topic[96];
  buildAvailabilityTopic(node, topic, sizeof(topic));
  const char* payload = online ? "online" : "offline";
  bool ok = mqtt.publish(topic, payload, true);
  Serial.printf("[gateway] availability %s -> %s ok=%d\n", topic, payload, ok);
  return ok;
}

static void ledUpdateError() {
  bool anyOffline = false;
  for (uint8_t i = 0; i < nodesCount; ++i) {
    if (!nodes[i].online) { anyOffline = true; break; }
  }
  digitalWrite(LED_PIN, (nodesCount > 0 && anyOffline) ? HIGH : LOW);
}

static void checkNodeTimeouts() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < nodesCount; ++i) {
    NodeState& n = nodes[i];
    if (!n.online) continue;
    if ((now - n.lastSeenMs) <= NODE_TIMEOUT_MS) continue;
    if (publishAvailability(n.name.c_str(), false)) {
      n.online = false;
      ledUpdateError();
    }
  }
}

static void loraInit() {
  int16_t s = loraBegin();
  if (s != RADIOLIB_ERR_NONE) {
    Serial.printf("[gateway] LoRa init failed: %d\n", s);
    digitalWrite(LED_PIN, HIGH);
    while (true) delay(1000);
  }
  loraRadio.startReceive();
}

static void wifiInit() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[gateway] WiFi connecting to %s\n", WIFI_SSID);
}

#if WITH_OLED
static void oledInit() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.beginTransmission(OLED_I2C_ADDR);
  oledPresent = (Wire.endTransmission() == 0);
  if (!oledPresent) {
    Serial.printf("[gateway] OLED absent at 0x%02X, skipping\n", OLED_I2C_ADDR);
    return;
  }
  oled.begin();
  oled.setContrast(255);
  Serial.println("[gateway] OLED ready");
}

static void oledRender() {
  if (!oledPresent) return;
  uint32_t now = millis();
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  bool mqttUp = mqtt.connected();

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);

  // Header: Gateway + WiFi/MQTT status
  oled.drawStr(0, 9, "Gateway");
  char hdr[16];
  snprintf(hdr, sizeof(hdr), "W:%s M:%s",
           wifiUp ? "OK" : "--",
           mqttUp ? "OK" : "--");
  oled.drawStr(60, 9, hdr);
  oled.drawHLine(0, 12, 128);

  // Big tank percentage (or "----" if no data)
  oled.setFont(u8g2_font_logisoso24_tn);
  char num[12];
  if (lastTankPct < 0) {
    oled.drawStr(8, 44, "----");
  } else {
    snprintf(num, sizeof(num), "%d", lastTankPct);
    oled.drawStr(8, 44, num);
  }
  oled.setFont(u8g2_font_6x10_tf);
  if (lastTankPct >= 0) oled.drawStr(72, 44, "%");

  // Small temperature on the right if available
  if (!isnan(lastWaterTempC)) {
    char tmp[12];
    snprintf(tmp, sizeof(tmp), "%.1fC", lastWaterTempC);
    oled.drawStr(86, 36, tmp);
  }

  // Footer: node name + age + RSSI
  oled.drawHLine(0, 50, 128);
  char foot[32];
  if (lastRxMs == 0) {
    snprintf(foot, sizeof(foot), "no RX yet");
  } else {
    uint32_t ageS = (now - lastRxMs) / 1000;
    snprintf(foot, sizeof(foot), "%s %lus %ddBm",
             lastNodeName.length() ? lastNodeName.c_str() : "?",
             static_cast<unsigned long>(ageS),
             lastRssi);
  }
  oled.drawStr(0, 62, foot);

  oled.sendBuffer();
}

static void oledRecordRx(const JsonDocument& doc, int rssi, float snr) {
  const char* node = doc["node"] | "";
  lastNodeName = node;
  lastTankCm     = doc["tank_cm"].isNull()      ? NAN : doc["tank_cm"].as<float>();
  lastWaterTempC = doc["water_temp_c"].isNull() ? NAN : doc["water_temp_c"].as<float>();
  lastTankPct    = doc["tank_pct"].isNull()     ? -1  : doc["tank_pct"].as<int>();
  lastRssi = rssi;
  lastSnr  = snr;
  lastRxMs = millis();
}
#endif // WITH_OLED

static void wifiPoll() {
  static wl_status_t last = WL_NO_SHIELD;
  wl_status_t now = WiFi.status();
  if (now == last) return;
  if (now == WL_CONNECTED) {
    Serial.printf("[gateway] WiFi up: ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  } else {
    Serial.printf("[gateway] WiFi status=%d\n", static_cast<int>(now));
  }
  last = now;
}

static void publishConfigDiscovery() {
  for (size_t i = 0; i < GW_CONFIG_N; ++i) {
    const GwConfig& c = GW_CONFIG[i];

    char stateTopic[64];
    buildConfigTopic(c.key, stateTopic, sizeof(stateTopic));

    JsonDocument doc;
    doc["name"]                = c.name;
    char unique[80];
    snprintf(unique, sizeof(unique), "%s-gateway-%s", MQTT_BASE_TOPIC, c.key);
    doc["unique_id"]           = unique;
    doc["state_topic"]         = stateTopic;
    doc["command_topic"]       = stateTopic;
    doc["min"]                 = c.min;
    doc["max"]                 = c.max;
    doc["step"]                = 1;
    doc["unit_of_measurement"] = c.unit;
    doc["mode"]                = "box";
    doc["retain"]              = true;
    doc["entity_category"]     = "config";

    JsonObject device = doc["device"].to<JsonObject>();
    char devId[64];
    snprintf(devId, sizeof(devId), "%s-gateway", MQTT_BASE_TOPIC);
    device["identifiers"].add(devId);
    device["name"]         = "Jardin Gateway";
    device["model"]        = "LILYGO LoRa32 T3 V1.6.1";
    device["manufacturer"] = HA_DEVICE_MANUFACTURER;

    char topic[160];
    snprintf(topic, sizeof(topic), "%s/number/%s/config",
             HA_DISCOVERY_PREFIX, unique);

    char buf[512];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    bool ok = mqtt.publish(topic, reinterpret_cast<const uint8_t*>(buf), n, true);
    Serial.printf("[gateway] HA cfg %s len=%u ok=%d\n",
                  topic, static_cast<unsigned>(n), ok);
  }
}

static void loadRelayTarget() {
  Preferences p;
  p.begin("gw-relay", true);
  int v1 = p.getInt("r1t", -1);
  int v2 = p.getInt("r2t", -1);
  p.end();
  if (v1 >= 0 && v1 <= 1) g_relay1Target = v1;
  if (v2 >= 0 && v2 <= 1) g_relay2Target = v2;
  Serial.printf("[gateway] relay target loaded: r1=%d r2=%d\n", g_relay1Target, g_relay2Target);
}

static void mqttInit() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  // Discovery payload + topic + headers exceed the default 256 bytes.
  // Switch discovery payload can reach ~550 bytes.
  mqtt.setBufferSize(1024);
}

static void mqttEnsureConnected() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  static uint32_t nextAttempt = 0;
  if (millis() < nextAttempt) return;

  char gwAvail[96];
  snprintf(gwAvail, sizeof(gwAvail), "%s/gateway/availability", MQTT_BASE_TOPIC);

  // PubSubClient distinguishes NULL (no auth) from "" (auth with empty user).
  const char* user = strlen(MQTT_USER) > 0 ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_USER) > 0 ? MQTT_PASSWORD : nullptr;

  bool ok = mqtt.connect(MQTT_CLIENT_ID, user, pass,
                         gwAvail, 0, true, "offline");
  if (ok) {
    mqtt.publish(gwAvail, "online", true);
    char sub[64];
    snprintf(sub, sizeof(sub), "%s/config/+", MQTT_BASE_TOPIC);
    mqtt.subscribe(sub);
    mqtt.subscribe(MQTT_BASE_TOPIC "/" ACTUATOR_NODE_ID "/+/set");
    publishConfigDiscovery();
    Serial.printf("[gateway] MQTT connected to %s:%d, subscribed %s\n",
                  MQTT_HOST, MQTT_PORT, sub);
  } else {
    Serial.printf("[gateway] MQTT connect failed rc=%d, retry in %lu ms\n",
                  mqtt.state(),
                  static_cast<unsigned long>(MQTT_RECONNECT_BACKOFF_MS));
    nextAttempt = millis() + MQTT_RECONNECT_BACKOFF_MS;
  }
}

static bool publishDiscovery(const char* node) {
  return publishSensorDiscoveries(node, HA_DEVICE_NAME, HA_SENSORS, HA_SENSORS_N);
}

static void augmentDerived(JsonDocument& doc, NodeState& st) {
  uint32_t now = millis();
  if (!doc["tank_cm"].isNull()) {
    st.lastValidTankCm = doc["tank_cm"].as<float>();
    st.lastValidTankCmMs = now;
  } else if (!isnan(st.lastValidTankCm) &&
             (now - st.lastValidTankCmMs) < TANK_NULL_GRACE_MS) {
    // Isolated null in a valid stream: substitute the last known measurement
    // to avoid flicker to 100%. If the null persists past the grace window,
    // we fall back to the "null = full tank" case below.
    doc["tank_cm"] = st.lastValidTankCm;
  }

  if (g_tankEmptyCm == g_tankFullCm) return;
  // tank_cm null = surface within the sensor dead zone (~25 cm) = tank
  // considered full. This is intentional: the sensor is mounted at the top,
  // a full tank puts the surface below the lowest measurable distance.
  int distCm = doc["tank_cm"].isNull() ? 0 : doc["tank_cm"].as<int>();
  long pct = map(distCm, g_tankEmptyCm, g_tankFullCm, 0, 100);
  doc["tank_pct"] = constrain(pct, 0L, 100L);
}

// Reply over LoRa to an emitter that asked for fresh config (cfg_req=1).
// The reply echoes the request's seq as `ack` so the emitter can reject any
// replayed older response. HMAC ensures authenticity (only the holder of the
// PSK can produce a valid MAC over this payload).
static void sendConfigTo(const char* node, uint32_t ackSeq) {
  JsonDocument doc;
  doc["to"]  = node;
  doc["ack"] = ackSeq;
  JsonObject cfg = doc["cfg"].to<JsonObject>();
  cfg["tx_interval_s"] = g_cuveTxIntervalS;

  char buf[200];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  n = authAppendMac(buf, n, sizeof(buf),
                    reinterpret_cast<const uint8_t*>(LORA_PSK),
                    strlen(LORA_PSK));

  bool txOk = (loraTx(reinterpret_cast<const uint8_t*>(buf), n) == RADIOLIB_ERR_NONE);

  Serial.printf("[gateway] cfg TX to=%s ack=%u tx_interval_s=%d bytes=%u tx_ok=%d\n",
                node, static_cast<unsigned>(ackSeq),
                g_cuveTxIntervalS, static_cast<unsigned>(n),
                txOk ? 1 : 0);
}

static void publishMeasurement(const char* json, size_t jsonLen, int rssi, float snr) {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, jsonLen);
  if (err) {
    Serial.printf("[gateway] JSON parse error: %s\n", err.c_str());
    return;
  }
  const char* node = doc["node"] | "unknown";

  bool isActuator = (strcmp(node, ACTUATOR_NODE_ID) == 0);

  NodeState* st = findNode(node);
  if (!st) {
    bool discovered = isActuator ? publishActuatorDiscovery(node) : publishDiscovery(node);
    if (!discovered) return;
    st = registerNode(node);
    if (!st) return;
  }

  // Anti-replay: reject seq <= last seen (unless very small, which suggests
  // an emitter reboot starting from 0).
  uint32_t incomingSeq = doc["seq"].as<uint32_t>();
  bool emitterRebooted = false;
  if (st->seenSeq) {
    bool forward = incomingSeq > st->lastSeq;
    bool likelyReboot = incomingSeq < 100 && st->lastSeq > incomingSeq;
    if (!forward && !likelyReboot) {
      Serial.printf("[gateway] replay/reorder drop node=%s seq=%u (last=%u)\n",
                    node, static_cast<unsigned>(incomingSeq),
                    static_cast<unsigned>(st->lastSeq));
      return;
    }
    emitterRebooted = likelyReboot && !forward;
  }
  st->lastSeq = incomingSeq;
  st->seenSeq = true;
  st->lastSeenMs = millis();
  if (emitterRebooted) {
    // Emitter just power-cycled: persist immediately so the new low seq
    // survives a gateway reboot before SEQ_PERSIST_EVERY accumulates.
    st->persistedSeq = 0;
  }
  persistSeqIfDue(*st);
  if (!st->online && publishAvailability(node, true)) {
    st->online = true;
    ledUpdateError();
  }

  // cfg_req: emitter wants the latest config back. Reply over LoRa first
  // (before MQTT publish, which can block on Wi-Fi) so the response lands
  // inside the emitter's RX window (~2 s after its TX).
  if (doc["cfg_req"].as<bool>()) {
    sendConfigTo(node, incomingSeq);
  }

  doc["rssi"] = rssi;
  doc["snr"]  = snr;

  if (!isActuator) {
    augmentDerived(doc, *st);
  } else {
    int actual1 = doc["relay1"] | -1;
    int actual2 = doc["relay2"] | -1;
    if (actual1 >= 0) g_relay1Actual = actual1;
    if (actual2 >= 0) g_relay2Actual = actual2;
    bool stale = false;
    if (g_relay1Desired >= 0 && actual1 >= 0) {
      if (actual1 != g_relay1Desired) { g_relayCommandPending = true; stale = true; }
      else g_relay1Desired = -1;
    }
    if (g_relay2Desired >= 0 && actual2 >= 0) {
      if (actual2 != g_relay2Desired) { g_relayCommandPending = true; stale = true; }
      else g_relay2Desired = -1;
    }
    // Overlay desired before publishing so HA doesn't flip back while in-flight.
    if (stale) {
      if (g_relay1Desired >= 0) doc["relay1"] = g_relay1Desired;
      if (g_relay2Desired >= 0) doc["relay2"] = g_relay2Desired;
    }
  }

  char topic[96];
  buildStateTopic(node, topic, sizeof(topic));

  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  // retain=true: HA receives the last known value on (re)connection.
  bool ok = mqtt.publish(topic, reinterpret_cast<const uint8_t*>(buf), n, true);
  Serial.printf("[gateway] MQTT %s len=%u ok=%d\n",
                topic, static_cast<unsigned>(n), ok);

#if WITH_OLED
  oledRecordRx(doc, rssi, snr);
#endif
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {}

  watchdogInit();
  loadRelayTarget();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  Serial.printf("[gateway] band=%lu Hz\n",
                static_cast<unsigned long>(LORA_BAND));
#if WITH_OLED
  oledInit();
#endif
  loraInit();
  wifiInit();
  mqttInit();
#if WITH_OLED
  oledRender();
#endif
  Serial.println("[gateway] ready, listening");
}

static void softWatchdog() {
  static uint32_t wifiDownSinceMs = 0;
  static uint32_t mqttDownSinceMs = 0;
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDownSinceMs == 0) wifiDownSinceMs = now;
    if (now - wifiDownSinceMs > WIFI_DOWN_REBOOT_MS) {
      Serial.println("[gateway] WiFi down too long, restarting");
      ESP.restart();
    }
  } else {
    wifiDownSinceMs = 0;
  }

  // Only count MQTT-down if WiFi is up: otherwise the WiFi watchdog already
  // covers it (MQTT cannot connect without WiFi).
  if (!mqtt.connected() && WiFi.status() == WL_CONNECTED) {
    if (mqttDownSinceMs == 0) mqttDownSinceMs = now;
    if (now - mqttDownSinceMs > MQTT_DOWN_REBOOT_MS) {
      Serial.println("[gateway] MQTT down too long, restarting");
      ESP.restart();
    }
  } else if (mqtt.connected()) {
    mqttDownSinceMs = 0;
  }
}

void loop() {
  watchdogFeed();
  wifiPoll();
  mqttEnsureConnected();
  mqtt.loop();
  softWatchdog();
  checkNodeTimeouts();

#if WITH_OLED
  static uint32_t lastOledMs = 0;
  uint32_t now = millis();
  if (now - lastOledMs >= OLED_REFRESH_MS) {
    lastOledMs = now;
    oledRender();
  }
#endif

  bool relayRetry = !g_relayCommandPending &&
                    (g_relay1Desired >= 0 || g_relay2Desired >= 0) &&
                    (millis() - g_lastRelayTxMs) >= RELAY_CMD_RETRY_MS;
  if (g_relayCommandPending || relayRetry) {
    g_relayCommandPending = false;
    if (relayRetry) Serial.printf("[gateway] relay retry relay1=%d relay2=%d\n",
                                  g_relay1Desired, g_relay2Desired);
    g_lastRelayTxMs = millis();
    sendRelayCommand(ACTUATOR_NODE_ID, g_relay1Desired, g_relay2Desired);
  }

  if (!loraRxFlag) return;
  loraRxFlag = false;

  int rssi  = static_cast<int>(loraRadio.getRSSI());
  float snr = loraRadio.getSNR();

  char rxBuf[200];
  size_t pktLen = loraReadPacket(rxBuf, sizeof(rxBuf));
  if (pktLen == 0) return;

  int jsonLen = authVerifyMac(rxBuf, static_cast<int>(pktLen),
                              reinterpret_cast<const uint8_t*>(LORA_PSK),
                              strlen(LORA_PSK));
  if (jsonLen < 0) {
    Serial.printf("[gateway] HMAC invalid, drop %u bytes rssi=%d\n",
                  static_cast<unsigned>(pktLen), rssi);
    return;
  }
  Serial.printf("[gateway] RX rssi=%d snr=%.2f bytes=%u json=%.*s\n",
                rssi, snr, static_cast<unsigned>(pktLen), jsonLen, rxBuf);

  publishMeasurement(rxBuf, static_cast<size_t>(jsonLen), rssi, snr);
  digitalWrite(LED_PIN, HIGH);
  delay(50);
  ledUpdateError();
}
