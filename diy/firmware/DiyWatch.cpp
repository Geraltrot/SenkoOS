// Watch list: neighbours that must be heard regularly (key repeaters of the network).
// A watched node that stays silent longer than its limit raises an alarm: a message to the
// channel "Home" (if set) and a callback (MQTT / Home Assistant); when it is heard again - "back".
#include "DiyMesh.h"

#include <Preferences.h>

static const char* NVS_NS = "diy";
static const uint32_t CHECK_MS = 30000;

struct WatchBlob { uint8_t n; struct { uint8_t key[4]; uint8_t sz; uint8_t hours; } w[DiyMesh::MAX_WATCH]; };

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "4869" / "48A1B2C3" -> bytes; 1..4 bytes
static int parseHexKey(const char* s, uint8_t out[4]) {
  int n = 0;
  while (s[2 * n] && s[2 * n + 1] && n < 4) {
    int hi = hexNibble(s[2 * n]), lo = hexNibble(s[2 * n + 1]);
    if (hi < 0 || lo < 0) return 0;
    out[n++] = (uint8_t)(hi << 4 | lo);
  }
  return (s[2 * n] == 0 || s[2 * n] == ' ') ? n : 0;
}

void DiyMesh::watchKeyHex(const Watch& w, char* out) const {
  for (int i = 0; i < w.sz; i++) sprintf(out + 2 * i, "%02X", w.key[i]);
  out[2 * w.sz] = 0;
}

void DiyMesh::loadWatch() {
  WatchBlob b = {};
  Preferences p;
  if (p.begin(NVS_NS, true)) {
    if (p.getBytesLength("watch") == sizeof(b)) p.getBytes("watch", &b, sizeof(b));
    p.end();
  }
  _watch_n = 0;
  for (int i = 0; i < b.n && i < MAX_WATCH; i++) {
    Watch& w = _watch[_watch_n++];
    memset(&w, 0, sizeof(w));
    memcpy(w.key, b.w[i].key, 4);
    w.sz = b.w[i].sz;
    w.hours = b.w[i].hours ? b.w[i].hours : 6;
  }
}

void DiyMesh::saveWatch() {
  WatchBlob b = {};
  b.n = _watch_n;
  for (int i = 0; i < _watch_n; i++) { memcpy(b.w[i].key, _watch[i].key, 4); b.w[i].sz = _watch[i].sz; b.w[i].hours = _watch[i].hours; }
  Preferences p;
  if (p.begin(NVS_NS, false)) { p.putBytes("watch", &b, sizeof(b)); p.end(); }
}

// last time we heard the node: directly (neighbour table) or anywhere in a path (graph)
void DiyMesh::refreshWatch(Watch& w) {
  const NodeInfo* n = findNodeByPrefix(w.key, w.sz);
  if (n) {
    if (n->last_ms && (!w.heard || (int32_t)(n->last_ms - w.last_ms) > 0)) { w.last_ms = n->last_ms; w.heard = true; }
    if (n->name[0]) strlcpy(w.name, n->name, sizeof(w.name));
  }
  for (int i = 0; i < MAX_GNODES; i++) {
    const GNode& g = gnodes[i];
    if (!g.used || !g.last_ms) continue;
    int k = g.sz < w.sz ? g.sz : w.sz;
    if (k > 0 && memcmp(g.key, w.key, k) == 0 && (!w.heard || (int32_t)(g.last_ms - w.last_ms) > 0)) {
      w.last_ms = g.last_ms;
      w.heard = true;
    }
  }
}

uint32_t DiyMesh::watchSilentS(const Watch& w) const {
  return (millis() - (w.heard ? w.last_ms : 0)) / 1000;   // never heard since boot: silent since boot
}

