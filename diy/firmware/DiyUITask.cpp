#include "DiyUITask.h"
#include "DiyMesh.h"
#include "WifiLink.h"
#include "MqttLink.h"
#include "Updater.h"
#include "target.h"
#include <Arduino.h>
#include <diy_version.h>   // DIY_CHANNEL
#include "WebPanel.h"      // diyResetReason()
#include "DiySensors.h"    // diyBattMilliVolts()
#ifndef DIY_BOARD_NAME
  #define DIY_BOARD_NAME "< Repeater >"
#endif

#define AUTO_OFF_MILLIS      30000  // 30 seconds
#define BOOT_SCREEN_MILLIS   4000   // 4 seconds
#define POWEROFF_DELAY       3000

// 'meshcore', 128x13px (same logo as upstream UITask)
static const uint8_t meshcore_logo [] PROGMEM = {
    0x3c, 0x01, 0xe3, 0xff, 0xc7, 0xff, 0x8f, 0x03, 0x87, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe,
    0x3c, 0x03, 0xe3, 0xff, 0xc7, 0xff, 0x8e, 0x03, 0x8f, 0xfe, 0x3f, 0xfe, 0x1f, 0xff, 0x1f, 0xfe,
    0x3e, 0x03, 0xc3, 0xff, 0x8f, 0xff, 0x0e, 0x07, 0x8f, 0xfe, 0x7f, 0xfe, 0x1f, 0xff, 0x1f, 0xfc,
    0x3e, 0x07, 0xc7, 0x80, 0x0e, 0x00, 0x0e, 0x07, 0x9e, 0x00, 0x78, 0x0e, 0x3c, 0x0f, 0x1c, 0x00,
    0x3e, 0x0f, 0xc7, 0x80, 0x1e, 0x00, 0x0e, 0x07, 0x1e, 0x00, 0x70, 0x0e, 0x38, 0x0f, 0x3c, 0x00,
    0x7f, 0x0f, 0xc7, 0xfe, 0x1f, 0xfc, 0x1f, 0xff, 0x1c, 0x00, 0x70, 0x0e, 0x38, 0x0e, 0x3f, 0xf8,
    0x7f, 0x1f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x0e, 0x38, 0x0e, 0x3f, 0xf8,
    0x7f, 0x3f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x1e, 0x3f, 0xfe, 0x3f, 0xf0,
    0x77, 0x3b, 0x87, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xfc, 0x38, 0x00,
    0x77, 0xfb, 0x8f, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xf8, 0x38, 0x00,
    0x73, 0xf3, 0x8f, 0xff, 0x0f, 0xff, 0x1c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x78, 0x7f, 0xf8,
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfe, 0x3c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x3c, 0x7f, 0xf8,
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfc, 0x3c, 0x0e, 0x1f, 0xf8, 0xff, 0xf8, 0x70, 0x3c, 0x7f, 0xf8,
};

void DiyUITask::begin(NodePrefs* node_prefs, DiyMesh* mesh, WifiLink* wifi, const char* diy_version, const char* fw_version) {
  _auto_off = millis() + AUTO_OFF_MILLIS;
  _started_at = millis();
  _node_prefs = node_prefs;
  _mesh = mesh;
  _wifi = wifi;
  _display->turnOn();
  _small = _display->height() < 100;   // SSD1306 128x64 (Heltec) vs SH1107 128x128 (SenkoPhone)
  _pages = _small ? 4 : 3;

#if defined(PIN_USER_BTN) && defined(DISPLAY_CLASS)
  user_btn.begin();
#endif

  // "v1.4.0-mc1.18.0-20261120" -> "v1.4.0"
  strlcpy(_diy_short, diy_version, sizeof(_diy_short));
  char* dash = strchr(_diy_short, '-');
  if (dash) *dash = 0;
  snprintf(_version_info, sizeof(_version_info), "%s / MC %s", _diy_short, fw_version);
}

void DiyUITask::wake() {
  if (!_display->isOn()) _display->turnOn();
  _auto_off = millis() + AUTO_OFF_MILLIS;
}

