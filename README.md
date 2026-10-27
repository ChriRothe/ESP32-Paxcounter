# ESP32-Paxcounter – Event-Edition

Angepasste Version des [ESP32-Paxcounter](https://github.com/cyberman54/ESP32-Paxcounter)
von Oliver Brandmüller / Klaus Wilting zur **anonymen Besucherzählung bei Veranstaltungen**.
Gezählt werden BLE- und WiFi-Geräte in Reichweite. Die Daten landen auf der SD-Karte und
werden dort direkt für die Auswertung aufbereitet: Spitzenzeiten, Tagesverlauf, Event-Phasen.

[![Firmware Build](https://github.com/ChriRothe/ESP32-Paxcounter/actions/workflows/build.yml/badge.svg)](https://github.com/ChriRothe/ESP32-Paxcounter/actions/workflows/build.yml)

**Ausführliche Projektdokumentation:** [PROJEKTDOKU_Paxcounter.md](PROJEKTDOKU_Paxcounter.md)

## Unterschiede zum Original

- **LoRaWAN deaktiviert** (der Code ist noch vorhanden, `HAS_LORA 0`)
- **Status-LED ruhig** (kein Dauerblinken für den LoRa-Join)
- **DS3231-RTC** mit **NTP-Sync beim Kaltstart**: Das Gerät verbindet sich kurz mit dem WLAN,
  stellt die RTC und schaltet WiFi danach wieder ab. Das passiert **vor** dem Start des
  Sniffers, sonst würde der WiFi-Zähler dauerhaft 0 zeigen.
- **SD-Logging für Events**:
  - `raw_YYYY-MM-DD.csv` – Rohdaten pro Messzyklus
  - `stats_YYYY-MM-DD.csv` – 10-Minuten-Intervalle mit Peak, Delta, gleitendem 30-Minuten-Mittel, Trend und Phase
  - `daily_summary.csv` – eine Zeile pro Tag, wird nach jedem Intervall aktualisiert
  - Zeitstempel im Format ISO 8601 in **Lokalzeit (MEZ/MESZ)**, ohne Suffix, damit Excel sie direkt versteht
  - neustart- und stromausfallsicher: Nach einem Reboot wird der Tageskontext von der Karte gelesen

## Hardware

| Komponente | Details |
|---|---|
| Board | LILYGO TTGO LoRa32 V2.1 (1.6.x), ESP32 – HAL-Datei `ttgov21new.h` |
| RTC | DS3231 + AT24C32 (ZS-042), I²C an GPIO 21 (SDA) / 22 (SCL), 3,3 V |
| SD-Karte | integrierter Slot, FAT32, **vor dem Einschalten einstecken** |
| Display | integriertes OLED (SSD1306) |

## Bauen & Flashen

Voraussetzung: VS Code mit der PlatformIO-Erweiterung.

1. Repo klonen und in VS Code öffnen.
2. Zugangsdaten anlegen (diese Dateien sind per `.gitignore` ausgeschlossen):
   ```
   shared/ota_sample.conf    -> shared/ota.conf     (WLAN-SSID/Passwort für den NTP-Sync eintragen)
   shared/loraconf_sample.h  -> shared/loraconf.h   (unverändert lassen, LoRa ist aus)
   ```
3. Board per USB anschließen und in PlatformIO **Upload** (Umgebung `usb`) ausführen.
4. Optional: Den **Serial Monitor** (115200 Baud) öffnen. Beim Start erscheint
   `>>> Custom Build v1.2.0 ...`, danach `Boot NTP sync successful`.

Nach Änderungen an `shared/paxcounter.conf` immer zuerst **Clean** und dann **Build** ausführen.
Gespeicherte Einstellungen im NVRAM lassen sich mit `pio run -t erase` zurücksetzen.

Die Firmware aus dem GitHub-Actions-Build enthält nur Platzhalter-WLAN-Daten. Sie dient
zur Build-Kontrolle; für den Einsatz bitte lokal mit der eigenen `ota.conf` flashen.

## Wichtige Einstellungen

| Datei | Parameter | Wert |
|---|---|---|
| `shared/paxcounter.conf` | `BLECOUNTER` / `WIFICOUNTER` | 1 / 1 |
| | `RSSILIMIT` | -70 dBm (Reichweite begrenzen) |
| | `SENDCYCLE` | 30 (= Messzyklus 60 s) |
| | `TIME_SYNC_TIMEZONE` | `CET-1CEST,M3.5.0,M10.5.0/3` |
| | `SDLOGGING` | 1 (Systemlog auf SD) |
| `include/sdcard.h` | `STATS_INTERVAL_SECONDS` | 600 |
| | `PEAK_PHASE_THRESHOLD` | 0.80 |
| `include/ntp_sync.h` | NTP-Server, Timeouts | |

## Lizenz

Copyright 2018-2022 Oliver Brandmueller <ob@sysadm.in>  
Copyright 2018-2022 Klaus Wilting <verkehrsrot@arcor.de>  
Anpassungen: Christian Rothe

Licensed under the Apache License, Version 2.0 – siehe [LICENSE](LICENSE).
Teile des Quellcodes stehen unter anderen Lizenzen, siehe die jeweiligen Dateien.

## Credits

Dank an alle Beitragenden des Originalprojekts, u. a.
[Oliver Brandmüller](https://github.com/spmrider) (Idee & Grundgerüst),
[Charles Hallard](https://github.com/hallard),
[robbi5](https://github.com/robbi5),
[Caspar Armster](https://www.dasdigidings.de/),
[terrillmoore](https://github.com/mcci-catena),
[sbamueller](https://github.com/sbamueller),
[Stefan](https://github.com/nerdyscout),
[August Quint](https://github.com/AugustQu) (SD-Logger),
[t-huyeng](https://github.com/t-huyeng),
[TD-er](https://github.com/TD-er).
