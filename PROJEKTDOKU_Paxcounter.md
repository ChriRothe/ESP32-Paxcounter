# Paxcounter Projektdokumentation
**Stand: März 2026**

---

## 1. Projektübersicht

Angepasster ESP32-Paxcounter zur Besucherzählung bei Events/Veranstaltungen.
Basis ist der originale [ESP32-Paxcounter](https://github.com/cyberman54/ESP32-Paxcounter) von Oliver Brandmueller / Klaus Wilting.

**Einsatzzweck:** Anonyme Erfassung von BLE- und WiFi-Geräten als Indikator für Personenanzahl bei Veranstaltungen. Schwerpunkt auf Spitzenzeiten und Tagesverlauf.

---

## 2. Hardware

| Komponente | Details |
|---|---|
| Board | LILYGO TTGO LoRa32 (ESP32, nicht S3) |
| RTC | DS3231 + AT24C32 Kombiboard (ZS-042, "blaues Modul") |
| SD-Karte | Via SPI, FAT32 formatiert |
| LoRa | SX1276, 868MHz (Europa) |
| Display | Kleines OLED (bereits im Original vorhanden) |

**I²C Pinbelegung (TTGO LoRa32):**
```
DS3231 SDA → GPIO 21
DS3231 SCL → GPIO 22
DS3231 VCC → 3.3V
DS3231 GND → GND
```

**Wichtig:** `HAS_RTC` ist im Projekt ein Feature-Flag (= 1), **kein Pin**. Deshalb muss `Wire.begin(21, 22)` und `Rtc.Begin(21, 22)` mit expliziten Pins aufgerufen werden — nicht `Wire.begin(HAS_RTC)`.

---

## 3. RTC-Integration

Das Projekt nutzt die **Makuna RTC-Bibliothek** (`RtcDS3231`) — keine eigene I²C-Implementierung.

**Relevante Datei:** `src/rtctime.cpp` / `include/rtctime.h`

Der originale Code hatte einen Bug:
```cpp
// FALSCH (original):
Wire.begin(HAS_RTC);   // HAS_RTC = 1, kein Pin!
Rtc.Begin(HAS_RTC);

// RICHTIG (gefixt):
Wire.begin(21, 22);
Rtc.Begin(21, 22);
```

**Zeitfluss beim Boot:**
1. `rtc_init()` → DS3231 lesen → `settimeofday()` setzt ESP32-Systemzeit
2. `ntp_sync_and_set_rtc()` → WiFi → NTP → `set_rtctime()` aktualisiert DS3231
3. Danach läuft alles über `time(NULL)` — kein dauerhaftes WiFi nötig

---

## 4. NTP-Synchronisation

**Datei:** `src/ntp_sync.cpp` / `include/ntp_sync.h`

Wird nur bei `RUNMODE_POWERCYCLE` (Kaltstart) ausgeführt. WiFi wird danach komplett deaktiviert damit der Sniffer-Modus nicht gestört wird.

**Zeitzone:** Mitteleuropäische Zeit mit automatischer Sommer/Winterzeit:
```cpp
setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
tzset();
configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
```

**WiFi-Zugangsdaten** kommen aus `ota.conf` und sind im Projekt als `WIFI_SSID` / `WIFI_PASS` definiert (nicht `OTA_WIFI_SSID`).

**Einbindung in `main.cpp`** — direkt nach dem `HAS_RTC`-Block, vor `init_libpax()`:
```cpp
#ifdef HAS_RTC
  if (RTC_runmode == RUNMODE_POWERCYCLE) {
    ntp_sync_and_set_rtc();
  }
#endif
```

---

## 5. SD-Karten Datenstruktur

Drei Dateitypen werden täglich neu angelegt (automatischer Tagesrollover):

### `raw_YYYY-MM-DD.csv` — Rohdaten
Jede Einzelmessung, ca. 1x pro Minute:
```
Timestamp,WiFi,BLE,Voltage_mV
2026-03-06T08:03:12,0,42,4422
```

### `stats_YYYY-MM-DD.csv` — 10-Minuten-Aggregation
```
IntervalStart,IntervalEnd,BLE_Peak,BLE_Delta,BLE_MA30,WiFi_Peak,WiFi_Delta,WiFi_MA30,Trend,Phase
2026-03-06T08:00:00Z,2026-03-06T08:10:00Z,50,-3,45,0,0,0,stable,arriving
```

**Spalten erklärt:**
- `BLE_Peak` — Maximalwert im Intervall (wichtigste Kennzahl)
- `BLE_Delta` — Veränderung zum Vorinterval (+/- absolut)
- `BLE_MA30` — Gleitender 30-Minuten-Durchschnitt (3 Intervalle à 10 Min)
- `Trend` — `rising` / `falling` / `stable` (ab 10% Änderung)
- `Phase` — `pre-event` / `arriving` / `peak` / `leaving` / `post-event`

**Phase-Logik:**
- `peak` wenn aktueller Wert > 80% des Tagesmaximums
- `arriving` wenn noch kein Peak erreicht + Trend rising
- `leaving` wenn Peak bereits erreicht + Trend falling
- Schwellwert konfigurierbar: `PEAK_PHASE_THRESHOLD 0.80f`

### `daily_summary.csv` — Tageszusammenfassung (eine Datei für alle Tage)
```
Date,BLE_DayPeak,BLE_PeakTime,BLE_TotalVisitors,BLE_PeakDuration_min,...
2026-03-06,161,2026-03-06T14:20:00Z,4823,40,...
```

- `BLE_TotalVisitors` — Summe aller Intervall-Peaks (Näherungswert, kein exakter Zähler)
- `BLE_PeakDuration_min` — Minuten über 80% des Tagesmaximums

**Wichtig:** SD-Karte muss vor dem Start eingesteckt sein — Hot-Plug wird nicht erkannt.

---

## 6. Timestamps

Alle Zeitstempel im Format **ISO 8601**:
```
2026-03-06T08:03:12
```
Kein `Z`-Suffix mehr — Zeit ist MEZ/MESZ (lokale Zeit).

Excel erkennt dieses Format direkt als Datum/Uhrzeit.

---

## 7. Bekannte Eigenheiten / Fixes

| Problem | Ursache | Fix |
|---|---|---|
| `rtc_init()` Konflikt | Eigene `rtc_ds3231.h` kollidiert mit `rtctime.h` | `rtc_ds3231.h/.cpp` löschen, nur `rtctime.cpp` verwenden |
| `TAG` Makro-Konflikt | Globales `TAG`-Makro im Projekt | `#undef TAG` vor eigener Definition in `sdcard.cpp` und `ntp_sync.cpp` |
| `fsync` nicht deklariert | Fehlendes Include | `#include <unistd.h>` in `sdcard.cpp` |
| `OTA_WIFI_SSID` unbekannt | Falscher Makroname | Korrekte Namen sind `WIFI_SSID` / `WIFI_PASS` |
| `SDCARD_SLOTCONFIG` unbekannt | Projekt-spezifisches Makro fehlt | `SDMMC_SLOT_CONFIG_DEFAULT()` verwenden |
| Abgeschnittene letzte CSV-Zeile | SD-Karte gezogen während Schreibvorgang | Beim Einlesen filtern: `raw['Timestamp'].str.len() == 20` |

---

## 8. Projektdateien (geändert / neu)

| Datei | Status | Beschreibung |
|---|---|---|
| `src/rtctime.cpp` | **Geändert** | `Wire.begin(21,22)` / `Rtc.Begin(21,22)` |
| `src/sdcard.cpp` | **Neu** | Vollständige Neuentwicklung mit Aggregation |
| `include/sdcard.h` | **Neu** | Neue Datenstrukturen für Event-Auswertung |
| `src/ntp_sync.cpp` | **Neu** | NTP-Boot-Sync mit MEZ/MESZ Zeitzone |
| `include/ntp_sync.h` | **Neu** | Header für NTP-Sync |
| `include/custom_version.h` | **Neu** | Build-Versionierung |
| `include/rtc_ds3231.h` | **LÖSCHEN** | Kollidiert mit rtctime.h |
| `src/rtc_ds3231.cpp` | **LÖSCHEN** | Nicht benötigt |

---

## 9. Einbindung in main.h / main.cpp

In `include/main.h` folgende Includes hinzufügen:
```cpp
#include "ntp_sync.h"
#include "custom_version.h"
```

In `main.cpp` nach dem RTC-Init-Block:
```cpp
// Custom Build Info
ESP_LOGI(TAG, ">>> Custom Build v%s | %s | built %s %s <<<",
         CUSTOM_BUILD_VERSION, CUSTOM_BUILD_FEATURES,
         CUSTOM_BUILD_DATE, CUSTOM_BUILD_TIME);

// NTP Sync (nur bei Kaltstart)
#ifdef HAS_RTC
  if (RTC_runmode == RUNMODE_POWERCYCLE) {
    ntp_sync_and_set_rtc();
  }
#endif
```

---

## 10. Custom Version

**Datei:** `include/custom_version.h`

Bei Änderungen bitte `CUSTOM_BUILD_VERSION` hochzählen und `CUSTOM_BUILD_FEATURES` ergänzen:
```cpp
#define CUSTOM_BUILD_VERSION  "1.1"
#define CUSTOM_BUILD_FEATURES "DS3231-RTC | NTP-Boot-Sync | SD-Aggregation(10min) | ISO8601 | MEZ/MESZ"
```

---

## 11. Konfigurierbare Parameter

In `include/sdcard.h`:
```cpp
#define STATS_INTERVAL_SECONDS    600     // Aggregationsintervall (Standard 10 Min)
#define MA_WINDOW_SIZE            3       // MA30 = 3 x 10 Min
#define PEAK_PHASE_THRESHOLD      0.80f   // Ab 80% des Tagesmax = "peak"
#define TREND_CHANGE_PERCENT      0.10f   // Ab 10% Änderung = rising/falling
```

In `include/ntp_sync.h`:
```cpp
#define NTP_WIFI_TIMEOUT_MS   15000   // Max. Wartezeit WiFi-Verbindung
#define NTP_SYNC_TIMEOUT_MS   10000   // Max. Wartezeit NTP-Antwort
#define NTP_RETRY_COUNT       3       // Versuche bei fehlgeschlagenem Sync
```

---

## 12. Auswertung / Diagramme

Python-Script `analyze.py` erzeugt 4 Charts aus den CSV-Dateien:
- `chart1_rohdaten.png` — Zeitverlauf pro Tag
- `chart2_aggregation.png` — 10-Min-Aggregation mit Min/Max Band
- `chart3_tagesvergleich.png` — Stündliche Durchschnitte im Tagesvergleich
- `chart4_verteilung.png` — Boxplot Verteilung pro Tag

**Pandas-Hinweis:** Letzte Zeile jeder CSV kann abgeschnitten sein → vor dem Einlesen filtern:
```python
raw = raw[raw['Timestamp'].str.len() == 20]
```

---

## 13. Beobachtungen aus ersten Messungen (06.-08.03.2026)

- BLE dominiert, WiFi praktisch bei 0 → Standort ohne aktive WiFi-Geräte
- 06.03. deutlich verkehrsreicher (BLE Ø 88, Max 161) vs. 07./08.03. (Ø ~35)
- Klarer Tagesverlauf mit Anstieg bis Nachmittag erkennbar

---

## 14. Nächste Schritte / Offene Punkte

- [ ] MQTT-Anbindung via HiveMQ Cloud für Live-Daten
- [ ] Telegram-Bot für Push-Benachrichtigungen
- [ ] Neue Hardware evaluieren: LILYGO T3-S3 + externes DS3231
- [ ] 3D-Druck Case: Thingiverse thing:6572887 (T3-S3 Slimline mit SD-Slot)
- [ ] Display-Rotationsgeschwindigkeit: `DISPLAYCYCLE` in `paxcounter.conf` anpassen
