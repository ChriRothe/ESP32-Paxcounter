# Paxcounter Projektdokumentation
**Stand: Oktober 2026 – Custom Build v1.2.0**

---

## 1. Projektübersicht

Ein angepasster ESP32-Paxcounter zur Besucherzählung bei Events und Veranstaltungen.
Basis ist der [ESP32-Paxcounter](https://github.com/cyberman54/ESP32-Paxcounter) v3.7.1
von Oliver Brandmüller / Klaus Wilting.

**Einsatzzweck:** Anonyme Erfassung von BLE- und WiFi-Geräten als Indikator für die
Personenzahl. Im Vordergrund stehen Spitzenzeiten und der Tagesverlauf.

**Repository:** https://github.com/ChriRothe/ESP32-Paxcounter

---

## 2. Hardware

| Komponente | Details |
|---|---|
| Board | LILYGO TTGO LoRa32 V2.1 (1.6.x), ESP32 (kein S3) – `shared/hal/ttgov21new.h` |
| RTC | DS3231 + AT24C32 Kombiboard (ZS-042, „blaues Modul“) |
| SD-Karte | integrierter Slot (SDMMC), FAT32 |
| LoRa | SX1276, 868 MHz – **deaktiviert** (`HAS_LORA 0`) |
| Display | OLED SSD1306 (on board) |

**I²C-Pinbelegung (gemeinsamer Bus mit dem OLED):**
```
DS3231 SDA → GPIO 21
DS3231 SCL → GPIO 22
DS3231 VCC → 3.3V
DS3231 GND → GND
```

`HAS_RTC` ist ein Feature-Flag (= 1) und **kein Pin**. Die Pins kommen deshalb in
`src/rtctime.cpp` aus `MY_DISPLAY_SDA`/`MY_DISPLAY_SCL` (Fallback 21/22) oder aus
optionalen `RTC_SDA`/`RTC_SCL` in der HAL-Datei.

---

## 3. Boot-Ablauf und Zeit

```
setup()
 ├─ sdcard_init()          Karte mounten (Dateien werden noch NICHT geöffnet)
 ├─ do_after_reset()       u. a. TZ = TIME_SYNC_TIMEZONE setzen
 ├─ i2c_init(), Display …
 ├─ rtc_init()             DS3231 starten
 ├─ ntp_sync_and_set_rtc() nur bei Kaltstart: WLAN → NTP → DS3231 → WLAN AUS
 ├─ init_libpax()          BLE-/WiFi-Sniffer starten
 └─ time_init()            Systemzeit zyklisch aus der RTC (TIME_SYNC_INTERVAL)
```

- **Die Reihenfolge ist wichtig:** Der NTP-Sync muss *vor* `init_libpax()` laufen. Bis
  v1.1 lief er danach. `WiFi.mode(WIFI_OFF)` hat dann den WiFi-Sniffer abgeschaltet, und
  WiFi zählte dauerhaft 0 (siehe Beobachtungen in Abschnitt 13).
- Für NTP wird `configTzTime(TIME_SYNC_TIMEZONE, …)` verwendet. Das früher genutzte
  `configTime(0, 0, …)` setzt intern `TZ=UTC0` und hat damit die MEZ/MESZ-Einstellung
  überschrieben.
- Ein erfolgreicher Sync wird über den SNTP-Status erkannt (`SNTP_SYNC_STATUS_COMPLETED`),
  nicht nur über „Jahr > 2024“.
- Die RTC speichert die UTC-Epoch. In Lokalzeit umgerechnet wird nur bei der Ausgabe.
- Zeitzone: `TIME_SYNC_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"` = letzter Sonntag im März
  bzw. Oktober. Der Original-Eintrag `M3.4.0/2,M10.4.0/3` (4. Sonntag) lag in manchen
  Jahren eine Woche daneben.
- **WLAN-Zugangsdaten:** `shared/ota.conf` (`OTA_WIFI_SSID`/`OTA_WIFI_PASS`).
  `shared/build.py` stellt sie im Code als `WIFI_SSID`/`WIFI_PASS` bereit.
- Schlägt der NTP-Sync fehl (kein WLAN), läuft das Gerät mit der RTC-Zeit weiter.

---

## 4. SD-Karten-Datenstruktur

Die Dateien werden erst geöffnet, wenn die Uhrzeit gültig ist (ab 2024). Dateien wie
`raw_1970-01-01.csv` entstehen dadurch nicht mehr. Der Tageswechsel erfolgt um
Mitternacht **Lokalzeit**.

### `raw_YYYY-MM-DD.csv` — Rohdaten
Eine Zeile pro Messzyklus (`SENDCYCLE 30` = alle 60 s). Jede Zeile wird sofort gesichert
(`fflush` + `fsync`).
```
Timestamp,WiFi,BLE,Voltage_mV
2026-03-06T08:03:12,0,42,4422
```

### `stats_YYYY-MM-DD.csv` — 10-Minuten-Aggregation
```
IntervalStart,IntervalEnd,BLE_Peak,BLE_Delta,BLE_MA30,WiFi_Peak,WiFi_Delta,WiFi_MA30,Trend,Phase
2026-03-06T13:30:00,2026-03-06T13:40:00,152,4,148,38,1,37,rising,arriving
```

| Spalte | Bedeutung |
|---|---|
| `*_Peak` | Maximalwert im Intervall (wichtigste Kennzahl) |
| `*_Delta` | Veränderung zum vorherigen Intervall (erstes Intervall des Tages: 0) |
| `*_MA30` | gleitender Durchschnitt über 3 Intervalle (30 min) inkl. aktuellem |
| `Trend` | `rising` / `falling` / `stable`: aktueller Peak im Vergleich zu vor 30 min, Schwelle ±10 % |
| `Phase` | `pre-event` / `arriving` / `peak` / `leaving` / `post-event` |

**Phasen-Logik** (online, kennt nur den bisherigen Tagesverlauf):
- Solange es keinen ausgeprägten Verlauf gibt, gilt `pre-event` bzw. `arriving` bei
  steigendem Trend. Das ist der Fall, wenn das Tagesmax < `PHASE_MIN_DEVICES` (5) liegt,
  das Tagesmax < 2 × Tagesminimum (Grundlast) beträgt oder noch weniger als 3 Intervalle
  vorliegen.
- `arriving`: Der Trend steigt noch, der Peak ist noch nicht erreicht.
- `peak`: mindestens 80 % des bisherigen Tagesmax und kein steigender Trend mehr.
- `leaving`: nach dem Peak, zwischen 30 und 80 % des Tagesmax.
- `post-event`: nach dem Peak, unter 30 % des Tagesmax.

### `daily_summary.csv` — Tageszusammenfassung (eine Datei für alle Tage)
```
Date,BLE_DayPeak,BLE_PeakTime,BLE_TotalVisitors,BLE_PeakDuration_min,WiFi_DayPeak,WiFi_PeakTime,WiFi_TotalVisitors,WiFi_PeakDuration_min
2026-03-06,155,2026-03-06T14:00:00,4594,160,38,2026-03-06T13:30:00,1116,160
```
- Die Datei wird **nach jedem Intervall neu geschrieben**: Die Zeile des aktuellen Tages wird
  ersetzt, alle anderen Tage bleiben erhalten. Geschrieben wird zuerst in `.tmp`, danach wird
  umbenannt. Zieht man den Strom, fehlen also höchstens die letzten 10 Minuten.
- `BLE_TotalVisitors`: Summe aller Intervall-Peaks. Das ist ein Näherungswert, kein exakter
  Zähler, weil Personen mehrfach gezählt werden.
- `BLE_PeakDuration_min`: Minuten mit mindestens 80 % des **endgültigen** Tagesmax.
  Berechnet wird das aus allen Intervall-Peaks des Tages, die im RAM gehalten werden
  (144 Slots).

### `paxcount_YYYY-MM-DD.log` — Systemlog (`SDLOGGING 1`)
Ein Logfile pro Boot, benannt nach dem Boot-Datum. Es startet, sobald die Uhrzeit gültig
ist. Frühere Boot-Meldungen erscheinen nur auf der seriellen Konsole.

### Verhalten bei Neustart / Stromausfall
- Beim Öffnen der Tagesdateien wird `stats_<heute>.csv` eingelesen. Tagesmax,
  gleitender Durchschnitt, Phase und die Slots für die Tageszusammenfassung laufen dann
  nahtlos weiter.
- Das zum Zeitpunkt des Stromausfalls laufende 10-Minuten-Intervall fehlt in `stats`.
  Die Rohdaten sind vollständig.
- Nach einem Neustart im selben Intervall kann ein Intervall doppelt in `stats` stehen.
  Für die Tageszusammenfassung zählt dann das Maximum.
- Die SD-Karte muss vor dem Start eingesteckt sein. Hot-Plug wird nicht erkannt.

---

## 5. Zeitstempel

Alle Zeitstempel sind **ISO 8601 in Lokalzeit (MEZ/MESZ) ohne Suffix**, z. B.
`2026-03-06T08:03:12`. Excel erkennt sie direkt als Datum/Uhrzeit.

**Achtung bei Daten vor v1.2:** Diese wurden in **UTC mit `Z`** geschrieben
(`2026-03-06T08:03:12Z`, 20 Zeichen), und der Tageswechsel lag bei 01:00 bzw. 02:00 Uhr
Lokalzeit. Neue Daten haben 19 Zeichen.

Bei der Zeitumstellung im Oktober kommt die Stunde 02:00–03:00 doppelt vor. In den
Rohdaten ist das an den Zeitstempeln erkennbar. In `daily_summary` zählt pro
10-Minuten-Slot das Maximum.

---

## 6. Bekannte Eigenheiten / Fixes

| Problem | Ursache | Fix (Version) |
|---|---|---|
| WiFi zählt dauerhaft 0 | NTP-Sync lief nach `init_libpax()`, `WIFI_OFF` hat den Sniffer abgeschaltet | NTP-Sync vor `init_libpax()` (1.2) |
| Lokalzeit falsch / UTC | `configTime(0,0)` setzt `TZ=UTC0`, SD-Code nutzte `gmtime` | `configTzTime()`, `localtime_r` (1.2) |
| Sommerzeit eine Woche daneben | `M3.4.0` = 4. statt letzter Sonntag | `M3.5.0,M10.5.0/3` (1.2) |
| `raw_1970-01-01.csv` | SD-Dateien wurden vor dem Stellen der RTC geöffnet | Dateien erst bei gültiger Zeit öffnen (1.2) |
| `rtc_init()` doppelt | Integrations-Schnipsel wörtlich in `main.cpp` eingefügt | `main.cpp` bereinigt (1.2) |
| Phase sofort `peak`, `arriving` fehlt | Vergleich mit laufendem Max, `peakReached` nie zurückgesetzt | neue Phasen-Logik (1.2) |
| PeakDuration zu hoch | gegen laufendes statt endgültiges Tagesmax gezählt | Berechnung aus Tages-Slots (1.2) |
| Tageszusammenfassung fehlt / doppelt | nur beim Tageswechsel geschrieben, Reset bei Neustart | Neuschreiben pro Intervall + Restore (1.2) |
| erster Messwert im falschen Intervall | Peak vor dem Intervallwechsel übernommen | Reihenfolge getauscht (1.2) |
| doppelte CSV-Header | `fgetpos()` liefert im Append-Modus 0 | `fseek(END)` + `ftell()` (1.2) |
| SD-Log-Absturz möglich | `va_list` zweimal verwendet | `va_copy()` (1.2) |
| Build-Fehler `WIFI_CHANNEL_ALL` | libpax hat die API umbenannt (`LIBPAX_WIFI_CHANNEL_*`) | Namen angepasst, libpax auf Commit gepinnt (1.2) |
| `rtc_init()`-Konflikt | eigene `rtc_ds3231.h` kollidierte mit `rtctime.h` | nur `rtctime.cpp` verwenden (1.0) |
| `TAG`-Makro-Konflikt | globales `TAG`-Makro | `#undef TAG` in `sdcard.cpp`/`ntp_sync.cpp` (1.0) |

---

## 7. Projektdateien (Custom Build)

| Datei | Beschreibung |
|---|---|
| `src/sdcard.cpp`, `include/sdcard.h` | SD-Logging, Aggregation, Tageszusammenfassung |
| `src/ntp_sync.cpp`, `include/ntp_sync.h` | NTP-Sync beim Kaltstart |
| `src/rtctime.cpp` | DS3231 mit expliziten I²C-Pins |
| `src/main.cpp` | Boot-Reihenfolge RTC → NTP → libpax |
| `include/custom_version.h` | Build-Version, Ausgabe im Boot-Log |
| `shared/hal/ttgov21new.h` | Board: LoRa aus, RTC an, SD an |
| `shared/paxcounter.conf` | Laufzeit-Konfiguration (im Repo versioniert) |
| `platformio.ini` | Build-Konfiguration (im Repo versioniert, libpax gepinnt) |
| `shared/ota.conf`, `shared/loraconf.h` | Zugangsdaten, **nicht** im Repo (Vorlagen `*_sample.*`) |
| `.github/workflows/build.yml` | GitHub Actions: Build, bei Tag `v*` Release mit `.bin` |

---

## 8. Custom Version

`include/custom_version.h`. Bei Änderungen `CUSTOM_BUILD_VERSION` hochzählen und
Abschnitt 15 (Changelog) ergänzen. Die Version erscheint beim Boot im Log:
```
>>> Custom Build v1.2.0 | DS3231-RTC | NTP-Boot-Sync | ... | built Oct 27 2026 21:30:00 <<<
```

Release auf GitHub: einen Tag `v1.2.0` pushen. GitHub Actions baut die Firmware und hängt
die `.bin`-Dateien an das Release. Diese Builds enthalten nur Platzhalter-WLAN-Daten.

---

## 9. Konfigurierbare Parameter

`include/sdcard.h`:
```cpp
#define STATS_INTERVAL_SECONDS 600   // Aggregationsintervall (Teiler von 86400)
#define MA_WINDOW_SIZE         3     // MA30 = 3 x 10 Min, zugleich Trend-Horizont
#define PEAK_PHASE_THRESHOLD   0.80f // ab 80% des Tagesmax = "peak"
#define POST_EVENT_RATIO       0.30f // nach dem Peak unter 30% = "post-event"
#define TREND_CHANGE_PERCENT   0.10f // ab 10% Änderung ggü. vor 30 Min = rising/falling
#define PHASE_MIN_INTERVALS    3
#define PHASE_MIN_DEVICES      5
#define PHASE_BASELINE_FACTOR  2
```

`include/ntp_sync.h`:
```cpp
#define NTP_SERVER_1 "de.pool.ntp.org"
#define NTP_WIFI_TIMEOUT_MS 15000
#define NTP_SYNC_TIMEOUT_MS 10000
#define NTP_RETRY_COUNT     3
```

`shared/paxcounter.conf` (Auszug): `BLECOUNTER 1`, `WIFICOUNTER 1`, `RSSILIMIT -70`,
`WIFI_MY_COUNTRY "DE"`, `DISPLAYCYCLE 10`, `SDLOGGING 1`, `USE_OTA 0`,
`TIME_SYNC_INTERVAL 10` (Systemzeit alle 10 min aus der RTC).

---

## 10. Build & Upload

1. `shared/ota_sample.conf` nach `shared/ota.conf` kopieren und SSID/Passwort eintragen.
2. `shared/loraconf_sample.h` nach `shared/loraconf.h` kopieren.
3. VS Code / PlatformIO: Umgebung `usb`, **Upload**. Monitor mit 115200 Baud.

Nach Änderungen an `paxcounter.conf` immer zuerst **Clean** ausführen.

Geprüft am 27.10.2026: Build mit espressif32@6.4.0 (arduino-esp32 2.0.11) erfolgreich,
RAM 21,9 %, Flash 58,9 %. Die SD-Logik wurde zusätzlich auf dem PC simuliert:
Tageswechsel, Neustart mitten am Tag, Umstellung auf Sommerzeit (29.03.2026, 138
Intervalle).

---

## 11. Auswertung / Diagramme

Das Python-Script `analyze.py` (nicht im Repo) erzeugt vier Charts aus den CSV-Dateien.

Hinweis zum Einlesen: Die letzte Zeile kann abgeschnitten sein, wenn der Strom während des
Schreibens weg war. Statt über die Länge (`== 20`) besser über das Parsen filtern. Das
funktioniert für alte (UTC, `Z`) und neue (Lokalzeit) Daten:
```python
raw["Timestamp"] = pd.to_datetime(raw["Timestamp"], errors="coerce", utc=False)
raw = raw.dropna(subset=["Timestamp"])
```

---

## 12. Beobachtungen aus ersten Messungen (06.–08.03.2026)

- BLE dominiert, WiFi praktisch bei 0. **Ursache war sehr wahrscheinlich der Bug aus
  Abschnitt 3:** Der WiFi-Sniffer wurde nach dem NTP-Sync abgeschaltet. Ab v1.2 erneut
  prüfen. Moderne Smartphones senden wegen MAC-Randomisierung ohnehin wenige
  Probe-Requests, BLE bleibt daher die Hauptkennzahl.
- Am 06.03. war deutlich mehr los (BLE Ø 88, Max 161) als am 07./08.03. (Ø ~35).
- Ein klarer Tagesverlauf mit Anstieg bis zum Nachmittag ist erkennbar.
- Die Zeitstempel dieser Messungen sind UTC (siehe Abschnitt 5).

---

## 13. Nächste Schritte / Offene Punkte

- [ ] v1.2 auf dem Board testen: Boot-Log (NTP vor libpax), WiFi-Werte > 0, Lokalzeit in den CSVs
- [ ] Upstream v3.8.0 (u. a. libpax-Umzug nach `dbinfrago/libpax`, neuere Libs) einpflegen
- [ ] `analyze.py` ins Repo aufnehmen (`tools/`)
- [ ] MQTT-Anbindung via HiveMQ Cloud für Live-Daten
- [ ] Telegram-Bot für Push-Benachrichtigungen
- [ ] Neue Hardware evaluieren: LILYGO T3-S3 + externes DS3231
- [ ] 3D-Druck-Case: Thingiverse thing:6572887 (T3-S3 Slimline mit SD-Slot)

---

## 14. Changelog

| Version | Datum | Änderungen |
|---|---|---|
| 1.2.0 | 27.10.2026 | NTP vor libpax (WiFi-Zähler-Fix), Lokalzeit (`configTzTime`, `localtime_r`), DST-Regel korrigiert, SD-Dateien erst bei gültiger Zeit, neue Phasen-/Trend-Logik (30 min), Tageszusammenfassung pro Intervall und neustartsicher, Header-/va_list-Fixes, `main.cpp` bereinigt, libpax gepinnt + API-Umbenennung, `.gitignore` repariert, CI für das eigene Board |
| 1.1 | 03/2026 | MEZ/MESZ beim NTP-Sync, Doku |
| 1.0 | 02/2026 | DS3231-RTC, NTP-Boot-Sync, SD-Aggregation (10 min), ISO-8601-Zeitstempel |