void DiyUITask::renderCurrScreen() {
  char tmp[48];
  if (millis() < _started_at + BOOT_SCREEN_MILLIS || _powering_off_at > 0) {
    _display->setColor(UIColor::corp_blue);
    int logoWidth = 128;
    _display->drawXbm((_display->width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);
    _display->setColor(UIColor::primary_txt);
    _display->setTextSize(1);
    _display->drawTextCentered(_display->width() / 2, 22, "https://meshcore.io");
    const int y1 = _small ? 34 : 35, y2 = _small ? 46 : 48;
    if (_powering_off_at > 0) {
      _display->drawTextCentered(_display->width() / 2, y2, "Turning OFF");
    } else {
      _display->drawTextCentered(_display->width() / 2, y1, _version_info);
      _display->drawTextCentered(_display->width() / 2, y2, DIY_BOARD_NAME);
    }
    return;
  }

  if (_wifi && _wifi->setupAPActive() && _small) {   // first-run wizard on 128x64
    _display->setTextSize(1);
    _display->setColor(UIColor::primary_txt);
    _display->setCursor(0, 0);
    _display->print("SETUP: join Wi-Fi");
    _display->setCursor(0, 10);
    _display->print(_wifi->apSsid());
    _display->setCursor(0, 20);
    _display->print("Password:");
    _display->setTextSize(2);
    _display->setCursor(0, 30);
    _display->print(_wifi->apPass());
    _display->setTextSize(1);
    _display->setCursor(0, 50);
    snprintf(tmp, sizeof(tmp), "http://%s/", _wifi->apIpString().c_str());
    _display->print(tmp);
    return;
  }
  if (_wifi && _wifi->setupAPActive()) {   // first-run wizard: how to connect (the font is Latin only)
    _display->setTextSize(1);
    _display->setColor(UIColor::primary_txt);
    _display->drawTextCentered(_display->width() / 2, 0, "SETUP");
    _display->setCursor(0, 16);
    _display->print("1. Join Wi-Fi:");
    _display->setCursor(0, 28);
    _display->print(_wifi->apSsid());
    _display->setCursor(0, 44);
    _display->print("2. Password:");
    _display->setTextSize(2);
    _display->setCursor(0, 56);
    _display->print(_wifi->apPass());
    _display->setTextSize(1);
    _display->setCursor(0, 80);
    _display->print("3. Open in browser:");
    _display->setCursor(0, 92);
    snprintf(tmp, sizeof(tmp), "http://%s/", _wifi->apIpString().c_str());
    _display->print(tmp);
    _display->setCursor(0, 112);
    _display->print("(page opens itself)");
    return;
  }

  if (_small) {
    switch (_page) {
      case 1: renderSmallRadio(tmp, sizeof(tmp)); break;
      case 2: renderSmallSystem(tmp, sizeof(tmp)); break;
      case 3: renderSmallInfo(tmp, sizeof(tmp)); break;
      default: renderSmallMain(tmp, sizeof(tmp)); break;
    }
  } else {
    switch (_page) {
      case 1: renderRadio(tmp, sizeof(tmp)); break;
      case 2: renderSystem(tmp, sizeof(tmp)); break;
      default: renderMain(tmp, sizeof(tmp)); break;
    }
  }
  drawPageDots();
}

// ======================================================================
//  pages (128x128 SH1107 or 128x64 SSD1306, built-in 6x8 Latin font; size 2 = 12x16)
//  short click: next page; triple click: setup AP; long press: power off
// ======================================================================
static const int OLED_W = 128;

void DiyUITask::drawHeader(const char* title) {
  _display->setTextSize(1);
  _display->setColor(UIColor::primary_txt);
  _display->drawTextEllipsized(0, 0, _small ? 84 : 100, title);   // 128x64: page dots sit in the header
  drawWifiBars(OLED_W - 14, 0);
  _display->fillRect(0, 10, OLED_W, 1);
}

// four bars, 2 px wide, rising to 8 px; filled bars = signal; "x" when not connected
void DiyUITask::drawWifiBars(int x, int y) {
  if (!_wifi || !_wifi->isConnected()) {
    if (_wifi && _wifi->setupAPActive()) { _display->setCursor(x, y); _display->print("AP"); }
    else { _display->setCursor(x + 4, y); _display->print("x"); }
    return;
  }
  int rssi = _wifi->rssi();
  const int thr[4] = {-85, -75, -65, -55};
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2, bx = x + i * 4, by = y + 8 - h;
    if (rssi > thr[i]) _display->fillRect(bx, by, 2, h);
    else _display->drawRect(bx, by, 2, h);
  }
}

