#include "DiyMesh.h"

#include <Preferences.h>
#include <helpers/AdvertDataHelpers.h>

// MeshCore public channel key (well-known, 16 bytes)
static const uint8_t PUBLIC_PSK[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                       0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
#define CTL_DISCOVER_RESP 0x90
static const uint32_t SCAN_WINDOW_MS = 60000;
static const uint32_t TRACE_TIMEOUT_MS = 30000;

// ======================================================================
//  setup / loop
// ======================================================================
void DiyMesh::beginDiy() {
  uint8_t h[1];
  mesh::Utils::sha256(h, 1, PUBLIC_PSK, sizeof(PUBLIC_PSK));
  public_hash = h[0];
  loadBlocks();
  _last_minute_ms = _last_nf_ms = millis();
  _min_air_rx = airRxMs();
  _min_air_tx = airTxMs();
}

void DiyMesh::loopDiy() {
  uint32_t now = millis();
  loopHome();   // queued parts of long messages to the channel "Home"
  loopDelivery();   // echo check, resends, missing-part requests
  loopWatch();  // watched neighbours: alarm when silent

  // noise floor every 4 s
  if (now - _last_nf_ms >= 4000) {
    _last_nf_ms = now;
    int nf = noiseFloor();
    if (nf < -20 && nf > -160) {
      nf_recent[nf_head] = (int8_t)nf;
      nf_head = (nf_head + 1) % NF_SAMPLES;
      if (nf_count < NF_SAMPLES) nf_count++;
      _nf_sum += nf; _nf_n++;
      if (nf < _nf_lo || _nf_n == 1) _nf_lo = nf;
      if (nf > _nf_hi || _nf_n == 1) _nf_hi = nf;
    }
  }

  // one-minute bucket
  if (now - _last_minute_ms >= 60000) {
    _last_minute_ms += 60000;
    uint32_t rx = rx_flood + rx_direct, tx = tx_flood + tx_direct;
    Bucket b = {};
    b.rx = rx - _min_rx; b.tx = tx - _min_tx;
    b.rx_air_ms = airRxMs() - _min_air_rx;
    b.tx_air_ms = airTxMs() - _min_air_tx;
    b.nf_avg = _nf_n ? (int8_t)(_nf_sum / _nf_n) : 0;
    b.nf_min = _nf_n ? _nf_lo : 0;
    b.nf_max = _nf_n ? _nf_hi : 0;
    b.valid = 1;
    _min_rx = rx; _min_tx = tx; _min_air_rx = airRxMs(); _min_air_tx = airTxMs();
    _nf_sum = 0; _nf_n = 0;

    minute_head = (minute_head + 1) % MIN_BUCKETS;
    minutes[minute_head] = b;

    // fold into the current 10-minute bucket
    if (_minutes_in_ten == 0) {
      ten_head = (ten_head + 1) % TEN_BUCKETS;
      tens[ten_head] = b;
    } else {
      Bucket& t = tens[ten_head];
      t.rx += b.rx; t.tx += b.tx; t.rx_air_ms += b.rx_air_ms; t.tx_air_ms += b.tx_air_ms;
      if (b.nf_avg) {
        t.nf_avg = t.nf_avg ? (int8_t)((t.nf_avg * _minutes_in_ten + b.nf_avg) / (_minutes_in_ten + 1)) : b.nf_avg;
        if (!t.nf_min || b.nf_min < t.nf_min) t.nf_min = b.nf_min;
        if (!t.nf_max || b.nf_max > t.nf_max) t.nf_max = b.nf_max;
      }
    }
    _minutes_in_ten = (_minutes_in_ten + 1) % 10;
    if (_minutes_in_ten == 0) {   // transit counters: next 10-minute slot
      _relay_slot = (_relay_slot + 1) % 6;
      for (int i = 0; i < num_nodes; i++) nodes[i].relay10[_relay_slot] = 0;
    }
  }

  if (trace.pending && now - trace.sent_ms > TRACE_TIMEOUT_MS) {
    trace.pending = false;
    trace.ok = false;
  }
}

// ======================================================================
//  packet hooks
// ======================================================================
void DiyMesh::logRx(mesh::Packet* pkt, int len, float score) {
  MyMesh::logRx(pkt, len, score);
  int16_t rssi = (int16_t)_radio->getLastRSSI();
  addLog(0, pkt, len, pkt->_snr, rssi);
  notePacketRx(pkt, pkt->_snr, rssi);
  noteOutEcho(pkt);
  if (_rx_hook) _rx_hook(pkt);   // LAN bridge: pass on to the other repeaters
}

// a packet from the LAN bridge: processed like one just received over LoRa (dedup, filters,
// forwarding), so the repeater re-sends it in its own direction
bool DiyMesh::injectPacket(const uint8_t* raw, int len) {
  mesh::Packet* pkt = _mgr->allocNew();
  if (!pkt) return false;
  if (!pkt->readFrom(raw, len)) { _mgr->free(pkt); return false; }
  pkt->_snr = 0;
  _mgr->queueInbound(pkt, millis());
  return true;
}

void DiyMesh::logTx(mesh::Packet* pkt, int len) {
  MyMesh::logTx(pkt, len);
  noteOutTx(pkt);
  tx_by_type[pkt->getPayloadType() & 15]++;
  if (pkt->isRouteFlood()) tx_flood++; else tx_direct++;
  addLog(1, pkt, len, 0, 0);
}

void DiyMesh::logTxFail(mesh::Packet* pkt, int len) {
  MyMesh::logTxFail(pkt, len);
  noteOutTx(pkt);   // the echo window starts anyway: no echo -> resend
  tx_fail++;
  addLog(2, pkt, len, 0, 0);
}

void DiyMesh::addLog(uint8_t dir, const mesh::Packet* pkt, int len, int8_t snr4, int16_t rssi) {
  PktEntry& e = _log[_log_head];
  e.seq = ++_log_seq;
  e.ts = getRTCClock()->getCurrentTime();
  e.ms = millis();
  e.dir = dir;
  e.type = pkt->getPayloadType();
  e.route = pkt->getRouteType();
  e.hops = pkt->getPayloadType() == PAYLOAD_TYPE_TRACE ? pkt->path_len : pkt->getPathHashCount();
  e.hash_sz = pkt->getPathHashSize();
  e.len = len > 255 ? 255 : len;
  e.ch = (e.type == PAYLOAD_TYPE_GRP_TXT || e.type == PAYLOAD_TYPE_GRP_DATA) && pkt->payload_len > 0 ? pkt->payload[0] : 255;
  e.snr4 = snr4;
  e.rssi = rssi;
  if (e.type == PAYLOAD_TYPE_TRACE) {
    e.path_n = 0;                     // trace "path" holds SNRs, not hashes
  } else {
    int n = pkt->getPathByteLen();
    e.path_n = n > PATH_KEEP ? PATH_KEEP : n;
    memcpy(e.path, pkt->path, e.path_n);
  }
  _log_head = (_log_head + 1) % LOG_SIZE;
  if (_log_count < LOG_SIZE) _log_count++;
}

const DiyMesh::PktEntry* DiyMesh::lastRx() const {
  for (int i = _log_count - 1; i >= 0; i--) {
    const PktEntry& e = logAt(i);
    if (e.dir == 0) return &e;
  }
  return nullptr;
}

void DiyMesh::notePacketRx(const mesh::Packet* pkt, int8_t snr4, int16_t rssi) {
  uint8_t type = pkt->getPayloadType() & 15;
  rx_by_type[type]++;
  if (pkt->isRouteFlood()) rx_flood++; else rx_direct++;
  if (type == PAYLOAD_TYPE_TRACE) return;

  uint32_t now = millis();
  int sz = pkt->getPathHashSize();
  int hops = pkt->getPathHashCount();
  if (sz > 3) return;

  // last hop of a flood = a repeater we hear directly
  if (pkt->isRouteFlood() && hops > 0) {
    const uint8_t* last = &pkt->path[(hops - 1) * sz];
    NodeInfo* n = nodeFor(last, sz);
    if (n) {
      n->seen |= SEEN_DIRECT | SEEN_PATH;
      n->snr4 = snr4; n->rssi = rssi; n->last_ms = now; n->count++;
      if (!n->type) n->type = ADV_TYPE_REPEATER;
    }
  }

  // transit: every repeater in the path of a flood packet passed it on (counted only when the
  // hash prefix matches exactly one known repeater, short prefixes collide)
  if (pkt->isRouteFlood()) {
    for (int k = 0; k < hops; k++) {
      const uint8_t* h = &pkt->path[k * sz];
      NodeInfo* hit = nullptr;
      int matches = 0;
      for (int i = 0; i < num_nodes && matches < 2; i++) {
        NodeInfo& x = nodes[i];
        if (x.known < sz || memcmp(x.pub, h, sz) != 0) continue;
        if (x.type && x.type != ADV_TYPE_REPEATER && x.type != ADV_TYPE_ROOM) continue;
        hit = &x; matches++;
      }
      if (matches == 1) { hit->relay10[_relay_slot]++; hit->relay_total++; hit->relay_last_ms = now; }
    }
  }

  // group channels and their entry repeaters
  if ((type == PAYLOAD_TYPE_GRP_TXT || type == PAYLOAD_TYPE_GRP_DATA) && pkt->payload_len > 0) {
    uint8_t ch = pkt->payload[0];
    ChanStat* slot = nullptr;
    for (auto& c : chans) {
      if (c.used && c.hash == ch) { slot = &c; break; }
    }
    if (!slot) {
      for (auto& c : chans) if (!c.used) { slot = &c; break; }
      if (!slot) {   // replace the least used
        slot = &chans[0];
        for (auto& c : chans) if (c.count < slot->count) slot = &c;
      }
      *slot = {}; slot->used = 1; slot->hash = ch;
    }
    slot->count++; slot->last_ms = now;

    if (pkt->isRouteFlood() && hops > 0) {
      EntryStat* es = nullptr;
      for (int i = 0; i < num_entries; i++) {
        if (entries[i].key.sz == sz && memcmp(entries[i].key.b, pkt->path, sz) == 0) { es = &entries[i]; break; }
      }
      if (!es) {
        if (num_entries < MAX_ENTRIES) es = &entries[num_entries++];
        else {
          es = &entries[0];
          for (int i = 0; i < num_entries; i++) if (entries[i].count < es->count) es = &entries[i];
        }
        *es = {};
        memcpy(es->key.b, pkt->path, sz); es->key.sz = sz;
      }
      es->count++; es->last_ms = now;
    }
  }

  if (pkt->isRouteFlood() && (hops > 0 || type == PAYLOAD_TYPE_ADVERT)) addToGraph(pkt, snr4);
}

// ======================================================================
//  forwarding filter
// ======================================================================
bool DiyMesh::keyMatch(const Key3& k, const uint8_t* p, int sz) {
  int n = k.sz < sz ? k.sz : sz;     // compare on the shorter prefix
  return n > 0 && memcmp(k.b, p, n) == 0;
}

bool DiyMesh::allowPacketForward(const mesh::Packet* pkt) {
  if (!MyMesh::allowPacketForward(pkt)) return false;
  uint8_t type = pkt->getPayloadType();
  if (type != PAYLOAD_TYPE_GRP_TXT && type != PAYLOAD_TYPE_GRP_DATA) return true;   // only channel traffic is filtered
  if (pkt->payload_len == 0) return true;

  uint8_t ch = pkt->payload[0];
  bool block = (!repeat_public && ch == public_hash);
  for (int i = 0; !block && i < num_blocked_chans; i++) block = blocked_chans[i] == ch;

  int sz = pkt->getPathHashSize(), hops = pkt->getPathHashCount();
  if (!block && pkt->isRouteFlood() && hops > 0 && sz <= 3) {
    for (int i = 0; !block && i < num_blocked_entries; i++) block = keyMatch(blocked_entries[i], pkt->path, sz);
    // routes: every pair of consecutive hops, plus (last hop -> us)
    for (int r = 0; !block && r < num_blocked_routes; r++) {
      for (int i = 0; !block && i < hops; i++) {
        const uint8_t* a = &pkt->path[i * sz];
        const uint8_t* b = (i + 1 < hops) ? &pkt->path[(i + 1) * sz] : self_id.pub_key;
        block = keyMatch(blocked_routes[r].a, a, sz) && keyMatch(blocked_routes[r].b, b, sz);
      }
    }
  }
  if (block) blocked_count++;
  return !block;
}

bool DiyMesh::blockChannel(uint8_t hash, bool block) {
  int idx = -1;
  for (int i = 0; i < num_blocked_chans; i++) if (blocked_chans[i] == hash) idx = i;
  if (block && idx < 0) {
    if (num_blocked_chans >= MAX_BLOCKS) return false;
    blocked_chans[num_blocked_chans++] = hash;
  } else if (!block && idx >= 0) {
    blocked_chans[idx] = blocked_chans[--num_blocked_chans];
  }
  saveBlocks();
  return true;
}

bool DiyMesh::blockEntry(const Key3& k, bool block) {
  int idx = -1;
  for (int i = 0; i < num_blocked_entries; i++)
    if (blocked_entries[i].sz == k.sz && memcmp(blocked_entries[i].b, k.b, k.sz) == 0) idx = i;
  if (block && idx < 0) {
    if (num_blocked_entries >= MAX_BLOCKS) return false;
    blocked_entries[num_blocked_entries++] = k;
  } else if (!block && idx >= 0) {
    blocked_entries[idx] = blocked_entries[--num_blocked_entries];
  }
  saveBlocks();
  return true;
}

bool DiyMesh::blockRoute(const RouteKey& r, bool block) {
  int idx = -1;
  for (int i = 0; i < num_blocked_routes; i++) {
    const RouteKey& x = blocked_routes[i];
    if (x.a.sz == r.a.sz && x.b.sz == r.b.sz && !memcmp(x.a.b, r.a.b, r.a.sz) && !memcmp(x.b.b, r.b.b, r.b.sz)) idx = i;
  }
  if (block && idx < 0) {
    if (num_blocked_routes >= MAX_BLOCKS) return false;
    blocked_routes[num_blocked_routes++] = r;
  } else if (!block && idx >= 0) {
    blocked_routes[idx] = blocked_routes[--num_blocked_routes];
  }
  saveBlocks();
  return true;
}

void DiyMesh::setRepeatPublic(bool on) { repeat_public = on; saveBlocks(); }

void DiyMesh::clearChannelStats() {
  memset(chans, 0, sizeof(chans));
  memset(entries, 0, sizeof(entries));
  num_entries = 0;
}

uint8_t DiyMesh::hashtagChannelHash(const char* tag) {
  uint8_t secret[16], h[1];
  mesh::Utils::sha256(secret, sizeof(secret), (const uint8_t*)tag, strlen(tag));
  mesh::Utils::sha256(h, 1, secret, sizeof(secret));
  return h[0];
}

bool DiyMesh::parseKey(const char* hex, Key3& out) {
  int n = strlen(hex);
  if (n < 2 || n > 6 || (n & 1)) return false;
  for (int i = 0; i < n / 2; i++) {
    char tmp[3] = {hex[i * 2], hex[i * 2 + 1], 0};
    char* end;
    long v = strtol(tmp, &end, 16);
    if (*end) return false;
    out.b[i] = (uint8_t)v;
  }
  out.sz = n / 2;
  return true;
}

struct BlockBlob {
  uint8_t ver, repeat_public, n_ch, n_e, n_r;
  uint8_t ch[DiyMesh::MAX_BLOCKS];
  DiyMesh::Key3 e[DiyMesh::MAX_BLOCKS];
  DiyMesh::RouteKey r[DiyMesh::MAX_BLOCKS];
};

void DiyMesh::saveBlocks() {
  BlockBlob b = {};
  b.ver = 1; b.repeat_public = repeat_public;
  b.n_ch = num_blocked_chans; b.n_e = num_blocked_entries; b.n_r = num_blocked_routes;
  memcpy(b.ch, blocked_chans, sizeof(b.ch));
  memcpy(b.e, blocked_entries, sizeof(b.e));
  memcpy(b.r, blocked_routes, sizeof(b.r));
  Preferences p;
  if (p.begin("diy", false)) { p.putBytes("blk", &b, sizeof(b)); p.end(); }
}

void DiyMesh::loadBlocks() {
  BlockBlob b = {};
  Preferences p;
  if (p.begin("diy", true)) {
    size_t n = p.getBytesLength("blk");
    if (n == sizeof(b)) p.getBytes("blk", &b, sizeof(b));
    p.end();
  }
  if (b.ver != 1) return;
  repeat_public = b.repeat_public;
  num_blocked_chans = b.n_ch < MAX_BLOCKS ? b.n_ch : MAX_BLOCKS;
  num_blocked_entries = b.n_e < MAX_BLOCKS ? b.n_e : MAX_BLOCKS;
  num_blocked_routes = b.n_r < MAX_BLOCKS ? b.n_r : MAX_BLOCKS;
  memcpy(blocked_chans, b.ch, sizeof(b.ch));
  memcpy(blocked_entries, b.e, sizeof(b.e));
  memcpy(blocked_routes, b.r, sizeof(b.r));
}

// ======================================================================
//  nodes
// ======================================================================
const DiyMesh::NodeInfo* DiyMesh::findNodeByPrefix(const uint8_t* key, int sz) const {
  for (int i = 0; i < num_nodes; i++) {
    const NodeInfo& n = nodes[i];
    int k = n.known < sz ? n.known : sz;
    if (k > 0 && memcmp(n.pub, key, k) == 0) return &n;
  }
  return nullptr;
}

DiyMesh::NodeInfo* DiyMesh::nodeFor(const uint8_t* key, int known) {
  if (known > 8) known = 8;
  // exact or prefix match; a longer key upgrades a shorter one
  for (int i = 0; i < num_nodes; i++) {
    NodeInfo& n = nodes[i];
    int k = n.known < known ? n.known : known;
    if (k > 0 && memcmp(n.pub, key, k) == 0) {
      if (known > n.known) { memcpy(n.pub, key, known); n.known = known; }
      return &n;
    }
  }
  NodeInfo* slot;
  if (num_nodes < MAX_NODES) {
    slot = &nodes[num_nodes++];
  } else {   // evict the one heard longest ago
    slot = &nodes[0];
    for (int i = 0; i < num_nodes; i++) if (nodes[i].last_ms < slot->last_ms) slot = &nodes[i];
  }
  *slot = {};
  memcpy(slot->pub, key, known);
  slot->known = known;
  slot->snr4 = -128;
  slot->their_snr4 = -128;
  return slot;
}

void DiyMesh::onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                           const uint8_t* app_data, size_t app_data_len) {
  MyMesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len);
  if (id.matches(self_id)) return;
  AdvertDataParser parser(app_data, app_data_len);
  NodeInfo* n = nodeFor(id.pub_key, 8);
  if (!n) return;
  if (parser.isValid()) {
    n->type = parser.getType();
    if (parser.getName()) strlcpy(n->name, parser.getName(), sizeof(n->name));
    if (parser.hasLatLon()) { n->lat = parser.getIntLat(); n->lon = parser.getIntLon(); }
    uint16_t f2 = parser.getFeat2();
    n->senko = (app_data_len && (app_data[0] & ADV_FEAT2_MASK) && (f2 >> 8) == SENKO_MARK) ? (uint8_t)(0x80 | (f2 & 0x7F)) : 0;
  }
  n->seen |= SEEN_ADVERT;
  n->hops = packet->getPathHashCount();
  if (n->hops == 0) {
    n->seen |= SEEN_DIRECT;
    n->snr4 = packet->_snr;
    n->rssi = (int16_t)_radio->getLastRSSI();
  }
  n->last_ms = millis();
  n->count++;
}

