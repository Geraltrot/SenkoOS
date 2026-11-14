// Private group channel "Home": a chat in the MeshCore app that is bridged to Home Assistant.
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
int DiyMesh::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) {
  if (_home_set && max_matches > 0 && hash[0] == _home.hash[0]) {
    channels[0] = _home;
    return 1;
  }
  return 0;
}

void DiyMesh::onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) {
  if (type != PAYLOAD_TYPE_GRP_TXT || !_home_set || len < 6) return;
  if (memcmp(channel.secret, _home.secret, sizeof(_home.secret)) != 0) return;
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
  home_rx++;
  if (_home_chat_fn) _home_chat_fn(sender, text, (int8_t)(packet->getSNR() * 4), packet->getPathHashCount());
}

bool DiyMesh::sendHomeChat(const char* text) {
  if (!_home_set || !text || !text[0]) return false;
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
  mesh::Packet* pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, _home, temp, 5 + n);
  if (!pkt) return false;
  sendFlood(pkt, (uint32_t)0, (uint8_t)(getNodePrefs()->path_hash_mode + 1));
  home_tx++;
  return true;
}

// ---------------------------------------------------------------------------
//  long messages: split into parts that fit one packet, sent 12 s apart
//  (back-to-back flood packets arrive out of order)
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

bool DiyMesh::queueHomeChat(const char* text) {
  if (!_home_set || !text || !text[0]) return false;
  int len = strlen(text);
  // room for the text in one packet: data = timestamp(4) + type(1) + "<name>: " + text
  int room = MAX_GROUP_DATA_LENGTH - 5 - (int)strlen(getNodePrefs()->node_name) - 2;
  if (len <= room && homeQueued() == 0) return sendHomeChat(text);
  if (homeQueued() > 0) return false;   // previous long message still being sent
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
  _home_next_ms = millis();
  return true;
}

void DiyMesh::loopHome() {
  if (_home_parts_i >= _home_parts_n || (int32_t)(millis() - _home_next_ms) < 0) return;
  sendHomeChat(_home_parts[_home_parts_i++]);
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