void DiyUITask::drawPageDots() {
  const int n = _pages;
  const int x0 = _small ? 88 : OLED_W / 2 - (n * 8) / 2, y = _small ? 2 : 123, step = _small ? 6 : 8;
  for (int i = 0; i < n; i++) {
    if (i == _page) _display->fillRect(x0 + i * step, y, 4, 4);
    else _display->drawRect(x0 + i * step, y, 4, 4);
  }
}

// bar chart of values[0..n-1] (oldest first) in the box x,y,w,h; returns the max
static int drawBars(DisplayDriver* d, int x, int y, int w, int h, const int* values, int n) {
  int mx = 1;
  for (int i = 0; i < n; i++) if (values[i] > mx) mx = values[i];
  int bw = w / n; if (bw < 1) bw = 1;
  for (int i = 0; i < n; i++) {
    int bh = values[i] > 0 ? (values[i] * h + mx - 1) / mx : 0;
    if (bh > 0) d->fillRect(x + i * bw, y + h - bh, bw > 1 ? bw - 1 : 1, bh);
  }
  d->fillRect(x, y + h, w, 1);   // baseline
  return mx;
}

// "Last <from> <snr>dB <ago>" for the last received packet
int DiyUITask::lastRxText(char* tmp, size_t sz) {
  if (!_mesh) { tmp[0] = 0; return 0; }
  if (const DiyMesh::PktEntry* e = _mesh->lastRx()) {
    unsigned long ago = (millis() - e->ms) / 1000;
    char from[8] = "--";
    if (e->hops > 0 && e->hash_sz >= 1 && e->hash_sz <= 3 && e->path_n >= e->hops * e->hash_sz) {
      const uint8_t* k = &e->path[(e->hops - 1) * e->hash_sz];
      int o = 0;
      for (int i = 0; i < e->hash_sz; i++) o += snprintf(from + o, sizeof(from) - o, "%02X", k[i]);
    }
    if (ago < 60) return snprintf(tmp, sz, "Last %s %+.1fdB %lus", from, e->snr4 / 4.0f, ago);
    if (ago < 3600) return snprintf(tmp, sz, "Last %s %+.1fdB %lum", from, e->snr4 / 4.0f, ago / 60);
    return snprintf(tmp, sz, "Last %s %+.1fdB %luh", from, e->snr4 / 4.0f, ago / 3600);
  }
  return snprintf(tmp, sz, "Last: no packets yet");
}

// page 0: name + Wi-Fi, packet counters, packets per minute for the last hour, last packet, panel address
void DiyUITask::renderMain(char* tmp, size_t sz) {
  drawHeader(_node_prefs->node_name);
  if (_mesh) {
    _display->setTextSize(2);
    snprintf(tmp, sz, "RX %lu", (unsigned long)(_mesh->rx_flood + _mesh->rx_direct));
    _display->setCursor(0, 14); _display->print(tmp);
    snprintf(tmp, sz, "TX %lu", (unsigned long)(_mesh->tx_flood + _mesh->tx_direct));
    _display->setCursor(0, 32); _display->print(tmp);
    _display->setTextSize(1);

    int v[DiyMesh::MIN_BUCKETS];
    for (int i = 0; i < DiyMesh::MIN_BUCKETS; i++) {
      const DiyMesh::Bucket& b = _mesh->minutes[(_mesh->minute_head + 1 + i) % DiyMesh::MIN_BUCKETS];
      v[i] = b.valid ? b.rx + b.tx : 0;
    }
    int mx = drawBars(_display, 4, 62, 120, 24, v, DiyMesh::MIN_BUCKETS);
    snprintf(tmp, sz, "pkt/min 1h  max %d", mx);
    _display->setCursor(0, 52); _display->print(tmp);

    lastRxText(tmp, sz);
    _display->setCursor(0, 92); _display->print(tmp);
  }
  // where to open the panel; every other 3 s a new release, if there is one
  if (_upd && _upd->available()[0] && (millis() / 3000) % 2) {
    snprintf(tmp, sz, "Update: %s", _upd->available());
    char* dash = strchr(tmp + 8, '-');
    if (dash) *dash = 0;
  } else if (_wifi && _wifi->isConnected()) snprintf(tmp, sz, "%s", _wifi->ipString().c_str());
  else if (_wifi && _wifi->ssid()[0]) snprintf(tmp, sz, "WiFi: connecting...");
  else snprintf(tmp, sz, "WiFi off: 3x click");
  _display->drawTextCentered(OLED_W / 2, 108, tmp);
}

