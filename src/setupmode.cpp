// =============================================================================
// setupmode.cpp — Einrichtungsmodus mit Weboberfläche (Custom Build)
// Siehe include/setupmode.h und web/setup.html
// =============================================================================

#include "globals.h"
#include "setupmode.h"
#include "configmanager.h"
#include "reset.h"
#include <Preferences.h>

#if (HAS_SETUPMODE)

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "ff.h"
#include "setup_html.h"

#ifdef HAS_DISPLAY
#include "display.h"
#endif
#ifdef HAS_RTC
#include "rtctime.h"
#endif
#if (defined BAT_MEASURE_ADC || defined HAS_PMU || defined HAS_IP5306)
#include "power.h"
#define SETUP_HAS_BATT 1
#endif
#if (HAS_SDCARD)
#include "sdcard.h"
#endif
#include "custom_version.h"

#endif // HAS_SETUPMODE

#undef TAG
static const char *TAG = "SETUP";

setupPrefs_t setupPrefs;

// ---------------------------------------------------------------------------
// Einstellungen (NVS)
// ---------------------------------------------------------------------------
void setup_prefs_load(void) {
  Preferences p;
  int8_t def = (RSSILIMIT != 0) ? (int8_t)RSSILIMIT : -90;
  p.begin("paxsetup", true);
  setupPrefs.thrBle = p.getChar("thrBle", def);
  setupPrefs.thrWifi = p.getChar("thrWifi", def);
  setupPrefs.setupMin = p.getUChar("setupMin", SETUP_DEFAULT_MINUTES);
  p.getString("site", setupPrefs.site, sizeof(setupPrefs.site));
  p.getString("ssid", setupPrefs.ssid, sizeof(setupPrefs.ssid));
  p.getString("pass", setupPrefs.pass, sizeof(setupPrefs.pass));
  p.end();
  ESP_LOGI(TAG, "Prefs: BLE %d dBm, WiFi %d dBm, setup %u min, site '%s'",
           setupPrefs.thrBle, setupPrefs.thrWifi, setupPrefs.setupMin,
           setupPrefs.site);
}

void setup_prefs_save(void) {
  Preferences p;
  p.begin("paxsetup", false);
  p.putChar("thrBle", setupPrefs.thrBle);
  p.putChar("thrWifi", setupPrefs.thrWifi);
  p.putUChar("setupMin", setupPrefs.setupMin);
  p.putString("site", setupPrefs.site);
  p.putString("ssid", setupPrefs.ssid);
  p.putString("pass", setupPrefs.pass);
  p.end();
}

#if !(HAS_SETUPMODE)

void setupmode_run(void) {}

#else

// ---------------------------------------------------------------------------
// Geräte-Tabellen für den Lernmodus (anonymisiert: nur 16-bit-Kurzkennung)
// ---------------------------------------------------------------------------
typedef struct {
  uint32_t id;
  int8_t rssi;
  uint32_t seen;
} seenDev_t;

static seenDev_t bleDevs[SETUP_MAX_DEVICES];
static seenDev_t wifiDevs[SETUP_MAX_DEVICES];
static portMUX_TYPE devMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t salt;

static uint32_t mac_hash(const uint8_t *mac) {
  uint32_t h = 2166136261u ^ salt;
  for (int i = 0; i < 6; i++) {
    h ^= mac[i];
    h *= 16777619u;
  }
  return h ? h : 1;
}

static void dev_seen(seenDev_t *tab, const uint8_t *mac, int rssi) {
  uint32_t id = mac_hash(mac), now = millis();
  int freeSlot = -1, oldest = 0;
  portENTER_CRITICAL(&devMux);
  for (int i = 0; i < SETUP_MAX_DEVICES; i++) {
    if (tab[i].id == id) {
      // geglättet, damit die Anzeige nicht springt
      tab[i].rssi = (int8_t)((tab[i].rssi * 2 + rssi) / 3);
      tab[i].seen = now;
      portEXIT_CRITICAL(&devMux);
      return;
    }
    if (freeSlot < 0 && (tab[i].id == 0 || now - tab[i].seen > SETUP_DEVICE_TIMEOUT_MS))
      freeSlot = i;
    if (tab[i].seen < tab[oldest].seen)
      oldest = i;
  }
  int k = freeSlot >= 0 ? freeSlot : oldest;
  tab[k].id = id;
  tab[k].rssi = (int8_t)rssi;
  tab[k].seen = now;
  portEXIT_CRITICAL(&devMux);
}

