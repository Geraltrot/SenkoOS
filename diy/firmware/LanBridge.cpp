#include "LanBridge.h"
#include "DiyMesh.h"
#include "WifiLink.h"
#include "JsonOut.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <SHA256.h>

LanBridge lan_bridge;

static const char* NVS_NS = "diy";
static const uint8_t MAGIC0 = 'S', MAGIC1 = 'L', VER = 2;
static const uint8_t KIND_PACKET = 0, KIND_HELLO = 1;
static const size_t HDR1 = 2 + 1 + 1 + 4 + 4 + 4;            // ver 1: magic, ver, kind, id, epoch, seq
static const size_t HDR = 2 + 1 + 1 + 1 + 4 + 4 + 4 + 4;     // ver 2: magic, ver, kind, hops, id, boot, epoch, seq
static const size_t MAC = 16;
static const uint32_t HELLO_MS = 30000;
static const uint32_t PEER_ACTIVE_MS = 95000;       // ~3 hellos
static const uint32_t PEER_FORGET_MS = 300000;      // learned peers
static const uint32_t MAX_SKEW_S = 300;

static WiFiUDP s_udp;

static uint32_t rd32(const uint8_t* b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static void wr32(uint8_t* b, uint32_t v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }

// ---------------------------------------------------------------------------
//  settings
// ---------------------------------------------------------------------------
void LanBridge::load() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return;
  _on = p.getUChar("lb_on", 0) != 0;
  p.getString("lb_sec", _secret, sizeof(_secret));
  _port = p.getUShort("lb_port", 4280);
  _hop_limit = p.getUChar("lb_hops", 2);
  if (_hop_limit < 1 || _hop_limit > 8) _hop_limit = 2;
  char list[80] = "";
  p.getString("lb_peers", list, sizeof(list));   // "192.168.2.10,10.8.0.3"
  p.end();
  int n = 0;
  for (char* t = strtok(list, ","); t && n < MAX_FIXED; t = strtok(nullptr, ",")) {
    IPAddress ip;
    if (!ip.fromString(t)) continue;
    _peers[n].ip = (uint32_t)ip;
    _peers[n].fixed = true;
    n++;
  }
}

void LanBridge::save() const {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putUChar("lb_on", _on ? 1 : 0);
  p.putString("lb_sec", _secret);
  p.putUShort("lb_port", _port);
  p.putUChar("lb_hops", _hop_limit);
  char list[80] = "";
  size_t o = 0;
  for (const Peer& x : _peers) {
    if (!x.fixed || !x.ip) continue;
    o += snprintf(list + o, sizeof(list) - o, "%s%s", o ? "," : "", IPAddress(x.ip).toString().c_str());
  }
  p.putString("lb_peers", list);
  p.end();
}

// ---------------------------------------------------------------------------
//  lifecycle
// ---------------------------------------------------------------------------
void LanBridge::begin(DiyMesh* mesh, WifiLink* wifi, const uint8_t* self_pub) {
  _mesh = mesh;
  _wifi = wifi;
  memcpy(_self, self_pub, 4);
  memset(_seen, 0, sizeof(_seen));
  do { _boot = esp_random(); } while (_boot == 0);   // new on every start: the seq window restarts with it
  load();
}

void LanBridge::start() {
  if (_running) return;
  if (!s_udp.begin(_port)) return;
  _running = true;
  _next_hello = millis();   // announce at once
  Serial.printf("LAN bridge: on, UDP %u\n", _port);
}

void LanBridge::stop() {
  if (!_running) return;
  s_udp.stop();
  _running = false;
  Serial.println("LAN bridge: off");
}

const char* LanBridge::stateText() const {
  if (!_on) return "off";
  if (strlen(_secret) < 8) return "no secret";
  if (!_wifi || !_wifi->isConnected()) return "no wifi";
  if (!_running) return "starting";
  return activePeers() ? "linked" : "alone";
}

int LanBridge::activePeers() const {
  int n = 0;
  for (const Peer& x : _peers) if (x.last_ms && millis() - x.last_ms < PEER_ACTIVE_MS) n++;
  return n;
}