void DiyMesh::loopWatch() {
  if (millis() - _watch_check_ms < CHECK_MS) return;
  _watch_check_ms = millis();
  for (int i = 0; i < _watch_n; i++) {
    Watch& w = _watch[i];
    refreshWatch(w);
    bool silent = watchSilentS(w) > (uint32_t)w.hours * 3600UL;
    if (silent == w.alarm) continue;
    w.alarm = silent;
    char key[9], msg[96];
    watchKeyHex(w, key);
    const char* who = w.name[0] ? w.name : key;
    if (silent) snprintf(msg, sizeof(msg), "⚠ Узел %s (%s) молчит больше %u ч", who, key, (unsigned)w.hours);
    else snprintf(msg, sizeof(msg), "✅ Узел %s (%s) снова на связи", who, key);
    if (_home_set) queueHomeChat(msg);
    if (_watch_fn) _watch_fn(key, who, !silent, false);
  }
}

// CLI (local only):  watch | watch add <hex> [hours] | watch del <hex>
bool DiyMesh::handleWatchCommand(const char* command, char* reply, size_t reply_len) {
  if (strncmp(command, "watch", 5) != 0 || (command[5] != 0 && command[5] != ' ')) return false;
  const char* arg = command + 5;
  while (*arg == ' ') arg++;
  if (*arg == 0) {
    if (!_watch_n) { snprintf(reply, reply_len, "watch: empty (watch add <id hex> [hours])"); return true; }
    size_t o = snprintf(reply, reply_len, "watch:");
    for (int i = 0; i < _watch_n && o < reply_len - 48; i++) {
      Watch& w = _watch[i];
      refreshWatch(w);
      char key[9];
      watchKeyHex(w, key);
      o += snprintf(reply + o, reply_len - o, " %s%s%s %uh %s %lum;", key, w.name[0] ? " " : "", w.name, (unsigned)w.hours,
                    w.alarm ? "SILENT" : "ok", (unsigned long)(watchSilentS(w) / 60));
    }
    return true;
  }
  bool add = strncmp(arg, "add ", 4) == 0, del = strncmp(arg, "del ", 4) == 0;
  if (!add && !del) { snprintf(reply, reply_len, "Err - watch [add <id hex> [hours]|del <id hex>]"); return true; }
  uint8_t key[4] = {0};
  int sz = parseHexKey(arg + 4, key);
  if (!sz) { snprintf(reply, reply_len, "Err - id: 2..8 hex chars, e.g. 4869"); return true; }
  int idx = -1;
  for (int i = 0; i < _watch_n; i++) if (_watch[i].sz == sz && memcmp(_watch[i].key, key, sz) == 0) idx = i;
  char khex[9];
  if (add) {
    const char* h = strchr(arg + 4, ' ');
    int hours = h ? atoi(h + 1) : 6;
    if (hours < 1 || hours > 168) { snprintf(reply, reply_len, "Err - hours 1..168"); return true; }
    if (idx < 0) {
      if (_watch_n >= MAX_WATCH) { snprintf(reply, reply_len, "Err - at most %d nodes", MAX_WATCH); return true; }
      idx = _watch_n++;
      memset(&_watch[idx], 0, sizeof(Watch));
      memcpy(_watch[idx].key, key, sz);
      _watch[idx].sz = sz;
    }
    _watch[idx].hours = hours;
    refreshWatch(_watch[idx]);
    saveWatch();
    watchKeyHex(_watch[idx], khex);
    if (_watch_fn) _watch_fn(khex, _watch[idx].name[0] ? _watch[idx].name : khex, !_watch[idx].alarm, false);
    snprintf(reply, reply_len, "OK - watching %s%s%s, alarm after %d h of silence", khex, _watch[idx].name[0] ? " " : "", _watch[idx].name, hours);
  } else {
    if (idx < 0) { snprintf(reply, reply_len, "Err - not in the list"); return true; }
    watchKeyHex(_watch[idx], khex);
    for (int i = idx; i < _watch_n - 1; i++) _watch[i] = _watch[i + 1];
    _watch_n--;
    saveWatch();
    if (_watch_fn) _watch_fn(khex, khex, true, true);
    snprintf(reply, reply_len, "OK - %s removed", khex);
  }
  return true;
}
