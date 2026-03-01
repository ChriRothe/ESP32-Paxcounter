// =============================================================================
// ntp_sync.cpp — Einmaliger NTP-Sync beim Boot
// =============================================================================

#include "ntp_sync.h"
#include "globals.h"      // TAG, ESP_LOGx
#include "rtctime.h"      // set_rtctime()

#include <WiFi.h>
#include <esp_wifi.h>
#include "time.h"
#include <sys/time.h>

#undef TAG
static const char *TAG = "NTP";

// ---------------------------------------------------------------------------
// Hilfsfunktion: Prüft ob die geholte Zeit plausibel ist (> 2024)
// ---------------------------------------------------------------------------
static bool time_is_plausible(time_t t) {
    struct tm tt;
    gmtime_r(&t, &tt);
    return (tt.tm_year > 124); // Jahr > 2024
}

// ---------------------------------------------------------------------------
// ntp_sync_and_set_rtc()
//
// Ablauf:
//   1. WiFi im Station-Modus verbinden (OTA_WIFI_SSID / OTA_WIFI_PASS)
//   2. NTP konfigurieren und auf Sync warten
//   3. Zeit in DS3231 schreiben via set_rtctime()
//   4. ESP32-Systemzeit setzen
//   5. WiFi komplett trennen und deaktivieren
//
// Gibt true zurück bei Erfolg, false wenn WiFi oder NTP fehlschlägt.
// Im Fehlerfall bleibt die bestehende RTC-Zeit erhalten.
// ---------------------------------------------------------------------------
bool ntp_sync_and_set_rtc(void) {

    ESP_LOGI(TAG, "Starting NTP boot sync...");
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);

    // --- 1. WiFi verbinden ---
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - wifiStart > NTP_WIFI_TIMEOUT_MS) {
            ESP_LOGW(TAG, "WiFi connect timeout after %dms — skipping NTP sync", NTP_WIFI_TIMEOUT_MS);
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    ESP_LOGI(TAG, "WiFi connected, IP: %s", WiFi.localIP().toString().c_str());

    // --- 2. NTP konfigurieren ---
    // timezone = 0 → wir wollen immer UTC
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

    // Auf gültige NTP-Zeit warten
    bool ntpSuccess = false;
    uint32_t ntpStart = millis();

    for (int attempt = 1; attempt <= NTP_RETRY_COUNT; attempt++) {
        ESP_LOGI(TAG, "Waiting for NTP sync (attempt %d/%d)...", attempt, NTP_RETRY_COUNT);

        uint32_t attemptStart = millis();
        while (millis() - attemptStart < NTP_SYNC_TIMEOUT_MS) {
            time_t now = time(NULL);
            if (time_is_plausible(now)) {
                ntpSuccess = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (ntpSuccess) break;
        ESP_LOGW(TAG, "NTP attempt %d failed, retrying...", attempt);
    }

    // --- 3. Zeit in DS3231 schreiben ---
    if (ntpSuccess) {
        time_t now = time(NULL);
        struct tm tt;
        gmtime_r(&now, &tt);

        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tt);
        ESP_LOGI(TAG, "NTP time received: %s UTC", timeBuf);

        // In DS3231 schreiben (set_rtctime ist bereits in rtctime.cpp)
        set_rtctime(now);

        // ESP32-Systemzeit explizit setzen
        struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
        settimeofday(&tv, NULL);

        ESP_LOGI(TAG, "DS3231 updated successfully. Total sync time: %lums",
                 (unsigned long)(millis() - wifiStart));
    } else {
        ESP_LOGW(TAG, "NTP sync failed after %d attempts — keeping existing RTC time",
                 NTP_RETRY_COUNT);
    }

    // --- 4. WiFi komplett deaktivieren ---
    // Wichtig: Muss vor init_libpax() passieren, sonst stört es den Sniffer-Modus
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // Kurze Pause damit der WiFi-Stack sauber herunterfährt
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "WiFi disabled, ready for pax sniffing");

    return ntpSuccess;
}