bool DiyMesh::startScan() {
  uint32_t now = millis();
  if (scan_started_ms && now - scan_started_ms < 30000) return false;
  scan_started_ms = now ? now : 1;
  sendNodeDiscoverReq();
  return true;
}

void DiyMesh::clearNodes() {
  memset(nodes, 0, sizeof(nodes));
  num_nodes = 0;
}

void DiyMesh::onControlDataRecv(mesh::Packet* packet) {
  if ((packet->payload[0] & 0xF0) == CTL_DISCOVER_RESP && packet->payload_len >= 6 + 8 &&
      scan_started_ms && millis() - scan_started_ms < SCAN_WINDOW_MS) {
    int known = packet->payload_len - 6;
    NodeInfo* n = nodeFor(&packet->payload[6], known > 8 ? 8 : known);
    if (n) {
      n->type = packet->payload[0] & 0x0F;
      n->their_snr4 = (int8_t)packet->payload[1];
      n->snr4 = packet->_snr;
      n->rssi = (int16_t)_radio->getLastRSSI();
      n->seen |= SEEN_SCAN | SEEN_DIRECT;
      n->last_ms = millis();
      n->count++;
    }
  }
  MyMesh::onControlDataRecv(packet);
}

// ======================================================================
//  path graph: a trie of paths into this repeater, rooted at us
// ======================================================================
void DiyMesh::clearGraph() {
  memset(gnodes, 0, sizeof(gnodes));
  num_gnodes = 0;
  graph_forgotten = 0;
}

