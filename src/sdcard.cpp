// =============================================================================
// sdcard.cpp — SD-Karten Interface für ESP32-Paxcounter
// Optimiert für Event/Veranstaltungs-Auswertung
// =============================================================================

#ifdef HAS_SDCARD

#include "sdcard.h"
#include "esp_log.h"
#include "time.h"
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#undef TAG
static const char *TAG = "SD";

// ---------------------------------------------------------------------------
// Interne Zustandsvariablen
// ---------------------------------------------------------------------------
sdmmc_card_t  *card;
const char     mount_point[] = MOUNT_POINT;
static bool    useSDCard      = false;

static FILE   *data_file      = NULL;   // raw_YYYY-MM-DD.csv
static FILE   *stats_file     = NULL;   // stats_YYYY-MM-DD.csv
static FILE   *daily_file     = NULL;   // daily_YYYY-MM-DD.csv

static char    current_date[11] = {0};

static PaxAggBuffer_t aggBuf  = {0};
static DailyStats_t   dayStats = {0};

#if (SDLOGGING)
static FILE   *log_file    = NULL;
static FILE   *uart_stdout = NULL;
#endif

// ---------------------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------------------
static void get_date_string(char *buf, size_t len) {
    time_t t = time(NULL);
    struct tm tt;
    gmtime_r(&t, &tt);
    strftime(buf, len, "%Y-%m-%d", &tt);
}

static bool openFile(FILE **fd, const char *filename) {
    char path[64];
    snprintf(path, sizeof(path), "%s%s", MOUNT_POINT, filename);
    if ((*fd = fopen(path, "a")) == NULL) {
        ESP_LOGE(TAG, "Cannot open: %s", path);
        return false;
    }
    ESP_LOGI(TAG, "Opened: %s", path);
    return true;
}

static void writeHeaderIfEmpty(FILE *fd, const char *header) {
    if (!fd) return;
    fpos_t pos;
    fgetpos(fd, &pos);
    if (pos == 0) fprintf(fd, "%s", header);
}

// ---------------------------------------------------------------------------
// Gleitender Durchschnitt — Ringpuffer
// ---------------------------------------------------------------------------
static void ma_push(MABuffer_t *ma, uint16_t value) {
    ma->values[ma->head] = value;
    ma->head = (ma->head + 1) % MA_WINDOW_SIZE;
    if (ma->count < MA_WINDOW_SIZE) ma->count++;
}

static uint16_t ma_get(const MABuffer_t *ma) {
    if (ma->count == 0) return 0;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < ma->count; i++) sum += ma->values[i];
    return (uint16_t)(sum / ma->count);
}

// ---------------------------------------------------------------------------
// Trend bestimmen
// ---------------------------------------------------------------------------
static PaxTrend_t calc_trend(uint16_t current, uint16_t previous) {
    if (previous == 0) return TREND_STABLE;
    int diff = (int)current - (int)previous;
    if (diff >  (int)(previous * TREND_CHANGE_PERCENT)) return TREND_RISING;
    if (diff < -(int)(previous * TREND_CHANGE_PERCENT)) return TREND_FALLING;
    return TREND_STABLE;
}

static const char* trend_str(PaxTrend_t t) {
    switch(t) {
        case TREND_RISING:  return "rising";
        case TREND_FALLING: return "falling";
        default:            return "stable";
    }
}

// ---------------------------------------------------------------------------
// Event-Phase bestimmen
// Logik:
//   - Kein Tagespeak bisher + niedrig       → pre-event
//   - Kein Tagespeak bisher + rising        → arriving
//   - Über 80% des Tagesmax                 → peak
//   - Peak bereits erreicht + falling       → leaving
//   - Peak bereits erreicht + stabil/niedrig→ post-event
// ---------------------------------------------------------------------------
static EventPhase_t calc_phase(uint16_t blePeak, PaxTrend_t trend) {
    if (dayStats.bleDayPeak == 0) return PHASE_PRE_EVENT;

    float ratio = (float)blePeak / (float)dayStats.bleDayPeak;

    if (ratio >= PEAK_PHASE_THRESHOLD)
        return PHASE_PEAK;

    if (!aggBuf.peakReached) {
        return (trend == TREND_RISING) ? PHASE_ARRIVING : PHASE_PRE_EVENT;
    } else {
        return (trend == TREND_FALLING || ratio < 0.3f) ? PHASE_POST_EVENT : PHASE_LEAVING;
    }
}