// page 1: radio profile, power, noise floor for the last 4 minutes, airtime 24 h
void DiyUITask::renderRadio(char* tmp, size_t sz) {
  drawHeader("RADIO");
  _display->setTextSize(2);
  snprintf(tmp, sz, "%.3f", _node_prefs->freq);
  _display->setCursor(0, 14); _display->print(tmp);
  _display->setTextSize(1);
  _display->setCursor(98, 21); _display->print("MHz");
  snprintf(tmp, sz, "BW %.1f  SF%d  CR%d", _node_prefs->bw, _node_prefs->sf, _node_prefs->cr);
  _display->setCursor(0, 34); _display->print(tmp);
  snprintf(tmp, sz, "TX %d dBm  boost %s", _node_prefs->tx_power_dbm, _node_prefs->rx_boosted_gain ? "on" : "off");
  _display->setCursor(0, 44); _display->print(tmp);
  if (!_mesh) return;

  snprintf(tmp, sz, "Noise %d dBm, 4 min", _mesh->noiseFloor());
  _display->setCursor(0, 56); _display->print(tmp);
  int v[DiyMesh::NF_SAMPLES], lo = 0, n = _mesh->nf_count;
  for (int i = 0; i < n; i++) {
    int x = _mesh->nf_recent[(_mesh->nf_head - n + i + DiyMesh::NF_SAMPLES) % DiyMesh::NF_SAMPLES];
    v[i] = x; if (i == 0 || x < lo) lo = x;
  }
  for (int i = 0; i < n; i++) v[i] = v[i] - lo + 1;   // relative to the quietest sample
  if (n > 1) drawBars(_display, 4, 66, 120, 16, v, n);

  uint32_t rx24 = 0, tx24 = 0; int n24 = 0;
  for (int i = 0; i < DiyMesh::TEN_BUCKETS; i++) {
    const DiyMesh::Bucket& b = _mesh->tens[i];
    if (b.valid) { rx24 += b.rx_air_ms; tx24 += b.tx_air_ms; n24++; }
  }
  if (n24) {
    float span = n24 * 600000.0f;
    snprintf(tmp, sz, "Air 24h TX%.1f%% RX%.1f%%", tx24 * 100.0f / span, rx24 * 100.0f / span);
  } else {
    snprintf(tmp, sz, "Air 24h: collecting");
  }
  _display->setCursor(0, 90); _display->print(tmp);
  snprintf(tmp, sz, "Fwd %s  advert %dm", _node_prefs->disable_fwd ? "OFF" : "on", _node_prefs->advert_interval * 2);
  _display->setCursor(0, 102); _display->print(tmp);
}

// page 2: Wi-Fi, neighbours, uptime, last reset, memory, version and board
void DiyUITask::renderSystem(char* tmp, size_t sz) {
  drawHeader("SYSTEM");
  int y = 14;
  if (_wifi && _wifi->setupAPActive()) snprintf(tmp, sz, "Setup AP is on");
  else if (_wifi && _wifi->ssid()[0]) snprintf(tmp, sz, "WiFi %s", _wifi->ssid());
  else snprintf(tmp, sz, "WiFi off");
  _display->drawTextEllipsized(0, y, OLED_W, tmp); y += 11;
  if (_wifi && _wifi->isConnected()) {
    snprintf(tmp, sz, "IP %s", _wifi->ipString().c_str()); _display->setCursor(0, y); _display->print(tmp); y += 11;
    snprintf(tmp, sz, "RSSI %d dBm", _wifi->rssi()); _display->setCursor(0, y); _display->print(tmp); y += 11;
  } else {
    y += 22;
  }
  if (_mesh) {
    int direct = 0;
    for (int i = 0; i < _mesh->num_nodes; i++) if (_mesh->nodes[i].seen & DiyMesh::SEEN_DIRECT) direct++;
    snprintf(tmp, sz, "Nodes %d  direct %d", _mesh->num_nodes, direct);
    _display->setCursor(0, y); _display->print(tmp);
  }
  y += 11;
  unsigned long up = millis() / 1000;
  if (up >= 86400) snprintf(tmp, sz, "Up %lud %luh %02lum", up / 86400, (up / 3600) % 24, (up / 60) % 60);
  else snprintf(tmp, sz, "Up %luh %02lum", up / 3600, (up / 60) % 60);
  _display->setCursor(0, y); _display->print(tmp); y += 11;
  snprintf(tmp, sz, "Reset: %s", diyResetReason());
  _display->setCursor(0, y); _display->print(tmp); y += 11;
  snprintf(tmp, sz, "RAM %lu KB free", (unsigned long)(ESP.getFreeHeap() / 1024));
  _display->setCursor(0, y); _display->print(tmp); y += 11;
  snprintf(tmp, sz, "%s %s", _diy_short, DIY_CHANNEL);
  _display->setCursor(0, y); _display->print(tmp); y += 11;
  _display->drawTextEllipsized(0, y, OLED_W, DIY_BOARD_NAME); y += 11;
  if (_mqtt && _mqtt->enabled()) {
    snprintf(tmp, sz, "HA/MQTT: %s", _mqtt->stateText());
    _display->setCursor(0, y); _display->print(tmp);
  }
}

