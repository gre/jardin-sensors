#include <Arduino.h>
#include <NimBLEDevice.h>
#include "wdt.h"

#ifndef FLOWERCARE_MAC
#error "FLOWERCARE_MAC undefined: set -DFLOWERCARE_MAC=\"xx:xx:xx:xx:xx:xx\" in build_flags"
#endif

#if WITH_OLED
#include <Wire.h>
#include <U8g2lib.h>
#endif

#ifndef OLED_SDA
#define OLED_SDA 21
#endif
#ifndef OLED_SCL
#define OLED_SCL 22
#endif

#define POLL_INTERVAL_MS 10000UL

#define SVC_UUID  "00001204-0000-1000-8000-00805f9b34fb"
#define CMD_UUID  "00001a00-0000-1000-8000-00805f9b34fb"
#define DATA_UUID "00001a01-0000-1000-8000-00805f9b34fb"
#define BAT_UUID  "00001a02-0000-1000-8000-00805f9b34fb"

#if WITH_OLED
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
#endif

static NimBLEClient* bleClient = nullptr;
static float     lastTemp        = NAN;
static uint32_t  lastLight       = 0;
static uint8_t   lastMoisture    = 0;
static int16_t   lastConductivity = 0;
static uint8_t   lastBattery     = 0;
static bool      hasData         = false;
static unsigned long lastPollMs  = 0;

#if WITH_OLED
static void oledDraw() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    if (!hasData) {
        u8g2.drawStr(0, 10, "Flower Care");
        bool conn = bleClient && bleClient->isConnected();
        u8g2.drawStr(0, 26, conn ? "Connected..." : "Connecting...");
    } else {
        char buf[32];
        u8g2.drawStr(0, 10, "Flower Care");
        snprintf(buf, sizeof(buf), "T:%.1fC  H:%u%%", lastTemp, (unsigned)lastMoisture);
        u8g2.drawStr(0, 22, buf);
        snprintf(buf, sizeof(buf), "L:%lu lux", (unsigned long)lastLight);
        u8g2.drawStr(0, 34, buf);
        snprintf(buf, sizeof(buf), "C:%d uS/cm", (int)lastConductivity);
        u8g2.drawStr(0, 46, buf);
        snprintf(buf, sizeof(buf), "BAT:%u%%", (unsigned)lastBattery);
        u8g2.drawStr(0, 58, buf);
    }
    u8g2.sendBuffer();
}
#endif

static bool poll() {
    if (!bleClient) {
        bleClient = NimBLEDevice::createClient();
        if (!bleClient) {
            Serial.printf("[flower-care] createClient failed\n");
            return false;
        }
    }
    if (!bleClient->isConnected()) {
        Serial.printf("[flower-care] connecting to %s\n", FLOWERCARE_MAC);
        // BLE_ADDR_PUBLIC (0): Flower Care uses a public address by default.
        // Change to 1 (BLE_ADDR_RANDOM) if connection never succeeds.
        if (!bleClient->connect(NimBLEAddress(FLOWERCARE_MAC, 0))) {
            Serial.printf("[flower-care] connect failed\n");
            return false;
        }
        Serial.printf("[flower-care] connected\n");
    }

    NimBLERemoteService* svc = bleClient->getService(SVC_UUID);
    if (!svc) {
        Serial.printf("[flower-care] service not found\n");
        bleClient->disconnect();
        return false;
    }

    NimBLERemoteCharacteristic* cmdChar  = svc->getCharacteristic(CMD_UUID);
    NimBLERemoteCharacteristic* dataChar = svc->getCharacteristic(DATA_UUID);
    NimBLERemoteCharacteristic* batChar  = svc->getCharacteristic(BAT_UUID);

    if (!cmdChar || !dataChar) {
        Serial.printf("[flower-care] characteristics not found\n");
        bleClient->disconnect();
        return false;
    }

    uint8_t cmd[] = {0xA0, 0x1F};
    if (!cmdChar->writeValue(cmd, sizeof(cmd), true)) {
        Serial.printf("[flower-care] cmd write failed\n");
        bleClient->disconnect();
        return false;
    }
    delay(300);

    std::string raw = dataChar->readValue();
    if (raw.length() < 10) {
        Serial.printf("[flower-care] short read: %u bytes\n", (unsigned)raw.length());
        bleClient->disconnect();
        return false;
    }

    const uint8_t* d = reinterpret_cast<const uint8_t*>(raw.data());
    int16_t rawTemp;
    memcpy(&rawTemp, d,     2);  lastTemp         = rawTemp / 10.0f;
    memcpy(&lastLight, d + 3, 4);
    lastMoisture = d[7];
    memcpy(&lastConductivity, d + 8, 2);

    if (batChar) {
        std::string batRaw = batChar->readValue();
        if (!batRaw.empty()) lastBattery = (uint8_t)batRaw[0];
    }

    Serial.printf("[flower-care] T:%.1f H:%u L:%lu C:%d BAT:%u\n",
        lastTemp, (unsigned)lastMoisture, (unsigned long)lastLight,
        (int)lastConductivity, (unsigned)lastBattery);
    hasData = true;
    return true;
}

void setup() {
    Serial.begin(115200);
    Serial.printf("[flower-care] boot, target MAC: %s\n", FLOWERCARE_MAC);
#if WITH_OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    u8g2.begin();
    oledDraw();
#endif
    NimBLEDevice::init("");
    watchdogInit();
}

void loop() {
    watchdogFeed();
    unsigned long now = millis();
    if (lastPollMs == 0 || now - lastPollMs >= POLL_INTERVAL_MS) {
        poll();
        lastPollMs = millis();
#if WITH_OLED
        oledDraw();
#endif
    }
}
