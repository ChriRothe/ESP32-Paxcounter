// =============================================================================
// sdcard.cpp — SD-Karten Interface für ESP32-Paxcounter (Custom Build)
// Optimiert für Event-/Veranstaltungs-Auswertung, siehe include/sdcard.h
//
// Robustheit:
//   • Dateien werden erst geöffnet, wenn die Uhr gültig ist (kein 1970-Datum)
//   • Tagesrollover und Zeitstempel in Lokalzeit (MEZ/MESZ)
//   • Nach einem Neustart wird der Tageskontext aus stats_<heute>.csv gelesen
//   • daily_summary.csv wird nach jedem Intervall neu geschrieben
//     (Zeile des aktuellen Tages wird ersetzt -> keine Duplikate,
//      kein Datenverlust wenn einfach der Strom gezogen wird)
// =============================================================================

#ifdef HAS_SDCARD

#include "sdcard.h"
#include "esp_log.h"
#include "time.h"
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#if (HAS_SDS011)
#include "sds011read.h"
#endif

#undef TAG
static const char *TAG = "SD";

// ---------------------------------------------------------------------------
// Interne Zustandsvariablen
// ---------------------------------------------------------------------------
sdmmc_card_t *card;
const char mount_point[] = MOUNT_POINT;
static bool useSDCard = false;

static FILE *data_file = NULL;  // raw_YYYY-MM-DD.csv
static FILE *stats_file = NULL; // stats_YYYY-MM-DD.csv

static char current_date[11] = {0}; // YYYY-MM-DD des offenen Tages

static PaxInterval_t cur = {};
static PaxDay_t day;

#if (SDLOGGING)
static FILE *log_file = NULL;
#endif

#define DAILY_FILE "/daily_summary.csv"
#define DAILY_TMP "/daily_summary.tmp"

// ---------------------------------------------------------------------------
// Zeit-Hilfsfunktionen (alles in Lokalzeit gemäß TZ)
// ---------------------------------------------------------------------------
static inline bool time_valid(time_t t) { return t >= SDCARD_MIN_VALID_EPOCH; }

static void fmt_iso(char *buf, size_t len, time_t t) {
  struct tm tt;
  localtime_r(&t, &tt);
  strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tt);
}

static uint16_t slot_of(const struct tm *tt) {
  uint32_t sod = tt->tm_hour * 3600UL + tt->tm_min * 60UL + tt->tm_sec;
  uint16_t s = sod / STATS_INTERVAL_SECONDS;
  return (s < SLOTS_PER_DAY) ? s : SLOTS_PER_DAY - 1;
}

// Slot-Index -> "YYYY-MM-DDTHH:MM:00" des aktuellen Tages
static void fmt_slot(char *buf, size_t len, uint16_t slot) {
  uint32_t sod = (uint32_t)slot * STATS_INTERVAL_SECONDS;
  snprintf(buf, len, "%sT%02u:%02u:00", current_date, (unsigned)(sod / 3600),
           (unsigned)((sod / 60) % 60));
}

// ---------------------------------------------------------------------------
// Datei-Hilfsfunktionen
// ---------------------------------------------------------------------------
static void mkpath(char *path, size_t len, const char *filename) {
  snprintf(path, len, "%s%s", MOUNT_POINT, filename);
}

static bool openFile(FILE **fd, const char *filename, const char *mode = "a") {
  char path[64];
  mkpath(path, sizeof(path), filename);
  if ((*fd = fopen(path, mode)) == NULL) {
    ESP_LOGE(TAG, "Cannot open: %s", path);
    return false;
  }
  ESP_LOGI(TAG, "Opened: %s", path);
  return true;
}

// Header nur schreiben, wenn die Datei leer ist.
// Hinweis: fgetpos() liefert im Append-Modus direkt nach fopen() 0, auch wenn
// die Datei schon Inhalt hat -> deshalb explizit ans Ende springen.
static void writeHeaderIfEmpty(FILE *fd, const char *header) {
  if (!fd)
    return;
  fseek(fd, 0, SEEK_END);
  if (ftell(fd) == 0) {
    fputs(header, fd);
    fflush(fd);
    fsync(fileno(fd));
  }
}

