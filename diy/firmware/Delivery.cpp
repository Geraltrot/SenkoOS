// Delivery of our chat messages (v1.6.0):
//  - echo check: every flood packet we send is re-sent by the repeaters around us and we hear that echo;
//    no echo within the window -> the very same bytes go out again (up to dl_retry times). Repeaters drop
//    packets they have already seen (hash of type + payload), so an identical resend only passes where nobody
//    heard the first one - exactly the case "lost on the first hop", and nothing is shown twice anywhere.
//  - missing parts "[i/n]" (option, SenkoOS boards only): a board that got [1/3] and [3/3] but not [2/3]
//    sends a small GRP_DATA packet in the same channel (encrypted with the channel key, the MeshCore app
//    does not show it as a message); the sender re-sends just the missing parts as new packets, and the
//    receiving boards drop parts they already have.
//  - SenkoOS mark: our adverts carry FEAT2 = 'S' << 8 | version, so SenkoOS boards can tell each other apart
//    on the map. FEAT1/FEAT2 are reserved upstream ("FUTURE"); the upstream parser skips them.
#include "DiyMesh.h"

#include <Preferences.h>
#include <helpers/AdvertDataHelpers.h>
#include <diy_version.h>

static const char* NVS_NS = "diy";
static const uint16_t SK_DATA_TYPE = 0x4B53;   // "SK" little-endian: our GRP_DATA requests
static const uint8_t SK_VER = 1, SK_OP_ASK = 1;
static const uint32_t ASK_AFTER_MS = 60000;    // silence after the last part before asking
static const uint32_t ASK_EVERY_MS = 60000;
static const uint32_t REASM_KEEP_MS = 300000;  // a message is "the same" for 5 min after its last part
static const uint32_t LM_ANSWER_MS = 900000;   // we answer requests for our last long message for 15 min
static const int LM_MAX_ANSWERS = 3;

// ---------------------------------------------------------------------------
//  settings
// ---------------------------------------------------------------------------
void DiyMesh::loadDelivery() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return;
  dl_retry = p.getUChar("dl_retry", 2);
  dl_ask = p.getUChar("dl_ask", 0) != 0;
  p.end();
  if (dl_retry > 3) dl_retry = 2;
}

void DiyMesh::saveDelivery() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putUChar("dl_retry", dl_retry);
  p.putUChar("dl_ask", dl_ask ? 1 : 0);
  p.end();
}

// ---------------------------------------------------------------------------
//  echo check and resend
// ---------------------------------------------------------------------------
// how long the repeaters around us may take to re-send a flood packet: their rx delay (score based, up to a few
// airtimes) + retransmit delay (random, up to 5 x airtime x tx delay factor) + their own airtime
uint32_t DiyMesh::echoWindowMs(int len) {
  uint32_t air = _radio->getEstAirtimeFor(len + 2);
  float txd = getNodePrefs()->tx_delay_factor;
  if (txd < 0.5f) txd = 0.5f;
  uint32_t w = (uint32_t)(air * (5.0f * txd + 3.0f)) + 3000;
  if (w < 6000) w = 6000;
  if (w > 45000) w = 45000;
  return w;
}

void DiyMesh::trackOut(int c, const mesh::Packet* pkt, uint32_t chat_seq, bool part) {
  dl.sent++;
  if (!dl_retry) {   // echo marks only
    // still tracked, so the panel shows "heard"; no resends
  }
  OutPkt* o = nullptr;
  for (auto& x : _out) if (!x.used) { o = &x; break; }
  if (!o) {   // all busy: drop the oldest one
    o = &_out[0];
    for (auto& x : _out) if (x.queued_ms < o->queued_ms) o = &x;
  }
  memset(o, 0, sizeof(*o));
  o->used = 1;
  o->ch = (uint8_t)c;
  o->part = part ? 1 : 0;
  o->type = pkt->getPayloadType();
  o->len = pkt->payload_len;
  memcpy(o->payload, pkt->payload, pkt->payload_len);
  pkt->calculatePacketHash(o->hash);
  o->queued_ms = millis();
  o->chat_seq = chat_seq;
}

static bool sameHash(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, MAX_HASH_SIZE) == 0; }

void DiyMesh::noteOutTx(const mesh::Packet* pkt) {
  if (pkt->getPayloadType() != PAYLOAD_TYPE_GRP_TXT || !pkt->isRouteFlood() || pkt->getPathHashCount() != 0) return;
  uint8_t h[MAX_HASH_SIZE];
  pkt->calculatePacketHash(h);
  for (auto& o : _out)
    if (o.used && !o.sent_ms && sameHash(o.hash, h)) o.sent_ms = millis() | 1;
}

