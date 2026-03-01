// =============================================================================
// sdcard.cpp — SD-Karten Interface für ESP32-Paxcounter
// Erweiterungen:
//   • Zeitstempel via DS3231 RTC (präzise UTC-Zeit)
//   • Dateiname enthält Datum (täglicher Rollover)
//   • 10-Minuten-Aggregation: Avg, Min, Max, Trend, Peak-Erkennung
//   • Zwei Dateien: raw_YYYY-MM-DD.csv und stats_YYYY-MM-DD.csv
// =============================================================================

#ifdef HAS_SDCARD

#include "sdcard.h"
#include "esp_log.h"
#include "time.h"
#include <string.h>
#include <sys/time.h>
#include <unistd.h>   // fsync(), fileno()

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

static char    current_date[11] = {0};  // "YYYY-MM-DD" — für Tagesrollover

// Aggregationspuffer
static PaxAggBuffer_t aggBuf = {0};

#if (SDLOGGING)
static FILE   *log_file       = NULL;
static FILE   *uart_stdout    = NULL;
#endif

// ---------------------------------------------------------------------------
// Hilfsfunktion: aktuelles Datum als String
// ---------------------------------------------------------------------------
static void get_date_string(char *buf, size_t len) {
    time_t t = time(NULL);
    struct tm tt;
    gmtime_r(&t, &tt);
    strftime(buf, len, "%Y-%m-%d", &tt);
}

// ---------------------------------------------------------------------------
// Hilfsfunktion: Datei öffnen (append)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Hilfsfunktion: Header schreiben wenn Datei leer
// ---------------------------------------------------------------------------
static void writeHeaderIfEmpty(FILE *fd, const char *header) {
    if (!fd) return;
    fpos_t pos;
    fgetpos(fd, &pos);
    if (pos == 0) {
        fprintf(fd, "%s", header);
    }
}

// ---------------------------------------------------------------------------
// Tagesdateien öffnen (oder wiederverwenden wenn Tag gleich)
// Wird bei jedem sdcardWriteData-Aufruf geprüft → automatischer Tagesrollover
// ---------------------------------------------------------------------------
static void ensure_daily_files(void) {
    char today[11];
    get_date_string(today, sizeof(today));

    // Datum hat sich nicht geändert und Dateien sind offen → nichts tun
    if (strcmp(today, current_date) == 0 && data_file && stats_file) return;

    // Datum hat sich geändert oder erster Aufruf → Dateien wechseln
    if (data_file)  { fclose(data_file);  data_file  = NULL; }
    if (stats_file) { fclose(stats_file); stats_file = NULL; }

    strncpy(current_date, today, sizeof(current_date));

    // Rohdaten-Datei
    char buf[64];
    snprintf(buf, sizeof(buf), "/raw_%s.csv", today);
    if (openFile(&data_file, buf)) {
        // Header aufbauen
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
    } else {
        useSDCard = false;
    }

    // Statistik-Datei
    snprintf(buf, sizeof(buf), "/stats_%s.csv", today);
    if (openFile(&stats_file, buf)) {
        writeHeaderIfEmpty(stats_file, SDCARD_STATS_HEADER);
    } else {
        useSDCard = false;
    }

    // Bei Tageswechsel Aggregationspuffer zurücksetzen
    sdcard_reset_aggregation();
}

