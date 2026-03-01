#pragma once

// =============================================================================
// sdcard.h — SD-Karten Interface für ESP32-Paxcounter
// Erweitert um: Zeitstempel via DS3231, 10-Min-Aggregation,
// Min/Max, Bewegungstrend und Peak-Erkennung
// =============================================================================

#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Konfiguration — in deiner board hal oder paxcounter.conf anpassen
// ---------------------------------------------------------------------------
#ifndef MOUNT_POINT
  #define MOUNT_POINT           "/sdcard"
#endif

#ifndef SDCARD_FILE_NAME
  #define SDCARD_FILE_NAME      "paxcount"
#endif

// --- Rohdaten-Header ---
#define SDCARD_FILE_HEADER \
    "Timestamp,WiFi,BLE"

#define SDCARD_FILE_HEADER_VOLTAGE \
    ",Voltage_mV"

#define SDCARD_FILE_HEADER_SDS011 \
    ",PM10,PM2.5"

// --- Aggregations-Header (separate Datei) ---
#define SDCARD_STATS_HEADER \
    "IntervalStart,IntervalEnd," \
    "WiFi_Avg,BLE_Avg," \
    "WiFi_Max,BLE_Max," \
    "WiFi_Min,BLE_Min," \
    "Samples,Trend,Peak_WiFi,Peak_BLE\n"

// ---------------------------------------------------------------------------
// Aggregationsintervall
// ---------------------------------------------------------------------------
#define STATS_INTERVAL_SECONDS  600   // 10 Minuten
#define PEAK_THRESHOLD_FACTOR   1.5f  // Wert > Faktor*Avg gilt als Peak

// ---------------------------------------------------------------------------
// Trend-Klassifizierung
// ---------------------------------------------------------------------------
typedef enum {
    TREND_STABLE   = 0,
    TREND_RISING   = 1,
    TREND_FALLING  = -1
} PaxTrend_t;

// ---------------------------------------------------------------------------
// Aggregationspuffer: sammelt Messwerte eines Intervalls
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t  intervalStart;   // Unix-Timestamp Intervallbeginn
    uint32_t  sampleCount;     // Anzahl Messungen im Intervall
    float     wifiSum;
    float     bleSum;
    uint16_t  wifiMax;
    uint16_t  bleMax;
    uint16_t  wifiMin;
    uint16_t  bleMin;
    uint16_t  prevWifiAvg;     // Durchschnitt letztes Intervall (für Trend)
    uint16_t  prevBleAvg;      // Durchschnitt letztes Intervall (für Trend)
    bool      initialized;
} PaxAggBuffer_t;

// ---------------------------------------------------------------------------
// Öffentliche Funktionen
// ---------------------------------------------------------------------------
bool  sdcard_init(bool create = false);
void  sdcard_flush(void);
void  sdcard_close(void);

// Rohdaten schreiben (jede Messung)
void  sdcardWriteData(uint16_t noWifi, uint16_t noBle,
                      uint16_t voltage = 0);

// Aggregationspuffer zurücksetzen (z.B. nach Neustart)
void  sdcard_reset_aggregation(void);

// Manuell ein Aggregationsintervall abschließen (für Tests)
void  sdcard_flush_stats(void);

// Logging (optional)
#if (SDLOGGING)
int   print_to_sd_card(const char *fmt, va_list args);
#endif