static void dev_json(String &out, seenDev_t *tab) {
  uint32_t now = millis();
  char buf[24];
  bool first = true;
  out += '[';
  portENTER_CRITICAL(&devMux);
  for (int i = 0; i < SETUP_MAX_DEVICES; i++) {
    if (tab[i].id && now - tab[i].seen <= SETUP_DEVICE_TIMEOUT_MS) {
      snprintf(buf, sizeof(buf), "%s[\"%04x\",%d]", first ? "" : ",",
               (unsigned)(tab[i].id & 0xFFFF), tab[i].rssi);
      out += buf;
      first = false;
    }
  }
  portEXIT_CRITICAL(&devMux);
  out += ']';
}

// ---------------------------------------------------------------------------
// Funk: WLAN-Sniffer auf dem AP-Kanal + BLE-Scan
// ---------------------------------------------------------------------------
static void wifi_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT)
    return;
  const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
  if (p->rx_ctrl.sig_len < 24)
    return;
  const uint8_t *pl = p->payload;
  if (pl[0] != 0x40) // nur Probe-Requests, wie libpax
    return;
  dev_seen(wifiDevs, pl + 10, p->rx_ctrl.rssi);
}

static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
  case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
    esp_ble_gap_start_scanning(0); // 0 = dauerhaft
    break;
  case ESP_GAP_BLE_SCAN_RESULT_EVT:
    if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT)
      dev_seen(bleDevs, param->scan_rst.bda, param->scan_rst.rssi);
    break;
  default:
    break;
  }
}

static bool ble_start(void) {
  esp_err_t err;
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK ||
      (err = esp_bt_controller_enable(ESP_BT_MODE_BLE)) != ESP_OK ||
      (err = esp_bluedroid_init()) != ESP_OK ||
      (err = esp_bluedroid_enable()) != ESP_OK ||
      (err = esp_ble_gap_register_callback(ble_gap_cb)) != ESP_OK) {
    ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(err));
    return false;
  }
  static esp_ble_scan_params_t scan = {
      .scan_type = BLE_SCAN_TYPE_PASSIVE,
      .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
      .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
      .scan_interval = 0x80, // 80 ms
      .scan_window = 0x40,   // 40 ms: Rest der Zeit fürs WLAN (Koexistenz)
      .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE};
  esp_ble_gap_set_scan_params(&scan);
  return true;
}

// ---------------------------------------------------------------------------
// Webserver
// ---------------------------------------------------------------------------
static WebServer server(80);
static DNSServer dns;
static char apName[24];
static uint32_t deadline;   // millis(), wann der Modus endet
static bool finishReq = false, resetReq = false;
static uint32_t finishAt = 0;

static void touch(void) {
  uint32_t ext = millis() + SETUP_ACTIVITY_EXTEND_S * 1000UL;
  if ((int32_t)(ext - deadline) > 0)
    deadline = ext;
}

static int32_t remaining_s(void) {
  int32_t r = (int32_t)(deadline - millis()) / 1000;
  return r > 0 ? r : 0;
}

static void send_json(const String &s) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

static void h_index(void) {
  touch();
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", (const char *)SETUP_HTML_GZ, SETUP_HTML_GZ_LEN);
}

static void json_str(String &out, const char *key, const char *val, bool comma = true) {
  out += '"';
  out += key;
  out += "\":\"";
  for (const char *c = val; *c; c++) {
    if (*c == '"' || *c == '\\') out += '\\';
    if ((uint8_t)*c >= 0x20) out += *c;
  }
  out += '"';
  if (comma) out += ',';
}