static const char* phase_str(EventPhase_t p) {
    switch(p) {
        case PHASE_PRE_EVENT:  return "pre-event";
        case PHASE_ARRIVING:   return "arriving";
        case PHASE_PEAK:       return "peak";
        case PHASE_LEAVING:    return "leaving";
        case PHASE_POST_EVENT: return "post-event";
        default:               return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Tages-Tracking aktualisieren
// ---------------------------------------------------------------------------
static void update_daily_stats(uint16_t blePeak, uint16_t wifiPeak, uint32_t intervalStart) {
    // BLE Tagespeak
    if (blePeak > dayStats.bleDayPeak) {
        dayStats.bleDayPeak  = blePeak;
        dayStats.blePeakTime = intervalStart;
    }
    // WiFi Tagespeak
    if (wifiPeak > dayStats.wifiDayPeak) {
        dayStats.wifiDayPeak  = wifiPeak;
        dayStats.wifiPeakTime = intervalStart;
    }
    // Gesamtbesucher (Summe der Intervall-Peaks als Näherung)
    dayStats.bleTotalVisitors  += blePeak;
    dayStats.wifiTotalVisitors += wifiPeak;

    // Peak-Dauer: Intervalle über 80% des Tagesmax
    if (dayStats.bleDayPeak > 0 &&
        (float)blePeak / dayStats.bleDayPeak >= PEAK_PHASE_THRESHOLD) {
        dayStats.blePeakIntervals++;
        aggBuf.peakReached = true;
    }
    if (dayStats.wifiDayPeak > 0 &&
        (float)wifiPeak / dayStats.wifiDayPeak >= PEAK_PHASE_THRESHOLD) {
        dayStats.wifiPeakIntervals++;
    }
}

// ---------------------------------------------------------------------------
// Intervall abschließen und in stats_file schreiben
// ---------------------------------------------------------------------------
static void flush_interval(void) {
    if (!useSDCard || !stats_file || !aggBuf.initialized) return;
    if (aggBuf.blePeak == 0 && aggBuf.wifiPeak == 0) return;

    // Gleitenden Durchschnitt aktualisieren
    ma_push(&aggBuf.bleMa,  aggBuf.blePeak);
    ma_push(&aggBuf.wifiMa, aggBuf.wifiPeak);
    uint16_t bleMa30  = ma_get(&aggBuf.bleMa);
    uint16_t wifiMa30 = ma_get(&aggBuf.wifiMa);

    // Delta zum vorherigen Intervall
    int16_t bleDelta  = (int16_t)aggBuf.blePeak  - (int16_t)aggBuf.prevBlePeak;
    int16_t wifiDelta = (int16_t)aggBuf.wifiPeak - (int16_t)aggBuf.prevWifiPeak;

    // Trend & Phase
    PaxTrend_t  bleTrend  = calc_trend(aggBuf.blePeak,  aggBuf.prevBlePeak);
    PaxTrend_t  wifiTrend = calc_trend(aggBuf.wifiPeak, aggBuf.prevWifiPeak);
    PaxTrend_t  trend     = (aggBuf.blePeak >= aggBuf.wifiPeak) ? bleTrend : wifiTrend;

    // Tages-Tracking aktualisieren BEVOR Phase berechnet wird
    update_daily_stats(aggBuf.blePeak, aggBuf.wifiPeak, aggBuf.intervalStart);

    EventPhase_t phase = calc_phase(aggBuf.blePeak, bleTrend);

    // Zeitstempel
    char startBuf[22], endBuf[22];
    struct tm ts, te;
    time_t tStart = (time_t)aggBuf.intervalStart;
    time_t tEnd   = (time_t)(aggBuf.intervalStart + STATS_INTERVAL_SECONDS);
    gmtime_r(&tStart, &ts);
    gmtime_r(&tEnd,   &te);
    strftime(startBuf, sizeof(startBuf), "%Y-%m-%dT%H:%M:%SZ", &ts);
    strftime(endBuf,   sizeof(endBuf),   "%Y-%m-%dT%H:%M:%SZ", &te);

    fprintf(stats_file,
            "%s,%s,"       // IntervalStart, IntervalEnd
            "%d,%d,%d,"    // BLE_Peak, BLE_Delta, BLE_MA30
            "%d,%d,%d,"    // WiFi_Peak, WiFi_Delta, WiFi_MA30
            "%s,%s\n",     // Trend, Phase
            startBuf, endBuf,
            aggBuf.blePeak,  bleDelta,  bleMa30,
            aggBuf.wifiPeak, wifiDelta, wifiMa30,
            trend_str(trend), phase_str(phase)
    );

    fsync(fileno(stats_file));

    ESP_LOGI(TAG, "Interval: BLE peak=%d delta=%+d ma30=%d | phase=%s | trend=%s",
             aggBuf.blePeak, bleDelta, bleMa30, phase_str(phase), trend_str(trend));

    // Vorherige Peaks für nächstes Intervall merken
    aggBuf.prevBlePeak  = aggBuf.blePeak;
    aggBuf.prevWifiPeak = aggBuf.wifiPeak;
}

// ---------------------------------------------------------------------------
// Neues Intervall starten
// ---------------------------------------------------------------------------
static void start_new_interval(uint32_t startTime) {
    aggBuf.intervalStart = startTime;
    aggBuf.blePeak       = 0;
    aggBuf.wifiPeak      = 0;
    aggBuf.initialized   = true;
    // MA-Puffer und peakReached bleiben erhalten über Intervallgrenzen!
}

// ---------------------------------------------------------------------------
// sdcard_reset_aggregation()
// ---------------------------------------------------------------------------
void sdcard_reset_aggregation(void) {
    uint16_t prevBle  = aggBuf.prevBlePeak;
    uint16_t prevWifi = aggBuf.prevWifiPeak;
    MABuffer_t bleMa  = aggBuf.bleMa;
    MABuffer_t wifiMa = aggBuf.wifiMa;
    bool peakReached  = aggBuf.peakReached;

    memset(&aggBuf, 0, sizeof(aggBuf));

    // MA-Puffer und Kontext über Tagesgrenzen erhalten
    aggBuf.prevBlePeak  = prevBle;
    aggBuf.prevWifiPeak = prevWifi;
    aggBuf.bleMa        = bleMa;
    aggBuf.wifiMa       = wifiMa;
    aggBuf.peakReached  = peakReached;
    aggBuf.initialized  = false;
}

// ---------------------------------------------------------------------------
// sdcard_write_daily_summary() — Tageszusammenfassung schreiben
// ---------------------------------------------------------------------------
void sdcard_write_daily_summary(void) {
    if (!useSDCard || !daily_file) return;
    if (dayStats.bleDayPeak == 0 && dayStats.wifiDayPeak == 0) return;

    char peakTimeBle[22]  = "N/A";
    char peakTimeWifi[22] = "N/A";

    if (dayStats.blePeakTime > 0) {
        struct tm t;
        time_t pt = (time_t)dayStats.blePeakTime;
        gmtime_r(&pt, &t);
        strftime(peakTimeBle, sizeof(peakTimeBle), "%Y-%m-%dT%H:%M:%SZ", &t);
    }
    if (dayStats.wifiPeakTime > 0) {
        struct tm t;
        time_t pt = (time_t)dayStats.wifiPeakTime;
        gmtime_r(&pt, &t);
        strftime(peakTimeWifi, sizeof(peakTimeWifi), "%Y-%m-%dT%H:%M:%SZ", &t);
    }

    fprintf(daily_file,
            "%s,"           // Date
            "%d,%s,%lu,%d," // BLE: DayPeak, PeakTime, TotalVisitors, PeakDuration_min
            "%d,%s,%lu,%d\n",// WiFi: DayPeak, PeakTime, TotalVisitors, PeakDuration_min
            current_date,
            dayStats.bleDayPeak,  peakTimeBle,
            (unsigned long)dayStats.bleTotalVisitors,
            dayStats.blePeakIntervals * (STATS_INTERVAL_SECONDS / 60),
            dayStats.wifiDayPeak, peakTimeWifi,
            (unsigned long)dayStats.wifiTotalVisitors,
            dayStats.wifiPeakIntervals * (STATS_INTERVAL_SECONDS / 60)
    );

    fsync(fileno(daily_file));

    ESP_LOGI(TAG, "Daily summary: BLE peak=%d at %s | total~%lu | peak duration=%dmin",
             dayStats.bleDayPeak, peakTimeBle,
             (unsigned long)dayStats.bleTotalVisitors,
             dayStats.blePeakIntervals * (STATS_INTERVAL_SECONDS / 60));
}

// ---------------------------------------------------------------------------
// sdcard_flush_stats() — manuell abschließen
// ---------------------------------------------------------------------------
void sdcard_flush_stats(void) {
    flush_interval();
    sdcard_reset_aggregation();
}

// ---------------------------------------------------------------------------
// Tagesdateien öffnen / Tagesrollover
// ---------------------------------------------------------------------------
static void ensure_daily_files(void) {
    char today[11];
    get_date_string(today, sizeof(today));

    if (strcmp(today, current_date) == 0 && data_file && stats_file) return;

    // Tageswechsel: altes Intervall + Tageszusammenfassung abschließen
    if (aggBuf.initialized) flush_interval();
    if (strlen(current_date) > 0) sdcard_write_daily_summary();

    if (data_file)  { fclose(data_file);  data_file  = NULL; }
    if (stats_file) { fclose(stats_file); stats_file = NULL; }
    if (daily_file) { fclose(daily_file); daily_file = NULL; }

    strncpy(current_date, today, sizeof(current_date));

    // Rohdaten
    char buf[64];
    snprintf(buf, sizeof(buf), "/raw_%s.csv", today);
    if (openFile(&data_file, buf)) {
        char header[128];
        snprintf(header, sizeof(header), "%s", SDCARD_FILE_HEADER);
#if (defined BAT_MEASURE_ADC || defined HAS_PMU)
        strncat(header, SDCARD_FILE_HEADER_VOLTAGE, sizeof(header) - strlen(header) - 1);
#endif
#if (HAS_SDS011)
        strncat(header, SDCARD_FILE_HEADER_SDS011, sizeof(header) - strlen(header) - 1);
#endif
        strncat(header, "\n", sizeof(header) - strlen(header) - 1);
        writeHeaderIfEmpty(data_file, header);
    } else { useSDCard = false; }

    // Statistik
    snprintf(buf, sizeof(buf), "/stats_%s.csv", today);
    if (openFile(&stats_file, buf)) {
        writeHeaderIfEmpty(stats_file, SDCARD_STATS_HEADER);
    } else { useSDCard = false; }

    // Tageszusammenfassung — eine Datei für alle Tage (append)
    if (openFile(&daily_file, "/daily_summary.csv")) {
        writeHeaderIfEmpty(daily_file, SDCARD_DAILY_HEADER);
    } else { useSDCard = false; }

    // Tages-Tracking zurücksetzen
    memset(&dayStats, 0, sizeof(dayStats));
    sdcard_reset_aggregation();
}

// ---------------------------------------------------------------------------
// SDLOGGING
// ---------------------------------------------------------------------------
#if (SDLOGGING)
int print_to_sd_card(const char *fmt, va_list args) {
    static bool     fatal_error  = false;
    static uint32_t write_counter = 0;
    static const uint32_t CACHE_CYCLE = 5;

    if (!log_file || fatal_error) return vprintf(fmt, args);

    int result = vfprintf(log_file, fmt, args);
    if (result < 0) {
        printf("SD logging write error\n");
        fatal_error = true;
    } else {
        if ((write_counter++ % CACHE_CYCLE) == 0)
            fsync(fileno(log_file));
    }
    return vprintf(fmt, args);
}
#endif

// ---------------------------------------------------------------------------
// sdcard_init()
// ---------------------------------------------------------------------------
bool sdcard_init(bool create) {
    esp_err_t ret;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 6,
    };

    ESP_LOGI(TAG, "Initializing SD card...");

#if (HAS_SDCARD == 1)
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = (gpio_num_t)SDCARD_MOSI,
        .miso_io_num     = (gpio_num_t)SDCARD_MISO,
        .sclk_io_num     = (gpio_num_t)SDCARD_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
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

    ensure_daily_files();

#if (SDLOGGING)
    char logname[64];
    snprintf(logname, sizeof(logname), "/paxcount_%s.log", current_date);
    if (openFile(&log_file, logname)) {
        uart_stdout = stdout;
        esp_log_set_vprintf(&print_to_sd_card);
    }
#endif

    return useSDCard;
}

// ---------------------------------------------------------------------------
// sdcard_flush()
// ---------------------------------------------------------------------------
void sdcard_flush(void) {
    if (data_file)  fsync(fileno(data_file));
    if (stats_file) fsync(fileno(stats_file));
    if (daily_file) fsync(fileno(daily_file));
#if (SDLOGGING)
    if (log_file)   fsync(fileno(log_file));
#endif
}

// ---------------------------------------------------------------------------
// sdcard_close()
// ---------------------------------------------------------------------------
void sdcard_close(void) {
    if (!useSDCard) return;

    ESP_LOGI(TAG, "Closing SD card...");
    flush_interval();
    sdcard_write_daily_summary();
    sdcard_flush();

#if (SDLOGGING)
    esp_log_set_vprintf(&vprintf);
#endif

    fcloseall();
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    useSDCard = false;
    ESP_LOGI(TAG, "SD card unmounted");
}

// ---------------------------------------------------------------------------
// sdcardWriteData() — Herzstück
// ---------------------------------------------------------------------------
void sdcardWriteData(uint16_t noWifi, uint16_t noBle,
                     __attribute__((unused)) uint16_t voltage) {
    if (!useSDCard) return;

    ensure_daily_files();

    time_t now = time(NULL);
    struct tm tt;
    gmtime_r(&now, &tt);

    char timeBuffer[22];
    strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%dT%H:%M:%SZ", &tt);

    // --- Rohdaten schreiben ---
    fprintf(data_file, "%s,%d,%d", timeBuffer, noWifi, noBle);
#if (defined BAT_MEASURE_ADC || defined HAS_PMU)
    fprintf(data_file, ",%d", voltage);
#endif
#if (HAS_SDS011)
    sdsStatus_t sds;
    sds011_store(&sds);
    fprintf(data_file, ",%5.1f,%4.1f", sds.pm10 / 10.0f, sds.pm25 / 10.0f);
#endif
    fprintf(data_file, "\n");

    static uint32_t writeCount = 0;
    if ((++writeCount % 5) == 0) fsync(fileno(data_file));

    ESP_LOGI(TAG, "[%s] WiFi=%d BLE=%d", timeBuffer, noWifi, noBle);

    // --- Aggregationspuffer aktualisieren ---
    uint32_t nowEpoch = (uint32_t)now;

    if (!aggBuf.initialized) {
        uint32_t intervalStart = nowEpoch - (nowEpoch % STATS_INTERVAL_SECONDS);
        start_new_interval(intervalStart);
    }

    // Peak im laufenden Intervall tracken
    if (noBle  > aggBuf.blePeak)  aggBuf.blePeak  = noBle;
    if (noWifi > aggBuf.wifiPeak) aggBuf.wifiPeak = noWifi;

    // Intervall abgelaufen?
    if (nowEpoch >= aggBuf.intervalStart + STATS_INTERVAL_SECONDS) {
        flush_interval();
        uint32_t intervalStart = nowEpoch - (nowEpoch % STATS_INTERVAL_SECONDS);
        start_new_interval(intervalStart);
        // Erstes Sample des neuen Intervalls
        if (noBle  > aggBuf.blePeak)  aggBuf.blePeak  = noBle;
        if (noWifi > aggBuf.wifiPeak) aggBuf.wifiPeak = noWifi;
    }
}

#endif // HAS_SDCARD
