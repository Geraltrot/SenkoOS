#include "MqttLink.h"
#include "DiyMesh.h"
#include "WifiLink.h"
#include "WebPanel.h"      // diyResetReason()
#include "Updater.h"
#include "DiySensors.h"

#include <WiFi.h>
#include <Preferences.h>
#include <mqtt_client.h>   // esp-mqtt from ESP-IDF
#include <diy_version.h>

static portMUX_TYPE s_qmux = portMUX_INITIALIZER_UNLOCKED;

#ifndef DIY_BOARD_NAME
  #define DIY_BOARD_NAME "DIY board"
#endif

static const char* NVS_NS = "diy";

// ---------------------------------------------------------------------------
//  settings
// ---------------------------------------------------------------------------
void MqttLink::load() {
  Preferences p;
  if (p.begin(NVS_NS, true)) {
    _on = p.getUChar("mq_on", 0) != 0;
    p.getString("mq_host", _host, sizeof(_host));
    _port = p.getUShort("mq_port", 1883);
    p.getString("mq_user", _user, sizeof(_user));
    p.getString("mq_pwd", _pwd, sizeof(_pwd));
    _interval = p.getUShort("mq_int", 60);
    p.end();
  }
  if (_interval < 30 || _interval > 3600) _interval = 60;
  if (_port == 0) _port = 1883;
}

void MqttLink::save() {
  Preferences p;
  if (p.begin(NVS_NS, false)) {
    p.putUChar("mq_on", _on ? 1 : 0);
    p.putString("mq_host", _host);
    p.putUShort("mq_port", _port);
    p.putString("mq_user", _user);
    p.putString("mq_pwd", _pwd);
    p.putUShort("mq_int", _interval);
    p.end();
  }
}

void MqttLink::begin(DiyMesh* mesh, WifiLink* wifi, const uint8_t* pub_key) {
  _mesh = mesh;
  _wifi = wifi;
  snprintf(_id, sizeof(_id), "%02x%02x%02x%02x", pub_key[0], pub_key[1], pub_key[2], pub_key[3]);
  snprintf(_topic_base, sizeof(_topic_base), "senkophone/%s", _id);
  load();
}

const char* MqttLink::stateText() const {
  if (!_on) return "off";
  if (!_host[0]) return "no broker";
  if (!_wifi || !_wifi->isConnected()) return "no wifi";
  return _connected ? "connected" : "connecting";
}

// ---------------------------------------------------------------------------
//  client
// ---------------------------------------------------------------------------
void MqttLink::onEvent(void* arg, const char* base, int32_t id, void* data) {
  MqttLink* self = (MqttLink*)arg;
  if (id == MQTT_EVENT_CONNECTED) {
    self->_connected = true;
    self->_need_discovery = true;   // done from loop(): it reads mesh data
    self->_need_subscribe = true;
  } else if (id == MQTT_EVENT_DATA) {
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    char want[48];
    snprintf(want, sizeof(want), "%s/update/set", self->_topic_base);
    if (e->topic_len == (int)strlen(want) && memcmp(e->topic, want, e->topic_len) == 0) {
      if (e->data_len == 7 && memcmp(e->data, "install", 7) == 0) self->_want_install = true;
      return;
    }
    snprintf(want, sizeof(want), "%s/chat/out", self->_topic_base);
    if (e->topic_len != (int)strlen(want) || memcmp(e->topic, want, e->topic_len) != 0) return;
    if (e->data_len <= 0 || e->current_data_offset != 0) return;   // ignore empty and fragmented messages
    char msg[OUTQ_LEN];
    int n = e->data_len < (int)sizeof(msg) - 1 ? e->data_len : (int)sizeof(msg) - 1;
    while (n > 0 && n < e->data_len && ((uint8_t)e->data[n] & 0xC0) == 0x80) n--;   // do not cut a UTF-8 character
    memcpy(msg, e->data, n);
    msg[n] = 0;
    portENTER_CRITICAL(&s_qmux);
    if (self->_outq_n < OUTQ) {
      int slot = (self->_outq_head + self->_outq_n) % OUTQ;
      memcpy(self->_outq[slot], msg, n + 1);
      self->_outq_n++;
    }
    portEXIT_CRITICAL(&s_qmux);
  } else if (id == MQTT_EVENT_DISCONNECTED) {
    self->_connected = false;
  }
}

