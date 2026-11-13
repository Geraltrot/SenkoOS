#include "Updater.h"
#include "WifiLink.h"
#include "OtaServer.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <SHA256.h>
#include <ed_25519.h>
#include <esp_ota_ops.h>
#include <diy_version.h>

#ifndef DIY_BOARD_ID
  #define DIY_BOARD_ID "e22wroom"   // technical board id, same as in release tags
#endif
#ifndef DIY_UPDATE_PUBKEY
  #define DIY_UPDATE_PUBKEY ""      // 64 hex chars: Ed25519 public key of the release signing key
#endif
#ifndef DIY_UPDATE_URL
  #define DIY_UPDATE_URL ""         // default manifest URL (GitHub: .../releases/latest/download/manifest.json)
#endif

static const char* NVS_NS = "diy";
static const uint32_t FIRST_CHECK_MS = 5UL * 60UL * 1000UL;      // 5 min after boot
static const uint32_t CHECK_EVERY_MS = 24UL * 3600UL * 1000UL;   // then daily
static const uint32_t TRIAL_OK_MS = 10UL * 60UL * 1000UL;        // new firmware is "good" after 10 min
static const uint8_t TRIAL_MAX_BOOTS = 3;

enum { ST_IDLE, ST_CHECKING, ST_DOWNLOADING, ST_REBOOT, ST_ERROR };

static Updater* s_self = nullptr;
static uint32_t (*s_now)() = nullptr;   // epoch seconds from the mesh RTC (NTP-synced), 0 = unknown

// ---------------------------------------------------------------------------
//  helpers
// ---------------------------------------------------------------------------
static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool hexDecode(const char* s, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int hi = hexVal(s[2 * i]), lo = hi < 0 ? -1 : hexVal(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}

// value of "key": "string" or "key": number in a flat JSON object we produce ourselves
static bool jsonField(const char* json, const char* key, char* out, size_t n) {
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(json, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  size_t o = 0;
  if (*p == '"') {
    p++;
    while (*p && *p != '"' && o < n - 1) out[o++] = *p++;
    if (*p != '"') return false;
  } else {
    while (*p && *p != ',' && *p != '}' && *p != ' ' && o < n - 1) out[o++] = *p++;
  }
  out[o] = 0;
  return o > 0;
}

// "v1.4.0-e22wroom-..." -> 1004000; anything else (dev builds: "876d6bf") -> 0
static long versionNum(const char* v) {
  int a = 0, b = 0, c = 0;
  if (!v || v[0] != 'v' || sscanf(v + 1, "%d.%d.%d", &a, &b, &c) != 3) return 0;
  return (long)a * 1000000L + (long)b * 1000L + c;
}

// same rule as the OTA upload: application image (0xE9) for a classic ESP32 (chip id 0)
static bool imageHeaderOk(const uint8_t* b, size_t n) {
  return n >= 16 && b[0] == 0xE9 && (b[12] | (b[13] << 8)) == 0;
}

// ---------------------------------------------------------------------------
//  trial / rollback (runs before anything else in setup)
// ---------------------------------------------------------------------------
void Updater::earlyBoot() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  if (p.getUChar("up_trial", 0)) {
    uint8_t boots = p.getUChar("up_boots", 0) + 1;
    p.putUChar("up_boots", boots);
    if (boots > TRIAL_MAX_BOOTS) {
      char prev[17] = {0};
      p.getString("up_prev", prev, sizeof(prev));
      p.putUChar("up_trial", 0);
      p.putUChar("up_rb", 1);
      p.end();
      const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, prev);
      if (part && esp_ota_set_boot_partition(part) == ESP_OK) {
        Serial.printf("Update: new firmware failed %u times, back to %s\n", boots - 1, prev);
        delay(100);
        ESP.restart();
      }
      return;
    }
  }
  p.end();
}

// ---------------------------------------------------------------------------
//  settings
// ---------------------------------------------------------------------------
void Updater::load() {
  Preferences p;
  if (p.begin(NVS_NS, true)) {
    _mode = p.getUChar("up_mode", 1);
    _beta = p.getUChar("up_beta", 0) != 0;
    if (p.isKey("up_url")) p.getString("up_url", _url, sizeof(_url));
    else strlcpy(_url, DIY_UPDATE_URL, sizeof(_url));
    _rolled_back = p.getUChar("up_rb", 0) != 0;
    p.end();
  } else {
    strlcpy(_url, DIY_UPDATE_URL, sizeof(_url));
  }
  if (_mode > 2) _mode = 1;
}

void Updater::save() {
  Preferences p;
  if (p.begin(NVS_NS, false)) {
    p.putUChar("up_mode", _mode);
    p.putUChar("up_beta", _beta ? 1 : 0);
    p.putString("up_url", _url);
    p.end();
  }
}

