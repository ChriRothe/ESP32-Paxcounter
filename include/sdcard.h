#pragma once

// =============================================================================
// sdcard.h — SD-Karten Interface für ESP32-Paxcounter
// Optimiert für Event/Veranstaltungs-Auswertung:
//   • Rohdaten pro Messung
//   • 10-Min-Intervalle: Peak, Delta, MA30, Trend, Phase
//   • Tages-Zusammenfassung: Spitzenzeit, Gesamtbesucher, Peak-Dauer
// =============================================================================

#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Konfiguration
// ---------------------------------------------------------------------------
#ifndef MOUNT_POINT
  #define MOUNT_POINT             "/sdcard"
#endif

#ifndef SDCARD_FILE_NAME
  #define SDCARD_FILE_NAME        "paxcount"
#endif

// ---------------------------------------------------------------------------
// Intervall & Schwellwerte
// ---------------------------------------------------------------------------
#define STATS_INTERVAL_SECONDS    600     // 10 Minuten pro Intervall
#define MA_WINDOW_SIZE            3       // Gleitender Durchschnitt über 3 Intervalle = 30 Min
#define PEAK_PHASE_THRESHOLD      0.80f   // Ab 80% des Tagesmax = "peak phase"
#define TREND_CHANGE_PERCENT      0.10f   // Ab 10% Änderung = rising/falling

// ---------------------------------------------------------------------------
// CSV Header — Rohdaten
// ---------------------------------------------------------------------------
#define SDCARD_FILE_HEADER          "Timestamp,WiFi,BLE"
#define SDCARD_FILE_HEADER_VOLTAGE  ",Voltage_mV"
#define SDCARD_FILE_HEADER_SDS011   ",PM10,PM2.5"

// ---------------------------------------------------------------------------
// CSV Header — 10-Min Statistik
// ---------------------------------------------------------------------------
#define SDCARD_STATS_HEADER \
    "IntervalStart,IntervalEnd," \
    "BLE_Peak,BLE_Delta,BLE_MA30," \
    "WiFi_Peak,WiFi_Delta,WiFi_MA30," \
    "Trend,Phase\n"

// ---------------------------------------------------------------------------
// CSV Header — Tageszusammenfassung
// ---------------------------------------------------------------------------
#define SDCARD_DAILY_HEADER \
    "Date," \
    "BLE_DayPeak,BLE_PeakTime,BLE_TotalVisitors,BLE_PeakDuration_min," \
    "WiFi_DayPeak,WiFi_PeakTime,WiFi_TotalVisitors,WiFi_PeakDuration_min\n"

// ---------------------------------------------------------------------------
// Trend
// ---------------------------------------------------------------------------
typedef enum {
    TREND_STABLE  =  0,
    TREND_RISING  =  1,
    TREND_FALLING = -1
} PaxTrend_t;

// ---------------------------------------------------------------------------
// Event-Phase (automatische Klassifizierung)
// ---------------------------------------------------------------------------
typedef enum {
    PHASE_PRE_EVENT  = 0,   // Vor dem Event
    PHASE_ARRIVING   = 1,   // Zustrom
    PHASE_PEAK       = 2,   // Höhepunkt (>80% des Tagesmax)
    PHASE_LEAVING    = 3,   // Abfluss nach Peak
    PHASE_POST_EVENT = 4    // Nach dem Event
} EventPhase_t;

// ---------------------------------------------------------------------------
// Ringpuffer für gleitenden Durchschnitt (MA30 = 3 x 10 Min)
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t values[MA_WINDOW_SIZE];
    uint8_t  head;
    uint8_t  count;
} MABuffer_t;

// ---------------------------------------------------------------------------
// Aggregationspuffer: ein 10-Min-Intervall
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t   intervalStart;
    uint16_t   blePeak;
    uint16_t   wifiPeak;
    uint16_t   prevBlePeak;
    uint16_t   prevWifiPeak;
    MABuffer_t bleMa;
    MABuffer_t wifiMa;
    bool       peakReached;
    bool       initialized;
} PaxAggBuffer_t;

// ---------------------------------------------------------------------------
// Tages-Tracking
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t  bleDayPeak;
    uint32_t  blePeakTime;
    uint32_t  bleTotalVisitors;
    uint16_t  blePeakIntervals;

    uint16_t  wifiDayPeak;
    uint32_t  wifiPeakTime;
    uint32_t  wifiTotalVisitors;
    uint16_t  wifiPeakIntervals;
} DailyStats_t;

// ---------------------------------------------------------------------------
// Öffentliche Funktionen
// ---------------------------------------------------------------------------
bool  sdcard_init(bool create = false);
void  sdcard_flush(void);
void  sdcard_close(void);

void  sdcardWriteData(uint16_t noWifi, uint16_t noBle, uint16_t voltage = 0);

void  sdcard_reset_aggregation(void);
void  sdcard_flush_stats(void);
void  sdcard_write_daily_summary(void);

#if (SDLOGGING)
int   print_to_sd_card(const char *fmt, va_list args);
#endif