// ======================================================================
//  128x64 pages (Heltec V3 / V4): header 0..10, then 10 px text lines from y 13
// ======================================================================

// page 0: counters, packets per minute for the last hour, last packet, panel address / update
void DiyUITask::renderSmallMain(char* tmp, size_t sz) {
  drawHeader(_node_prefs->node_name);
  if (_mesh) {
    snprintf(tmp, sz, "RX %lu  TX %lu", (unsigned long)(_mesh->rx_flood + _mesh->rx_direct),
             (unsigned long)(_mesh->tx_flood + _mesh->tx_direct));
    _display->setCursor(0, 13); _display->print(tmp);
    int v[DiyMesh::MIN_BUCKETS];
    for (int i = 0; i < DiyMesh::MIN_BUCKETS; i++) {
      const DiyMesh::Bucket& b = _mesh->minutes[(_mesh->minute_head + 1 + i) % DiyMesh::MIN_BUCKETS];
      v[i] = b.valid ? b.rx + b.tx : 0;
    }
    drawBars(_display, 4, 23, 120, 10, v, DiyMesh::MIN_BUCKETS);   // packets per minute, 1 h; baseline y 33
    lastRxText(tmp, sz);
    _display->drawTextEllipsized(0, 37, OLED_W, tmp);
  }
  // where to open the panel; every other 3 s a new release, if there is one
  if (_upd && _upd->available()[0] && (millis() / 3000) % 2) {
    snprintf(tmp, sz, "Update: %s", _upd->available());
    char* dash = strchr(tmp + 8, '-');
    if (dash) *dash = 0;
  } else if (_wifi && _wifi->isConnected()) snprintf(tmp, sz, "%s", _wifi->ipString().c_str());
  else if (_wifi && _wifi->ssid()[0]) snprintf(tmp, sz, "WiFi: connecting...");
  else snprintf(tmp, sz, "WiFi off: 3x click");
  _display->drawTextCentered(OLED_W / 2, 52, tmp);
}

// page 1: radio profile, power, noise floor for the last 4 minutes, airtime 24 h
void DiyUITask::renderSmallRadio(char* tmp, size_t sz) {
  drawHeader("RADIO");
  snprintf(tmp, sz, "%.3f MHz BW%.1f", _node_prefs->freq, _node_prefs->bw);
  _display->setCursor(0, 13); _display->print(tmp);
  snprintf(tmp, sz, "SF%d CR%d TX %d dBm", _node_prefs->sf, _node_prefs->cr, _node_prefs->tx_power_dbm);
  _display->setCursor(0, 23); _display->print(tmp);
  if (!_mesh) return;
  snprintf(tmp, sz, "Noise %d", _mesh->noiseFloor());
  _display->setCursor(0, 33); _display->print(tmp);
  int v[DiyMesh::NF_SAMPLES], lo = 0, n = _mesh->nf_count;
  for (int i = 0; i < n; i++) {
    int x = _mesh->nf_recent[(_mesh->nf_head - n + i + DiyMesh::NF_SAMPLES) % DiyMesh::NF_SAMPLES];
    v[i] = x; if (i == 0 || x < lo) lo = x;
  }
  for (int i = 0; i < n; i++) v[i] = v[i] - lo + 1;   // relative to the quietest sample
  if (n > 1) drawBars(_display, 64, 33, 60, 7, v, n);  // next to the value, baseline y 40
  uint32_t rx24 = 0, tx24 = 0; int n24 = 0;
  for (int i = 0; i < DiyMesh::TEN_BUCKETS; i++) {
    const DiyMesh::Bucket& b = _mesh->tens[i];
    if (b.valid) { rx24 += b.rx_air_ms; tx24 += b.tx_air_ms; n24++; }
  }
  if (n24) {
    float span = n24 * 600000.0f;
    snprintf(tmp, sz, "Air TX%.1f%% RX%.1f%%", tx24 * 100.0f / span, rx24 * 100.0f / span);
  } else {
    snprintf(tmp, sz, "Air 24h: collecting");
  }
  _display->setCursor(0, 44); _display->print(tmp);
  snprintf(tmp, sz, "Fwd %s  advert %dm", _node_prefs->disable_fwd ? "OFF" : "on", _node_prefs->advert_interval * 2);
  _display->setCursor(0, 54); _display->print(tmp);
}