static void h_status(void) {
  touch();
  time_t now = time(NULL);
  String s;
  s.reserve(512);
  s += "{\"time\":";
  s += (uint32_t)now;
  s += ",\"timeValid\":";
  s += (now > 1704067200) ? "true" : "false";
  s += ",\"cd\":";
  s += remaining_s();
  s += ",\"thrBle\":";
  s += setupPrefs.thrBle;
  s += ",\"thrWifi\":";
  s += setupPrefs.thrWifi;
  s += ",\"interval\":";
  s += (cfg.sendcycle * 2 + 30) / 60;
  s += ",\"setup\":";
  s += setupPrefs.setupMin;
  s += ",\"display\":";
  s += cfg.screenon ? "true" : "false";
  s += ",\"hasPass\":";
  s += setupPrefs.pass[0] ? "true" : "false";
  s += ',';
  json_str(s, "site", setupPrefs.site);
  json_str(s, "ssid", setupPrefs.ssid);
  char fw[48];
  snprintf(fw, sizeof(fw), "%s / Custom %s", PROGVERSION, CUSTOM_BUILD_VERSION);
  json_str(s, "fw", fw);
#if (HAS_SDCARD)
  uint64_t tot = 0, fre = 0;
  bool sd = sdcard_space(&tot, &fre);
  s += "\"sd\":";
  s += sd ? "true" : "false";
  s += ",\"sdFree\":";
  s += (uint32_t)(fre / (1024ULL * 1024ULL));
  s += ",\"sdTotal\":";
  s += (uint32_t)(tot / (1024ULL * 1024ULL));
#else
  s += "\"sd\":false";
#endif
#ifdef SETUP_HAS_BATT
  s += ",\"batMv\":";
  s += read_voltage();
  s += ",\"batPct\":";
  s += read_battlevel();
#endif
  s += '}';
  send_json(s);
}

static void h_live(void) {
  String s;
  s.reserve(4096);
  s += "{\"cd\":";
  s += remaining_s();
  s += ",\"sta\":";
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0)
    s += list.sta[0].rssi;
  else
    s += "null";
  s += ",\"ble\":";
  dev_json(s, bleDevs);
  s += ",\"wifi\":";
  dev_json(s, wifiDevs);
  s += '}';
  send_json(s);
}

static void h_time(void) {
  touch();
  long epoch = server.arg("epoch").toInt();
  if (epoch < 1704067200L) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
  settimeofday(&tv, NULL);
#ifdef HAS_RTC
  set_rtctime((time_t)epoch);
#endif
  ESP_LOGI(TAG, "Time set from browser: %ld", epoch);
  send_json("{\"ok\":true}");
}

static void h_config(void) {
  touch();
  bool cfgChanged = false;
  if (server.hasArg("thrBle"))
    setupPrefs.thrBle = constrain(server.arg("thrBle").toInt(), -100, -20);
  if (server.hasArg("thrWifi"))
    setupPrefs.thrWifi = constrain(server.arg("thrWifi").toInt(), -100, -20);
  if (server.hasArg("setup")) {
    int m = server.arg("setup").toInt();
    setupPrefs.setupMin = constrain(m, 2, 60);
  }
  if (server.hasArg("site"))
    strlcpy(setupPrefs.site, server.arg("site").c_str(), sizeof(setupPrefs.site));
  if (server.hasArg("ssid"))
    strlcpy(setupPrefs.ssid, server.arg("ssid").c_str(), sizeof(setupPrefs.ssid));
  if (server.hasArg("pass"))
    strlcpy(setupPrefs.pass, server.arg("pass").c_str(), sizeof(setupPrefs.pass));
  if (server.hasArg("interval")) {
    int m = constrain(server.arg("interval").toInt(), 1, 8); // 8 min = max. 255*2 s
    uint8_t sc = (uint8_t)(m * 30);
    if (sc != cfg.sendcycle) {
      cfg.sendcycle = sc;
      cfgChanged = true;
    }
  }
  if (server.hasArg("display")) {
    uint8_t on = server.arg("display").toInt() ? 1 : 0;
    if (on != cfg.screenon) {
      cfg.screenon = on;
      cfgChanged = true;
    }
  }
  setup_prefs_save();
  if (cfgChanged)
    saveConfig(false);
  ESP_LOGI(TAG, "Config saved: BLE %d, WiFi %d, sendcycle %u, screen %u, setup %u, site '%s'",
           setupPrefs.thrBle, setupPrefs.thrWifi, cfg.sendcycle, cfg.screenon,
           setupPrefs.setupMin, setupPrefs.site);
  send_json("{\"ok\":true}");
}