void MqttLink::start() {
  stop();
  if (!_on || !_host[0]) return;
  char uri[96], lwt[48], cid[32];
  snprintf(uri, sizeof(uri), "mqtt://%s:%u", _host, (unsigned)_port);
  snprintf(lwt, sizeof(lwt), "%s/status", _topic_base);
  snprintf(cid, sizeof(cid), "senkophone_%s", _id);

  esp_mqtt_client_config_t cfg = {};   // IDF 4.4 layout (Arduino-ESP32 2.x); strings are copied by init
  cfg.uri = uri;
  cfg.client_id = cid;
  if (_user[0]) cfg.username = _user;
  if (_pwd[0]) cfg.password = _pwd;
  cfg.lwt_topic = lwt;
  cfg.lwt_msg = "offline";
  cfg.lwt_qos = 1;
  cfg.lwt_retain = 1;
  cfg.keepalive = 60;
  cfg.buffer_size = 1024;

  esp_mqtt_client_handle_t c = esp_mqtt_client_init(&cfg);
  if (!c) return;
  esp_mqtt_client_register_event(c, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, onEvent, this);
  if (esp_mqtt_client_start(c) != ESP_OK) { esp_mqtt_client_destroy(c); return; }
  _client = c;
}

void MqttLink::stop() {
  if (!_client) return;
  esp_mqtt_client_handle_t c = (esp_mqtt_client_handle_t)_client;
  if (_connected) {   // say goodbye so HA shows "unavailable" at once
    char t[48];
    snprintf(t, sizeof(t), "%s/status", _topic_base);
    esp_mqtt_client_publish(c, t, "offline", 0, 1, 1);
  }
  esp_mqtt_client_stop(c);
  esp_mqtt_client_destroy(c);
  _client = nullptr;
  _connected = false;
}

// queued: the esp-mqtt task sends it, the mesh loop never waits for the network
bool MqttLink::publish(const char* topic, const char* payload, bool retain) {
  if (!_client || !_connected) return false;
  return esp_mqtt_client_enqueue((esp_mqtt_client_handle_t)_client, topic, payload, 0, retain ? 1 : 0, retain ? 1 : 0, true) >= 0;
}

void MqttLink::loop() {
  bool wifi_ok = _wifi && _wifi->isConnected();
  if (_on && _host[0] && wifi_ok && !_client) start();
  else if ((!_on || !wifi_ok) && _client) stop();
  if (!_connected) return;

  if (_need_subscribe) {
    _need_subscribe = false;
    char t[48];
    snprintf(t, sizeof(t), "%s/chat/out", _topic_base);
    esp_mqtt_client_subscribe((esp_mqtt_client_handle_t)_client, t, 1);
    snprintf(t, sizeof(t), "%s/update/set", _topic_base);
    esp_mqtt_client_subscribe((esp_mqtt_client_handle_t)_client, t, 1);
  }
  // HA "Install" on the update entity
  if (_want_install) {
    _want_install = false;
    if (_upd && _upd->available()[0]) _upd->requestInstall();
  }
  if (millis() - _upd_checked_ms > 2000) {   // follow the updater: available version, progress
    _upd_checked_ms = millis();
    publishUpdateState(false);
  }
  // HA -> channel "Home"
  while (_outq_n > 0 && (!_mesh || _mesh->homeQueued() == 0)) {   // keep order: wait until a long message is out
    char msg[OUTQ_LEN];
    portENTER_CRITICAL(&s_qmux);
    memcpy(msg, _outq[_outq_head], sizeof(msg));
    _outq_head = (_outq_head + 1) % OUTQ;
    _outq_n--;
    portEXIT_CRITICAL(&s_qmux);
    char* text = msg;
    if (msg[0] == '{') {   // {"text": "..."}
      char* k = strstr(msg, "\"text\"");
      char* q1 = k ? strchr(k + 6, '"') : nullptr;
      char* q2 = q1 ? strrchr(q1 + 1, '"') : nullptr;
      if (q1 && q2 && q2 > q1) { *q2 = 0; text = q1 + 1; }
    }
    if (_mesh && text[0]) _mesh->queueHomeChat(text);
  }
  if (_need_discovery) {
    _need_discovery = false;
    char t[48];
    snprintf(t, sizeof(t), "%s/status", _topic_base);
    publish(t, "online", true);
    publishDiscovery();
    publishUpdateState(true);
    _sens_known_n = 0;   // announce the sensors again with the next state
    _last_state = 0;
  }
  if (_last_state == 0 || millis() - _last_state >= (uint32_t)_interval * 1000UL) {
    _last_state = millis() | 1;
    publishState();
  }
}

