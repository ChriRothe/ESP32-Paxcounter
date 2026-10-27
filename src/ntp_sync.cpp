// =============================================================================
// ntp_sync.cpp — Einmaliger NTP-Sync beim Boot (Custom Build)
//
// Ablauf:
//   1. WiFi im Station-Modus verbinden (WIFI_SSID / WIFI_PASS aus ota.conf)
//   2. NTP-Sync abwarten (SNTP-Status, nicht nur "Zeit plausibel")
//   3. Zeit (UTC-Epoch) in den DS3231 schreiben
//   4. WiFi komplett abschalten
//
// WICHTIG: muss VOR init_libpax() laufen. Wird WiFi erst danach abgeschaltet,
// ist der WiFi-Sniffer von libpax tot und zählt dauerhaft 0.
// =============================================================================

#include "ntp_sync.h"
#include "globals.h"
#include "rtctime.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include "esp_sntp.h"
#include "time.h"
#include <sys/time.h>

#undef TAG
static const char *TAG = "NTP";

#ifndef TIME_SYNC_TIMEZONE
#define TIME_SYNC_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

static void wifi_shutdown(void) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  vTaskDelay(pdMS_TO_TICKS(100)); // WiFi-Stack sauber herunterfahren lassen
}

bool ntp_sync_and_set_rtc(void) {
  ESP_LOGI(TAG, "Starting NTP boot sync, SSID: %s", WIFI_SSID);

  // --- 1. WiFi verbinden ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > NTP_WIFI_TIMEOUT_MS) {
      ESP_LOGW(TAG, "WiFi connect timeout after %dms - skipping NTP sync",
               NTP_WIFI_TIMEOUT_MS);
      wifi_shutdown();
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  ESP_LOGI(TAG, "WiFi connected, IP: %s", WiFi.localIP().toString().c_str());

  // --- 2. NTP ---
  // configTzTime() statt configTime(0,0,...): configTime() überschreibt TZ
  // mit "UTC0" und macht damit die MEZ/MESZ-Einstellung zunichte.
  configTzTime(TIME_SYNC_TIMEZONE, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  bool ntpSuccess = false;
  for (int attempt = 1; attempt <= NTP_RETRY_COUNT && !ntpSuccess; attempt++) {
    ESP_LOGI(TAG, "Waiting for NTP sync (attempt %d/%d)...", attempt,
             NTP_RETRY_COUNT);
    uint32_t attemptStart = millis();
    while (millis() - attemptStart < NTP_SYNC_TIMEOUT_MS) {
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
          time(NULL) >= NTP_MIN_VALID_EPOCH) {
        ntpSuccess = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!ntpSuccess)
      ESP_LOGW(TAG, "NTP attempt %d failed", attempt);
  }
  sntp_stop();

  // --- 3. DS3231 stellen ---
  if (ntpSuccess) {
    time_t now = time(NULL);
    struct tm tt;
    localtime_r(&now, &tt);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S %Z", &tt);
    ESP_LOGI(TAG, "NTP time received: %s", timeBuf);

#ifdef HAS_RTC
    set_rtctime(now); // RTC speichert UTC-Epoch
#endif
    ESP_LOGI(TAG, "RTC updated. Total sync time: %lums",
             (unsigned long)(millis() - wifiStart));
  } else {
    ESP_LOGW(TAG, "NTP sync failed after %d attempts - keeping RTC time",
             NTP_RETRY_COUNT);
  }

  // --- 4. WiFi aus ---
  wifi_shutdown();
  ESP_LOGI(TAG, "WiFi disabled, ready for pax sniffing");
  return ntpSuccess;
}