static void sync_file(FILE *fd) {
  if (!fd)
    return;
  fflush(fd);
  fsync(fileno(fd));
}

// ---------------------------------------------------------------------------
// Gleitender Durchschnitt — Ringpuffer
// ---------------------------------------------------------------------------
static void ma_push(MABuffer_t *ma, uint16_t value) {
  ma->values[ma->head] = value;
  ma->head = (ma->head + 1) % MA_WINDOW_SIZE;
  if (ma->count < MA_WINDOW_SIZE)
    ma->count++;
}

static uint16_t ma_get(const MABuffer_t *ma) {
  if (ma->count == 0)
    return 0;
  uint32_t sum = 0;
  for (uint8_t i = 0; i < ma->count; i++)
    sum += ma->values[i];
  return (uint16_t)(sum / ma->count);
}

// ---------------------------------------------------------------------------
// Trend
// ---------------------------------------------------------------------------
static PaxTrend_t calc_trend(uint16_t current, uint16_t previous) {
  if (previous == 0)
    return (current > 0) ? TREND_RISING : TREND_STABLE;
  int diff = (int)current - (int)previous;
  int limit = (int)(previous * TREND_CHANGE_PERCENT);
  if (limit < 1)
    limit = 1;
  if (diff > limit)
    return TREND_RISING;
  if (diff < -limit)
    return TREND_FALLING;
  return TREND_STABLE;
}

static const char *trend_str(PaxTrend_t t) {
  switch (t) {
  case TREND_RISING:
    return "rising";
  case TREND_FALLING:
    return "falling";
  default:
    return "stable";
  }
}

// Wert von vor MA_WINDOW_SIZE Intervallen (= vor 30 Min) aus dem MA-Ringpuffer
static uint16_t ma_oldest(const MABuffer_t *ma) {
  if (ma->count == 0)
    return 0;
  return (ma->count < MA_WINDOW_SIZE) ? ma->values[0] : ma->values[ma->head];
}

// ---------------------------------------------------------------------------
// Event-Phase (online, nur mit dem bisher bekannten Tagesverlauf)
//   - Kein ausgeprägter Verlauf (Tagesmax < PHASE_MIN_DEVICES oder
//     < PHASE_BASELINE_FACTOR x Tagesminimum, < PHASE_MIN_INTERVALS)
//                                          -> pre-event / arriving
//   - steigt noch (30-Min-Trend rising)     -> arriving (vor dem Peak)
//   - >= 80% des bisherigen Tagesmax        -> peak
//   - Peak erreicht, < 30% des Tagesmax     -> post-event
//   - Peak erreicht, sonst                  -> leaving
// ---------------------------------------------------------------------------
static EventPhase_t calc_phase(uint16_t blePeak, PaxTrend_t trend) {
  bool enoughData = (day.intervals + 1 >= PHASE_MIN_INTERVALS) &&
                    (day.bleRunMax >= PHASE_MIN_DEVICES) &&
                    (day.bleRunMax >= PHASE_BASELINE_FACTOR * day.bleRunMin);

  if (enoughData) {
    float ratio = (float)blePeak / (float)day.bleRunMax;
    if (ratio >= PEAK_PHASE_THRESHOLD &&
        (trend != TREND_RISING || day.peakReached)) {
      day.peakReached = true;
      return PHASE_PEAK;
    }
    if (day.peakReached)
      return (ratio < POST_EVENT_RATIO) ? PHASE_POST_EVENT : PHASE_LEAVING;
  }
  return (trend == TREND_RISING) ? PHASE_ARRIVING : PHASE_PRE_EVENT;
}

