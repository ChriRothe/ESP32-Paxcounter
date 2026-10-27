#pragma once

// =============================================================================
// ntp_sync.h — Einmaliger NTP-Sync beim Boot (Custom Build)
// Verbindet kurz mit WiFi, holt die Zeit per NTP, stellt den DS3231 und
// schaltet WiFi wieder ab. Muss VOR init_libpax() aufgerufen werden!
// Zugangsdaten: shared/ota.conf (OTA_WIFI_SSID / OTA_WIFI_PASS), von
// shared/build.py als WIFI_SSID / WIFI_PASS in den Build übernommen.
// =============================================================================

#include <stdbool.h>

// NTP-Server
#define NTP_SERVER_1 "de.pool.ntp.org"
#define NTP_SERVER_2 "pool.ntp.org"
#define NTP_SERVER_3 "time.google.com"

// Timeouts
#define NTP_WIFI_TIMEOUT_MS 15000 // max. Wartezeit auf WiFi-Verbindung
#define NTP_SYNC_TIMEOUT_MS 10000 // max. Wartezeit auf NTP-Antwort je Versuch
#define NTP_RETRY_COUNT 3         // Versuche

// Zeit gilt als gültig ab 2024-01-01
#define NTP_MIN_VALID_EPOCH 1704067200

// true, wenn die Zeit per NTP geholt und in den DS3231 geschrieben wurde
bool ntp_sync_and_set_rtc(void);
