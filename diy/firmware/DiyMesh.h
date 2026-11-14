#pragma once

#include "MyMesh.h"

// DiyMesh = upstream MyMesh (simple_repeater) + observation and filtering for the
// web panel. Only virtual hooks are overridden; every hook calls MyMesh first,
// so repeating behaves exactly like upstream unless a block rule matches.
//
// Memory budget (WROOM-32, no PSRAM): ~22 KB of static tables in total.
class DiyMesh : public MyMesh {
public:
  DiyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
          mesh::RTCClock& rtc, mesh::MeshTables& tables)
    : MyMesh(board, radio, ms, rng, rtc, tables) {}

  void beginDiy();     // after MyMesh::begin(): loads block lists
  void loopDiy();      // once per loop(): history sampling, trace timeout

  // ---------- packet log ----------
  static const int LOG_SIZE = 150;
  static const int PATH_KEEP = 12;
  struct PktEntry {
    uint32_t seq, ts, ms;
    uint8_t dir;            // 0 = RX, 1 = TX, 2 = TX failed
    uint8_t type, route, hops, hash_sz, len, ch;   // ch: group channel hash, 255 = n/a
    uint8_t path_n;         // bytes stored in path[]
    int8_t snr4;
    int16_t rssi;
    uint8_t path[PATH_KEEP];
  };
  uint32_t logSeq() const { return _log_seq; }
  int logCount() const { return _log_count; }
  // i = 0 is the oldest entry still kept
  const PktEntry& logAt(int i) const { return _log[(_log_head - _log_count + i + LOG_SIZE) % LOG_SIZE]; }
  const PktEntry* lastRx() const;

  // ---------- counters ----------
  uint32_t rx_by_type[16] = {}, tx_by_type[16] = {};
  uint32_t rx_flood = 0, rx_direct = 0, tx_flood = 0, tx_direct = 0, tx_fail = 0, blocked_count = 0;

  // ---------- history (sampled every minute) ----------
  struct Bucket {
    uint32_t rx_air_ms, tx_air_ms;
    uint16_t rx, tx;
    int8_t nf_avg, nf_min, nf_max;
    uint8_t valid;
  };
  static const int MIN_BUCKETS = 60;     // last hour, 1 min each
  static const int TEN_BUCKETS = 144;    // last 24 h, 10 min each
  Bucket minutes[MIN_BUCKETS] = {};
  Bucket tens[TEN_BUCKETS] = {};
  int minute_head = 0, ten_head = 0;     // index of the newest bucket
  static const int NF_SAMPLES = 60;      // noise floor every 4 s, last 4 min
  int8_t nf_recent[NF_SAMPLES] = {};
  int nf_head = 0, nf_count = 0;
  uint32_t airTxMs() const { return getTotalAirTime(); }
  uint32_t airRxMs() const { return getReceiveAirTime(); }
  int noiseFloor() const { return _radio->getNoiseFloor(); }

  // ---------- channels and entry repeaters ----------
  static const int MAX_CHANS = 32, MAX_ENTRIES = 48, MAX_BLOCKS = 16;
  struct ChanStat { uint8_t hash; uint8_t used; uint32_t count, last_ms; };
  struct Key3 { uint8_t b[3]; uint8_t sz; };          // 1..3 byte node hash prefix
  struct EntryStat { Key3 key; uint32_t count, last_ms; };
  struct RouteKey { Key3 a, b; };
  ChanStat chans[MAX_CHANS] = {};
  EntryStat entries[MAX_ENTRIES] = {};
  int num_entries = 0;
  uint8_t blocked_chans[MAX_BLOCKS] = {};  int num_blocked_chans = 0;
  Key3 blocked_entries[MAX_BLOCKS] = {};   int num_blocked_entries = 0;
  RouteKey blocked_routes[MAX_BLOCKS] = {}; int num_blocked_routes = 0;
  bool repeat_public = true;
  uint8_t public_hash = 0;
  bool blockChannel(uint8_t hash, bool block);
  bool blockEntry(const Key3& k, bool block);
  bool blockRoute(const RouteKey& r, bool block);
  void setRepeatPublic(bool on);
  void clearChannelStats();
  static uint8_t hashtagChannelHash(const char* tag);   // "#name" -> channel hash byte
  static bool parseKey(const char* hex, Key3& out);

  // ---------- nodes (adverts, discovery, direct hearing) ----------
  static const int MAX_NODES = 64;
  enum { SEEN_ADVERT = 1, SEEN_DIRECT = 2, SEEN_SCAN = 4, SEEN_PATH = 8 };
  struct NodeInfo {
    uint8_t pub[8];          // first 8 bytes of public key (or hash prefix if only seen in paths)
    uint8_t known;           // bytes of pub[] that are known (1..8)
    char name[24];
    uint8_t type;            // ADV_TYPE_*
    uint8_t seen;            // SEEN_* bits
    int8_t snr4;             // our SNR when heard directly (x4), -128 = n/a
    int8_t their_snr4;       // their SNR of us (from scan answer), -128 = n/a
    int16_t rssi;
    uint8_t hops;            // hops of the last advert
    uint32_t last_ms, count;
    int32_t lat, lon;        // 1e-6 deg, 0 = n/a
  };
  NodeInfo nodes[MAX_NODES] = {};
  int num_nodes = 0;
  uint32_t scan_started_ms = 0;
  bool startScan();
  void clearNodes();
  const NodeInfo* findNodeByPrefix(const uint8_t* key, int sz) const;

  // ---------- path graph ----------
  static const int MAX_GNODES = 320;
  struct GNode {
    uint16_t parent;         // index, 0xFFFF for the root (us)
    uint8_t key[3];
    uint8_t sz : 2, orig : 1, used : 1;
    uint16_t count;
    int8_t snr4;             // first column only: SNR we heard that neighbour with
    uint32_t last_ms;
  };
  GNode gnodes[MAX_GNODES] = {};
  int num_gnodes = 0;
  uint32_t graph_forgotten = 0;
  void clearGraph();

  // ---------- trace ----------
  struct TraceState {
    bool pending = false, ok = false;
    uint32_t tag = 0, sent_ms = 0, rtt_ms = 0;
    uint8_t sz = 1, n = 0;           // hash size, number of hops in the full (there-and-back) path
    uint8_t hashes[32 * 4];
    int8_t snrs[32];
    int8_t final_snr4 = 0;
  } trace;
  // hops: list of node hashes (hex, 1/2/4 bytes each), "and back" appended automatically
  const char* startTrace(const char* hops_csv);

  // ---------- private channel "Home" <-> Home Assistant (HomeChannel.cpp) ----------
  typedef void (*HomeChatFn)(const char* sender, const char* text, int8_t snr4, uint8_t hops);
  void setHomeChatHandler(HomeChatFn fn) { _home_chat_fn = fn; }
  void beginHome() { loadHome(); }
  bool homeChannelSet() const { return _home_set; }
  const char* homeChannelName() const { return _home_name; }
  bool sendHomeChat(const char* text);
  // long text: split into packets " [1/3]" ... and send them one by one, 12 s apart
  bool queueHomeChat(const char* text);
  int homeQueued() const { return _home_parts_n - _home_parts_i; }
  bool handleHomeCommand(const char* command, char* reply, size_t reply_len);
  uint32_t home_rx = 0, home_tx = 0;

