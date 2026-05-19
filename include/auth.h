#pragma once

#include <stdint.h>
#include <stddef.h>

// HMAC-SHA256(key, msg) -> 32 bytes in out.
#ifdef ESP_PLATFORM
// mbedtls ships with Arduino-ESP32; no lib_deps entry needed.
#include "mbedtls/md.h"
inline void authHmacSha256(const uint8_t* key, size_t keyLen,
                           const uint8_t* msg, size_t msgLen,
                           uint8_t out[32]) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, msg, msgLen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}
#else
#include <SHA256.h>
inline void authHmacSha256(const uint8_t* key, size_t keyLen,
                           const uint8_t* msg, size_t msgLen,
                           uint8_t out[32]) {
  SHA256 sha256;
  sha256.resetHMAC(key, keyLen);
  sha256.update(msg, msgLen);
  sha256.finalizeHMAC(key, keyLen, out, 32);
}
#endif

// Append "|" + 16 hex chars (= 8 truncated HMAC bytes) to the buffer.
// Returns the new length. buf must have at least (currentLen + 17 + 1).
inline size_t authAppendMac(char* buf, size_t currentLen, size_t bufSize,
                            const uint8_t* key, size_t keyLen) {
  if (currentLen + 17 >= bufSize) return currentLen;
  uint8_t mac[32];
  authHmacSha256(key, keyLen,
                 reinterpret_cast<const uint8_t*>(buf), currentLen, mac);
  static const char hex[] = "0123456789abcdef";
  buf[currentLen++] = '|';
  for (int i = 0; i < 8; ++i) {
    buf[currentLen++] = hex[mac[i] >> 4];
    buf[currentLen++] = hex[mac[i] & 0x0F];
  }
  return currentLen;
}

inline int authHexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Verify the "|<hex16>" suffix on msg. Returns the length of the JSON
// portion (before the '|') if valid, or -1 if invalid.
// Const-time comparison to avoid timing attacks.
inline int authVerifyMac(const char* msg, int len,
                         const uint8_t* key, size_t keyLen) {
  if (len < 17) return -1;
  int jsonLen = len - 17;
  if (msg[jsonLen] != '|') return -1;

  uint8_t expected[8];
  for (int i = 0; i < 8; ++i) {
    int hi = authHexNibble(msg[jsonLen + 1 + i * 2]);
    int lo = authHexNibble(msg[jsonLen + 1 + i * 2 + 1]);
    if (hi < 0 || lo < 0) return -1;
    expected[i] = static_cast<uint8_t>((hi << 4) | lo);
  }

  uint8_t actual[32];
  authHmacSha256(key, keyLen,
                 reinterpret_cast<const uint8_t*>(msg), jsonLen, actual);

  uint8_t diff = 0;
  for (int i = 0; i < 8; ++i) diff |= expected[i] ^ actual[i];
  return (diff == 0) ? jsonLen : -1;
}