void DiyMesh::gEvict() {
  static uint8_t has_child[(MAX_GNODES + 7) / 8];
  memset(has_child, 0, sizeof(has_child));
  for (int i = 0; i < MAX_GNODES; i++) {
    if (gnodes[i].used && gnodes[i].parent != 0xFFFF) has_child[gnodes[i].parent >> 3] |= 1 << (gnodes[i].parent & 7);
  }
  uint32_t now = millis();
  int victim = -1;
  for (int i = 0; i < MAX_GNODES; i++) {
    const GNode& g = gnodes[i];
    if (!g.used || (has_child[i >> 3] & (1 << (i & 7))) || g.last_ms == now) continue;
    if (victim < 0 || g.count < gnodes[victim].count ||
        (g.count == gnodes[victim].count && g.last_ms < gnodes[victim].last_ms)) victim = i;
  }
  if (victim >= 0) {
    gnodes[victim].used = 0;
    num_gnodes--;
    graph_forgotten++;
  }
}

int DiyMesh::gChild(int parent, const uint8_t* key, int sz) {
  uint16_t p = parent < 0 ? 0xFFFF : (uint16_t)parent;
  for (int i = 0; i < MAX_GNODES; i++) {
    const GNode& g = gnodes[i];
    if (g.used && g.parent == p && g.sz == sz && memcmp(g.key, key, sz) == 0) return i;
  }
  if (num_gnodes >= MAX_GNODES) gEvict();
  for (int i = 0; i < MAX_GNODES; i++) {
    if (!gnodes[i].used) {
      GNode& g = gnodes[i];
      g = {};
      g.used = 1; g.parent = p; g.sz = sz;
      memcpy(g.key, key, sz);
      g.snr4 = -128;
      num_gnodes++;
      return i;
    }
  }
  return -1;
}

