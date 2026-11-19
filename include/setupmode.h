#pragma once

// =============================================================================
// setupmode.h — Einrichtungsmodus mit Weboberfläche (Custom Build)
//
// Nach einem Kaltstart spannt das Gerät für einige Minuten ein eigenes WLAN
// "Paxcounter-XXXX" auf (offen, Captive Portal, QR-Code auf dem Display).
// Im Browser lassen sich Status prüfen, Uhrzeit übernehmen, die Empfindlichkeit
// einlernen, Messdaten herunterladen und Einstellungen ändern.
// Danach startet das Gerät neu und zählt wie gewohnt (WiFi aus).
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#define SETUP_DEFAULT_MINUTES 10
#define SETUP_ACTIVITY_EXTEND_S 180 // nach jeder Bedienung mind. noch 3 Min.
#define SETUP_AP_CHANNEL 1
#define SETUP_MAX_DEVICES 160       // je Funkart, für den Lernmodus
#define SETUP_DEVICE_TIMEOUT_MS 30000

// Dauerhaft gespeicherte Einstellungen (NVS-Namespace "paxsetup")
typedef struct {
  int8_t thrBle;       // RSSI-Grenze Bluetooth [dBm]
  int8_t thrWifi;      // RSSI-Grenze WLAN [dBm]
  uint8_t setupMin;    // Einrichtungszeit nach dem Einschalten [min]
  char site[48];       // Name des Messorts
  char ssid[33];       // optional: WLAN für den NTP-Zeitabgleich
  char pass[65];
} setupPrefs_t;

extern setupPrefs_t setupPrefs;

void setup_prefs_load(void);
void setup_prefs_save(void);

// Läuft blockierend, solange der Einrichtungsmodus aktiv ist, und startet das
// Gerät danach neu. Kehrt nur zurück, wenn der Modus nicht gestartet wurde.
void setupmode_run(void);