void LanBridge::loop() {
  bool want = _on && strlen(_secret) >= 8 && _wifi && _wifi->isConnected();
  if (want && !_running) start();
  if (!want && _running) stop();
  if (!_running) return;

  receive();
  if ((int32_t)(millis() - _next_hello) >= 0) {
    _next_hello = millis() + HELLO_MS;
    const char* name = _mesh ? _mesh->getNodeName() : "";
    sendFrame(KIND_HELLO, (const uint8_t*)name, strnlen(name, 23));
    for (Peer& x : _peers)   // learned peers that went quiet
      if (!x.fixed && x.ip && x.last_ms && millis() - x.last_ms > PEER_FORGET_MS) x = Peer();
  }
}

// ---------------------------------------------------------------------------
//  frames
// ---------------------------------------------------------------------------
void LanBridge::sign(const uint8_t* data, size_t len, uint8_t out[16]) const {
  SHA256 h;
  size_t kl = strlen(_secret);
  h.resetHMAC(_secret, kl);
  h.update(data, len);
  h.finalizeHMAC(_secret, kl, out, MAC);
}

bool LanBridge::seen(const uint8_t* hash) const {
  uint32_t now = millis();
  for (const Seen& e : _seen)
    if (e.ms && now - e.ms < DEDUP_TTL_MS && memcmp(e.h, hash, 8) == 0) return true;
  return false;
}

void LanBridge::markSeen(const uint8_t* hash) {
  memcpy(_seen[_seen_next].h, hash, 8);
  _seen[_seen_next].ms = millis() | 1;
  _seen_next = (_seen_next + 1) % DEDUP_SIZE;
}

// ---------------------------------------------------------------------------
//  replay window and rate limit per sender
// ---------------------------------------------------------------------------
LanBridge::Sender* LanBridge::senderFor(const uint8_t* id) {
  Sender* slot = nullptr;
  for (Sender& x : _senders) {
    if (x.used && memcmp(x.id, id, 4) == 0) return &x;
    if (!x.used && !slot) slot = &x;
  }
  if (!slot)   // full: reuse the one heard longest ago
    for (Sender& x : _senders) if (!slot || x.last_ms < slot->last_ms) slot = &x;
  *slot = Sender();
  slot->used = true;
  memcpy(slot->id, id, 4);
  return slot;
}

// true = first time this (boot, seq) is seen. Each boot id of a sender has its own window of the
// last 64 sequence numbers; a boot id we have not seen gets the slot used longest ago. Nothing is
// blacklisted, so replaying someone's old frames cannot lock out their current boot. Limit without
// NTP: frames of a boot id this node has never seen (or forgot) are accepted once each.
bool LanBridge::windowAccept(Sender& s, uint32_t boot, uint32_t seq, uint32_t now_ms) {
  Sender::Win* w = nullptr;
  for (Sender::Win& x : s.win) if (x.used_ms && x.boot == boot) { w = &x; break; }
  if (!w) {
    for (Sender::Win& x : s.win) if (!w || x.used_ms < w->used_ms) w = &x;
    w->boot = boot;
    w->max_seq = seq;
    w->bits = 1;
    w->used_ms = now_ms | 1;
    return true;
  }
  w->used_ms = now_ms | 1;
  if (seq > w->max_seq) {
    uint32_t d = seq - w->max_seq;
    w->bits = d >= 64 ? 1 : (w->bits << d) | 1;
    w->max_seq = seq;
    return true;
  }
  uint32_t d = w->max_seq - seq;
  if (d >= 64) return false;            // too old
  uint64_t bit = (uint64_t)1 << d;
  if (w->bits & bit) return false;      // already seen
  w->bits |= bit;
  return true;
}

// token bucket: RATE_PER_S packets per second, bursts up to RATE_BURST
bool LanBridge::rateAccept(Sender& s, uint32_t now_ms) {
  if (s.refill_ms) {
    float add = (now_ms - s.refill_ms) * (RATE_PER_S / 1000.0f);
    s.tokens = s.tokens + add > RATE_BURST ? RATE_BURST : s.tokens + add;
  }
  s.refill_ms = now_ms;
  if (s.tokens < 1.0f) return false;
  s.tokens -= 1.0f;
  return true;
}

