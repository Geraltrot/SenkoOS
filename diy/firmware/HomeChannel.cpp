// Private group channel "Home": a chat in the MeshCore app that is bridged to Home Assistant,
// plus up to 4 extra channels (private key, #hashtag or Public) that are only shown in the panel chat.
// Uses the upstream group-channel API (searchChannelsByHash / onGroupDataRecv / createGroupDatagram),
// the MeshCore core is not modified.
#include "DiyMesh.h"

#include <Preferences.h>
#include <Utils.h>

static const char* NVS_NS = "diy";

// ---------------------------------------------------------------------------
//  key parsing: 32 / 64 hex chars or base64 of 16 / 32 bytes (what the app shares)
// ---------------------------------------------------------------------------
static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

// returns key length (16 or 32), 0 = not a key
static int parseHomeKey(const char* s, uint8_t out[32]) {
  size_t n = strlen(s);
  memset(out, 0, 32);
  if (n == 32 || n == 64) {
    bool ok = true;
    for (size_t i = 0; i < n / 2 && ok; i++) {
      int hi = hexv(s[2 * i]), lo = hexv(s[2 * i + 1]);
      if (hi < 0 || lo < 0) ok = false; else out[i] = (uint8_t)(hi << 4 | lo);
    }
    if (ok) return (int)(n / 2);
  }
  uint32_t acc = 0; int bits = 0, o = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '=') break;
    int v = b64v(s[i]);
    if (v < 0) return 0;
    acc = (acc << 6) | (uint32_t)v; bits += 6;
    if (bits >= 8) { bits -= 8; if (o >= 32) return 0; out[o++] = (uint8_t)(acc >> bits); }
  }
  return (o == 16 || o == 32) ? o : 0;
}

void DiyMesh::applyHomeKey(const uint8_t* key, int len) {
  memset(&_home, 0, sizeof(_home));
  memcpy(_home.secret, key, len);
  mesh::Utils::sha256(_home.hash, sizeof(_home.hash), _home.secret, len);   // same as BaseChatMesh::addChannel
  _home_set = true;
}

void DiyMesh::loadHome() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return;
  uint8_t key[32];
  size_t n = p.getBytesLength("hc_key");
  if ((n == 16 || n == 32) && p.getBytes("hc_key", key, n) == n) applyHomeKey(key, (int)n);
  p.getString("hc_name", _home_name, sizeof(_home_name));
  p.end();
  if (!_home_name[0]) strcpy(_home_name, "Дом");
}

// ---------------------------------------------------------------------------
//  mesh hooks
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  channels: 0 = "Home", 1..MAX_EXTRA_CH = extra ones
// ---------------------------------------------------------------------------
const mesh::GroupChannel* DiyMesh::chanKey(int c) const {
  if (c == 0) return _home_set ? &_home : nullptr;
  if (c < 1 || c > MAX_EXTRA_CH || !_xch[c - 1].len) return nullptr;
  return &_xch[c - 1].ch;
}
bool DiyMesh::chanSet(int c) const { return chanKey(c) != nullptr; }
const char* DiyMesh::chanName(int c) const { return c == 0 ? _home_name : (c <= MAX_EXTRA_CH ? _xch[c - 1].name : ""); }
uint8_t DiyMesh::chanHash(int c) const { const mesh::GroupChannel* k = chanKey(c); return k ? k->hash[0] : 0; }

struct StoredChan { uint8_t len; uint8_t key[32]; char name[24]; };

void DiyMesh::loadChans() {
  StoredChan sc[MAX_EXTRA_CH];
  Preferences p;
  if (!p.begin(NVS_NS, true)) return;
  bool ok = p.getBytesLength("xch") == sizeof(sc) && p.getBytes("xch", sc, sizeof(sc)) == sizeof(sc);
  p.end();
  if (!ok) return;
  for (int i = 0; i < MAX_EXTRA_CH; i++) {
    ExtraChan& x = _xch[i];
    memset(&x, 0, sizeof(x));
    if (sc[i].len != 16 && sc[i].len != 32) continue;
    x.len = sc[i].len;
    memcpy(x.ch.secret, sc[i].key, x.len);
    mesh::Utils::sha256(x.ch.hash, sizeof(x.ch.hash), x.ch.secret, x.len);
    memcpy(x.name, sc[i].name, sizeof(x.name) - 1);
  }
  memset(sc, 0, sizeof(sc));
}