void Updater::begin(WifiLink* wifi) {
  _wifi = wifi;
  s_self = this;
  load();
}

void setUpdaterClock(uint32_t (*now_epoch)()) { s_now = now_epoch; }

bool Updater::hasKey() const { return strlen(DIY_UPDATE_PUBKEY) == 64; }

const char* Updater::stateText() const {
  switch (_state) {
    case ST_CHECKING: return "checking";
    case ST_DOWNLOADING: return "downloading";
    case ST_REBOOT: return "rebooting";
    case ST_ERROR: return "error";
    default: return "idle";
  }
}

uint32_t Updater::lastCheckAgoS() const { return _checked_once ? (millis() - _last_check_ms) / 1000 : 0; }

// ---------------------------------------------------------------------------
//  network (runs in the updater task)
// ---------------------------------------------------------------------------
bool Updater::fetch(const char* url, char* out, size_t out_size, size_t* out_len) {
  WiFiClientSecure tls;
  tls.setInsecure();   // authenticity comes from the Ed25519 signature, not from TLS
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.setUserAgent("SenkoPhone-updater");
  if (!http.begin(tls, url)) { strlcpy(_err, "bad url", sizeof(_err)); return false; }
  int code = http.GET();
  if (code != 200) { snprintf(_err, sizeof(_err), "http %d", code); http.end(); return false; }
  WiFiClient* s = http.getStreamPtr();
  size_t n = 0;
  uint32_t idle_since = millis();
  int total = http.getSize();   // -1 when chunked
  while (n < out_size - 1 && (total < 0 || (int)n < total)) {
    int a = s->available();
    if (a > 0) {
      int r = s->readBytes(out + n, min((size_t)a, out_size - 1 - n));
      n += r;
      idle_since = millis();
    } else if (!s->connected() || millis() - idle_since > 10000) {
      break;
    } else {
      delay(5);
    }
  }
  http.end();
  out[n] = 0;
  if (out_len) *out_len = n;
  if (n >= out_size - 1) { strlcpy(_err, "manifest too big", sizeof(_err)); return false; }
  return n > 0;
}

bool Updater::doCheck() {
  _err[0] = 0;
  if (!hasKey()) { strlcpy(_err, "no signing key in this build", sizeof(_err)); return false; }
  if (!_url[0]) { strlcpy(_err, "no update url", sizeof(_err)); return false; }

  // beta: GitHub "latest" skips pre-releases, so CI keeps the newest manifest of any release
  // (stable or beta) in the rolling pre-release "beta-channel"
  char murl[200];   // manifest URL actually used
  strlcpy(murl, _url, sizeof(murl));
  const char* LATEST = "/releases/latest/download/";
  char* p = strstr(murl, LATEST);
  if (_beta && p) {
    char tail[100];
    strlcpy(tail, p + strlen(LATEST), sizeof(tail));
    snprintf(p, sizeof(murl) - (p - murl), "/releases/download/beta-channel/%s", tail);
  }

  static char man[1024];
  char sig_hex[160], sig_url[210];
  size_t man_len = 0;
  if (!fetch(murl, man, sizeof(man), &man_len)) return false;

  strlcpy(sig_url, murl, sizeof(sig_url));
  char* dot = strrchr(sig_url, '.');
  if (dot && strcmp(dot, ".json") == 0) strcpy(dot, ".sig");
  else strlcat(sig_url, ".sig", sizeof(sig_url));
  size_t sig_len = 0;
  if (!fetch(sig_url, sig_hex, sizeof(sig_hex), &sig_len)) return false;
  while (sig_len && (sig_hex[sig_len - 1] == '\n' || sig_hex[sig_len - 1] == '\r' || sig_hex[sig_len - 1] == ' ')) sig_hex[--sig_len] = 0;

  uint8_t sig[64], pub[32];
  if (sig_len != 128 || !hexDecode(sig_hex, sig, 64)) { strlcpy(_err, "bad signature file", sizeof(_err)); return false; }
  if (!hexDecode(DIY_UPDATE_PUBKEY, pub, 32)) { strlcpy(_err, "bad key in build", sizeof(_err)); return false; }
  if (!ed25519_verify(sig, (const unsigned char*)man, man_len, pub)) { strlcpy(_err, "signature does not match", sizeof(_err)); return false; }

  char version[48], board[24], channel[12], url[200], size[16], sha[80];
  if (!jsonField(man, "version", version, sizeof(version)) || !jsonField(man, "board", board, sizeof(board)) ||
      !jsonField(man, "channel", channel, sizeof(channel)) || !jsonField(man, "url", url, sizeof(url)) ||
      !jsonField(man, "size", size, sizeof(size)) || !jsonField(man, "sha256", sha, sizeof(sha))) {
    strlcpy(_err, "manifest incomplete", sizeof(_err));
    return false;
  }
  if (strcmp(board, DIY_BOARD_ID) != 0) { snprintf(_err, sizeof(_err), "manifest is for board %s", board); return false; }

  _avail_version[0] = 0;
  bool is_beta = strcmp(channel, "release") != 0;
  if (is_beta && !_beta) return true;                                // pre-releases not wanted
  if (versionNum(version) <= versionNum(DIY_VERSION)) return true;    // nothing newer
  uint8_t h[32];
  if (strlen(sha) != 64 || !hexDecode(sha, h, 32)) { strlcpy(_err, "bad sha256 in manifest", sizeof(_err)); return false; }
  memcpy(_avail_sha, h, 32);
  _avail_size = strtoul(size, nullptr, 10);
  strlcpy(_avail_url, url, sizeof(_avail_url));
  strlcpy(_avail_channel, channel, sizeof(_avail_channel));
  strlcpy(_avail_version, version, sizeof(_avail_version));
  return true;
}