void DiyMesh::addToGraph(const mesh::Packet* pkt, int8_t snr4) {
  int sz = pkt->getPathHashSize(), hops = pkt->getPathHashCount();
  uint32_t now = millis();
  int cur = -1;
  for (int h = hops - 1; h >= 0; h--) {           // walk from our neighbour back to the source
    int idx = gChild(cur, &pkt->path[h * sz], sz);
    if (idx < 0) return;
    GNode& g = gnodes[idx];
    if (g.count < 0xFFFF) g.count++;
    g.last_ms = now;
    if (cur < 0) g.snr4 = snr4;
    cur = idx;
  }
  if (pkt->getPayloadType() == PAYLOAD_TYPE_ADVERT && pkt->payload_len >= PUB_KEY_SIZE) {
    int idx = gChild(cur, pkt->payload, sz);     // originator: first bytes of its public key
    if (idx < 0) return;
    GNode& g = gnodes[idx];
    g.orig = 1;
    if (g.count < 0xFFFF) g.count++;
    g.last_ms = now;
    if (cur < 0) g.snr4 = snr4;
  }
}

// ======================================================================
//  trace
// ======================================================================
const char* DiyMesh::startTrace(const char* hops_csv) {
  if (trace.pending) return "Err - trace already running";
  uint8_t list[16 * 4];
  int n = 0, sz = 0;
  char buf[160];
  strlcpy(buf, hops_csv, sizeof(buf));
  for (char* tok = strtok(buf, " ,>;"); tok; tok = strtok(nullptr, " ,>;")) {
    int len = strlen(tok);
    if (len != 2 && len != 4 && len != 8) return "Err - each hop is 2, 4 or 8 hex digits";
    if (sz == 0) sz = len / 2;
    if (len / 2 != sz) return "Err - all hops must have the same length";
    if (n >= 16) return "Err - max 16 hops";
    for (int i = 0; i < sz; i++) {
      char t[3] = {tok[i * 2], tok[i * 2 + 1], 0};
      char* end;
      long v = strtol(t, &end, 16);
      if (*end) return "Err - not hex";
      list[n * sz + i] = (uint8_t)v;
    }
    n++;
  }
  if (n == 0) return "Err - no hops";

  // there and back: A,B,C -> A,B,C,B,A ; the last transmission (by A) reaches us
  int total = 0;
  for (int i = 0; i < n; i++) memcpy(&trace.hashes[(total++) * sz], &list[i * sz], sz);
  for (int i = n - 2; i >= 0; i--) memcpy(&trace.hashes[(total++) * sz], &list[i * sz], sz);

  uint8_t flags = sz == 1 ? 0 : sz == 2 ? 1 : 2;
  getRNG()->random((uint8_t*)&trace.tag, 4);
  mesh::Packet* pkt = createTrace(trace.tag, 0, flags);
  if (!pkt) return "Err - no free packet";
  sendDirect(pkt, trace.hashes, total * sz);
  trace.pending = true;
  trace.ok = false;
  trace.sz = sz;
  trace.n = total;
  trace.sent_ms = millis();
  trace.rtt_ms = 0;
  memset(trace.snrs, 0, sizeof(trace.snrs));
  return "OK - trace sent";
}

void DiyMesh::onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                          const uint8_t* path_snrs, const uint8_t* path_hashes, uint8_t path_len) {
  MyMesh::onTraceRecv(packet, tag, auth_code, flags, path_snrs, path_hashes, path_len);
  if (!trace.pending || tag != trace.tag) return;
  trace.pending = false;
  trace.ok = true;
  trace.rtt_ms = millis() - trace.sent_ms;
  int n = packet->path_len < sizeof(trace.snrs) ? packet->path_len : sizeof(trace.snrs);
  memcpy(trace.snrs, path_snrs, n);
  trace.final_snr4 = packet->_snr;
}
