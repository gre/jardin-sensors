#pragma once

#ifdef ESP_PLATFORM
#include <esp_task_wdt.h>

#ifndef TWDT_TIMEOUT_S
#define TWDT_TIMEOUT_S 30
#endif

inline void watchdogInit() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t cfg = {
      .timeout_ms     = TWDT_TIMEOUT_S * 1000U,
      .idle_core_mask = 0,
      .trigger_panic  = true,
  };
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(TWDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

inline void watchdogFeed() { esp_task_wdt_reset(); }

#else
static inline void watchdogInit() {}
static inline void watchdogFeed() {}
#endif
