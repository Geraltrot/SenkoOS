#pragma once

#include <Arduino.h>
#include <Mesh.h>

class DiyMesh;
class WifiLink;
class JsonOut;

// LAN bridge: joins SenkoOS repeaters over Wi-Fi (UDP), so packets heard by one of them are
// re-sent over LoRa by the others without being relayed between them over the air. Useful when
// two or more repeaters at one site look in different directions.
//
// - Every packet heard over LoRa goes to the other bridge nodes; each of them handles it like a
//   packet just received (dedup by packet hash, block rules, forwarding), so ordinary MeshCore
//   nodes see normal packets. Same idea as the upstream bridges (src/helpers/bridges), source = RX.
// - Nodes find each other by UDP broadcast in their subnet ("hello" every 30 s); up to 4 fixed
//   addresses can be added for nodes in another subnet or over a VPN. A node that sends to us
//   from an address we do not know is answered too (learned peer, forgotten after 5 min).
// - Every frame carries an HMAC-SHA256 (16 bytes) with the shared bridge secret: frames from
//   strangers in the LAN are dropped. MeshCore payloads are already encrypted end to end;
//   the secret is about who may inject packets, not about secrecy.
// - Replays: a sliding seq window per sender and boot id (works without NTP), plus the epoch
//   check when both clocks are set. Flooding: at most RATE_PER_S packets per second per sender.
// - Loops: packet hashes seen in either direction are kept for DEDUP_TTL_MS; the hop counter in
//   the frame is raised by nodes that pass frames on (gateways), frames at the limit are dropped.
//
// Frame ver 2 (big-endian; ver 1 = the same without hops and boot id, still accepted):
//   "SL" | ver | kind (0 packet, 1 hello) | hops | sender id (4, public key prefix) |
//   boot id (4, random per start) | epoch (4) | seq (4) | data (raw packet or node name) | HMAC (16)
//
// CLI: lan | lan on|off | lan secret <8-32 chars> | lan peer add|del <ip> | lan port <n> | lan hops <n>
class LanBridge {
public:
  static const int MAX_FIXED = 4, MAX_PEERS = 8, MAX_SENDERS = 8;
  static const int DEDUP_SIZE = 192;
  static const uint32_t DEDUP_TTL_MS = 60000;
  static const int RATE_PER_S = 20, RATE_BURST = 40;

  struct Peer {                 // an address we exchange frames with
    uint32_t ip = 0;            // IPv4 as IPAddress stores it
    uint8_t id[4] = {0};
    char name[24] = "";
    uint32_t last_ms = 0;       // last valid frame, 0 = never
    uint32_t rx = 0, tx = 0;    // packets from / to this peer
    bool fixed = false;         // from the settings, not learned
  };
  struct Sender {               // replay window and rate limit, per sender id (any address)
    uint8_t id[4] = {0};
    bool used = false;
    struct Win { uint32_t boot = 0, max_seq = 0, used_ms = 0; uint64_t bits = 0; };
    Win win[3];                 // one seq window per boot id (the last 3 starts of the sender)
    float tokens = RATE_BURST;
    uint32_t refill_ms = 0;
    uint32_t last_ms = 0;
    uint32_t rate_drop = 0, replay = 0;
  };

  void begin(DiyMesh* mesh, WifiLink* wifi, const uint8_t* self_pub);
  void loop();
  void onRadioRx(const mesh::Packet* pkt);   // from DiyMesh::logRx
  bool handleCommand(const char* cmd, char* reply, size_t reply_len);
  void writeStatus(JsonOut& j) const;          // "lan":{...} for /api/status
  void writePrefs(JsonOut& j) const;           // lan_* fields for /api/prefs

  bool enabled() const { return _on; }
  bool running() const { return _running; }
  int activePeers() const;
  uint32_t rxCount() const { return _rx; }
  uint32_t txCount() const { return _tx; }
  const char* stateText() const;

  // static: pure logic, testable on the host
  static bool windowAccept(Sender& s, uint32_t boot, uint32_t seq, uint32_t now_ms);
  static bool rateAccept(Sender& s, uint32_t now_ms);

private:
  DiyMesh* _mesh = nullptr;
  WifiLink* _wifi = nullptr;
  uint8_t _self[4] = {0};
  uint32_t _boot = 0;
  bool _on = false, _running = false;
  char _secret[33] = "";
  uint16_t _port = 4280;
  uint8_t _hop_limit = 2;
  Peer _peers[MAX_PEERS];
  Sender _senders[MAX_SENDERS];
  uint32_t _seq = 0, _next_hello = 0;
  uint32_t _rx = 0, _tx = 0, _bad_auth = 0, _dup = 0, _inj_fail = 0, _replay = 0, _rate_drop = 0, _hop_drop = 0;
  struct Seen { uint8_t h[8]; uint32_t ms; };
  Seen _seen[DEDUP_SIZE];      // recent packet hashes, both directions
  uint16_t _seen_next = 0;

  void load();
  void save() const;
  void start();
  void stop();
  bool seen(const uint8_t* h) const;
  void markSeen(const uint8_t* h);
  void sendFrame(uint8_t kind, const uint8_t* data, size_t len);
  void sendTo(uint32_t ip, const uint8_t* frame, size_t len);
  void receive();
  Peer* peerFor(uint32_t ip, bool create);
  Sender* senderFor(const uint8_t* id);
  void sign(const uint8_t* data, size_t len, uint8_t out[16]) const;
};

extern LanBridge lan_bridge;