LanBridge::Peer* LanBridge::peerFor(uint32_t ip, bool create) {
  for (Peer& x : _peers) if (x.ip == ip) return &x;
  if (!create) return nullptr;
  Peer* slot = nullptr;
  for (Peer& x : _peers) if (!x.ip) { slot = &x; break; }
  if (!slot)   // table full: replace the learned peer heard longest ago
    for (Peer& x : _peers) if (!x.fixed && (!slot || x.last_ms < slot->last_ms)) slot = &x;
  if (!slot) return nullptr;
  *slot = Peer();
  slot->ip = ip;
  return slot;
}

void LanBridge::sendTo(uint32_t ip, const uint8_t* frame, size_t len) {
  if (!s_udp.beginPacket(IPAddress(ip), _port)) return;
  s_udp.write(frame, len);
  s_udp.endPacket();
}

// to the subnet broadcast address, plus every known peer outside our subnet (VPN, other LAN)
void LanBridge::sendFrame(uint8_t kind, const uint8_t* data, size_t len) {
  uint8_t f[HDR + MAX_TRANS_UNIT + MAC];
  if (len > MAX_TRANS_UNIT) return;
  f[0] = MAGIC0; f[1] = MAGIC1; f[2] = VER; f[3] = kind;
  f[4] = 0;   // hops: frames start here; a node passing a frame on raises it
  memcpy(f + 5, _self, 4);
  wr32(f + 9, _boot);
  wr32(f + 13, _mesh ? _mesh->getRTCClock()->getCurrentTime() : 0);
  wr32(f + 17, ++_seq);
  memcpy(f + HDR, data, len);
  sign(f, HDR + len, f + HDR + len);
  size_t n = HDR + len + MAC;

  sendTo((uint32_t)WiFi.broadcastIP(), f, n);
  uint32_t mask = (uint32_t)WiFi.subnetMask(), self = (uint32_t)WiFi.localIP();
  for (Peer& x : _peers) {
    if (!x.ip || (x.ip & mask) == (self & mask)) continue;   // same subnet: the broadcast reaches it
    sendTo(x.ip, f, n);
    if (kind == KIND_PACKET) x.tx++;
  }
}

void LanBridge::onRadioRx(const mesh::Packet* pkt) {
  if (!_running || !pkt) return;
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  if (seen(hash)) return;   // came from the bridge itself (heard back over the air)
  markSeen(hash);
  uint8_t raw[MAX_TRANS_UNIT + 1];
  uint8_t n = pkt->writeTo(raw);
  sendFrame(KIND_PACKET, raw, n);
  _tx++;
}

