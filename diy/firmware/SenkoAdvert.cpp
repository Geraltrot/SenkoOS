// Our adverts with the SenkoOS mark (FEAT2 = 'S' << 8 | version), and the "deliver" CLI.
//
// Upstream MyMesh builds its advert in a private, non-virtual createSelfAdvert() and keeps the advert
// timers and the default flood scope private. To add the mark without touching upstream files we:
//  - override the virtual sendSelfAdvertisement() (CLI "advert", the panel buttons, the start-up advert);
//  - run the periodic adverts ourselves just before MyMesh::loop() would: same timers, same scope, then
//    reschedule them with the upstream updateFloodAdvertTimer()/updateAdvertTimer(), so MyMesh never fires.
// The private members are reached with the standard explicit-instantiation access idiom (no UB, no
// header changes); if upstream renames them the build fails here instead of silently misbehaving.
#include "DiyMesh.h"

#include <helpers/AdvertDataHelpers.h>

namespace {
template <typename Tag, typename Tag::type M> struct Rob { friend typename Tag::type get(Tag) { return M; } };
struct NextLocal { typedef unsigned long MyMesh::*type; friend type get(NextLocal); };
struct NextFlood { typedef unsigned long MyMesh::*type; friend type get(NextFlood); };
struct Scope { typedef TransportKey MyMesh::*type; friend type get(Scope); };
struct Cli { typedef CommonCLI MyMesh::*type; friend type get(Cli); };
template struct Rob<NextLocal, &MyMesh::next_local_advert>;
template struct Rob<NextFlood, &MyMesh::next_flood_advert>;
template struct Rob<Scope, &MyMesh::default_scope>;
template struct Rob<Cli, &MyMesh::_cli>;
}

// upstream app data (type, location policy, name) + FEAT2; the name is shortened if it no longer fits
static uint8_t buildMarkedAdvert(CommonCLI& cli, uint16_t feat2, uint8_t out[MAX_ADVERT_DATA_SIZE]) {
  uint8_t base[MAX_ADVERT_DATA_SIZE];
  uint8_t n = cli.buildAdvertData(ADV_TYPE_REPEATER, base);
  if (n == 0) return 0;
  uint8_t flags = base[0];
  if (flags & ADV_FEAT2_MASK) { memcpy(out, base, n); return n; }   // upstream uses it now: leave as is
  int at = 1 + ((flags & ADV_LATLON_MASK) ? 8 : 0) + ((flags & ADV_FEAT1_MASK) ? 2 : 0);
  if (at > n) { memcpy(out, base, n); return n; }
  int tail = n - at;                                   // the name
  if (at + 2 + tail > MAX_ADVERT_DATA_SIZE) {
    tail = MAX_ADVERT_DATA_SIZE - at - 2;
    while (tail > 0 && (base[at + tail] & 0xC0) == 0x80) tail--;   // do not cut a UTF-8 character
  }
  memcpy(out, base, at);
  out[0] = flags | ADV_FEAT2_MASK;
  out[at] = feat2 & 0xFF; out[at + 1] = feat2 >> 8;   // little-endian, as the upstream parser reads it
  memcpy(out + at + 2, base + at, tail);
  return (uint8_t)(at + 2 + tail);
}

void DiyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  uint8_t app[MAX_ADVERT_DATA_SIZE];
  uint8_t n = buildMarkedAdvert(this->*get(Cli()), (uint16_t)(SENKO_MARK << 8 | senkoVersionByte()), app);
  mesh::Packet* pkt = createAdvert(self_id, app, n);
  if (!pkt) return;
  if (flood) sendFloodScoped(this->*get(Scope()), pkt, delay_millis, getNodePrefs()->path_hash_mode + 1);
  else sendZeroHop(pkt, delay_millis);
}

// the same decisions as MyMesh::loop(), taken a moment earlier
void DiyMesh::loopAdvert() {
  unsigned long& nf = this->*get(NextFlood());
  unsigned long& nl = this->*get(NextLocal());
  if (nf && millisHasNowPassed(nf)) {
    sendSelfAdvertisement(0, true);
    updateFloodAdvertTimer();
    updateAdvertTimer();
  } else if (nl && millisHasNowPassed(nl)) {
    sendSelfAdvertisement(0, false);
    updateAdvertTimer();
  }
}

// ---------------------------------------------------------------------------
//  deliver                       status and counters
//  deliver retry <0-3>           resends without an echo (0 = off)
//  deliver ask on|off            ask SenkoOS senders for missing parts [i/n] and answer such requests
// ---------------------------------------------------------------------------
bool DiyMesh::handleDeliverCommand(const char* command, char* reply, size_t reply_len) {
  if (strncmp(command, "deliver", 7) != 0 || (command[7] != 0 && command[7] != ' ')) return false;
  const char* a = command + 7;
  while (*a == ' ') a++;
  if (!*a) {
    snprintf(reply, reply_len, "deliver: retry %u, ask %s; sent %lu, echo %lu, resent %lu, no echo %lu, asks out %lu / in %lu, parts again %lu, dup %lu",
             dl_retry, dl_ask ? "on" : "off", (unsigned long)dl.sent, (unsigned long)dl.echoed, (unsigned long)dl.resent,
             (unsigned long)dl.failed, (unsigned long)dl.asks_tx, (unsigned long)dl.asks_rx, (unsigned long)dl.parts_resent,
             (unsigned long)dl.dups);
  } else if (strncmp(a, "retry ", 6) == 0) {
    int v = atoi(a + 6);
    if (v < 0 || v > 3 || !isdigit((unsigned char)a[6])) { snprintf(reply, reply_len, "Err - retry 0..3"); return true; }
    dl_retry = (uint8_t)v;
    saveDelivery();
    snprintf(reply, reply_len, "OK - resend without an echo: %d", v);
  } else if (strcmp(a, "ask on") == 0 || strcmp(a, "ask off") == 0) {
    dl_ask = a[5] == 'n';
    saveDelivery();
    snprintf(reply, reply_len, "OK - missing parts: %s", dl_ask ? "on" : "off");
  } else {
    snprintf(reply, reply_len, "Err - deliver [retry <0-3>|ask on|off]");
  }
  return true;
}