// ---------------------------------------------------------------------------
//  Home Assistant discovery: one sensor per value of the state JSON
// ---------------------------------------------------------------------------
struct SensorDef {
  const char* key;        // field in the state JSON, part of unique_id
  const char* name;       // entity name (HA prefixes it with the device name)
  const char* unit;       // nullptr = none
  const char* dev_class;  // nullptr = none
  const char* state_class;
  bool diagnostic;
  const char* icon;
};

static const SensorDef SENSORS[] = {
  {"rx_total",  "Принято пакетов",        nullptr,   nullptr,           "total_increasing", false, "mdi:download"},
  {"tx_total",  "Передано пакетов",       nullptr,   nullptr,           "total_increasing", false, "mdi:upload"},
  {"rx_min",    "Принято за минуту",      "пак/мин", nullptr,           "measurement",      false, "mdi:download-network"},
  {"tx_min",    "Передано за минуту",     "пак/мин", nullptr,           "measurement",      false, "mdi:upload-network"},
  {"noise",     "Шум эфира",              "dBm",     "signal_strength", "measurement",      false, nullptr},
  {"last_snr",  "SNR последнего пакета",  "dB",      "signal_strength", "measurement",      false, nullptr},
  {"last_rssi", "RSSI последнего пакета", "dBm",     "signal_strength", "measurement",      false, nullptr},
  {"air_tx",    "Эфир TX за 24 ч",        "%",       nullptr,           "measurement",      false, "mdi:radio-tower"},
  {"air_rx",    "Эфир RX за 24 ч",        "%",       nullptr,           "measurement",      false, "mdi:radio-tower"},
  {"nodes",     "Узлов в сети",           nullptr,   nullptr,           "measurement",      false, "mdi:access-point-network"},
  {"direct",    "Соседей напрямую",       nullptr,   nullptr,           "measurement",      false, "mdi:access-point"},
  {"blocked",   "Заблокировано пакетов",  nullptr,   nullptr,           "total_increasing", false, "mdi:block-helper"},
  {"uptime",    "Аптайм",                 "s",       "duration",        "measurement",      true,  nullptr},
  {"wifi_rssi", "Сигнал Wi-Fi",           "dBm",     "signal_strength", "measurement",      true,  nullptr},
  {"heap",      "Свободная память",       "kB",      "data_size",       "measurement",      true,  nullptr},
  {"reset",     "Причина перезагрузки",   nullptr,   nullptr,           nullptr,            true,  "mdi:restart"},
};

// JSON string: escape quotes and backslashes (names come from user settings)
static void jstr(char* out, size_t n, const char* s) {
  size_t o = 0;
  if (o < n - 1) out[o++] = '"';
  for (; *s && o < n - 3; s++) {
    if (*s == '"' || *s == '\\') out[o++] = '\\';
    out[o++] = *s;
  }
  out[o++] = '"';
  out[o] = 0;
}

