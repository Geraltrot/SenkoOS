#pragma once

#include <Arduino.h>

class DiyMesh;
class WifiLink;
class Updater;

// MQTT link to Home Assistant (Mosquitto) with MQTT discovery.
// Uses the esp-mqtt client from ESP-IDF (own task); everything that reads DiyMesh
// runs from loop(), in the mesh task.
//
// Settings live in NVS (namespace "diy"), never in source code. CLI (USB and web console only):
//   mqtt                    - status
//   mqtt host <name|ip>     - broker address
//   mqtt port <n>           - broker port (1883)
//   mqtt user <name>        - broker login
//   mqtt pwd <password>     - broker password (never echoed)
//   mqtt interval <s>       - how often the state is sent, 30..3600 s (60)
//   mqtt on | off           - enable / disable
//   mqtt discovery          - send the Home Assistant discovery configs again
//
// Topics (id = first 4 bytes of the node key, hex):
//   senkophone/<id>/status  online / offline (LWT, retained)
//   senkophone/<id>/state   JSON with all values (retained)
//   homeassistant/sensor/senkophone_<id>/<key>/config  discovery (retained)
//   senkophone/<id>/chat/in   message from the private channel "Home" (JSON: from, text, snr, hops)
//   senkophone/<id>/chat/out  text to send to the channel (plain text or {"text": "..."})
//   senkophone/<id>/update      firmware state for the HA update entity (installed / latest / progress)
//   senkophone/<id>/update/set  "install" from HA starts the update found by the last check
//   senkophone/<id>/sensors     I2C sensor values (JSON, keys s<channel>_<lpp type>); a HA sensor per value
class MqttLink {
public:
  void begin(DiyMesh* mesh, WifiLink* wifi, const uint8_t* pub_key);
  void loop();
  bool handleCommand(const char* command, char* reply, size_t reply_len);

  bool enabled() const { return _on; }
  bool connected() const { return _connected; }
  const char* host() const { return _host; }
  uint16_t port() const { return _port; }
  const char* user() const { return _user; }
  uint16_t interval() const { return _interval; }
  const char* stateText() const;   // "off", "no wifi", "connecting", "connected"
  // channel "Home" -> HA (called from the mesh loop)
  void publishChatIn(const char* sender, const char* text, int8_t snr4, uint8_t hops);
  // re-send discovery (e.g. the channel "Home" was switched on or off)
  void requestDiscovery() { if (_connected) _need_discovery = true; }
  void setUpdater(Updater* u) { _upd = u; }
  // watched neighbour changed (or was removed): HA binary_sensor "<name>: на связи"
  void publishWatch(const char* key_hex, const char* name, bool online, bool removed);

private:
  void load();
  void save();
  void start();
  void stop();
  void publishDiscovery();
  void publishState();
  bool publish(const char* topic, const char* payload, bool retain);

  static void onEvent(void* arg, const char* base, int32_t id, void* data);

  DiyMesh* _mesh = nullptr;
  WifiLink* _wifi = nullptr;
  void* _client = nullptr;              // esp_mqtt_client_handle_t
  volatile bool _connected = false;
  volatile bool _need_discovery = false;
  bool _on = false;
  char _host[64] = {0};
  uint16_t _port = 1883;
  char _user[33] = {0};
  char _pwd[65] = {0};
  uint16_t _interval = 60;
  char _id[9] = {0};                    // "1a2b3c4d"
  char _topic_base[32] = {0};           // "senkophone/1a2b3c4d"
  uint32_t _last_state = 0;
  // HA -> channel "Home": filled by the esp-mqtt task, sent from loop()
  static const int OUTQ = 3;
  static const int OUTQ_LEN = 640;   // long texts are split into packets by DiyMesh::queueHomeChat
  char _outq[OUTQ][OUTQ_LEN] = {};
  volatile int _outq_head = 0, _outq_n = 0;
  volatile bool _need_subscribe = false;
  // firmware update entity
  Updater* _upd = nullptr;
  volatile bool _want_install = false;
  char _upd_sent[64] = {0};   // what was published last, to publish only on change
  uint32_t _upd_checked_ms = 0;
  void publishUpdateState(bool force);
  // I2C sensors: keys already announced to HA (cleared on every new discovery)
  static const int MAX_SENS = 16;
  char _sens_known[MAX_SENS][12] = {};
  int _sens_known_n = 0;
  void publishSensors();
  void publishAllWatch(bool discovery);
};