// ---------------------------------------------------------------------------
// Trend bestimmen
// ---------------------------------------------------------------------------
static PaxTrend_t calc_trend(uint16_t current, uint16_t previous) {
    if (previous == 0) return TREND_STABLE;
    int diff = (int)current - (int)previous;
    if (diff >  (int)(previous * 0.10f)) return TREND_RISING;
    if (diff < -(int)(previous * 0.10f)) return TREND_FALLING;
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
// Aggregations-Intervall abschließen und in stats_file schreiben
// ---------------------------------------------------------------------------
static void flush_interval(void) {
    if (!useSDCard || !stats_file || aggBuf.sampleCount == 0) return;

    uint16_t wifiAvg = (uint16_t)(aggBuf.wifiSum / aggBuf.sampleCount);
    uint16_t bleAvg  = (uint16_t)(aggBuf.bleSum  / aggBuf.sampleCount);

    // Trend (Vergleich mit vorherigem Intervall)
    PaxTrend_t wifiTrend = calc_trend(wifiAvg, aggBuf.prevWifiAvg);
    PaxTrend_t bleTrend  = calc_trend(bleAvg,  aggBuf.prevBleAvg);

    // Trend-String kombiniert
    char trendStr[32];
    if (wifiTrend == bleTrend) {
        snprintf(trendStr, sizeof(trendStr), "%s", trend_str(wifiTrend));
    } else {
        snprintf(trendStr, sizeof(trendStr), "wifi:%s/ble:%s",
                 trend_str(wifiTrend), trend_str(bleTrend));
    }

    // Peak-Erkennung: War ein Wert deutlich über dem Durchschnitt?
    bool wifiPeak = (aggBuf.wifiMax > (uint16_t)(wifiAvg * PEAK_THRESHOLD_FACTOR) && wifiAvg > 0);
    bool blePeak  = (aggBuf.bleMax  > (uint16_t)(bleAvg  * PEAK_THRESHOLD_FACTOR) && bleAvg  > 0);

    // Zeitstempel für Intervallstart und -ende
    char startBuf[22], endBuf[22];
    struct tm ts, te;
    time_t tStart = (time_t)aggBuf.intervalStart;
    time_t tEnd   = (time_t)(aggBuf.intervalStart + STATS_INTERVAL_SECONDS);
    gmtime_r(&tStart, &ts);
    gmtime_r(&tEnd,   &te);
    strftime(startBuf, sizeof(startBuf), "%Y-%m-%dT%H:%M:%SZ", &ts);
    strftime(endBuf,   sizeof(endBuf),   "%Y-%m-%dT%H:%M:%SZ", &te);

    fprintf(stats_file,
            "%s,%s,"        // IntervalStart, IntervalEnd
            "%.1f,%.1f,"    // WiFi_Avg, BLE_Avg
            "%d,%d,"        // WiFi_Max, BLE_Max
            "%d,%d,"        // WiFi_Min, BLE_Min
            "%lu,"          // Samples
            "%s,"           // Trend
            "%d,%d\n",      // Peak_WiFi, Peak_BLE
            startBuf, endBuf,
            (float)wifiAvg, (float)bleAvg,
            aggBuf.wifiMax, aggBuf.bleMax,
            aggBuf.wifiMin, aggBuf.bleMin,
            (unsigned long)aggBuf.sampleCount,
            trendStr,
            (int)wifiPeak, (int)blePeak
    );

    fsync(fileno(stats_file));

    ESP_LOGI(TAG, "Stats flushed: WiFi avg=%d max=%d | BLE avg=%d max=%d | %lu samples | trend=%s | peaks=(%d,%d)",
             wifiAvg, aggBuf.wifiMax, bleAvg, aggBuf.bleMax,
             (unsigned long)aggBuf.sampleCount, trendStr,
             (int)wifiPeak, (int)blePeak);

    // Vorherige Durchschnitte für nächstes Intervall merken
    aggBuf.prevWifiAvg = wifiAvg;
    aggBuf.prevBleAvg  = bleAvg;
}

// ---------------------------------------------------------------------------
// Aggregationspuffer für neues Intervall initialisieren
// ---------------------------------------------------------------------------
static void start_new_interval(uint32_t startTime, uint16_t firstWifi, uint16_t firstBle) {
    aggBuf.intervalStart = startTime;
    aggBuf.sampleCount   = 1;
    aggBuf.wifiSum       = firstWifi;
    aggBuf.bleSum        = firstBle;
    aggBuf.wifiMax       = firstWifi;
    aggBuf.bleMax        = firstBle;
    aggBuf.wifiMin       = firstWifi;
    aggBuf.bleMin        = firstBle;
    aggBuf.initialized   = true;
}

// ---------------------------------------------------------------------------
// sdcard_reset_aggregation()
// ---------------------------------------------------------------------------
void sdcard_reset_aggregation(void) {
    memset(&aggBuf, 0, sizeof(aggBuf));
    aggBuf.initialized = false;
}

// ---------------------------------------------------------------------------
// sdcard_flush_stats() — manuell abschließen
// ---------------------------------------------------------------------------
void sdcard_flush_stats(void) {
    flush_interval();
    sdcard_reset_aggregation();
}

// ---------------------------------------------------------------------------
// SDLOGGING: Ausgabe auf SD umleiten
// ---------------------------------------------------------------------------
#if (SDLOGGING)
int print_to_sd_card(const char *fmt, va_list args) {
    static bool    fatal_error       = false;
    static uint32_t write_counter    = 0;
    static const uint32_t CACHE_CYCLE = 5;

    if (!log_file || fatal_error) return vprintf(fmt, args);

    int result = vfprintf(log_file, fmt, args);
    if (result < 0) {
        printf("SD logging write error — disabling SD logging\n");
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
        .max_files              = 5,
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
        ESP_LOGI(TAG, "SD card not found or mount failed (%s)", esp_err_to_name(ret));
        return false;
    }

    useSDCard = true;
    ESP_LOGI(TAG, "SD card mounted");
    sdmmc_card_print_info(stdout, card);

    // Tages-Dateien öffnen (inkl. Header)
    ensure_daily_files();

    // Aggregationspuffer initialisieren
    sdcard_reset_aggregation();

#if (SDLOGGING)
    char logname[64];
    snprintf(logname, sizeof(logname), "/paxcount_%s.log", current_date);
    if (openFile(&log_file, logname)) {
        uart_stdout = stdout;
        esp_log_set_vprintf(&print_to_sd_card);
        ESP_LOGI(TAG, "SD logging active");
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

    // Letztes offenes Intervall noch schreiben
    flush_interval();
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
// Schreibt Rohdaten und aktualisiert Aggregationspuffer
// ---------------------------------------------------------------------------
void sdcardWriteData(uint16_t noWifi, uint16_t noBle,
                     __attribute__((unused)) uint16_t voltage) {
    if (!useSDCard) return;

    // Tagesrollover prüfen und ggf. neue Dateien öffnen
    ensure_daily_files();

    // Präziser UTC-Zeitstempel
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

    // Periodisch auf Disk flushen (jede 5. Messung)
    static uint32_t writeCount = 0;
    if ((++writeCount % 5) == 0)
        fsync(fileno(data_file));

    ESP_LOGI(TAG, "[%s] WiFi=%d BLE=%d", timeBuffer, noWifi, noBle);

    // --- Aggregationspuffer aktualisieren ---
    uint32_t nowEpoch = (uint32_t)now;

    if (!aggBuf.initialized) {
        // Erstes Sample: Intervall starten, auf Intervallgrenze runden
        uint32_t intervalStart = nowEpoch - (nowEpoch % STATS_INTERVAL_SECONDS);
        start_new_interval(intervalStart, noWifi, noBle);
    } else {
        // Prüfen ob aktuelles Intervall abgelaufen ist
        if (nowEpoch >= aggBuf.intervalStart + STATS_INTERVAL_SECONDS) {
            // Intervall abschließen und schreiben
            flush_interval();
            // Neues Intervall starten
            uint32_t intervalStart = nowEpoch - (nowEpoch % STATS_INTERVAL_SECONDS);
            start_new_interval(intervalStart, noWifi, noBle);
        } else {
            // Sample zum laufenden Intervall hinzufügen
            aggBuf.sampleCount++;
            aggBuf.wifiSum += noWifi;
            aggBuf.bleSum  += noBle;
            if (noWifi > aggBuf.wifiMax) aggBuf.wifiMax = noWifi;
            if (noBle  > aggBuf.bleMax)  aggBuf.bleMax  = noBle;
            if (noWifi < aggBuf.wifiMin) aggBuf.wifiMin = noWifi;
            if (noBle  < aggBuf.bleMin)  aggBuf.bleMin  = noBle;
        }
    }
}

#endif // HAS_SDCARD
