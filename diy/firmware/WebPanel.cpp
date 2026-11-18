#include "WebPanel.h"
#include "WifiLink.h"
#include "MqttLink.h"
#include "Updater.h"
#include "DiySensors.h"
#include "LanBridge.h"
#include "DiyMesh.h"
#include "JsonOut.h"

#include <WebServer.h>
#include <WiFi.h>
#ifndef DIY_BOARD_NAME
  #define DIY_BOARD_NAME "DIY board"
#endif
#ifndef DIY_BOARD_ID
  #define DIY_BOARD_ID "e22wroom"
#endif
#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif
#include "OtaServer.h"
#include <Preferences.h>
#include <esp_system.h>
#include <web_index.h>   // generated at build time by diy/scripts/embed_web.py
#include <diy_version.h> // generated at build time by diy/scripts/version.py

static WebServer server(80);
static WebPanel* self = nullptr;

static const uint32_t SESSION_IDLE_MS = 12UL * 3600UL * 1000UL;
// wait after a wrong password grows with consecutive failures: 3 s, 3 s, 10 s x3, then 60 s.
// Behind the reverse proxy every client has the same address, so the limit is global, not per IP.
static uint32_t loginBackoffMs(uint8_t fails) { return fails >= 6 ? 60000 : fails >= 3 ? 10000 : fails ? 3000 : 0; }
static const uint32_t LOGIN_FAIL_FORGET_MS = 10UL * 60UL * 1000UL;   // quiet for 10 min -> start over

// short reason of the last reset for the panel
const char* diyResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power";
    case ESP_RST_EXT: return "pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
  }
}