void LanBridge::receive() {
  uint8_t f[HDR + MAX_TRANS_UNIT + MAC + 8];
  for (int guard = 0; guard < 8; guard++) {
    int n = s_udp.parsePacket();
    if (n <= 0) return;
    uint32_t from = (uint32_t)s_udp.remoteIP();
    int len = s_udp.read(f, sizeof(f));
    while (s_udp.available()) s_udp.read();   // drop the rest of an oversized datagram
    if (len < 3 || f[0] != MAGIC0 || f[1] != MAGIC1 || (f[2] != 1 && f[2] != 2)) continue;
    const bool v1 = f[2] == 1;
    const size_t hdr = v1 ? HDR1 : HDR;
    if (len < (int)(hdr + MAC) || len > (int)(hdr + MAX_TRANS_UNIT + MAC)) continue;
    // field offsets: ver 1 has no hops and no boot id
    const uint8_t kind = f[3], hops = v1 ? 0 : f[4];
    const uint8_t* id = f + (v1 ? 4 : 5);
    const uint32_t boot = v1 ? 0 : rd32(f + 9);
    const uint32_t ts = rd32(f + (v1 ? 8 : 13)), seq = rd32(f + (v1 ? 12 : 17));
    if (memcmp(id, _self, 4) == 0) continue;   // our own broadcast

    uint8_t mac[MAC];
    sign(f, len - MAC, mac);
    uint8_t diff = 0;
    for (size_t i = 0; i < MAC; i++) diff |= mac[i] ^ f[len - MAC + i];
    if (diff) { _bad_auth++; continue; }

    if (hops >= _hop_limit) { _hop_drop++; continue; }

    // replayed old frames: by epoch when both clocks are set (NTP), by the seq window always (ver 2)
    uint32_t now = _mesh ? _mesh->getRTCClock()->getCurrentTime() : 0;
    if (ts > 1700000000UL && now > 1700000000UL && (ts > now + MAX_SKEW_S || now > ts + MAX_SKEW_S)) { _replay++; continue; }
    Sender* snd = senderFor(id);
    snd->last_ms = millis() | 1;
    if (!v1 && !windowAccept(*snd, boot, seq, millis())) { snd->replay++; _replay++; continue; }

    Peer* p = peerFor(from, true);
    if (p) { p->last_ms = millis() | 1; memcpy(p->id, id, 4); }

    const uint8_t* data = f + hdr;
    int dlen = len - hdr - MAC;
    if (kind == KIND_HELLO) {
      if (p) { int k = dlen < 23 ? dlen : 23; memcpy(p->name, data, k); p->name[k] = 0; }
      continue;
    }
    if (kind != KIND_PACKET || dlen <= 0) continue;
    if (!rateAccept(*snd, millis())) { snd->rate_drop++; _rate_drop++; continue; }
    if (p) p->rx++;

    mesh::Packet tmp;   // hash only, to drop what we already passed on or injected
    if (!tmp.readFrom(data, dlen)) continue;
    uint8_t hash[MAX_HASH_SIZE];
    tmp.calculatePacketHash(hash);
    if (seen(hash)) { _dup++; continue; }
    markSeen(hash);
    if (_mesh && _mesh->injectPacket(data, dlen)) _rx++;
    else _inj_fail++;
  }
}