// page 2: Wi-Fi, neighbours, uptime, Home Assistant
void DiyUITask::renderSmallSystem(char* tmp, size_t sz) {
  drawHeader("SYSTEM");
  if (_wifi && _wifi->setupAPActive()) snprintf(tmp, sz, "Setup AP is on");
  else if (_wifi && _wifi->ssid()[0]) snprintf(tmp, sz, "WiFi %s", _wifi->ssid());
  else snprintf(tmp, sz, "WiFi off");
  _display->drawTextEllipsized(0, 13, OLED_W, tmp);
  if (_wifi && _wifi->isConnected()) {
    snprintf(tmp, sz, "%s %ddBm", _wifi->ipString().c_str(), _wifi->rssi());
    _display->drawTextEllipsized(0, 23, OLED_W, tmp);
  }
  if (_mesh) {
    int direct = 0;
    for (int i = 0; i < _mesh->num_nodes; i++) if (_mesh->nodes[i].seen & DiyMesh::SEEN_DIRECT) direct++;
    snprintf(tmp, sz, "Nodes %d  direct %d", _mesh->num_nodes, direct);
    _display->setCursor(0, 33); _display->print(tmp);
  }
  unsigned long up = millis() / 1000;
  if (up >= 86400) snprintf(tmp, sz, "Up %lud %luh %02lum", up / 86400, (up / 3600) % 24, (up / 60) % 60);
  else snprintf(tmp, sz, "Up %luh %02lum", up / 3600, (up / 60) % 60);
  _display->setCursor(0, 43); _display->print(tmp);
  if (_mqtt && _mqtt->enabled()) {
    snprintf(tmp, sz, "HA/MQTT: %s", _mqtt->stateText());
    _display->drawTextEllipsized(0, 53, OLED_W, tmp);
  }
}

// page 3: battery, last reset, memory, version and board
void DiyUITask::renderSmallInfo(char* tmp, size_t sz) {
  drawHeader("INFO");
  int y = 13;
  if (uint16_t mv = diyBattMilliVolts()) {
    snprintf(tmp, sz, "Battery %.2f V", mv / 1000.0f);
    _display->setCursor(0, y); _display->print(tmp); y += 10;
  }
  snprintf(tmp, sz, "Reset: %s", diyResetReason());
  _display->drawTextEllipsized(0, y, OLED_W, tmp); y += 10;
  snprintf(tmp, sz, "RAM %lu KB free", (unsigned long)(ESP.getFreeHeap() / 1024));
  _display->setCursor(0, y); _display->print(tmp); y += 10;
  snprintf(tmp, sz, "%s %s", _diy_short, DIY_CHANNEL);
  _display->setCursor(0, y); _display->print(tmp); y += 10;
  _display->drawTextEllipsized(0, y, OLED_W, DIY_BOARD_NAME);
}

void DiyUITask::loop() {
#if defined(PIN_USER_BTN) && defined(DISPLAY_CLASS)
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    if (_display->isOn()) _page = (_page + 1) % _pages;   // screen on: next page; off: just wake
    wake();
    _next_refresh = 0;
  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {   // open the setup access point (wizard)
    if (_wifi) _wifi->startSetupAP();
    wake();
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    _display->turnOn();
    Serial.println("Powering Off");
    _powering_off_at = millis() + POWEROFF_DELAY;
  }
#endif

  if (_display->isOn()) {
    if (millis() >= _next_refresh) {
      _display->startFrame();
      renderCurrScreen();
      _display->endFrame();
      _next_refresh = millis() + 1000;
    }
    if (_wifi && _wifi->setupAPActive()) _auto_off = millis() + AUTO_OFF_MILLIS;   // keep the setup info visible
    if (millis() > _auto_off && _powering_off_at == 0) {
      _display->turnOff();
    }
  }

  if (_powering_off_at > 0 && millis() > _powering_off_at) {
    _board->powerOff();  // should not return
  }
}