bool Updater::doInstall() {
  _err[0] = 0;
  if (!_avail_version[0]) { strlcpy(_err, "nothing to install", sizeof(_err)); return false; }
  if (ota_server.running()) { strlcpy(_err, "manual upload in progress", sizeof(_err)); return false; }

  WiFiClientSecure tls;
  tls.setInsecure();   // the file is checked against the signed SHA-256
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  http.setUserAgent("SenkoPhone-updater");
  if (!http.begin(tls, _avail_url)) { strlcpy(_err, "bad url", sizeof(_err)); return false; }
  int code = http.GET();
  if (code != 200) { snprintf(_err, sizeof(_err), "http %d", code); http.end(); return false; }
  int total = http.getSize();
  if (total > 0 && (uint32_t)total != _avail_size) { strlcpy(_err, "size differs from manifest", sizeof(_err)); http.end(); return false; }

  WiFiClient* s = http.getStreamPtr();
  static uint8_t buf[1024];
  SHA256 sha;
  uint32_t got = 0, idle_since = millis();
  bool begun = false;
  _progress = 0;
  while (got < _avail_size) {
    int a = s->available();
    if (a <= 0) {
      if (!s->connected() || millis() - idle_since > 20000) break;
      delay(5);
      continue;
    }
    int r = s->readBytes(buf, min((size_t)a, sizeof(buf)));
    if (r <= 0) continue;
    idle_since = millis();
    if (!begun) {
      if (!imageHeaderOk(buf, r)) { strlcpy(_err, "not an ESP32 application image", sizeof(_err)); http.end(); return false; }
      if (!Update.begin(_avail_size, U_FLASH)) { snprintf(_err, sizeof(_err), "begin: %s", Update.errorString()); http.end(); return false; }
      begun = true;
    }
    sha.update(buf, r);
    if (Update.write(buf, r) != (size_t)r) { snprintf(_err, sizeof(_err), "write: %s", Update.errorString()); Update.abort(); http.end(); return false; }
    got += r;
    _progress = (int)((uint64_t)got * 100 / _avail_size);
  }
  http.end();
  if (!begun || got != _avail_size) { strlcpy(_err, "download incomplete", sizeof(_err)); if (begun) Update.abort(); return false; }

  uint8_t h[32];
  sha.finalize(h, sizeof(h));
  if (memcmp(h, _avail_sha, 32) != 0) { strlcpy(_err, "sha256 mismatch: not installed", sizeof(_err)); Update.abort(); return false; }

  // trial: if the new firmware keeps crashing, earlyBoot() switches back to this slot
  const esp_partition_t* running = esp_ota_get_running_partition();
  Preferences p;
  if (p.begin(NVS_NS, false)) {
    p.putString("up_prev", running ? running->label : "");
    p.putUChar("up_boots", 0);
    p.putUChar("up_trial", 1);
    p.putUChar("up_rb", 0);
    p.end();
  }
  if (!Update.end(true)) { snprintf(_err, sizeof(_err), "end: %s", Update.errorString()); return false; }
  return true;
}

