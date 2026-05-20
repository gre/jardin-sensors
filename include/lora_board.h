#pragma once
#include <SPI.h>
#include <RadioLib.h>

// IRAM_ATTR is ESP32-specific; on other platforms define it away.
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// LILYGO T3 V1.6.1: VBAT = GPIO35 through a 100k+100k divider (1:2).
#ifndef VBAT_PIN
#define VBAT_PIN 35
#endif

// Blue LED on GPIO25 (active HIGH). Red LED is a power indicator wired to 3.3V,
// not GPIO-controllable.
#ifndef LED_PIN
#define LED_PIN 25
#endif

#ifdef LORA_USE_SX1262
// SX1262 uses DIO1 as IRQ and requires a BUSY pin. SPI uses platform defaults
// (call SPI.begin() before loraBegin() if your pins are non-default).
static Module loraModule(LORA_SS, LORA_DIO1, LORA_RST, LORA_BUSY);
static SX1262 loraRadio(&loraModule);
#else
static SPIClass loraSPI(VSPI);
static Module   loraModule(LORA_SS, LORA_DIO0, LORA_RST, RADIOLIB_NC, loraSPI);
static SX1276   loraRadio(&loraModule);
#endif

static volatile bool loraRxFlag = false;
static void IRAM_ATTR loraOnReceive() { loraRxFlag = true; }

// Disable JTAG to free PB3 (JTDO), PB4 (NJTRST), PA15 (JTDI) as GPIO.
// Must be called before any pinMode/digitalWrite on those pins.
// SWD (PA13/PA14) is also released — use serial flashing only after this point.
static inline void loraDisableJtag() {
#if defined(__HAL_AFIO_REMAP_SWJ_DISABLE)
  __HAL_AFIO_REMAP_SWJ_DISABLE();
#endif
}

// Pins every radio parameter explicitly so a library update cannot silently
// change a default and desync nodes.
static inline int16_t loraBegin() {
#ifdef LORA_USE_SX1262
  // SPI.begin() MUST come before the RST pulse so that NSS is driven HIGH by
  // the time NRST releases. SPI.begin() only configures SCK/MISO/MOSI; NSS
  // (software CS) must be explicitly driven HIGH before the reset pulse, or
  // the SX1261 may receive garbage SPI frames during the NRST recovery window.
  SPI.begin();
  pinMode(LORA_SS, OUTPUT);
  digitalWrite(LORA_SS, HIGH);
#ifdef LORA_BUSY
  pinMode(LORA_BUSY, INPUT);
#endif
#ifdef LORA_RST
  pinMode(LORA_RST, OUTPUT);
  // 500ms LOW pulse: discharges the DX-LR30 NRST filter cap fully.
  // Longer than 200ms to handle larger-than-typical filter caps.
  digitalWrite(LORA_RST, LOW);
  delay(500);
  digitalWrite(LORA_RST, HIGH);
  delay(500);
#endif
  // PA15 (LORA_RST) is NOT wired to SX1261 NRST on the DX-LR30; the RST pulse
  // above is a no-op for the chip. Disable RadioLib's internal RST so it doesn't
  // issue its own 1ms pulse against an open pin.
  loraModule.rstPin = RADIOLIB_NC;
  // PA14 doubles as DIO3 on the DX-LR30 and goes HIGH during TX, which would
  // block SPI if RadioLib polls it as BUSY. Keep NC for all operations.
  loraModule.gpioPin = RADIOLIB_NC;
#else
  loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
#endif
  int16_t s = loraRadio.begin(
      (float)LORA_BAND / 1e6f,  // MHz
      125.0f, 7, 5,              // BW kHz, SF, CR denom (4/5)
#ifdef LORA_TCXO_VOLTAGE
      LORA_SYNC_WORD, 14, 8, LORA_TCXO_VOLTAGE);  // syncWord, power, preamble, TCXO V
#else
      // DX-LR30 uses XOSC (crystal on XTA/XTB), not a TCXO on DIO3. Pass 0.0f
      // to skip SetDio3AsTcxoCtrl; the SX1262::begin() default of 1.6V would
      // disconnect the crystal and cause every SetTx to fail with CMD_FAILED.
      LORA_SYNC_WORD, 14, 8, 0.0f);
#endif
  // -707 = RADIOLIB_ERR_SPI_CMD_FAILED from XOSC_START_ERR during Calibrate.
  // Continue anyway: TCXO may still oscillate despite the startup-detection fail.
  if (s != RADIOLIB_ERR_NONE && s != RADIOLIB_ERR_SPI_CMD_FAILED) return s;
  if (s == RADIOLIB_ERR_SPI_CMD_FAILED) {
    Serial.println("[lora] XOSC warning, continuing...");
  }
  loraRadio.setCRC(true);
#ifdef LORA_USE_SX1262
  loraRadio.setDio1Action(loraOnReceive);
#ifdef LORA_TXEN
  // DX-LR30 external RF switch: TXEN and RXEN are driven by the STM32,
  // not by SX1262's DIO2. Tell RadioLib to control them directly.
  loraRadio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);
#endif
#else
  loraRadio.setDio0Action(loraOnReceive, RISING);
#endif
  return RADIOLIB_ERR_NONE;
}

// Returns bytes read, or 0 on error. Always calls startReceive() before returning.
static inline size_t loraReadPacket(char* buf, size_t bufSize) {
  size_t pktLen = loraRadio.getPacketLength();
  bool ok = pktLen > 0 && pktLen < bufSize &&
            loraRadio.readData(reinterpret_cast<uint8_t*>(buf), pktLen) == RADIOLIB_ERR_NONE;
  loraRadio.startReceive();
  return ok ? pktLen : 0;
}

// CSMA/CAD: scan before TX; if busy, back off once then transmit anyway.
// Calls startReceive() before returning — caller must not call it again.
static inline int16_t loraTx(const uint8_t* buf, size_t len) {
  if (loraRadio.scanChannel() == RADIOLIB_LORA_DETECTED) {
    delay(20 + static_cast<int>(random(0, 100)));
  }
  int16_t s = loraRadio.transmit(buf, len);
  // Clear CadDone + TxDone spurious DIO flags before re-arming RX.
  // Any real packet during CAD/TX wasn't received anyway (half-duplex).
  loraRxFlag = false;
  loraRadio.startReceive();
  return s;
}

// Reading via analogReadMilliVolts uses the eFuse-calibrated ADC curve (ESP32).
// On other platforms, raw analogRead with 12-bit 3.3 V reference is assumed.
inline float readVbatVolts() {
#ifdef ESP_PLATFORM
  return (analogReadMilliVolts(VBAT_PIN) * 2) / 1000.0f;
#else
  return (analogRead(VBAT_PIN) * 3.3f / 4095.0f) * 2.0f;
#endif
}