void MqttLink::publishDiscovery() {
  char dev_name[72], ip[24], dev[256];
  jstr(dev_name, sizeof(dev_name), _mesh ? _mesh->getNodePrefs()->node_name : "SenkoPhone");
  snprintf(ip, sizeof(ip), "%s", _wifi ? _wifi->ipString().c_str() : "");
  snprintf(dev, sizeof(dev),
           "\"dev\":{\"ids\":[\"senkophone_%s\"],\"name\":%s,\"mdl\":\"%s\",\"mf\":\"DIY / MeshCore\",\"sw\":\"%s\"%s%s%s}",
           _id, dev_name, DIY_BOARD_NAME, DIY_VERSION, ip[0] ? ",\"cu\":\"http://" : "", ip, ip[0] ? "/\"" : "");

  char topic[96], payload[640], extra[160];
  for (const SensorDef& s : SENSORS) {
    extra[0] = 0;
    size_t o = 0;
    if (s.unit) o += snprintf(extra + o, sizeof(extra) - o, ",\"unit_of_meas\":\"%s\"", s.unit);
    if (s.dev_class) o += snprintf(extra + o, sizeof(extra) - o, ",\"dev_cla\":\"%s\"", s.dev_class);
    if (s.state_class) o += snprintf(extra + o, sizeof(extra) - o, ",\"stat_cla\":\"%s\"", s.state_class);
    if (s.diagnostic) o += snprintf(extra + o, sizeof(extra) - o, ",\"ent_cat\":\"diagnostic\"");
    if (s.icon) o += snprintf(extra + o, sizeof(extra) - o, ",\"ic\":\"%s\"", s.icon);
    if (s.unit && (!strcmp(s.unit, "s") || !strcmp(s.unit, "kB") || !strcmp(s.unit, "dBm")))   // whole numbers
      o += snprintf(extra + o, sizeof(extra) - o, ",\"sug_dsp_prc\":0");
    snprintf(topic, sizeof(topic), "homeassistant/sensor/senkophone_%s_%s/config", _id, s.key);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"senkophone_%s_%s\",\"stat_t\":\"%s/state\",\"val_tpl\":\"{{ value_json.%s }}\","
             "\"avty_t\":\"%s/status\"%s,%s}",
             s.name, _id, s.key, _topic_base, s.key, _topic_base, extra, dev);
    publish(topic, payload, true);
  }

  // firmware update entity
  if (_upd) {
    snprintf(topic, sizeof(topic), "homeassistant/update/senkophone_%s_fw/config", _id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Прошивка\",\"uniq_id\":\"senkophone_%s_fw\",\"stat_t\":\"%s/update\",\"cmd_t\":\"%s/update/set\","
             "\"pl_inst\":\"install\",\"dev_cla\":\"firmware\",\"avty_t\":\"%s/status\",%s}",
             _id, _topic_base, _topic_base, _topic_base, dev);
    publish(topic, payload, true);
  }

  // channel "Home": last message as a sensor, and a text entity that sends to the channel;
  // an empty retained config removes them from HA when the channel is switched off
  if (!_mesh || !_mesh->homeChannelSet()) {
    snprintf(topic, sizeof(topic), "homeassistant/sensor/senkophone_%s_chat/config", _id);
    publish(topic, "", true);
    snprintf(topic, sizeof(topic), "homeassistant/text/senkophone_%s_chat_out/config", _id);
    publish(topic, "", true);
  } else {
    snprintf(topic, sizeof(topic), "homeassistant/sensor/senkophone_%s_chat/config", _id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Канал Дом: сообщение\",\"uniq_id\":\"senkophone_%s_chat\",\"stat_t\":\"%s/chat/in\","
             "\"val_tpl\":\"{{ value_json.from }}: {{ value_json.text }}\",\"json_attr_t\":\"%s/chat/in\",\"ic\":\"mdi:message-text\","
             "\"avty_t\":\"%s/status\",%s}", _id, _topic_base, _topic_base, _topic_base, dev);
    publish(topic, payload, true);
    snprintf(topic, sizeof(topic), "homeassistant/text/senkophone_%s_chat_out/config", _id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Канал Дом: отправить\",\"uniq_id\":\"senkophone_%s_chat_out\",\"cmd_t\":\"%s/chat/out\",\"max\":120,"
             "\"ic\":\"mdi:send\",\"avty_t\":\"%s/status\",%s}", _id, _topic_base, _topic_base, dev);
    publish(topic, payload, true);
  }
}

// firmware update entity (HA "update" platform): installed vs. latest from the updater
void MqttLink::publishUpdateState(bool force) {
  if (!_connected || !_upd) return;
  const char* avail = _upd->available();
  bool busy = strcmp(_upd->stateText(), "downloading") == 0 || strcmp(_upd->stateText(), "rebooting") == 0;
  char sig[64];
  snprintf(sig, sizeof(sig), "%s|%d|%d", avail, busy ? 1 : 0, busy ? _upd->progress() / 10 : 0);
  if (!force && strcmp(sig, _upd_sent) == 0) return;
  strlcpy(_upd_sent, sig, sizeof(_upd_sent));
  char topic[48], payload[320];
  snprintf(topic, sizeof(topic), "%s/update", _topic_base);
  snprintf(payload, sizeof(payload),
           "{\"installed_version\":\"%s\",\"latest_version\":\"%s\",\"title\":\"SenkoOS\","
           "\"release_url\":\"https://github.com/Geraltrot/SenkoOS/releases\",\"in_progress\":%s,\"update_percentage\":%s}",
           DIY_VERSION, avail[0] ? avail : DIY_VERSION, busy ? "true" : "false",
           busy ? String(_upd->progress()).c_str() : "null");
  publish(topic, payload, true);
}

// JSON-escaped copy (quotes, backslashes, control characters)
static void jesc(char* out, size_t n, const char* s) {
  size_t o = 0;
  for (; *s && o + 7 < n; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
    else if (c < 0x20) o += snprintf(out + o, n - o, "\\u%04x", c);
    else out[o++] = c;
  }
  out[o] = 0;
}

