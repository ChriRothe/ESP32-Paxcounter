#pragma once

// =============================================================================
// ntp_sync.h — Einmaliger NTP-Sync beim Boot
// Verbindet kurz mit WiFi, holt Zeit via NTP, stellt DS3231, trennt WiFi.
// Muss VOR init_libpax() aufgerufen werden!
// Zugangsdaten kommen aus ota.conf: OTA_WIFI_SSID / OTA_WIFI_PASS
// =============================================================================

#pragma once

#include <stdbool.h>

// NTP-Server (können angepasst werden)
#define NTP_SERVER_1    "pool.ntp.org"
#define NTP_SERVER_2    "time.google.com"
#define NTP_SERVER_3    "europe.pool.ntp.org"

// Timeouts
#define NTP_WIFI_TIMEOUT_MS   15000   // Max. Wartezeit auf WiFi-Verbindung
#define NTP_SYNC_TIMEOUT_MS   10000   // Max. Wartezeit auf NTP-Antwort
#define NTP_RETRY_COUNT       3       // Versuche bei fehlgeschlagenem Sync

// Versuche NTP-Sync. Gibt true zurück wenn Zeit erfolgreich geholt und
// in DS3231 geschrieben wurde.
bool ntp_sync_and_set_rtc(void);
