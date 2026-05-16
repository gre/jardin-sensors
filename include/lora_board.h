#pragma once
#include <LoRa.h>

// LILYGO T3 V1.6.1: VBAT = GPIO35 through a 100k+100k divider (1:2).
#ifndef VBAT_PIN
#define VBAT_PIN 35
#endif

// Blue LED on GPIO25 (active HIGH). Red LED is a power indicator wired to 3.3V,
// not GPIO-controllable.
#ifndef LED_PIN
#define LED_PIN 25
#endif

// Pin every LoRa parameter explicitly so a future library update cannot
// silently change a default and desync nodes.
inline void loraConfigureRadio() {
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  // EU868 g-band ERP limit is 14 dBm. The lib otherwise defaults to 17 dBm
  // on PA_BOOST.
  LoRa.setTxPower(14, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.enableCrc();
}

// Reading via analogReadMilliVolts uses the eFuse-calibrated ADC curve.
inline float readVbatVolts() {
  return (analogReadMilliVolts(VBAT_PIN) * 2) / 1000.0f;
}