void MqttLink::publishChatIn(const char* sender, const char* text, int8_t snr4, uint8_t hops) {
  if (!_connected) return;
  char from[64], msg[340], topic[48], payload[480];
  jesc(from, sizeof(from), sender);
  jesc(msg, sizeof(msg), text);
  snprintf(topic, sizeof(topic), "%s/chat/in", _topic_base);
  snprintf(payload, sizeof(payload), "{\"from\":\"%s\",\"text\":\"%s\",\"snr\":%.2f,\"hops\":%u}", from, msg, snr4 / 4.0f, hops);
  publish(topic, payload, false);
}

// I2C sensors: one HA sensor per value, announced when it first appears
void MqttLink::publishSensors() {
  DiySensorValue v[MAX_SENS];
  int n = diyReadSensors(v, MAX_SENS);
  if (n <= 0) return;
  char topic[96], payload[640], dev[256], dev_name[72], ip[24];
  // device block, same as in publishDiscovery()
  const char* nm = _mesh ? _mesh->getNodePrefs()->node_name : "SenkoPhone";
  snprintf(dev_name, sizeof(dev_name), "\"%s\"", nm);
  snprintf(ip, sizeof(ip), "%s", _wifi ? _wifi->ipString().c_str() : "");
  snprintf(dev, sizeof(dev), "\"dev\":{\"ids\":[\"senkophone_%s\"],\"name\":%s,\"mdl\":\"%s\",\"mf\":\"DIY / MeshCore\",\"sw\":\"%s\"}",
           _id, dev_name, DIY_BOARD_NAME, DIY_VERSION);
  for (int i = 0; i < n; i++) {
    bool known = false;
    for (int k = 0; k < _sens_known_n; k++) if (strcmp(_sens_known[k], v[i].key) == 0) { known = true; break; }
    if (known || _sens_known_n >= MAX_SENS) continue;
    char extra[160] = "";
    size_t o = 0;
    if (v[i].unit) o += snprintf(extra + o, sizeof(extra) - o, ",\"unit_of_meas\":\"%s\"", v[i].unit);
    if (v[i].dev_class) o += snprintf(extra + o, sizeof(extra) - o, ",\"dev_cla\":\"%s\"", v[i].dev_class);
    snprintf(topic, sizeof(topic), "homeassistant/sensor/senkophone_%s_%s/config", _id, v[i].key);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Датчик %u: %s\",\"uniq_id\":\"senkophone_%s_%s\",\"stat_t\":\"%s/sensors\","
             "\"val_tpl\":\"{{ value_json.%s }}\",\"stat_cla\":\"measurement\",\"avty_t\":\"%s/status\"%s,%s}",
             v[i].channel, v[i].name, _id, v[i].key, _topic_base, v[i].key, _topic_base, extra, dev);
    publish(topic, payload, true);
    strlcpy(_sens_known[_sens_known_n++], v[i].key, sizeof(_sens_known[0]));
  }
  char state[400];
  size_t o = 0;
  o += snprintf(state + o, sizeof(state) - o, "{");
  for (int i = 0; i < n && o < sizeof(state) - 32; i++)
    o += snprintf(state + o, sizeof(state) - o, "%s\"%s\":%.3f", i ? "," : "", v[i].key, v[i].value);
  snprintf(state + o, sizeof(state) - o, "}");
  snprintf(topic, sizeof(topic), "%s/sensors", _topic_base);
  publish(topic, state, true);
}