// ---------------------------------------------------------------------------
//  CLI and JSON
// ---------------------------------------------------------------------------
bool LanBridge::handleCommand(const char* cmd, char* reply, size_t reply_len) {
  if (strncmp(cmd, "lan", 3) != 0 || (cmd[3] != 0 && cmd[3] != ' ')) return false;
  const char* a = cmd[3] ? cmd + 4 : "";
  if (!*a) {
    int o = snprintf(reply, reply_len, "lan %s, UDP %u, hops<%u, peers %d, rx %lu, tx %lu, dup %lu, bad %lu, replay %lu, rate %lu, hops %lu",
                     stateText(), (unsigned)_port, (unsigned)_hop_limit, activePeers(), (unsigned long)_rx, (unsigned long)_tx,
                     (unsigned long)_dup, (unsigned long)_bad_auth, (unsigned long)_replay, (unsigned long)_rate_drop,
                     (unsigned long)_hop_drop);
    for (const Peer& x : _peers) {
      if (!x.ip || o >= (int)reply_len - 40) continue;
      long ago = x.last_ms ? (long)((millis() - x.last_ms) / 1000) : -1;
      o += snprintf(reply + o, reply_len - o, "; %s%s %s %lds", IPAddress(x.ip).toString().c_str(), x.fixed ? "*" : "",
                    x.name[0] ? x.name : "?", ago);
    }
    return true;
  }
  if (strcmp(a, "on") == 0 || strcmp(a, "off") == 0) {
    _on = a[1] == 'n';
    save();
    if (_on && strlen(_secret) < 8) snprintf(reply, reply_len, "OK - on, but set the secret first: lan secret <8-32 chars>");
    else snprintf(reply, reply_len, "OK - LAN bridge %s", _on ? "on" : "off");
    return true;
  }
  if (strncmp(a, "secret ", 7) == 0) {
    const char* s = a + 7;
    size_t n = strlen(s);
    if (n < 8 || n > 32) { snprintf(reply, reply_len, "Err - secret: 8-32 chars, the same on every bridge node"); return true; }
    strlcpy(_secret, s, sizeof(_secret));
    save();
    snprintf(reply, reply_len, "OK - secret set");
    return true;
  }
  if (strncmp(a, "port ", 5) == 0) {
    int v = atoi(a + 5);
    if (v < 1024 || v > 65535) { snprintf(reply, reply_len, "Err - port 1024-65535"); return true; }
    _port = v;
    save();
    stop();   // restarts on the new port from loop()
    snprintf(reply, reply_len, "OK - port %d", v);
    return true;
  }
  if (strncmp(a, "hops ", 5) == 0) {
    int v = atoi(a + 5);
    if (v < 1 || v > 8) { snprintf(reply, reply_len, "Err - hops 1-8 (frames that passed this many nodes are dropped)"); return true; }
    _hop_limit = v;
    save();
    snprintf(reply, reply_len, "OK - hop limit %d", v);
    return true;
  }
  if (strncmp(a, "peer add ", 9) == 0 || strncmp(a, "peer del ", 9) == 0) {
    IPAddress ip;
    if (!ip.fromString(a + 9)) { snprintf(reply, reply_len, "Err - IPv4 address expected"); return true; }
    bool add = a[5] == 'a';
    Peer* p = peerFor((uint32_t)ip, false);
    if (add) {
      int fixed = 0;
      for (const Peer& x : _peers) if (x.fixed) fixed++;
      if (!(p && p->fixed) && fixed >= MAX_FIXED) { snprintf(reply, reply_len, "Err - %d addresses at most", MAX_FIXED); return true; }
      if (!p) p = peerFor((uint32_t)ip, true);
      if (!p) { snprintf(reply, reply_len, "Err - peer table full"); return true; }
      p->fixed = true;
    } else {
      if (!p || !p->fixed) { snprintf(reply, reply_len, "Err - not in the list"); return true; }
      *p = Peer();
    }
    save();
    snprintf(reply, reply_len, "OK - peer %s %s", a + 9, add ? "added" : "removed");
    return true;
  }
  snprintf(reply, reply_len, "Err - lan [on|off|secret <s>|port <n>|hops <n>|peer add|del <ip>]");
  return true;
}

void LanBridge::writeStatus(JsonOut& j) const {
  j.raw(",\"lan\":{\"state\":").str(stateText());
  j.f(",\"port\":%u,\"hops\":%u,\"rx\":%lu,\"tx\":%lu,\"dup\":%lu,\"bad\":%lu,\"replay\":%lu,\"rate_drop\":%lu,\"hop_drop\":%lu,\"peers\":[",
      (unsigned)_port, (unsigned)_hop_limit, (unsigned long)_rx, (unsigned long)_tx, (unsigned long)_dup, (unsigned long)_bad_auth,
      (unsigned long)_replay, (unsigned long)_rate_drop, (unsigned long)_hop_drop);
  bool first = true;
  for (const Peer& x : _peers) {
    if (!x.ip) continue;
    j.sep(first);
    j.raw("{\"ip\":").str(IPAddress(x.ip).toString().c_str()).raw(",\"name\":").str(x.name);
    j.f(",\"fixed\":%s,\"ago\":%ld,\"rx\":%lu,\"tx\":%lu}", x.fixed ? "true" : "false",
        x.last_ms ? (long)((millis() - x.last_ms) / 1000) : -1L, (unsigned long)x.rx, (unsigned long)x.tx);
  }
  j.raw("]}");
}

void LanBridge::writePrefs(JsonOut& j) const {
  j.f(",\"lan_hops\":%u", (unsigned)_hop_limit);
  j.f(",\"lan_on\":%u,\"lan_port\":%u,\"lan_secret_set\":%s", _on ? 1 : 0, (unsigned)_port, strlen(_secret) >= 8 ? "true" : "false");
  char list[80] = "";
  size_t o = 0;
  for (const Peer& x : _peers)
    if (x.fixed && x.ip) o += snprintf(list + o, sizeof(list) - o, "%s%s", o ? " " : "", IPAddress(x.ip).toString().c_str());
  j.raw(",\"lan_peers\":").str(list);
}