static bool constTimeEquals(const char* a, const char* b) {
  size_t la = strlen(a), lb = strlen(b);
  uint8_t diff = la != lb;
  for (size_t i = 0; i < la && i < lb; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

static void keyHex(char* out, const uint8_t* b, int n) {
  static const char* H = "0123456789ABCDEF";
  for (int i = 0; i < n; i++) { out[i * 2] = H[b[i] >> 4]; out[i * 2 + 1] = H[b[i] & 15]; }
  out[n * 2] = 0;
}

static bool clockOk(uint32_t t) { return t > 1700000000UL; }

// ======================================================================
//  lifecycle
// ======================================================================
void WebPanel::begin(DiyMesh* mesh, WifiLink* wifi, CommandFn run_command) {
  _mesh = mesh;
  _wifi = wifi;
  _run_command = run_command;
  self = this;

  Preferences p;
  if (p.begin("diy", true)) { p.getString("webpwd", _web_pwd, sizeof(_web_pwd)); p.end(); }

  static const char* headers[] = {"Cookie"};
  server.collectHeaders(headers, 1);

  server.on("/", HTTP_GET, [] { self->handlePage(); });
  server.on("/api/login", HTTP_POST, [] { self->handleLogin(); });
  server.on("/api/logout", HTTP_POST, [] { self->handleLogout(); });
  server.on("/api/status", HTTP_GET, [] { self->handleStatus(); });
  server.on("/api/stats", HTTP_GET, [] { self->handleStats(); });
  server.on("/api/log", HTTP_GET, [] { self->handleLog(); });
  server.on("/api/airtime", HTTP_GET, [] { self->handleAirtime(); });
  server.on("/api/channels", HTTP_GET, [] { self->handleChannels(); });
  server.on("/api/channels", HTTP_POST, [] { self->handleChannelAction(); });
  server.on("/api/nodes", HTTP_GET, [] { self->handleNodes(); });
  server.on("/api/nodes", HTTP_POST, [] { self->handleNodeAction(); });
  server.on("/api/graph", HTTP_GET, [] { self->handleGraph(); });
  server.on("/api/trace", HTTP_GET, [] { self->handleTrace(); });
  server.on("/api/trace", HTTP_POST, [] { self->handleTraceStart(); });
  server.on("/api/prefs", HTTP_GET, [] { self->handlePrefs(); });
  server.on("/api/cli", HTTP_POST, [] { self->handleCli(); });
  server.on("/api/webpass", HTTP_POST, [] { self->handleWebPass(); });
  server.on("/api/reboot", HTTP_POST, [] { self->handleReboot(); });
  server.on("/api/update/token", HTTP_POST, [] { self->handleUpdateToken(); });
  server.on("/api/setup", HTTP_GET, [] { self->handleSetupState(); });
  server.on("/api/setup", HTTP_POST, [] { self->handleSetupSave(); });
  server.on("/api/setup/scan", HTTP_GET, [] { self->handleSetupScan(); });
  server.on("/api/update", HTTP_GET, [] { self->handleUpdateState(); });
  server.on("/api/chat", HTTP_GET, [] { self->handleChat(); });
  server.on("/api/chat", HTTP_POST, [] { self->handleChatSend(); });
  server.on("/api/watch", HTTP_GET, [] { self->handleWatch(); });
  server.onNotFound([] { self->handleNotFound(); });
}

void WebPanel::loop() {
  if (!_reboot_at && ota_server.rebootRequested()) _reboot_at = millis() + 1500;
  if (_reboot_at && (int32_t)(millis() - _reboot_at) > 0) {
    delay(100);
    ESP.restart();
  }
  if (_setup_apply_at && (int32_t)(millis() - _setup_apply_at) > 0) {
    _setup_apply_at = 0;
    _wifi->stopSetupAP();
    if (_setup_ssid[0]) {
      char cmd[112], reply[160];
      snprintf(cmd, sizeof(cmd), "wifi ssid %s", _setup_ssid); _run_command(cmd, reply, sizeof(reply));
      snprintf(cmd, sizeof(cmd), "wifi pwd %s", _setup_wpwd); _run_command(cmd, reply, sizeof(reply));
      memset(cmd, 0, sizeof(cmd));
    }
    memset(_setup_wpwd, 0, sizeof(_setup_wpwd));
    if (_setup_reboot) _reboot_at = millis() + 3000;   // radio settings apply after a restart
  }
  if (!_wifi->isConnected() && !_wifi->setupAPActive()) return;
  if (!_started) {
    server.begin();
    _started = true;
  }
  server.handleClient();
}

// ======================================================================
//  auth
// ======================================================================
const char* WebPanel::loginPassword() {
  return _web_pwd[0] ? _web_pwd : _mesh->getNodePrefs()->password;
}

bool WebPanel::authorized() {
  String cookie = server.header("Cookie");
  int p = cookie.indexOf("s=");
  if (p < 0) return false;
  String token = cookie.substring(p + 2, p + 2 + 32);
  uint32_t now = millis();
  for (auto& s : _sessions) {
    if (s.token[0] && now - s.last_used < SESSION_IDLE_MS && constTimeEquals(s.token, token.c_str())) {
      s.last_used = now;
      return true;
    }
  }
  return false;
}

bool WebPanel::requireAuth() {
  if (authorized()) return true;
  sendError(401, "login required");
  return false;
}

void WebPanel::dropSessions() {
  for (auto& s : _sessions) s.token[0] = 0;
}

void WebPanel::handleLogin() {
  uint32_t now = millis();
  if (_failed_logins && now - _last_failed_login > LOGIN_FAIL_FORGET_MS) _failed_logins = 0;
  uint32_t wait = loginBackoffMs(_failed_logins);
  if (_failed_logins && now - _last_failed_login < wait) {
    char msg[48];
    snprintf(msg, sizeof(msg), "too many attempts, wait %lu s", (unsigned long)((wait - (now - _last_failed_login)) / 1000 + 1));
    sendError(429, msg);
    return;
  }
  const char* pwd_ok = loginPassword();
  if (pwd_ok[0] == 0) {
    sendError(403, "admin password is empty: set it via USB CLI first");
    return;
  }
  String pwd = server.arg("password");
  if (!constTimeEquals(pwd.c_str(), pwd_ok)) {
    _last_failed_login = now;
    if (_failed_logins < 255) _failed_logins++;
    sendError(401, "wrong password");
    return;
  }
  _last_failed_login = 0;
  _failed_logins = 0;

  Session* slot = &_sessions[0];
  for (auto& s : _sessions) {
    if (!s.token[0]) { slot = &s; break; }
    if (s.last_used < slot->last_used) slot = &s;
  }
  for (int i = 0; i < 16; i++) sprintf(&slot->token[i * 2], "%02x", (unsigned)(esp_random() & 0xFF));
  slot->last_used = now;

  server.sendHeader("Set-Cookie", String("s=") + slot->token + "; Path=/; HttpOnly; SameSite=Strict");
  sendOk();
}

void WebPanel::handleLogout() {
  String cookie = server.header("Cookie");
  int p = cookie.indexOf("s=");
  if (p >= 0) {
    String token = cookie.substring(p + 2, p + 2 + 32);
    for (auto& s : _sessions) if (s.token[0] && constTimeEquals(s.token, token.c_str())) s.token[0] = 0;
  }
  server.sendHeader("Set-Cookie", "s=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  sendOk();
}

void WebPanel::handleWebPass() {
  if (!requireAuth()) return;
  String cur = server.arg("current"), nw = server.arg("new");
  if (!constTimeEquals(cur.c_str(), loginPassword())) { sendError(403, "current password is wrong"); return; }
  if (nw.length() > 0 && nw.length() < 6) { sendError(400, "new password: at least 6 characters"); return; }
  if (nw.length() > 32) { sendError(400, "new password: at most 32 characters"); return; }
  strlcpy(_web_pwd, nw.c_str(), sizeof(_web_pwd));
  Preferences p;
  if (p.begin("diy", false)) { p.putString("webpwd", _web_pwd); p.end(); }
  dropSessions();
  sendOk(_web_pwd[0] ? "web password set" : "web password cleared, admin password is used");
}

// ======================================================================
//  page
// ======================================================================
void WebPanel::handlePage() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", (const char*)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

// ======================================================================
//  status / stats
// ======================================================================
void WebPanel::handleStatus() {
  if (!requireAuth()) return;
  NodePrefs* p = _mesh->getNodePrefs();
  uint32_t now_ms = millis();
  uint32_t rtc = _mesh->getRTCClock()->getCurrentTime();
  JsonOut j(server);
  j.begin();
  j.raw("{\"name\":").str(p->node_name);
  j.raw(",\"firmware\":").str(_mesh->getFirmwareVer());
  j.raw(",\"build\":").str(_mesh->getBuildDate());
  j.raw(",\"diy_version\":").str(DIY_VERSION).raw(",\"diy_build\":").str(DIY_BUILD_DATE);
  j.raw(",\"diy_channel\":").str(DIY_CHANNEL);
  j.raw(",\"reset_reason\":").str(diyResetReason());
  j.raw(",\"board\":").str(DIY_BOARD_NAME);
  j.raw(",\"board_id\":").str(DIY_BOARD_ID);
  j.f(",\"chip_id\":%d", DIY_IMAGE_CHIP_ID);   // the panel checks a firmware file against it before upload
  if (uint16_t mv = diyBattMilliVolts()) j.f(",\"batt_mv\":%u", (unsigned)mv);
  {   // I2C sensors: [[channel, "name", value, "unit"], ...]
    DiySensorValue v[16];
    int n = diyReadSensors(v, 16);
    j.raw(",\"sensors\":[");
    for (int i = 0; i < n; i++) {
      if (i) j.raw(",");
      j.f("[%u,", v[i].channel).str(v[i].name).f(",%.2f,", v[i].value).str(v[i].unit ? v[i].unit : "").raw("]");
    }
    j.raw("]");
  }
  j.raw(",\"public_key\":").hex(_mesh->self_id.pub_key, PUB_KEY_SIZE);
  j.f(",\"uptime_secs\":%lu,\"clock\":%lu,\"clock_ok\":%s", (unsigned long)(now_ms / 1000), (unsigned long)rtc,
      clockOk(rtc) ? "true" : "false");
  j.f(",\"lat\":%.6f,\"lon\":%.6f", p->node_lat, p->node_lon);
  j.f(",\"radio\":{\"freq\":%.3f,\"bw\":%.1f,\"sf\":%u,\"cr\":%u,\"tx_dbm\":%d,\"rx_boost\":%s}",
      p->freq, p->bw, p->sf, p->cr, p->tx_power_dbm, p->rx_boosted_gain ? "true" : "false");
  j.f(",\"repeat\":%s,\"advert_interval_min\":%u,\"flood_advert_h\":%u", p->disable_fwd ? "false" : "true",
      (unsigned)p->advert_interval * 2, (unsigned)p->flood_advert_interval);
  j.raw(",\"wifi\":{\"ssid\":").str(_wifi->ssid()).raw(",\"ip\":").str(_wifi->ipString().c_str());
  j.f(",\"rssi\":%d}", _wifi->rssi());
  j.f(",\"heap\":{\"free\":%u,\"min_free\":%u,\"max_block\":%u}", (unsigned)ESP.getFreeHeap(),
      (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  j.f(",\"noise_floor\":%d", _mesh->noiseFloor());

  // noise floor, last 4 minutes (oldest first)
  j.raw(",\"nf_recent\":[");
  bool first = true;
  for (int i = 0; i < _mesh->nf_count; i++) {
    int idx = (_mesh->nf_head - _mesh->nf_count + i + DiyMesh::NF_SAMPLES) % DiyMesh::NF_SAMPLES;
    j.sep(first).f("%d", _mesh->nf_recent[idx]);
  }
  j.raw("]");

  // last received packet
  if (const DiyMesh::PktEntry* lp = _mesh->lastRx()) {
    const DiyMesh::PktEntry& e = *lp;
    char from[8] = "";
    if (e.hops > 0 && e.hash_sz <= 3 && e.path_n >= e.hops * e.hash_sz) keyHex(from, &e.path[(e.hops - 1) * e.hash_sz], e.hash_sz);
    j.f(",\"last_rx\":{\"snr\":%.2f,\"rssi\":%d,\"ago\":%lu,\"hops\":%u,\"type\":%u,\"from\":\"%s\"}", e.snr4 / 4.0f, e.rssi,
        (unsigned long)((now_ms - e.ms) / 1000), e.hops, e.type, from);
  } else {
    j.raw(",\"last_rx\":null");
  }

  // airtime over the last 24 h (10-minute buckets)
  uint32_t rx24 = 0, tx24 = 0; int n24 = 0;
  for (auto& b : _mesh->tens) if (b.valid) { rx24 += b.rx_air_ms; tx24 += b.tx_air_ms; n24++; }
  j.f(",\"air24\":{\"tx_ms\":%lu,\"rx_ms\":%lu,\"span_s\":%lu}", (unsigned long)tx24, (unsigned long)rx24,
      (unsigned long)(n24 ? (n24 * 600UL < now_ms / 1000 ? n24 * 600UL : now_ms / 1000) : 0));
  lan_bridge.writeStatus(j);
  j.f(",\"air_total\":{\"tx_ms\":%lu,\"rx_ms\":%lu}", (unsigned long)_mesh->airTxMs(), (unsigned long)_mesh->airRxMs());
  j.raw("}");
  j.end();
}

void WebPanel::handleStats() {
  if (!requireAuth()) return;
  char buf[256];
  String s = "{\"core\":";
  buf[0] = 0; _mesh->formatStatsReply(buf);        s += buf[0] ? buf : "null";
  s += ",\"radio\":";
  buf[0] = 0; _mesh->formatRadioStatsReply(buf);   s += buf[0] ? buf : "null";
  s += ",\"packets\":";
  buf[0] = 0; _mesh->formatPacketStatsReply(buf);  s += buf[0] ? buf : "null";
  s += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

// ======================================================================
//  packet log and airtime
// ======================================================================
void WebPanel::handleLog() {
  if (!requireAuth()) return;
  uint32_t since = server.hasArg("since") ? strtoul(server.arg("since").c_str(), nullptr, 10) : 0;
  uint32_t now_ms = millis();
  JsonOut j(server);
  j.begin();
  j.f("{\"seq\":%lu,\"e\":[", (unsigned long)_mesh->logSeq());
  bool first = true;
  for (int i = 0; i < _mesh->logCount(); i++) {
    const DiyMesh::PktEntry& e = _mesh->logAt(i);
    if (e.seq <= since) continue;
    // [seq, ts, ago_s, dir, type, route, hops, hash_sz, len, snr, rssi, ch, "path"]
    j.sep(first).f("[%lu,%lu,%lu,%u,%u,%u,%u,%u,%u,%.2f,%d,%d,", (unsigned long)e.seq, (unsigned long)e.ts,
                   (unsigned long)((now_ms - e.ms) / 1000), e.dir, e.type, e.route, e.hops, e.hash_sz, e.len,
                   e.snr4 / 4.0f, e.rssi, e.ch == 255 ? -1 : e.ch);
    j.hex(e.path, e.path_n).raw("]");
  }
  j.raw("]}");
  j.end();
}

void WebPanel::handleAirtime() {
  if (!requireAuth()) return;
  uint32_t up = millis() / 1000;
  JsonOut j(server);
  j.begin();
  j.f("{\"uptime_secs\":%lu,\"tx_ms\":%lu,\"rx_ms\":%lu", (unsigned long)up, (unsigned long)_mesh->airTxMs(),
      (unsigned long)_mesh->airRxMs());
  j.f(",\"rx_flood\":%lu,\"rx_direct\":%lu,\"tx_flood\":%lu,\"tx_direct\":%lu,\"tx_fail\":%lu,\"blocked\":%lu",
      (unsigned long)_mesh->rx_flood, (unsigned long)_mesh->rx_direct, (unsigned long)_mesh->tx_flood,
      (unsigned long)_mesh->tx_direct, (unsigned long)_mesh->tx_fail, (unsigned long)_mesh->blocked_count);
  bool first = true;
  j.raw(",\"rx_by_type\":[");
  for (int i = 0; i < 16; i++) j.sep(first).f("%lu", (unsigned long)_mesh->rx_by_type[i]);
  first = true;
  j.raw("],\"tx_by_type\":[");
  for (int i = 0; i < 16; i++) j.sep(first).f("%lu", (unsigned long)_mesh->tx_by_type[i]);
  j.raw("]");

  // buckets oldest -> newest: [rx_air_ms, tx_air_ms, rx, tx, nf_avg, nf_min, nf_max]
  auto dump = [&](const char* name, const DiyMesh::Bucket* b, int count, int head) {
    j.raw(",\"").raw(name).raw("\":[");
    bool f1 = true;
    for (int i = 1; i <= count; i++) {
      const DiyMesh::Bucket& x = b[(head + i) % count];
      if (!x.valid) continue;
      j.sep(f1).f("[%lu,%lu,%u,%u,%d,%d,%d]", (unsigned long)x.rx_air_ms, (unsigned long)x.tx_air_ms, x.rx, x.tx,
                  x.nf_avg, x.nf_min, x.nf_max);
    }
    j.raw("]");
  };
  dump("minutes", _mesh->minutes, DiyMesh::MIN_BUCKETS, _mesh->minute_head);
  dump("tens", _mesh->tens, DiyMesh::TEN_BUCKETS, _mesh->ten_head);
  j.raw("}");
  j.end();
}

// ======================================================================
//  channels and entry repeaters
// ======================================================================
void WebPanel::handleChannels() {
  if (!requireAuth()) return;
  uint32_t now = millis();
  char k[8];
  JsonOut j(server);
  j.begin();
  j.f("{\"public_hash\":%u,\"repeat_public\":%s,\"blocked_count\":%lu", _mesh->public_hash,
      _mesh->repeat_public ? "true" : "false", (unsigned long)_mesh->blocked_count);
  bool first = true;
  j.raw(",\"rx_by_type\":[");
  for (int i = 0; i < 16; i++) j.sep(first).f("%lu", (unsigned long)_mesh->rx_by_type[i]);
  j.raw("],\"chans\":[");
  first = true;
  for (auto& c : _mesh->chans) {
    if (!c.used) continue;
    j.sep(first).f("{\"hash\":%u,\"count\":%lu,\"ago\":%lu}", c.hash, (unsigned long)c.count,
                   (unsigned long)((now - c.last_ms) / 1000));
  }
  j.raw("],\"entries\":[");
  first = true;
  for (int i = 0; i < _mesh->num_entries; i++) {
    const auto& e = _mesh->entries[i];
    keyHex(k, e.key.b, e.key.sz);
    const DiyMesh::NodeInfo* n = _mesh->findNodeByPrefix(e.key.b, e.key.sz);
    j.sep(first).f("{\"key\":\"%s\",\"count\":%lu,\"ago\":%lu,\"name\":", k, (unsigned long)e.count,
                   (unsigned long)((now - e.last_ms) / 1000));
    j.str(n ? n->name : "").raw("}");
  }
  j.raw("],\"blocked_chans\":[");
  first = true;
  for (int i = 0; i < _mesh->num_blocked_chans; i++) j.sep(first).f("%u", _mesh->blocked_chans[i]);
  j.raw("],\"blocked_entries\":[");
  first = true;
  for (int i = 0; i < _mesh->num_blocked_entries; i++) {
    keyHex(k, _mesh->blocked_entries[i].b, _mesh->blocked_entries[i].sz);
    j.sep(first).str(k);
  }
  j.raw("],\"blocked_routes\":[");
  first = true;
  for (int i = 0; i < _mesh->num_blocked_routes; i++) {
    char a[8], b[8];
    keyHex(a, _mesh->blocked_routes[i].a.b, _mesh->blocked_routes[i].a.sz);
    keyHex(b, _mesh->blocked_routes[i].b.b, _mesh->blocked_routes[i].b.sz);
    j.sep(first).f("\"%s>%s\"", a, b);
  }
  j.raw("]}");
  j.end();
}

// POST /api/channels  action=block|unblock|public|clear  kind=ch|tag|entry|route  value=...
void WebPanel::handleChannelAction() {
  if (!requireAuth()) return;
  String action = server.arg("action"), kind = server.arg("kind"), value = server.arg("value");
  value.trim();
  if (action == "clear") { _mesh->clearChannelStats(); sendOk(); return; }
  if (action == "public") { _mesh->setRepeatPublic(value == "1" || value == "on"); sendOk(); return; }
  bool block = action == "block";
  if (!block && action != "unblock") { sendError(400, "bad action"); return; }

  bool ok = false;
  if (kind == "ch") {
    char* end;
    long h = strtol(value.c_str(), &end, 16);
    if (*end || value.length() != 2) { sendError(400, "channel hash: 2 hex digits"); return; }
    ok = _mesh->blockChannel((uint8_t)h, block);
  } else if (kind == "tag") {
    if (value.length() < 2 || value[0] != '#') { sendError(400, "hashtag must start with #"); return; }
    value.toLowerCase();
    ok = _mesh->blockChannel(DiyMesh::hashtagChannelHash(value.c_str()), block);
  } else if (kind == "entry") {
    DiyMesh::Key3 key;
    value.toUpperCase();
    if (!DiyMesh::parseKey(value.c_str(), key)) { sendError(400, "repeater id: 2, 4 or 6 hex digits"); return; }
    ok = _mesh->blockEntry(key, block);
  } else if (kind == "route") {
    int p = value.indexOf('>');
    DiyMesh::RouteKey r;
    value.toUpperCase();
    if (p < 0 || !DiyMesh::parseKey(value.substring(0, p).c_str(), r.a) || !DiyMesh::parseKey(value.substring(p + 1).c_str(), r.b)) {
      sendError(400, "route: A>B, hex ids");
      return;
    }
    ok = _mesh->blockRoute(r, block);
  } else {
    sendError(400, "bad kind");
    return;
  }
  if (ok) sendOk(); else sendError(409, "block list is full (16)");
}

// ======================================================================
//  nodes, graph, trace
// ======================================================================
void WebPanel::handleNodes() {
  if (!requireAuth()) return;
  uint32_t now = millis();
  JsonOut j(server);
  j.begin();
  uint32_t scan_age = _mesh->scan_started_ms ? (now - _mesh->scan_started_ms) / 1000 : 0;
  j.f("{\"scanning\":%s,\"scan_age\":%lu,\"n\":[",
      (_mesh->scan_started_ms && now - _mesh->scan_started_ms < 60000) ? "true" : "false", (unsigned long)scan_age);
  bool first = true;
  for (int i = 0; i < _mesh->num_nodes; i++) {
    const auto& n = _mesh->nodes[i];
    j.sep(first).raw("{\"id\":").hex(n.pub, n.known);
    j.raw(",\"name\":").str(n.name);
    j.f(",\"type\":%u,\"seen\":%u,\"hops\":%u,\"ago\":%lu,\"count\":%lu", n.type, n.seen, n.hops,
        (unsigned long)((now - n.last_ms) / 1000), (unsigned long)n.count);
    if (n.snr4 != -128) j.f(",\"snr\":%.2f,\"rssi\":%d", n.snr4 / 4.0f, n.rssi);
    if (n.their_snr4 != -128) j.f(",\"their_snr\":%.2f", n.their_snr4 / 4.0f);
    if (n.lat || n.lon) j.f(",\"lat\":%.5f,\"lon\":%.5f", n.lat / 1e6, n.lon / 1e6);
    j.raw("}");
  }
  j.raw("]}");
  j.end();
}

void WebPanel::handleNodeAction() {
  if (!requireAuth()) return;
  String action = server.arg("action");
  if (action == "scan") {
    if (_mesh->startScan()) sendOk("scan sent"); else sendError(429, "wait 30 s between scans");
  } else if (action == "clear") {
    _mesh->clearNodes(); sendOk();
  } else if (action == "clear_graph") {
    _mesh->clearGraph(); sendOk();
  } else {
    sendError(400, "bad action");
  }
}

void WebPanel::handleGraph() {
  if (!requireAuth()) return;
  uint32_t now = millis();
  char self_hex[8];
  keyHex(self_hex, _mesh->self_id.pub_key, 1);
  JsonOut j(server);
  j.begin();
  j.f("{\"self\":\"%s\",\"forgotten\":%lu,\"max\":%d,\"n\":[", self_hex, (unsigned long)_mesh->graph_forgotten,
      DiyMesh::MAX_GNODES);
  bool first = true;
  char k[8];
  for (int i = 0; i < DiyMesh::MAX_GNODES; i++) {
    const auto& g = _mesh->gnodes[i];
    if (!g.used) continue;
    keyHex(k, g.key, g.sz);
    // [id, parent(-1 = us), "key", orig, count, snr|null, age_s]
    j.sep(first).f("[%d,%d,\"%s\",%u,%u,", i, g.parent == 0xFFFF ? -1 : (int)g.parent, k, g.orig, g.count);
    if (g.snr4 == -128) j.raw("null"); else j.f("%.2f", g.snr4 / 4.0f);
    j.f(",%lu]", (unsigned long)((now - g.last_ms) / 1000));
  }
  j.raw("]}");
  j.end();
}

void WebPanel::handleTrace() {
  if (!requireAuth()) return;
  const auto& t = _mesh->trace;
  JsonOut j(server);
  j.begin();
  j.f("{\"pending\":%s,\"ok\":%s,\"sent\":%s,\"rtt_ms\":%lu,\"sz\":%u,\"hops\":[", t.pending ? "true" : "false",
      t.ok ? "true" : "false", t.sent_ms ? "true" : "false", (unsigned long)t.rtt_ms, t.sz);
  bool first = true;
  char k[12];
  for (int i = 0; i < t.n; i++) {
    keyHex(k, &t.hashes[i * t.sz], t.sz);
    j.sep(first).f("{\"id\":\"%s\"", k);
    if (t.ok) j.f(",\"snr\":%.2f", t.snrs[i] / 4.0f);
    j.raw("}");
  }
  j.raw("]");
  if (t.ok) j.f(",\"final_snr\":%.2f", t.final_snr4 / 4.0f);
  j.raw("}");
  j.end();
}

void WebPanel::handleTraceStart() {
  if (!requireAuth()) return;
  String hops = server.arg("hops");
  hops.toUpperCase();
  const char* r = _mesh->startTrace(hops.c_str());
  if (strncmp(r, "OK", 2) == 0) sendOk(r); else sendError(400, r);
}

// ======================================================================
//  settings (read; writes go through CLI commands)
// ======================================================================
void WebPanel::handlePrefs() {
  if (!requireAuth()) return;
  NodePrefs* p = _mesh->getNodePrefs();
  char regions[200];
  char cmd[16];
  strcpy(cmd, "region");
  regions[0] = 0;
  _run_command(cmd, regions, sizeof(regions));
  char home[80];
  strcpy(cmd, "region home");
  home[0] = 0;
  _run_command(cmd, home, sizeof(home));

  JsonOut j(server);
  j.begin();
  j.raw("{\"name\":").str(p->node_name);
  j.raw(",\"owner\":").str(p->owner_info);
  j.f(",\"lat\":%.6f,\"lon\":%.6f", p->node_lat, p->node_lon);
  j.f(",\"freq\":%.3f,\"bw\":%.1f,\"sf\":%u,\"cr\":%u,\"tx\":%d,\"rxgain\":%u", p->freq, p->bw, p->sf, p->cr,
      p->tx_power_dbm, p->rx_boosted_gain);
  j.f(",\"repeat\":%u,\"adv_int\":%u,\"fadv_int\":%u,\"fmax\":%u,\"fmax_uns\":%u,\"fmax_adv\":%u,\"loop\":%u,\"hash_mode\":%u",
      p->disable_fwd ? 0 : 1, (unsigned)p->advert_interval * 2, p->flood_advert_interval, p->flood_max,
      p->flood_max_unscoped, p->flood_max_advert, p->loop_detect, p->path_hash_mode);
  j.f(",\"af\":%.2f,\"rxdelay\":%.2f,\"txdelay\":%.2f,\"dtxdelay\":%.2f,\"int_thresh\":%u,\"cad\":%u,\"agc\":%u,\"multi_acks\":%u",
      p->airtime_factor, p->rx_delay_base, p->tx_delay_factor, p->direct_tx_delay_factor, p->interference_threshold,
      p->cad_enabled, (unsigned)p->agc_reset_interval * 4, p->multi_acks);
  j.f(",\"adc_mult\":%.3f,\"read_only\":%u,\"has_guest\":%s", p->adc_multiplier, p->allow_read_only,
      p->guest_password[0] ? "true" : "false");
  j.raw(",\"regions\":").str(regions).raw(",\"region_home\":").str(home);
  j.raw(",\"wifi_ssid\":").str(_wifi->ssid());
  j.f(",\"web_pwd_set\":%s", _web_pwd[0] ? "true" : "false");
  j.f(",\"home_set\":%s,\"home_rx\":%lu,\"home_tx\":%lu", _mesh->homeChannelSet() ? "true" : "false",
      (unsigned long)_mesh->home_rx, (unsigned long)_mesh->home_tx);
  j.raw(",\"home_name\":").str(_mesh->homeChannelName());
  lan_bridge.writePrefs(j);   // the bridge secret is never sent back
  if (_mqtt) {   // the broker password is never sent back
    j.f(",\"mqtt_on\":%u,\"mqtt_port\":%u,\"mqtt_int\":%u", _mqtt->enabled() ? 1 : 0, (unsigned)_mqtt->port(), (unsigned)_mqtt->interval());
    j.raw(",\"mqtt_host\":").str(_mqtt->host()).raw(",\"mqtt_user\":").str(_mqtt->user()).raw(",\"mqtt_state\":").str(_mqtt->stateText());
  }
  j.raw("}");
  j.end();
}

// ======================================================================
//  console, reboot, firmware update
// ======================================================================
void WebPanel::handleCli() {
  if (!requireAuth()) return;
  String cmd = server.arg("cmd");
  cmd.trim();
  if (cmd.length() == 0 || cmd.length() > 150) { sendError(400, "bad command"); return; }
  char command[160];
  strlcpy(command, cmd.c_str(), sizeof(command));
  char reply[256];
  reply[0] = 0;
  _run_command(command, reply, sizeof(reply));
  JsonOut j(server);
  j.begin();
  j.raw("{\"reply\":").str(reply).raw("}");
  j.end();
}

void WebPanel::handleReboot() {
  if (!requireAuth()) return;
  _reboot_at = millis() + 1000;
  sendOk("rebooting");
}

// One-time token for the upload server (OtaServer, port 8081): the upload runs in
// its own task, so the repeater keeps working while the image is written.
void WebPanel::handleUpdateToken() {
  if (!requireAuth()) return;
  char token[33];
  if (!ota_server.issueToken(token)) { sendError(500, "could not start the upload task"); return; }
  JsonOut j(server);
  j.begin();
  j.f("{\"token\":\"%s\",\"port\":%u}", token, (unsigned)OtaServer::PORT);
  j.end();
}

// ======================================================================
//  first-run wizard
// ======================================================================
// The request came in over the setup access point (the phone is next to the board).
bool WebPanel::viaSetupAP() {
  return _wifi->setupAPActive() && server.client().localIP() == WiFi.softAPIP();
}

bool WebPanel::factoryPassword() {
  return strcmp(_mesh->getNodePrefs()->password, ADMIN_PASSWORD) == 0;
}

// Logged in, or over the setup AP while the admin password is still the factory one.
// Sends 401 when not allowed.
bool WebPanel::setupAllowed(bool ap) {
  if (authorized() || (ap && factoryPassword())) return true;
  sendError(401, "login required");
  return false;
}

// Captive portal: over the setup AP every unknown URL (phone connectivity checks) goes to the wizard.
void WebPanel::handleNotFound() {
  if (viaSetupAP()) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  sendError(404, "not found");
}

// What the wizard needs. Without a login only over the setup AP (and then only the
// "is setup open" flags unless the password is still the factory one).
void WebPanel::handleSetupState() {
  bool ap = viaSetupAP();
  if (!ap && !requireAuth()) return;
  bool open = authorized() || (ap && factoryPassword());
  NodePrefs* p = _mesh->getNodePrefs();
  JsonOut j(server);
  j.begin();
  j.f("{\"via_ap\":%s,\"open\":%s,\"need_password\":%s", ap ? "true" : "false", open ? "true" : "false",
      factoryPassword() ? "true" : "false");
  j.raw(",\"board\":").str(DIY_BOARD_NAME);
  if (open) {
    j.raw(",\"name\":").str(p->node_name);
    j.f(",\"lat\":%.6f,\"lon\":%.6f,\"radio\":{\"freq\":%.3f,\"bw\":%.1f,\"sf\":%u,\"cr\":%u}", p->node_lat, p->node_lon,
        p->freq, p->bw, p->sf, p->cr);
    j.raw(",\"wifi_ssid\":").str(_wifi->ssid());
  }
  j.raw("}");
  j.end();
}

void WebPanel::handleSetupScan() {
  if (!setupAllowed(viaSetupAP())) return;
  int n = _wifi->scanNetworks(server.hasArg("start"));
  JsonOut j(server);
  j.begin();
  if (n < 0) { j.raw("{\"scanning\":true,\"nets\":[]}"); j.end(); return; }
  j.raw("{\"scanning\":false,\"nets\":[");
  bool first = true;
  for (int i = 0; i < n && i < 30; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    j.sep(first).raw("[").str(s.c_str()).f(",%d,%s]", (int)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true");
  }
  j.raw("]}");
  j.end();
}

static bool parseCoord(const String& s, double lo, double hi, double& out) {
  if (!s.length()) return false;
  char* end = nullptr;
  out = strtod(s.c_str(), &end);
  return end && *end == 0 && out >= lo && out <= hi;
}

// Applies the wizard form through the same CLI commands a user would type.
void WebPanel::handleSetupSave() {
  if (!setupAllowed(viaSetupAP())) return;
  if (_setup_apply_at) { sendError(409, "already applying"); return; }

  String pwd = server.arg("password"), name = server.arg("name"), ssid = server.arg("ssid"), wpwd = server.arg("wpwd");
  name.trim();
  if (factoryPassword() && !pwd.length()) { sendError(400, "new admin password required"); return; }
  if (pwd.length() && (pwd.length() < 6 || pwd.length() > 15)) { sendError(400, "admin password: 6-15 characters"); return; }
  if (pwd.length() && pwd == ADMIN_PASSWORD) { sendError(400, "admin password must not be the factory one"); return; }
  if (name.length() > 31) { sendError(400, "name: at most 31 characters"); return; }
  if (ssid.length() > 32 || wpwd.length() > 64) { sendError(400, "wifi: name up to 32, password up to 64 characters"); return; }
  double lat = 0, lon = 0;
  bool has_lat = server.arg("lat").length() > 0, has_lon = server.arg("lon").length() > 0;
  if ((has_lat && !parseCoord(server.arg("lat"), -90, 90, lat)) || (has_lon && !parseCoord(server.arg("lon"), -180, 180, lon))) {
    sendError(400, "coordinates: latitude -90..90, longitude -180..180");
    return;
  }
  NodePrefs* p = _mesh->getNodePrefs();
  bool radio = false;
  double freq = p->freq, bw = p->bw;
  long sf = p->sf, cr = p->cr;
  if (server.arg("freq").length()) {
    freq = server.arg("freq").toDouble(); bw = server.arg("bw").toDouble();
    sf = server.arg("sf").toInt(); cr = server.arg("cr").toInt();
    if (freq < 150 || freq > 960 || bw <= 0 || sf < 5 || sf > 12 || cr < 5 || cr > 8) {
      sendError(400, "radio: check frequency, BW, SF (5-12), CR (5-8)");
      return;
    }
    radio = fabs(freq - p->freq) > 0.0005 || fabs(bw - p->bw) > 0.05 || sf != p->sf || cr != p->cr;
  }
  bool first_run = factoryPassword();

  char cmd[112], reply[200];
  if (name.length()) { snprintf(cmd, sizeof(cmd), "set name %s", name.c_str()); _run_command(cmd, reply, sizeof(reply)); }
  if (has_lat) { snprintf(cmd, sizeof(cmd), "set lat %.6f", lat); _run_command(cmd, reply, sizeof(reply)); }
  if (has_lon) { snprintf(cmd, sizeof(cmd), "set lon %.6f", lon); _run_command(cmd, reply, sizeof(reply)); }
  if (radio) { snprintf(cmd, sizeof(cmd), "set radio %.3f,%.1f,%ld,%ld", freq, bw, sf, cr); _run_command(cmd, reply, sizeof(reply)); }
  if (first_run) { strcpy(cmd, "set advert.interval 60"); _run_command(cmd, reply, sizeof(reply)); }   // hourly zero-hop adverts
  if (pwd.length()) {   // the reply of "password" echoes it: never sent back, wiped right away
    snprintf(cmd, sizeof(cmd), "password %s", pwd.c_str()); _run_command(cmd, reply, sizeof(reply));
    memset(reply, 0, sizeof(reply));
    memset(cmd, 0, sizeof(cmd));
    dropSessions();
  }
  strlcpy(_setup_ssid, ssid.c_str(), sizeof(_setup_ssid));
  strlcpy(_setup_wpwd, wpwd.c_str(), sizeof(_setup_wpwd));
  _setup_reboot = radio;
  _setup_apply_at = millis() + 2500;

  JsonOut j(server);
  j.begin();
  j.f("{\"ok\":true,\"reboot\":%s,\"wifi\":%s}", radio ? "true" : "false", _setup_ssid[0] ? "true" : "false");
  j.end();
}

// auto-update state for the Maintenance tab; actions go through the CLI ("update check" etc.)
void WebPanel::handleUpdateState() {
  if (!requireAuth()) return;
  if (!_updater) { sendError(404, "no updater"); return; }
  JsonOut j(server);
  j.begin();
  j.f("{\"mode\":%u,\"beta\":%s,\"key\":%s,\"rolled_back\":%s,\"progress\":%d,\"checked_ago\":%lu", _updater->mode(),
      _updater->beta() ? "true" : "false", _updater->hasKey() ? "true" : "false", _updater->rolledBack() ? "true" : "false",
      _updater->progress(), (unsigned long)_updater->lastCheckAgoS());
  j.raw(",\"state\":").str(_updater->stateText()).raw(",\"url\":").str(_updater->url());
  j.raw(",\"available\":").str(_updater->available()).raw(",\"channel\":").str(_updater->availableChannel());
  j.raw(",\"error\":").str(_updater->lastError());
  j.raw("}");
  j.end();
}

// panel chat: ?ch=0 "Home" (default), 1..4 extra channels; last messages both ways, and sending
static int chatChan(const String& s) { int c = s.toInt(); return (c >= 0 && c < DiyMesh::NUM_CH) ? c : 0; }

void WebPanel::handleChat() {
  if (!requireAuth()) return;
  int c = chatChan(server.arg("ch"));
  JsonOut j(server);
  j.begin();
  j.f("{\"ch\":%d,\"set\":%s,\"queued\":%d,\"qch\":%d,\"name\":", c, _mesh->chanSet(c) ? "true" : "false", _mesh->homeQueued(), _mesh->queuedChan());
  j.str(_mesh->chanName(c)).raw(",\"self\":").str(_mesh->getNodePrefs()->node_name).raw(",\"chans\":[");
  for (int i = 0; i < DiyMesh::NUM_CH; i++) {   // [index, name, set, hash, last seq]
    if (i) j.raw(",");
    j.f("[%d,", i).str(_mesh->chanName(i)).f(",%s,%u,%lu]", _mesh->chanSet(i) ? "true" : "false", _mesh->chanHash(i), (unsigned long)_mesh->chatLastSeq(i));
  }
  j.raw("],\"m\":[");
  for (int i = 0; i < _mesh->chatCount(c); i++) {
    const DiyMesh::ChatMsg& m = _mesh->chatAt(c, i);
    if (i) j.raw(",");
    j.f("[%lu,%lu,%u,", (unsigned long)m.seq, (unsigned long)m.ts, m.out).str(m.from).raw(",").str(m.text);
    if (m.out) j.raw(",null]"); else j.f(",%.2f]", m.snr4 / 4.0f);
  }
  j.raw("]}");
  j.end();
}

void WebPanel::handleChatSend() {
  if (!requireAuth()) return;
  String t = server.arg("text");
  t.trim();
  if (!t.length()) { sendError(400, "empty message"); return; }
  int c = chatChan(server.arg("ch"));
  if (!_mesh->chanSet(c)) { sendError(409, c ? "channel is not set" : "home channel is not set"); return; }
  if (!_mesh->queueChat(c, t.c_str())) { sendError(429, "a long message is still being sent"); return; }
  sendOk("sent");
}

// watched neighbours: [["4869","zoo_rep",6,silent_s,alarm], ...]
void WebPanel::handleWatch() {
  if (!requireAuth()) return;
  JsonOut j(server);
  j.begin();
  j.raw("{\"w\":[");
  for (int i = 0; i < _mesh->watchCount(); i++) {
    DiyMesh::Watch& w = _mesh->watchAt(i);
    _mesh->refreshWatch(w);
    char key[9];
    _mesh->watchKeyHex(w, key);
    if (i) j.raw(",");
    j.raw("[").str(key).raw(",").str(w.name).f(",%u,%lu,%s]", (unsigned)w.hours, (unsigned long)_mesh->watchSilentS(w), w.alarm ? "true" : "false");
  }
  j.raw("]}");
  j.end();
}

void WebPanel::sendOk(const char* msg) {
  JsonOut j(server);
  j.begin();
  j.raw("{\"ok\":true,\"msg\":").str(msg).raw("}");
  j.end();
}

void WebPanel::sendError(int code, const char* msg) {
  JsonOut j(server);
  j.begin(code);
  j.raw("{\"error\":").str(msg).raw("}");
  j.end();
}