static const char *phase_str(EventPhase_t p) {
  switch (p) {
  case PHASE_PRE_EVENT:
    return "pre-event";
  case PHASE_ARRIVING:
    return "arriving";
  case PHASE_PEAK:
    return "peak";
  case PHASE_LEAVING:
    return "leaving";
  case PHASE_POST_EVENT:
    return "post-event";
  default:
    return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Tageskontext zurücksetzen
// ---------------------------------------------------------------------------
static void day_reset(void) {
  memset(&day, 0, sizeof(day));
  for (int i = 0; i < SLOTS_PER_DAY; i++) {
    day.bleSlots[i] = SLOT_EMPTY;
    day.wifiSlots[i] = SLOT_EMPTY;
  }
  memset(&cur, 0, sizeof(cur));
}

// Ein abgeschlossenes Intervall in den Tageskontext übernehmen
static void day_apply(uint16_t slot, uint16_t blePeak, uint16_t wifiPeak) {
  if (slot < SLOTS_PER_DAY) {
    // bei doppeltem Slot (Neustart im selben Intervall / Zeitumstellung) Max nehmen
    if (day.bleSlots[slot] == SLOT_EMPTY || blePeak > day.bleSlots[slot])
      day.bleSlots[slot] = blePeak;
    if (day.wifiSlots[slot] == SLOT_EMPTY || wifiPeak > day.wifiSlots[slot])
      day.wifiSlots[slot] = wifiPeak;
  }
  ma_push(&day.bleMa, blePeak);
  ma_push(&day.wifiMa, wifiPeak);
  if (blePeak > day.bleRunMax)
    day.bleRunMax = blePeak;
  if (day.intervals == 0 || blePeak < day.bleRunMin)
    day.bleRunMin = blePeak;
  day.prevBlePeak = blePeak;
  day.prevWifiPeak = wifiPeak;
  day.havePrev = true;
  day.intervals++;
}

// ---------------------------------------------------------------------------
// Tageskontext nach Neustart aus stats_<heute>.csv rekonstruieren
// ---------------------------------------------------------------------------
static void day_restore_from_stats(const char *filename) {
  char path[64];
  mkpath(path, sizeof(path), filename);
  FILE *f = fopen(path, "r");
  if (!f)
    return;

  char line[160];
  uint16_t restored = 0;
  while (fgets(line, sizeof(line), f)) {
    int Y, M, D, h, m, s;
    if (sscanf(line, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &s) != 6)
      continue; // Header oder abgeschnittene Zeile
    // Felder: 0 Start,1 End,2 BLE_Peak,3 BLE_Delta,4 BLE_MA30,
    //         5 WiFi_Peak,6 WiFi_Delta,7 WiFi_MA30,8 Trend,9 Phase
    char *fields[10] = {0};
    int n = 0;
    for (char *p = strtok(line, ",\r\n"); p && n < 10; p = strtok(NULL, ",\r\n"))
      fields[n++] = p;
    if (n < 10)
      continue; // unvollständige Zeile (z.B. Strom weg beim Schreiben)
    uint16_t slot =
        (h * 3600UL + m * 60UL + s) / STATS_INTERVAL_SECONDS;
    day_apply(slot, (uint16_t)atoi(fields[2]), (uint16_t)atoi(fields[5]));
    if (strcmp(fields[9], "peak") == 0)
      day.peakReached = true;
    restored++;
  }
  fclose(f);
  if (restored)
    ESP_LOGI(TAG, "Restored %u intervals of today from %s", restored, path);
}

// ---------------------------------------------------------------------------
// Tageszusammenfassung aus den Slots berechnen
// ---------------------------------------------------------------------------
typedef struct {
  uint16_t peak;
  int16_t peakSlot; // -1 = keine Daten
  uint32_t total;   // Summe der Intervall-Peaks (Näherung, kein exakter Zähler)
  uint16_t peakIntervals;
} SlotSummary_t;

static SlotSummary_t summarize(const uint16_t *slots) {
  SlotSummary_t r = {0, -1, 0, 0};
  for (int i = 0; i < SLOTS_PER_DAY; i++) {
    if (slots[i] == SLOT_EMPTY)
      continue;
    r.total += slots[i];
    if (r.peakSlot < 0 || slots[i] > r.peak) {
      r.peak = slots[i];
      r.peakSlot = i;
    }
  }
  // Peak-Dauer gegen das ECHTE Tagesmax (nicht das laufende) berechnen
  if (r.peak > 0) {
    for (int i = 0; i < SLOTS_PER_DAY; i++)
      if (slots[i] != SLOT_EMPTY &&
          (float)slots[i] / (float)r.peak >= PEAK_PHASE_THRESHOLD)
        r.peakIntervals++;
  }
  return r;
}

// ---------------------------------------------------------------------------
// daily_summary.csv neu schreiben: alle Tage übernehmen, Zeile des
// aktuellen Tages ersetzen. Schreibt in .tmp und benennt dann um.
// ---------------------------------------------------------------------------
static void write_daily_summary(void) {
  if (!useSDCard || current_date[0] == 0 || day.intervals == 0)
    return;

  SlotSummary_t b = summarize(day.bleSlots);
  SlotSummary_t w = summarize(day.wifiSlots);

  char bleTime[24] = "N/A", wifiTime[24] = "N/A";
  if (b.peakSlot >= 0)
    fmt_slot(bleTime, sizeof(bleTime), b.peakSlot);
  if (w.peakSlot >= 0)
    fmt_slot(wifiTime, sizeof(wifiTime), w.peakSlot);

  char pathOld[64], pathTmp[64];
  mkpath(pathOld, sizeof(pathOld), DAILY_FILE);
  mkpath(pathTmp, sizeof(pathTmp), DAILY_TMP);

  FILE *out = fopen(pathTmp, "w");
  if (!out) {
    ESP_LOGE(TAG, "Cannot open %s", pathTmp);
    return;
  }
  fputs(SDCARD_DAILY_HEADER, out);

  FILE *in = fopen(pathOld, "r");
  if (in) {
    char line[200];
    while (fgets(line, sizeof(line), in)) {
      if (strncmp(line, "Date,", 5) == 0)
        continue; // alter Header
      if (strncmp(line, current_date, 10) == 0)
        continue; // heutige Zeile wird ersetzt
      size_t l = strlen(line);
      if (l < 11 || line[l - 1] != '\n')
        continue; // leere oder abgeschnittene Zeile
      fputs(line, out);
    }
    fclose(in);
  }

  fprintf(out, "%s,%u,%s,%lu,%u,%u,%s,%lu,%u\n", current_date, b.peak, bleTime,
          (unsigned long)b.total,
          (unsigned)(b.peakIntervals * (STATS_INTERVAL_SECONDS / 60)), w.peak,
          wifiTime, (unsigned long)w.total,
          (unsigned)(w.peakIntervals * (STATS_INTERVAL_SECONDS / 60)));
  sync_file(out);
  fclose(out);

  // FAT kann nicht über eine bestehende Datei umbenennen
  unlink(pathOld);
  if (rename(pathTmp, pathOld) != 0)
    ESP_LOGE(TAG, "Rename %s -> %s failed", pathTmp, pathOld);

  ESP_LOGI(TAG, "Daily summary %s: BLE peak=%u at %s | total~%lu | peak duration=%umin",
           current_date, b.peak, bleTime, (unsigned long)b.total,
           (unsigned)(b.peakIntervals * (STATS_INTERVAL_SECONDS / 60)));
}

// Falls beim letzten Lauf der Strom genau zwischen unlink() und rename()
// weg war: .tmp wiederherstellen
static void recover_daily_summary(void) {
  char pathOld[64], pathTmp[64];
  mkpath(pathOld, sizeof(pathOld), DAILY_FILE);
  mkpath(pathTmp, sizeof(pathTmp), DAILY_TMP);
  if (access(pathOld, F_OK) != 0 && access(pathTmp, F_OK) == 0) {
    ESP_LOGW(TAG, "Recovering %s from %s", pathOld, pathTmp);
    rename(pathTmp, pathOld);
  }
}

// ---------------------------------------------------------------------------
// Laufendes Intervall abschließen -> stats_file + daily_summary.csv
// ---------------------------------------------------------------------------
static void flush_interval(void) {
  if (!useSDCard || !stats_file || cur.samples == 0)
    return;

  uint16_t prevBle = day.havePrev ? day.prevBlePeak : 0;
  uint16_t prevWifi = day.havePrev ? day.prevWifiPeak : 0;
  int bleDelta = day.havePrev ? (int)cur.blePeak - (int)prevBle : 0;
  int wifiDelta = day.havePrev ? (int)cur.wifiPeak - (int)prevWifi : 0;

  // Trend über 30 Min (aktuelles Intervall vs. vor MA_WINDOW_SIZE Intervallen),
  // ein 10-Min-Vergleich ist bei langsamem Zustrom zu träge/zu nervös
  PaxTrend_t bleTrend = day.havePrev
                            ? calc_trend(cur.blePeak, ma_oldest(&day.bleMa))
                            : TREND_STABLE;
  PaxTrend_t wifiTrend = day.havePrev
                             ? calc_trend(cur.wifiPeak, ma_oldest(&day.wifiMa))
                             : TREND_STABLE;
  PaxTrend_t trend = (cur.blePeak >= cur.wifiPeak) ? bleTrend : wifiTrend;

  // Laufendes Tagesmax/-min inkl. aktuellem Intervall, dann Phase bestimmen
  if (cur.blePeak > day.bleRunMax)
    day.bleRunMax = cur.blePeak;
  if (day.intervals == 0 || cur.blePeak < day.bleRunMin)
    day.bleRunMin = cur.blePeak;
  EventPhase_t phase = calc_phase(cur.blePeak, bleTrend);

  // MA inkl. aktuellem Intervall (day_apply pusht danach denselben Wert)
  MABuffer_t bleMa = day.bleMa, wifiMa = day.wifiMa;
  ma_push(&bleMa, cur.blePeak);
  ma_push(&wifiMa, cur.wifiPeak);
  uint16_t bleMa30 = ma_get(&bleMa);
  uint16_t wifiMa30 = ma_get(&wifiMa);

  char startBuf[24], endBuf[24];
  fmt_iso(startBuf, sizeof(startBuf), cur.start);
  fmt_iso(endBuf, sizeof(endBuf), cur.start + STATS_INTERVAL_SECONDS);

  fprintf(stats_file, "%s,%s,%u,%d,%u,%u,%d,%u,%s,%s\n", startBuf, endBuf,
          cur.blePeak, bleDelta, bleMa30, cur.wifiPeak, wifiDelta, wifiMa30,
          trend_str(trend), phase_str(phase));
  sync_file(stats_file);

  ESP_LOGI(TAG, "Interval %s: BLE peak=%u delta=%+d ma30=%u | WiFi peak=%u | %s | %s",
           startBuf, cur.blePeak, bleDelta, bleMa30, cur.wifiPeak,
           trend_str(trend), phase_str(phase));

  day_apply(cur.slot, cur.blePeak, cur.wifiPeak);
  cur.samples = 0;

  write_daily_summary();
}

// ---------------------------------------------------------------------------
// Tagesdateien öffnen / Tagesrollover. Liefert false, solange die Zeit
// ungültig ist (RTC/NTP noch nicht gestellt) oder die Karte fehlt.
// ---------------------------------------------------------------------------
static bool ensure_daily_files(time_t now) {
  if (!useSDCard)
    return false;
  if (!time_valid(now)) {
    static bool warned = false;
    if (!warned) {
      ESP_LOGW(TAG, "System time not valid yet - SD logging paused");
      warned = true;
    }
    return false;
  }

  struct tm tt;
  localtime_r(&now, &tt);
  char today[11];
  strftime(today, sizeof(today), "%Y-%m-%d", &tt);

  if (strcmp(today, current_date) == 0 && data_file && stats_file)
    return true;

  // Tageswechsel: letztes Intervall des alten Tages abschließen
  if (current_date[0] != 0)
    flush_interval();

  if (data_file) {
    fclose(data_file);
    data_file = NULL;
  }
  if (stats_file) {
    fclose(stats_file);
    stats_file = NULL;
  }

  strncpy(current_date, today, sizeof(current_date));
  day_reset();

  char name[40];

  // Rohdaten
  snprintf(name, sizeof(name), "/raw_%s.csv", today);
  if (openFile(&data_file, name)) {
    char header[64] = SDCARD_FILE_HEADER;
#if (defined BAT_MEASURE_ADC || defined HAS_PMU)
    strncat(header, SDCARD_FILE_HEADER_VOLTAGE, sizeof(header) - strlen(header) - 1);
#endif
#if (HAS_SDS011)
    strncat(header, SDCARD_FILE_HEADER_SDS011, sizeof(header) - strlen(header) - 1);
#endif
    strncat(header, "\n", sizeof(header) - strlen(header) - 1);
    writeHeaderIfEmpty(data_file, header);
  }

  // Statistik — vorher bestehende Intervalle von heute einlesen (Neustart)
  snprintf(name, sizeof(name), "/stats_%s.csv", today);
  day_restore_from_stats(name);
  if (openFile(&stats_file, name))
    writeHeaderIfEmpty(stats_file, SDCARD_STATS_HEADER);

  recover_daily_summary();

#if (SDLOGGING)
  // ein Logfile pro Boot, benannt nach dem Boot-Datum (kein Wechsel zur
  // Laufzeit, da andere Tasks jederzeit loggen können)
  if (!log_file) {
    FILE *lf = NULL;
    snprintf(name, sizeof(name), "/%s_%s.log", SDCARD_FILE_NAME, today);
    if (openFile(&lf, name)) {
      log_file = lf;
      esp_log_set_vprintf(&print_to_sd_card);
    }
  }
#endif

  return data_file && stats_file;
}

// ---------------------------------------------------------------------------
// SDLOGGING
// ---------------------------------------------------------------------------
#if (SDLOGGING)
int print_to_sd_card(const char *fmt, va_list args) {
  static bool fatal_error = false;
  static uint32_t write_counter = 0;
  static const uint32_t CACHE_CYCLE = 5;

  // va_list darf nur einmal verbraucht werden -> Kopie für die SD-Karte
  if (log_file && !fatal_error) {
    va_list copy;
    va_copy(copy, args);
    int result = vfprintf(log_file, fmt, copy);
    va_end(copy);
    if (result < 0) {
      fatal_error = true;
    } else if ((write_counter++ % CACHE_CYCLE) == 0) {
      fflush(log_file);
      fsync(fileno(log_file));
    }
  }
  return vprintf(fmt, args);
}
#endif

// ---------------------------------------------------------------------------
// sdcard_init() — Karte mounten. Dateien werden erst beim ersten Schreiben
// mit gültiger Uhrzeit geöffnet.
// ---------------------------------------------------------------------------
bool sdcard_init(bool create) {
  esp_err_t ret;

  esp_vfs_fat_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 8,
  };

  ESP_LOGI(TAG, "Initializing SD card...");

#if (HAS_SDCARD == 1)
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  spi_bus_config_t bus_cfg = {
      .mosi_io_num = (gpio_num_t)SDCARD_MOSI,
      .miso_io_num = (gpio_num_t)SDCARD_MISO,
      .sclk_io_num = (gpio_num_t)SDCARD_SCLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4000,
  };
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = (gpio_num_t)SDCARD_CS;

  ret = spi_bus_initialize(SPI_HOST, &bus_cfg, 1);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
    return false;
  }
  ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

#elif (HAS_SDCARD == 2)
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = SDCARD_SLOTWIDTH;
#ifdef SDCARD_PULLUP
  slot_config.flags |= SDCARD_PULLUP;
#endif
  ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
#endif

  if (ret != ESP_OK) {
    ESP_LOGI(TAG, "SD card not found (%s)", esp_err_to_name(ret));
    return false;
  }

  useSDCard = true;
  ESP_LOGI(TAG, "SD card mounted");
  sdmmc_card_print_info(stdout, card);
  day_reset();

  // Dateien werden erst beim ersten sdcardWriteData() geöffnet: dann sind
  // Zeitzone (do_after_reset) und Uhrzeit (RTC/NTP) sicher gesetzt.
  return useSDCard;
}