void DiyMesh::noteOutEcho(const mesh::Packet* pkt) {
  if (pkt->getPayloadType() != PAYLOAD_TYPE_GRP_TXT || !pkt->isRouteFlood() || pkt->getPathHashCount() == 0) return;
  uint8_t h[MAX_HASH_SIZE];
  pkt->calculatePacketHash(h);
  for (auto& o : _out) {
    if (!o.used || !sameHash(o.hash, h)) continue;
    if (!o.echoes) dl.echoed++;
    if (o.echoes < 255) o.echoes++;
    ChatMsg* m = chatBySeq(o.ch, o.chat_seq);
    if (m) { m->dl = 2; m->echo = o.echoes; _chat_ver++; }
  }
}

// a part of a long message is still waiting for its echo (the next part waits for it)
bool DiyMesh::partInFlight() const {
  if (!dl_retry) return false;
  for (const auto& o : _out) if (o.used && o.part && !o.echoes) return true;
  return false;
}

void DiyMesh::loopDelivery() {
  uint32_t now = millis();
  for (auto& o : _out) {
    if (!o.used) continue;
    if (!o.sent_ms) {   // still in the send queue (or the pool was full)
      if (now - o.queued_ms > 120000) o.used = 0;
      continue;
    }
    if (now - o.sent_ms < echoWindowMs(o.len)) continue;
    if (o.echoes) { o.used = 0; continue; }
    ChatMsg* m = chatBySeq(o.ch, o.chat_seq);
    if (o.tries < dl_retry && chanSet(o.ch)) {
      mesh::Packet* p = _mgr->allocNew();
      if (!p) continue;   // try again on the next loop
      p->header = (uint8_t)(o.type << PH_TYPE_SHIFT);
      memcpy(p->payload, o.payload, o.len);
      p->payload_len = o.len;
      o.tries++;
      o.sent_ms = 0;
      o.queued_ms = now;
      dl.resent++;
      if (m) { m->tries = o.tries; _chat_ver++; }
      sendFlood(p, (uint32_t)0, (uint8_t)(getNodePrefs()->path_hash_mode + 1));
    } else {
      if (dl_retry) { dl.failed++; if (m) { m->dl = 3; _chat_ver++; } }
      else if (m) { m->dl = 0; _chat_ver++; }   // echo check off: no "no echo" mark
      o.used = 0;
    }
  }
  loopAsks();
}

// ---------------------------------------------------------------------------
//  missing parts
// ---------------------------------------------------------------------------
static uint16_t crc16(const char* s) {   // FNV-1a folded to 16 bits: tells two texts apart, nothing more
  uint32_t h = 2166136261u;
  for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
  return (uint16_t)(h ^ (h >> 16)) | 1;
}

// " [i/n]" at the end of the text -> i (1-based) and n, false if it is not a part
static bool partOf(const char* text, int& i, int& n) {
  const char* b = strrchr(text, '[');
  if (!b || b == text || b[-1] != ' ') return false;
  int a = 0, z = 0;
  char end = 0;
  if (sscanf(b, "[%d/%d%c", &a, &z, &end) != 3 || end != ']' || b[strlen(b) - 1] != ']') return false;
  if (z < 2 || z > 6 || a < 1 || a > z) return false;
  i = a; n = z;
  return true;
}

bool DiyMesh::notePart(int c, const char* sender, const char* text, uint32_t ts) {
  int i, n;
  if (!partOf(text, i, n)) return true;
  uint32_t now = millis();
  uint16_t crc = crc16(text);
  uint8_t bit = (uint8_t)(1u << (i - 1)), full = (uint8_t)((1u << n) - 1);
  Reasm* r = nullptr;
  for (auto& x : _rs)
    if (x.used && x.ch == c && x.n == n && now - x.last_ms < REASM_KEEP_MS && strcmp(x.from, sender) == 0) { r = &x; break; }
  // a re-sent part reaches every board in the channel, also those that had the whole message:
  // the same text in the same place within the window is a duplicate even when the message is complete
  // (so the same long text sent twice within 5 min shows once)
  if (r && (r->mask & bit)) {
    if (r->crc[i - 1] == crc) return false;
    r = nullptr;                              // the same number with another text: a new message
  }
  if (r && r->mask == full) r = nullptr;      // complete, and this is a part we do not know: a new message
  if (!r) {
    for (auto& x : _rs) if (!x.used) { r = &x; break; }
    if (!r) { r = &_rs[0]; for (auto& x : _rs) if (x.last_ms < r->last_ms) r = &x; }
    memset(r, 0, sizeof(*r));
    r->used = 1; r->ch = (uint8_t)c; r->n = (uint8_t)n;
    strlcpy(r->from, sender, sizeof(r->from));
    r->ref_i = (uint8_t)i; r->ref_ts = ts;
  }
  r->mask |= bit;
  r->crc[i - 1] = crc;
  r->last_ms = now;
  return true;
}