void DiyMesh::saveChans() {
  StoredChan sc[MAX_EXTRA_CH] = {};
  for (int i = 0; i < MAX_EXTRA_CH; i++) {
    sc[i].len = _xch[i].len;
    memcpy(sc[i].key, _xch[i].ch.secret, 32);
    strlcpy(sc[i].name, _xch[i].name, sizeof(sc[i].name));
  }
  Preferences p;
  if (p.begin(NVS_NS, false)) { p.putBytes("xch", sc, sizeof(sc)); p.end(); }
  memset(sc, 0, sizeof(sc));
}

// ---------------------------------------------------------------------------
//  mesh hooks
// ---------------------------------------------------------------------------
int DiyMesh::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) {
  int n = 0;
  for (int c = 0; c < NUM_CH && n < max_matches; c++) {
    const mesh::GroupChannel* k = chanKey(c);
    if (k && hash[0] == k->hash[0]) channels[n++] = *k;
  }
  return n;
}

void DiyMesh::onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) {
  int c = -1;
  for (int i = 0; i < NUM_CH && c < 0; i++) {
    const mesh::GroupChannel* k = chanKey(i);
    if (k && memcmp(channel.secret, k->secret, sizeof(k->secret)) == 0) c = i;
  }
  if (c < 0) return;
  if (type == PAYLOAD_TYPE_GRP_DATA) { onSenkoData(c, data, (int)len); return; }   // missing-part requests
  if (type != PAYLOAD_TYPE_GRP_TXT || len < 6) return;
  uint32_t ts;
  memcpy(&ts, data, 4);
  if ((data[4] >> 2) != 0) return;   // plain text only
  data[len] = 0;
  char* text = (char*)&data[5];
  // "<sender>: <message>"
  char sender[32] = "";
  char* sep = strstr(text, ": ");
  if (sep && sep - text < (int)sizeof(sender)) {
    memcpy(sender, text, sep - text);
    sender[sep - text] = 0;
    text = sep + 2;
  }
  if (strcmp(sender, getNodePrefs()->node_name) == 0) return;   // our own message heard back
  if (!notePart(c, sender, text, ts)) { dl.dups++; return; }       // a part we already have (re-sent on request)
  addChat(c, 0, sender, text, (int8_t)(packet->getSNR() * 4));
  if (c > 0) { _xch[c - 1].rx++; return; }   // extra channels: panel only, not Home Assistant
  home_rx++;
  if (_home_chat_fn) _home_chat_fn(sender, text, (int8_t)(packet->getSNR() * 4), packet->getPathHashCount());
}

bool DiyMesh::sendChat(int c, const char* text, bool part, uint32_t* ts_out) {
  const mesh::GroupChannel* key = chanKey(c);
  if (!key || !text || !text[0]) return false;
  // one group packet carries at most MAX_GROUP_DATA_LENGTH bytes: timestamp(4) + type(1) + "<name>: <text>"
  uint8_t temp[MAX_GROUP_DATA_LENGTH + 1];
  uint32_t ts = getRTCClock()->getCurrentTimeUnique();
  memcpy(temp, &ts, 4);
  temp[4] = 0;   // TXT_TYPE_PLAIN
  int n = snprintf((char*)&temp[5], sizeof(temp) - 5, "%s: %s", getNodePrefs()->node_name, text);
  if (n < 0) return false;
  int max = MAX_GROUP_DATA_LENGTH - 5;
  if (n > max) {   // too long: cut on a UTF-8 character boundary
    n = max;
    while (n > 0 && (temp[5 + n] & 0xC0) == 0x80) n--;
    temp[5 + n] = 0;
  }
  mesh::Packet* pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, *key, temp, 5 + n);
  if (!pkt) return false;
  if (ts_out) *ts_out = ts;
  if (c == 0) home_tx++; else _xch[c - 1].tx++;
  const char* body = strstr((const char*)&temp[5], ": ");
  uint32_t seq = addChat(c, 1, getNodePrefs()->node_name, body ? body + 2 : (const char*)&temp[5], 0);
  trackOut(c, pkt, seq, part);   // before sendFlood: the packet may be freed once sent
  sendFlood(pkt, (uint32_t)0, (uint8_t)(getNodePrefs()->path_hash_mode + 1));
  return true;
}