static bool safe_name(const String &n) {
  if (n.length() == 0 || n.length() > 40)
    return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'))
      return false;
  }
  return n[0] != '.';
}

static void h_files(void) {
  touch();
  String s = "{\"files\":[";
#if (HAS_SDCARD)
  DIR *d = opendir(MOUNT_POINT);
  if (d) {
    struct dirent *e;
    bool first = true;
    char path[96];
    while ((e = readdir(d)) != NULL) {
      if (e->d_type == DT_DIR || !safe_name(e->d_name))
        continue;
      snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, e->d_name);
      struct stat st;
      if (stat(path, &st) != 0)
        continue;
      if (!first) s += ',';
      first = false;
      s += "{\"n\":\"";
      s += e->d_name;
      s += "\",\"s\":";
      s += (uint32_t)st.st_size;
      s += '}';
    }
    closedir(d);
  }
#endif
  s += "]}";
  send_json(s);
}

static void h_download(void) {
  touch();
#if (HAS_SDCARD)
  String n = server.arg("f");
  if (!safe_name(n)) {
    server.send(400, "text/plain", "bad name");
    return;
  }
  String path = String(MOUNT_POINT) + "/" + n;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) {
    server.send(404, "text/plain", "not found");
    return;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  const char *type = n.endsWith(".csv") ? "text/csv" : "application/octet-stream";
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + n + "\"");
  server.setContentLength(size);
  server.send(200, type, "");
  static uint8_t buf[2048];
  size_t r;
  while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
    server.client().write(buf, r);
    touch();
  }
  fclose(f);
#else
  server.send(404, "text/plain", "no sd card");
#endif
}

static void h_finish(void) {
  send_json("{\"ok\":true}");
  finishReq = true;
  finishAt = millis() + 1500;
}

static void h_reset(void) {
  send_json("{\"ok\":true}");
  resetReq = true;
  finishAt = millis() + 1500;
}

