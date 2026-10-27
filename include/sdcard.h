#pragma once

// =============================================================================
// sdcard.h — SD-Karten Interface für ESP32-Paxcounter (Custom Build)
//
// Optimiert für Event-/Veranstaltungs-Auswertung:
//   • raw_YYYY-MM-DD.csv    Rohdaten pro Messzyklus (ca. 1x pro Minute)
//   • stats_YYYY-MM-DD.csv  10-Min-Intervalle: Peak, Delta, MA30, Trend, Phase
//   • daily_summary.csv     Eine Zeile pro Tag, wird nach jedem Intervall
//                           aktualisiert (stromausfallsicher, keine Duplikate)
//
// Alle Zeitstempel: ISO 8601, Lokalzeit (TIME_SYNC_TIMEZONE, MEZ/MESZ),
// ohne Zeitzonen-Suffix, z.B. 2026-03-06T08:03:12
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
#define MOUNT_POINT "/sdcard"
#endif

#ifndef SDCARD_FILE_NAME
#define SDCARD_FILE_NAME "paxcount"
#endif

// ---------------------------------------------------------------------------
// Intervall & Schwellwerte
// ---------------------------------------------------------------------------
#define STATS_INTERVAL_SECONDS 600  // 10 Minuten pro Intervall (Teiler von 86400!)
#define MA_WINDOW_SIZE 3            // gleitender Durchschnitt über 3 Intervalle = 30 Min
#define PEAK_PHASE_THRESHOLD 0.80f  // ab 80% des Tagesmax = "peak"
#define POST_EVENT_RATIO 0.30f      // nach dem Peak unter 30% des Tagesmax = "post-event"
#define TREND_CHANGE_PERCENT 0.10f  // ab 10% Änderung ggü. vor 30 Min = rising/falling
#define PHASE_MIN_INTERVALS 3       // Phase "peak" frühestens ab dem 3. Intervall des Tages
#define PHASE_MIN_DEVICES 5         // Tagesmax unter diesem Wert -> keine Peak-Erkennung
#define PHASE_BASELINE_FACTOR 2     // Tagesmax muss >= 2x Tagesminimum sein (sonst Grundrauschen)

#define SLOTS_PER_DAY (86400 / STATS_INTERVAL_SECONDS)
#define SLOT_EMPTY 0xFFFF

// Zeit gilt als gültig ab 2024-01-01 (vorher: RTC/NTP noch nicht gestellt)
#define SDCARD_MIN_VALID_EPOCH 1704067200

// ---------------------------------------------------------------------------
// CSV Header — Rohdaten
// ---------------------------------------------------------------------------
#define SDCARD_FILE_HEADER "Timestamp,WiFi,BLE"
#define SDCARD_FILE_HEADER_VOLTAGE ",Voltage_mV"
#define SDCARD_FILE_HEADER_SDS011 ",PM10,PM2.5"

// ---------------------------------------------------------------------------
// CSV Header — 10-Min Statistik
// ---------------------------------------------------------------------------
#define SDCARD_STATS_HEADER                                                    \
  "IntervalStart,IntervalEnd,"                                                 \
  "BLE_Peak,BLE_Delta,BLE_MA30,"                                               \
  "WiFi_Peak,WiFi_Delta,WiFi_MA30,"                                            \
  "Trend,Phase\n"

// ---------------------------------------------------------------------------
// CSV Header — Tageszusammenfassung
// ---------------------------------------------------------------------------
#define SDCARD_DAILY_HEADER                                                    \
  "Date,"                                                                      \
  "BLE_DayPeak,BLE_PeakTime,BLE_TotalVisitors,BLE_PeakDuration_min,"           \
  "WiFi_DayPeak,WiFi_PeakTime,WiFi_TotalVisitors,WiFi_PeakDuration_min\n"

// ---------------------------------------------------------------------------
// Trend
// ---------------------------------------------------------------------------
typedef enum { TREND_STABLE = 0, TREND_RISING = 1, TREND_FALLING = -1 } PaxTrend_t;

// ---------------------------------------------------------------------------
// Event-Phase (automatische Klassifizierung, online = nur mit bisherigem Wissen)
// ---------------------------------------------------------------------------
typedef enum {
  PHASE_PRE_EVENT = 0,  // vor dem Event
  PHASE_ARRIVING = 1,   // Zustrom
  PHASE_PEAK = 2,       // Höhepunkt (>= 80% des bisherigen Tagesmax)
  PHASE_LEAVING = 3,    // Abfluss nach dem Peak
  PHASE_POST_EVENT = 4  // nach dem Event (< 30% des Tagesmax)
} EventPhase_t;

// ---------------------------------------------------------------------------
// Ringpuffer für gleitenden Durchschnitt (MA30 = 3 x 10 Min)
// ---------------------------------------------------------------------------
typedef struct {
  uint16_t values[MA_WINDOW_SIZE];
  uint8_t head;
  uint8_t count;
} MABuffer_t;

// ---------------------------------------------------------------------------
// Laufendes 10-Min-Intervall
// ---------------------------------------------------------------------------
typedef struct {
  time_t start;     // Epoch, auf STATS_INTERVAL_SECONDS ausgerichtet (Lokalzeit)
  uint16_t slot;    // Index 0..SLOTS_PER_DAY-1 innerhalb des Tages
  uint16_t blePeak;
  uint16_t wifiPeak;
  uint16_t samples; // Anzahl Messungen im Intervall (0 = leer)
} PaxInterval_t;

// ---------------------------------------------------------------------------
// Tageskontext (wird beim Neustart aus stats_YYYY-MM-DD.csv rekonstruiert)
// ---------------------------------------------------------------------------
typedef struct {
  uint16_t bleSlots[SLOTS_PER_DAY];  // Intervall-Peaks des Tages (SLOT_EMPTY = keine Daten)
  uint16_t wifiSlots[SLOTS_PER_DAY];
  uint16_t prevBlePeak;
  uint16_t prevWifiPeak;
  bool havePrev;
  MABuffer_t bleMa;
  MABuffer_t wifiMa;
  uint16_t bleRunMax;   // bisheriges Tagesmax (BLE), Basis der Phasen-Erkennung
  uint16_t bleRunMin;   // bisheriges Tagesminimum (BLE) = Grundlast
  uint16_t intervals;   // abgeschlossene Intervalle heute
  bool peakReached;     // Phase "peak" heute schon erreicht?
} PaxDay_t;

// ---------------------------------------------------------------------------
// Öffentliche Funktionen
// ---------------------------------------------------------------------------
bool sdcard_init(bool create = false);
void sdcard_flush(void);
void sdcard_close(void);

void sdcardWriteData(uint16_t noWifi, uint16_t noBle, uint16_t voltage = 0);

#if (SDLOGGING)
int print_to_sd_card(const char *fmt, va_list args);
#endif