// ---------------------------------------------------------------------------
// sdcard_flush() — zyklisch aus cyclic.cpp (HOMECYCLE)
// ---------------------------------------------------------------------------
void sdcard_flush(void) {
  sync_file(data_file);
  sync_file(stats_file);
#if (SDLOGGING)
  sync_file(log_file);
#endif
}

// ---------------------------------------------------------------------------
// sdcard_close() — vor Deep Sleep / Reset
// ---------------------------------------------------------------------------
void sdcard_close(void) {
  if (!useSDCard)
    return;

  ESP_LOGI(TAG, "Closing SD card...");
  flush_interval(); // schreibt auch daily_summary.csv
  sdcard_flush();

#if (SDLOGGING)
  esp_log_set_vprintf(&vprintf);
  log_file = NULL;
#endif

  fcloseall();
  data_file = NULL;
  stats_file = NULL;
  esp_vfs_fat_sdcard_unmount(mount_point, card);
  useSDCard = false;
  ESP_LOGI(TAG, "SD card unmounted");
}

// ---------------------------------------------------------------------------
// sdcardWriteData() — wird pro Messzyklus aus senddata.cpp aufgerufen
// ---------------------------------------------------------------------------
void sdcardWriteData(uint16_t noWifi, uint16_t noBle,
                     __attribute__((unused)) uint16_t voltage) {
  time_t now = time(NULL);
  if (!ensure_daily_files(now))
    return;

  struct tm tt;
  localtime_r(&now, &tt);
  char timeBuffer[24];
  strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%dT%H:%M:%S", &tt);

  // --- Rohdaten schreiben (jede Zeile sofort sichern) ---
  fprintf(data_file, "%s,%u,%u", timeBuffer, noWifi, noBle);
#if (defined BAT_MEASURE_ADC || defined HAS_PMU)
  fprintf(data_file, ",%u", voltage);
#endif
#if (HAS_SDS011)
  sdsStatus_t sds;
  sds011_store(&sds);
  fprintf(data_file, ",%5.1f,%4.1f", sds.pm10 / 10.0f, sds.pm25 / 10.0f);
#endif
  fputc('\n', data_file);
  sync_file(data_file);

  ESP_LOGD(TAG, "[%s] WiFi=%u BLE=%u", timeBuffer, noWifi, noBle);

  // --- Intervall-Aggregation ---
  uint16_t slot = slot_of(&tt);

  // Intervallwechsel ZUERST prüfen, damit der aktuelle Messwert nicht noch
  // dem alten Intervall zugerechnet wird
  if (cur.samples > 0 && slot != cur.slot)
    flush_interval();

  if (cur.samples == 0) {
    uint32_t sod = tt.tm_hour * 3600UL + tt.tm_min * 60UL + tt.tm_sec;
    cur.start = now - (sod % STATS_INTERVAL_SECONDS);
    cur.slot = slot;
    cur.blePeak = 0;
    cur.wifiPeak = 0;
  }

  if (noBle > cur.blePeak)
    cur.blePeak = noBle;
  if (noWifi > cur.wifiPeak)
    cur.wifiPeak = noWifi;
  cur.samples++;
}

#endif // HAS_SDCARD