void MqttLink::publishState() {
  if (!_mesh) return;
  publishSensors();
  // last completed minute
  const DiyMesh::Bucket& m = _mesh->minutes[_mesh->minute_head];
  // airtime over the last 24 h
  uint32_t rx24 = 0, tx24 = 0; int n24 = 0;
  for (int i = 0; i < DiyMesh::TEN_BUCKETS; i++) {
    const DiyMesh::Bucket& b = _mesh->tens[i];
    if (b.valid) { rx24 += b.rx_air_ms; tx24 += b.tx_air_ms; n24++; }
  }
  float span = n24 ? n24 * 600000.0f : 1.0f;
  int direct = 0;
  for (int i = 0; i < _mesh->num_nodes; i++) if (_mesh->nodes[i].seen & DiyMesh::SEEN_DIRECT) direct++;

  char last[64] = "\"last_snr\":null,\"last_rssi\":null";
  if (const DiyMesh::PktEntry* e = _mesh->lastRx()) snprintf(last, sizeof(last), "\"last_snr\":%.2f,\"last_rssi\":%d", e->snr4 / 4.0f, e->rssi);

  char topic[48], payload[512];
  snprintf(topic, sizeof(topic), "%s/state", _topic_base);
  snprintf(payload, sizeof(payload),
           "{\"rx_total\":%lu,\"tx_total\":%lu,\"rx_min\":%u,\"tx_min\":%u,\"noise\":%d,%s,"
           "\"air_tx\":%s,\"air_rx\":%s,\"nodes\":%d,\"direct\":%d,\"blocked\":%lu,"
           "\"uptime\":%lu,\"wifi_rssi\":%d,\"heap\":%lu,\"reset\":\"%s\"}",
           (unsigned long)(_mesh->rx_flood + _mesh->rx_direct), (unsigned long)(_mesh->tx_flood + _mesh->tx_direct),
           m.valid ? m.rx : 0, m.valid ? m.tx : 0, _mesh->noiseFloor(), last,
           n24 ? String(tx24 * 100.0f / span, 2).c_str() : "null", n24 ? String(rx24 * 100.0f / span, 2).c_str() : "null",
           _mesh->num_nodes, direct, (unsigned long)_mesh->blocked_count,
           (unsigned long)(millis() / 1000), _wifi ? _wifi->rssi() : 0, (unsigned long)(ESP.getFreeHeap() / 1024), diyResetReason());
  publish(topic, payload, true);
}

// ---------------------------------------------------------------------------
//  CLI (local only: USB and the web console)
// ---------------------------------------------------------------------------
bool MqttLink::handleCommand(const char* command, char* reply, size_t reply_len) {
  if (strncmp(command, "mqtt", 4) != 0 || (command[4] != 0 && command[4] != ' ')) return false;
  const char* arg = command + 4;
  while (*arg == ' ') arg++;
  bool restart = false;

  if (*arg == 0) {
    snprintf(reply, reply_len, "mqtt: %s, broker %s:%u, user '%s', password %s, every %u s, topic %s",
             stateText(), _host[0] ? _host : "-", (unsigned)_port, _user, _pwd[0] ? "set" : "empty", (unsigned)_interval, _topic_base);
    return true;
  } else if (strncmp(arg, "host ", 5) == 0) {
    strlcpy(_host, arg + 5, sizeof(_host));
    snprintf(reply, reply_len, "OK - broker %s", _host);
    restart = true;
  } else if (strncmp(arg, "port ", 5) == 0) {
    long v = atol(arg + 5);
    if (v < 1 || v > 65535) { snprintf(reply, reply_len, "Err - port 1..65535"); return true; }
    _port = (uint16_t)v;
    snprintf(reply, reply_len, "OK - port %u", (unsigned)_port);
    restart = true;
  } else if (strncmp(arg, "user ", 5) == 0 || strcmp(arg, "user") == 0) {
    strlcpy(_user, arg[4] ? arg + 5 : "", sizeof(_user));
    snprintf(reply, reply_len, "OK - user '%s'", _user);
    restart = true;
  } else if (strncmp(arg, "pwd ", 4) == 0 || strcmp(arg, "pwd") == 0) {
    strlcpy(_pwd, arg[3] ? arg + 4 : "", sizeof(_pwd));
    snprintf(reply, reply_len, "OK - password %s", _pwd[0] ? "saved" : "cleared");   // never echoed
    restart = true;
  } else if (strncmp(arg, "interval ", 9) == 0) {
    long v = atol(arg + 9);
    if (v < 30 || v > 3600) { snprintf(reply, reply_len, "Err - interval 30..3600 s"); return true; }
    _interval = (uint16_t)v;
    snprintf(reply, reply_len, "OK - state every %u s", (unsigned)_interval);
  } else if (strcmp(arg, "on") == 0) {
    _on = true;
    snprintf(reply, reply_len, _host[0] ? "OK - mqtt on" : "OK - mqtt on, now set: mqtt host <broker>");
    restart = true;
  } else if (strcmp(arg, "off") == 0) {
    _on = false;
    snprintf(reply, reply_len, "OK - mqtt off");
    restart = true;
  } else if (strcmp(arg, "discovery") == 0) {
    _need_discovery = _connected;
    snprintf(reply, reply_len, _connected ? "OK - discovery will be sent" : "Err - not connected");
    return true;
  } else {
    snprintf(reply, reply_len, "Err - mqtt [host <h>|port <n>|user <u>|pwd <p>|interval <s>|on|off|discovery]");
    return true;
  }
  save();
  if (restart) stop();   // loop() starts it again with the new settings
  return true;
}