void Updater::task(void* arg) {
  Updater* u = (Updater*)arg;
  if (u->_want_install) {
    u->_want_install = false;
    u->_state = ST_DOWNLOADING;
    if (u->doInstall()) {
      u->_state = ST_REBOOT;
      Serial.println("Update: installed, rebooting");
      delay(1500);
      ESP.restart();
    }
    u->_state = ST_ERROR;
  } else {
    u->_want_check = false;
    u->_state = ST_CHECKING;
    bool ok = u->doCheck();
    u->_last_check_ms = millis();
    u->_checked_once = true;
    u->_state = ok ? ST_IDLE : ST_ERROR;
  }
  u->_busy = false;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
//  schedule (mesh loop)
// ---------------------------------------------------------------------------
void Updater::loop() {
  uint32_t now = millis();
  if (!_trial_cleared && now > TRIAL_OK_MS) {   // survived 10 minutes: the running firmware is good
    _trial_cleared = true;
    Preferences p;
    if (p.begin(NVS_NS, false)) {
      if (p.getUChar("up_trial", 0)) { p.putUChar("up_trial", 0); p.putUChar("up_boots", 0); }
      p.end();
    }
  }
  if (_busy || !_wifi || !_wifi->isConnected()) return;

  if (_mode != 0 && hasKey() && _url[0]) {
    bool due = !_checked_once ? now > FIRST_CHECK_MS : now - _last_check_ms > CHECK_EVERY_MS;
    if (due) _want_check = true;
  }
  // auto mode: install at night (00:00-02:59 UTC = 03-05 MSK), never over a dev build
  if (_mode == 2 && _avail_version[0] && !_want_install && strcmp(DIY_CHANNEL, "dev") != 0 && _state != ST_ERROR) {
    uint32_t t = s_now ? s_now() : 0;
    if (t > 1700000000UL && (t / 3600) % 24 < 3) _want_install = true;
  }
  if (_want_check || _want_install) {
    _busy = true;
    if (xTaskCreatePinnedToCore(task, "diy_upd", 12288, this, 1, nullptr, ARDUINO_RUNNING_CORE) != pdPASS) {
      _busy = false;
      _want_check = _want_install = false;
      strlcpy(_err, "could not start the task", sizeof(_err));
      _state = ST_ERROR;
    }
  }
}

// ---------------------------------------------------------------------------
//  CLI (local only)
// ---------------------------------------------------------------------------
bool Updater::handleCommand(const char* command, char* reply, size_t reply_len) {
  if (strncmp(command, "update", 6) != 0 || (command[6] != 0 && command[6] != ' ')) return false;
  const char* arg = command + 6;
  while (*arg == ' ') arg++;
  static const char* MODES[] = {"off", "notify", "auto"};

  if (*arg == 0) {
    snprintf(reply, reply_len, "update: %s, mode %s, beta %s, %s%s%s%s, url %s", stateText(), MODES[_mode], _beta ? "on" : "off",
             _avail_version[0] ? "available " : "no newer version", _avail_version, _err[0] ? ", last error: " : "", _err,
             _url[0] ? _url : "-");
    if (!hasKey()) strlcat(reply, " (no signing key in this build)", reply_len);
    if (_rolled_back) strlcat(reply, " (last update was rolled back)", reply_len);
    return true;
  }
  if (strcmp(arg, "check") == 0) {
    if (_busy) { snprintf(reply, reply_len, "Err - busy (%s)", stateText()); return true; }
    _want_check = true;
    snprintf(reply, reply_len, "OK - checking, see: update");
    return true;
  }
  if (strcmp(arg, "install") == 0) {
    if (_busy) { snprintf(reply, reply_len, "Err - busy (%s)", stateText()); return true; }
    if (!_avail_version[0]) { snprintf(reply, reply_len, "Err - nothing to install, run: update check"); return true; }
    _want_install = true;
    snprintf(reply, reply_len, "OK - installing %s, the board reboots when done", _avail_version);
    return true;
  }
  if (strncmp(arg, "mode ", 5) == 0) {
    int m = -1;
    for (int i = 0; i < 3; i++) if (strcmp(arg + 5, MODES[i]) == 0) m = i;
    if (m < 0) { snprintf(reply, reply_len, "Err - update mode off|notify|auto"); return true; }
    _mode = (uint8_t)m;
    save();
    snprintf(reply, reply_len, "OK - update mode %s", MODES[_mode]);
    return true;
  }
  if (strncmp(arg, "beta ", 5) == 0) {
    _beta = strcmp(arg + 5, "on") == 0;
    save();
    snprintf(reply, reply_len, "OK - pre-releases %s", _beta ? "accepted" : "ignored");
    return true;
  }
  if (strncmp(arg, "url ", 4) == 0 || strcmp(arg, "url") == 0) {
    strlcpy(_url, arg[3] ? arg + 4 : "", sizeof(_url));
    save();
    _avail_version[0] = 0;
    snprintf(reply, reply_len, "OK - url %s", _url[0] ? _url : "(none)");
    return true;
  }
  snprintf(reply, reply_len, "Err - update [check|install|mode off|notify|auto|beta on|off|url <manifest>]");
  return true;
}