static void h_notfound(void) {
  // Captive Portal: jede fremde Adresse auf die Einrichtungsseite umleiten
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

// ---------------------------------------------------------------------------
// Display: QR-Code zum Verbinden + Restzeit
// ---------------------------------------------------------------------------
#ifdef HAS_DISPLAY
static void draw_display(void) {
  static uint8_t qrbuf[128];
  static QRCode qr;
  static bool qrReady = false;
  char msg[64];
  const int sc = 2, off = 3;
  if (!qrReady) {
    snprintf(msg, sizeof(msg), "WIFI:T:nopass;S:%s;;", apName);
    qrcode_initText(&qr, qrbuf, 3, ECC_LOW, msg);
    qrReady = true;
  }
  dp->fillScreen(MY_DISPLAY_BGCOLOR);
  int w = qr.size * sc + 2 * off;
  dp->fillRect(0, 0, w, w, MY_DISPLAY_FGCOLOR); // heller Hintergrund = Ruhezone
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        dp->fillRect(x * sc + off, y * sc + off, sc, sc, MY_DISPLAY_BGCOLOR);
  int tx = w + 4;
  int32_t r = remaining_s();
  dp_setFont(MY_FONT_SMALL);
  dp->setCursor(tx, 0);
  dp->printf("SETUP");
  dp->setCursor(tx, 12);
  dp->printf("WLAN:");
  dp->setCursor(tx, 22);
  dp->printf("%.10s", apName);
  dp->setCursor(tx, 32);
  dp->printf("%s", strlen(apName) > 10 ? apName + 10 : "");
  dp->setCursor(tx, 46);
  dp->printf("noch %ld:%02ld", (long)(r / 60), (long)(r % 60));
  dp->setCursor(tx, 56);
  dp->printf("%s", WiFi.softAPgetStationNum() ? "verbunden" : "Code scannen");
  dp->display();
}

static void draw_message(const char *l1, const char *l2) {
  dp->fillScreen(MY_DISPLAY_BGCOLOR);
  dp_setFont(MY_FONT_NORMAL);
  dp->setCursor(0, 16);
  dp->printf("%s", l1);
  dp->setCursor(0, 32);
  dp->printf("%s", l2);
  dp->display();
}
#endif

// ---------------------------------------------------------------------------
// Hauptschleife
// ---------------------------------------------------------------------------
void setupmode_run(void) {
  if (setupPrefs.setupMin == 0) {
    ESP_LOGI(TAG, "Setup mode disabled");
    return;
  }

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(apName, sizeof(apName), "Paxcounter-%02X%02X", mac[4], mac[5]);
  salt = esp_random();
  memset(bleDevs, 0, sizeof(bleDevs));
  memset(wifiDevs, 0, sizeof(wifiDevs));

  ESP_LOGI(TAG, "Starting setup mode for %u min, AP '%s'", setupPrefs.setupMin, apName);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName, NULL, SETUP_AP_CHANNEL, 0, 2);
  delay(100);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));

  // WLAN-Geräte in der Umgebung (nur Kanal des APs)
  wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_cb);
  esp_wifi_set_promiscuous(true);

  ble_start();

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", IPAddress(192, 168, 4, 1));

  server.on("/", HTTP_GET, h_index);
  server.on("/index.html", HTTP_GET, h_index);
  server.on("/api/status", HTTP_GET, h_status);
  server.on("/api/live", HTTP_GET, h_live);
  server.on("/api/time", HTTP_POST, h_time);
  server.on("/api/config", HTTP_POST, h_config);
  server.on("/api/files", HTTP_GET, h_files);
  server.on("/dl", HTTP_GET, h_download);
  server.on("/api/finish", HTTP_POST, h_finish);
  server.on("/api/reset", HTTP_POST, h_reset);
  server.onNotFound(h_notfound);
  server.begin();

  deadline = millis() + setupPrefs.setupMin * 60000UL;
  uint32_t lastDraw = 0;

  while (true) {
    dns.processNextRequest();
    server.handleClient();

#ifdef HAS_DISPLAY
    if (millis() - lastDraw >= 1000) {
      lastDraw = millis();
      draw_display();
    }
#endif

    if ((finishReq || resetReq) && (int32_t)(millis() - finishAt) > 0)
      break;
    if (!finishReq && !resetReq && remaining_s() == 0) {
      ESP_LOGI(TAG, "Setup time elapsed");
      break;
    }
    delay(2);
  }

  // ----- Ende: aufräumen und neu starten -----
  server.stop();
  dns.stop();
  esp_wifi_set_promiscuous(false);

  if (resetReq) {
    ESP_LOGW(TAG, "Factory reset requested");
    Preferences p;
    p.begin("paxsetup", false);
    p.clear();
    p.end();
    saveConfig(true);
#ifdef HAS_DISPLAY
    draw_message("Werkseinst.", "Neustart ...");
#endif
    delay(500);
    reset_rtc_vars(); // nächster Start wie Kaltstart -> wieder Einrichtung
    esp_restart();
  }

#ifdef HAS_DISPLAY
  draw_message("Zaehlung", "startet ...");
#endif
  ESP_LOGI(TAG, "Leaving setup mode, restarting into counting mode");
  delay(500);
  // Software-Reset behält RTC_runmode -> kein erneuter Einrichtungsmodus,
  // kein NTP-Sync; libpax startet in einem frischen System.
  RTC_runmode = RUNMODE_NORMAL;
  esp_restart();
}

#endif // HAS_SETUPMODE