void DiyMesh::loopAsks() {
  if (!dl_ask) return;
  uint32_t now = millis();
  for (auto& r : _rs) {
    if (!r.used) continue;
    uint8_t full = (uint8_t)((1u << r.n) - 1);
    if (r.mask == full || now - r.last_ms > REASM_KEEP_MS) continue;
    if (now - r.last_ms < ASK_AFTER_MS || (r.ask_ms && now - r.ask_ms < ASK_EVERY_MS)) continue;
    if (r.asks >= (dl_retry ? dl_retry : 1)) continue;
    if (!isSenkoName(r.from)) continue;   // only SenkoOS boards can answer
    const mesh::GroupChannel* key = chanKey(r.ch);
    if (!key) continue;
    // data_type(2) len(1) | ver op n missing ref_i ref_ts(4) now(4, makes every request a new packet)
    uint8_t d[3 + 13];
    uint32_t t = getRTCClock()->getCurrentTimeUnique();
    d[0] = SK_DATA_TYPE & 0xFF; d[1] = SK_DATA_TYPE >> 8; d[2] = 13;
    d[3] = SK_VER; d[4] = SK_OP_ASK; d[5] = r.n; d[6] = (uint8_t)(full & ~r.mask); d[7] = r.ref_i;
    memcpy(&d[8], &r.ref_ts, 4);
    memcpy(&d[12], &t, 4);
    mesh::Packet* pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_DATA, *key, d, sizeof(d));
    if (!pkt) continue;
    sendFlood(pkt, (uint32_t)0, (uint8_t)(getNodePrefs()->path_hash_mode + 1));
    r.asks++;
    r.ask_ms = now | 1;
    dl.asks_tx++;
  }
}

void DiyMesh::onSenkoData(int c, const uint8_t* d, int len) {
  if (len < 3 + 13) return;
  if ((uint16_t)(d[0] | d[1] << 8) != SK_DATA_TYPE || d[2] < 13 || d[3] != SK_VER || d[4] != SK_OP_ASK) return;
  dl.asks_rx++;
  if (!dl_ask) return;
  uint8_t n = d[5], missing = d[6], ref_i = d[7];
  uint32_t ref_ts;
  memcpy(&ref_ts, &d[8], 4);
  if (!_lm_n || _lm_ch != c || n != _lm_n || ref_i < 1 || ref_i > n) return;
  if (millis() - _lm_ms > LM_ANSWER_MS || _lm_answers >= LM_MAX_ANSWERS) return;
  if (ref_ts != _lm_ts[ref_i - 1] && ref_ts != _lm_ts0[ref_i - 1]) return;   // not our message
  missing &= (uint8_t)((1u << n) - 1);
  if (!missing) return;
  _lm_resend |= missing;
  _lm_answers++;
}

// ---------------------------------------------------------------------------
//  SenkoOS mark
// ---------------------------------------------------------------------------
uint8_t DiyMesh::senkoVersionByte() const {
  int ma = 0, mi = 0;   // "v1.6.0-..." -> 0x16; dev builds -> 0
  if (DIY_VERSION[0] == 'v' && sscanf(DIY_VERSION + 1, "%d.%d", &ma, &mi) == 2 && ma >= 0 && ma < 8 && mi >= 0) {
    if (mi > 15) mi = 15;
    return (uint8_t)(ma << 4 | mi);
  }
  return 0;
}

// the chat sender is the node name; in the advert it may be shortened to make room for the mark
bool DiyMesh::isSenkoName(const char* name) const {
  for (int i = 0; i < num_nodes; i++) {
    size_t k = strlen(nodes[i].name);
    if (nodes[i].senko && k >= 4 && strncmp(nodes[i].name, name, k) == 0) return true;
  }
  return false;
}