protected:
  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void logTxFail(mesh::Packet* pkt, int len) override;
  bool allowPacketForward(const mesh::Packet* packet) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                    const uint8_t* app_data, size_t app_data_len) override;
  void onControlDataRecv(mesh::Packet* packet) override;
  void onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                   const uint8_t* path_snrs, const uint8_t* path_hashes, uint8_t path_len) override;
  int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override;
  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) override;

private:
  void addLog(uint8_t dir, const mesh::Packet* pkt, int len, int8_t snr4, int16_t rssi);
  void notePacketRx(const mesh::Packet* pkt, int8_t snr4, int16_t rssi);
  void addToGraph(const mesh::Packet* pkt, int8_t snr4);
  int gChild(int parent, const uint8_t* key, int sz);
  void gEvict();
  NodeInfo* nodeFor(const uint8_t* key, int known);
  void saveBlocks();
  void loadBlocks();
  static bool keyMatch(const Key3& k, const uint8_t* p, int sz);

  PktEntry _log[LOG_SIZE];
  int _log_head = 0, _log_count = 0;
  uint32_t _log_seq = 0;
  uint32_t _last_minute_ms = 0, _last_nf_ms = 0;
  uint32_t _min_rx = 0, _min_tx = 0, _min_air_rx = 0, _min_air_tx = 0;
  int32_t _nf_sum = 0; int _nf_n = 0; int8_t _nf_lo = 0, _nf_hi = -128;
  int _minutes_in_ten = 0;

  void loadHome();
  void applyHomeKey(const uint8_t* key, int len);
  mesh::GroupChannel _home = {};
  bool _home_set = false;
  char _home_name[24] = {0};
  HomeChatFn _home_chat_fn = nullptr;
  static const int HOME_MAX_PARTS = 6;
  char _home_parts[HOME_MAX_PARTS][MAX_GROUP_DATA_LENGTH] = {};
  int _home_parts_n = 0, _home_parts_i = 0;
  uint32_t _home_next_ms = 0;
  void loopHome();
};