uint32_t DiyMesh::addChat(int c, uint8_t out, const char* from, const char* text, int8_t snr4) {
  int k = c ? CHAT_KEEP_X : CHAT_KEEP, b = c ? CHAT_KEEP + (c - 1) * CHAT_KEEP_X : 0;
  ChatMsg& m = _chat[b + _chat_head[c]];
  m.seq = ++_chat_seq;
  _chat_last[c] = m.seq;
  m.ts = getRTCClock()->getCurrentTime();
  m.out = out;
  m.snr4 = snr4;
  m.dl = out ? 1 : 0; m.echo = 0; m.tries = 0;
  _chat_ver++;
  strlcpy(m.from, from ? from : "", sizeof(m.from));
  strlcpy(m.text, text ? text : "", sizeof(m.text));
  _chat_head[c] = (_chat_head[c] + 1) % k;
  if (_chat_n[c] < k) _chat_n[c]++;
  return m.seq;
}

DiyMesh::ChatMsg* DiyMesh::chatBySeq(int c, uint32_t seq) {
  if (c < 0 || c >= NUM_CH || !seq) return nullptr;
  for (int i = _chat_n[c] - 1; i >= 0; i--) {
    ChatMsg& m = const_cast<ChatMsg&>(chatAt(c, i));
    if (m.seq == seq) return &m;
    if (m.seq < seq) break;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
//  long messages: split into parts that fit one packet, sent at least 12 s apart
//  (back-to-back flood packets arrive out of order); with the echo check on, the next part
//  waits until the previous one was heard re-sent by a repeater (or all its resends are used)
// ---------------------------------------------------------------------------
static const uint32_t PART_GAP_MS = 12000;

// largest n <= max that does not cut a UTF-8 character; prefers a line break or a space in the last third
static int cutAt(const char* s, int len, int max) {
  if (len <= max) return len;
  int n = max;
  while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
  for (int k = n; k > max * 2 / 3; k--) if (s[k] == '\n') return k;
  for (int k = n; k > max * 2 / 3; k--) if (s[k] == ' ') return k;
  return n;
}

bool DiyMesh::queueChat(int c, const char* text) {
  if (!chanSet(c) || !text || !text[0]) return false;
  int len = strlen(text);
  // room for the text in one packet: data = timestamp(4) + type(1) + "<name>: " + text
  int room = MAX_GROUP_DATA_LENGTH - 5 - (int)strlen(getNodePrefs()->node_name) - 2;
  if (len <= room && homeQueued() == 0) return sendChat(c, text);   // short: at once
  if (homeQueued() > 0) return false;   // previous long message (or parts asked for again) still being sent
  int room_part = room - 7;             // " [1/6]"
  if (room_part < 20) return false;
  int starts[HOME_MAX_PARTS], lens[HOME_MAX_PARTS], n = 0, pos = 0;
  while (pos < len && n < HOME_MAX_PARTS) {
    while (pos < len && (text[pos] == ' ' || text[pos] == '\n')) pos++;   // no leading blanks in a part
    if (pos >= len) break;
    int l = cutAt(text + pos, len - pos, room_part);
    starts[n] = pos; lens[n] = l; n++;
    pos += l;
  }
  for (int i = 0; i < n; i++) {
    int l = lens[i];
    while (l > 0 && (text[starts[i] + l - 1] == ' ' || text[starts[i] + l - 1] == '\n')) l--;
    snprintf(_home_parts[i], sizeof(_home_parts[i]), "%.*s [%d/%d]", l, text + starts[i], i + 1, n);
  }
  _home_parts_n = n;
  _home_parts_i = 0;
  _home_parts_ch = c;
  _home_next_ms = millis();
  _lm_n = (uint8_t)n; _lm_ch = (uint8_t)c; _lm_answers = 0; _lm_resend = 0; _lm_ms = millis();
  memset(_lm_ts, 0, sizeof(_lm_ts)); memset(_lm_ts0, 0, sizeof(_lm_ts0));
  return true;
}

void DiyMesh::loopHome() {
  if ((int32_t)(millis() - _home_next_ms) < 0 || partInFlight()) return;
  int i = -1;
  bool again = false;
  if (_home_parts_i < _home_parts_n) i = _home_parts_i++;
  else if (_lm_resend) { i = __builtin_ctz(_lm_resend); _lm_resend &= ~(1u << i); again = true; }   // asked for again
  if (i < 0) return;
  if (i < _lm_n && chanSet(_lm_ch)) {
    uint32_t ts = 0;
    if (sendChat(_lm_ch, _home_parts[i], true, &ts)) {
      _lm_ts[i] = ts;
      if (!_lm_ts0[i]) _lm_ts0[i] = ts;
      if (again) dl.parts_resent++;
    }
  }
  _home_next_ms = millis() + PART_GAP_MS;
  if (_home_parts_i >= _home_parts_n) _home_parts_n = _home_parts_i = 0;
}

// ---------------------------------------------------------------------------
//  CLI (local only: USB and web console; the key is never echoed)
//    home                       status
//    home channel <key>         channel key from the app (hex or base64)
//    home channel off           forget the key
//    home name <text>           local label for the panel
//    home say <text>            send a message to the channel
// ---------------------------------------------------------------------------
bool DiyMesh::handleHomeCommand(const char* command, char* reply, size_t reply_len) {
  if (strncmp(command, "home", 4) != 0 || (command[4] != 0 && command[4] != ' ')) return false;
  const char* arg = command + 4;
  while (*arg == ' ') arg++;

  if (*arg == 0) {
    snprintf(reply, reply_len, "home: %s, channel '%s' (hash %02X), received %lu, sent %lu", _home_set ? "on" : "off (set: home channel <key>)",
             _home_name, _home_set ? _home.hash[0] : 0, (unsigned long)home_rx, (unsigned long)home_tx);
  } else if (strcmp(arg, "channel off") == 0) {
    Preferences p;
    if (p.begin(NVS_NS, false)) { p.remove("hc_key"); p.end(); }
    memset(&_home, 0, sizeof(_home));
    _home_set = false;
    snprintf(reply, reply_len, "OK - home channel off");
  } else if (strncmp(arg, "channel ", 8) == 0) {
    uint8_t key[32];
    int n = parseHomeKey(arg + 8, key);
    if (!n) { snprintf(reply, reply_len, "Err - key: 32 or 64 hex chars, or base64 of 16/32 bytes"); return true; }
    Preferences p;
    if (p.begin(NVS_NS, false)) { p.putBytes("hc_key", key, n); p.end(); }
    applyHomeKey(key, n);
    memset(key, 0, sizeof(key));
    snprintf(reply, reply_len, "OK - home channel set (hash %02X)", _home.hash[0]);
  } else if (strncmp(arg, "name ", 5) == 0) {
    strlcpy(_home_name, arg + 5, sizeof(_home_name));
    Preferences p;
    if (p.begin(NVS_NS, false)) { p.putString("hc_name", _home_name); p.end(); }
    snprintf(reply, reply_len, "OK - name '%s'", _home_name);
  } else if (strncmp(arg, "say ", 4) == 0) {
    snprintf(reply, reply_len, queueHomeChat(arg + 4) ? "OK - sent" : "Err - home channel is not set or a long message is still being sent");
  } else {
    snprintf(reply, reply_len, "Err - home [channel <key>|channel off|name <text>|say <text>]");
  }
  return true;
}

// ---------------------------------------------------------------------------
//  extra chat channels (local CLI only; keys are never echoed)
//    chan                               list
//    chan add <key|#hashtag|public> [name]
//    chan del <N>                       N = 1..4
//    chan name <N> <text>
//    chan say <N> <text>
// ---------------------------------------------------------------------------
static const uint8_t PUBLIC_KEY[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

static int chanIndex(const char* s) { int n = atoi(s); return (n >= 1 && n <= DiyMesh::MAX_EXTRA_CH) ? n : 0; }

bool DiyMesh::handleChanCommand(const char* command, char* reply, size_t reply_len) {
  if (strncmp(command, "chan", 4) != 0 || (command[4] != 0 && command[4] != ' ')) return false;
  const char* arg = command + 4;
  while (*arg == ' ') arg++;

  if (*arg == 0 || strcmp(arg, "list") == 0) {
    size_t o = snprintf(reply, reply_len, "chan:");
    for (int i = 0; i < MAX_EXTRA_CH && o < reply_len; i++) {
      const ExtraChan& x = _xch[i];
      if (x.len) o += snprintf(reply + o, reply_len - o, " %d '%s' (hash %02X, rx %lu, tx %lu);", i + 1, x.name, x.ch.hash[0], (unsigned long)x.rx, (unsigned long)x.tx);
      else o += snprintf(reply + o, reply_len - o, " %d -;", i + 1);
    }
  } else if (strncmp(arg, "add ", 4) == 0) {
    char k[96]; const char* rest = arg + 4;
    while (*rest == ' ') rest++;
    int kl = 0;
    while (rest[kl] && rest[kl] != ' ' && kl < (int)sizeof(k) - 1) { k[kl] = rest[kl]; kl++; }
    k[kl] = 0;
    const char* name = rest + kl;
    while (*name == ' ') name++;
    uint8_t key[32] = {0}; int n = 0; char defname[24] = "";
    if (k[0] == '#' && k[1]) {   // hashtag channel: key = first 16 bytes of sha256("#tag"), readable by anyone who knows the tag
      mesh::Utils::sha256(key, 16, (const uint8_t*)k, strlen(k));
      n = 16; strlcpy(defname, k, sizeof(defname));
    } else if (strcasecmp(k, "public") == 0) {
      memcpy(key, PUBLIC_KEY, 16); n = 16; strcpy(defname, "Public");
    } else {
      n = parseHomeKey(k, key);
      strcpy(defname, "Channel");
    }
    memset(k, 0, sizeof(k));
    if (!n) { snprintf(reply, reply_len, "Err - key: 32/64 hex chars, base64, #hashtag or public"); return true; }
    for (int c = 0; c < NUM_CH; c++) {
      const mesh::GroupChannel* ck = chanKey(c);
      if (ck && memcmp(ck->secret, key, 32) == 0) { memset(key, 0, sizeof(key)); snprintf(reply, reply_len, "Err - this channel is already added (%d)", c); return true; }
    }
    int slot = -1;
    for (int i = 0; i < MAX_EXTRA_CH && slot < 0; i++) if (!_xch[i].len) slot = i;
    if (slot < 0) { memset(key, 0, sizeof(key)); snprintf(reply, reply_len, "Err - all %d channels are used (chan del <N>)", MAX_EXTRA_CH); return true; }
    ExtraChan& x = _xch[slot];
    memset(&x, 0, sizeof(x));
    x.len = (uint8_t)n;
    memcpy(x.ch.secret, key, n);
    mesh::Utils::sha256(x.ch.hash, sizeof(x.ch.hash), x.ch.secret, n);
    strlcpy(x.name, *name ? name : defname, sizeof(x.name));
    memset(key, 0, sizeof(key));
    _chat_n[slot + 1] = _chat_head[slot + 1] = 0;
    saveChans();
    snprintf(reply, reply_len, "OK - channel %d '%s' (hash %02X)", slot + 1, x.name, x.ch.hash[0]);
  } else if (strncmp(arg, "del ", 4) == 0) {
    int c = chanIndex(arg + 4);
    if (!c || !_xch[c - 1].len) { snprintf(reply, reply_len, "Err - no channel %s", arg + 4); return true; }
    if (homeQueued() > 0 && _home_parts_ch == c) _home_parts_n = _home_parts_i = 0;
    if (_lm_ch == c) _lm_n = _lm_resend = 0;
    memset(&_xch[c - 1], 0, sizeof(ExtraChan));
    _chat_n[c] = _chat_head[c] = 0;
    saveChans();
    snprintf(reply, reply_len, "OK - channel %d removed", c);
  } else if (strncmp(arg, "name ", 5) == 0) {
    int c = chanIndex(arg + 5);
    const char* t = strchr(arg + 5, ' ');
    if (!c || !_xch[c - 1].len || !t || !t[1]) { snprintf(reply, reply_len, "Err - chan name <N> <text>"); return true; }
    strlcpy(_xch[c - 1].name, t + 1, sizeof(_xch[c - 1].name));
    saveChans();
    snprintf(reply, reply_len, "OK - channel %d '%s'", c, _xch[c - 1].name);
  } else if (strncmp(arg, "say ", 4) == 0) {
    int c = chanIndex(arg + 4);
    const char* t = strchr(arg + 4, ' ');
    if (!c || !t || !t[1]) { snprintf(reply, reply_len, "Err - chan say <N> <text>"); return true; }
    snprintf(reply, reply_len, queueChat(c, t + 1) ? "OK - sent" : "Err - no such channel or a long message is still being sent");
  } else {
    snprintf(reply, reply_len, "Err - chan [add <key|#tag|public> [name]|del <N>|name <N> <text>|say <N> <text>]");
  }
  return true;
}
