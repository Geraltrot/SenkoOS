#include "WifiLink.h"

#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <esp_system.h>

#ifndef DIY_AP_PREFIX
  #define DIY_AP_PREFIX "SenkoPhone-Setup"
#endif

static const char* NVS_NS = "diy";
static const uint32_t RETRY_MS = 30000;
static const uint32_t AP_IDLE_MS = 15UL * 60UL * 1000UL;   // setup AP stops after 15 min without clients
static DNSServer s_dns;

void WifiLink::load() {
  Preferences p;
  if (p.begin(NVS_NS, true)) {
    p.getString("ssid", _ssid, sizeof(_ssid));
    p.getString("pwd", _pwd, sizeof(_pwd));
    p.end();
  }
}

void WifiLink::save() {
  Preferences p;
  if (p.begin(NVS_NS, false)) {
    p.putString("ssid", _ssid);
    p.putString("pwd", _pwd);
    p.end();
  }
}

void WifiLink::begin(const char* hostname, const char* ap_suffix) {
  strlcpy(_ap_suffix, ap_suffix ? ap_suffix : "", sizeof(_ap_suffix));
  // hostname: letters, digits and '-' only
  size_t j = 0;
  for (size_t i = 0; hostname[i] && j < sizeof(_hostname) - 1; i++) {
    char c = hostname[i];
    if (isalnum((unsigned char)c)) _hostname[j++] = tolower(c);
    else if (j > 0 && _hostname[j - 1] != '-') _hostname[j++] = '-';
  }
  while (j > 0 && _hostname[j - 1] == '-') j--;
  _hostname[j] = 0;
  if (j == 0) strcpy(_hostname, "meshcore-repeater");

  load();
  _enabled = _ssid[0] != 0;
  if (_enabled) connect();
  else startSetupAP();   // fresh board: open the first-run wizard
}

void WifiLink::connect() {
  WiFi.persistent(false);         // credentials are kept by us, not by the SDK
  WiFi.mode(_ap ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(_hostname);
  WiFi.setAutoReconnect(true);
  WiFi.begin(_ssid, _pwd);
  _last_attempt = millis();
}

void WifiLink::loop() {
  if (_ap) {
    s_dns.processNextRequest();
    if (WiFi.softAPgetStationNum() > 0) _ap_last_client = millis();
    else if (millis() - _ap_last_client > AP_IDLE_MS) stopSetupAP();
    return;   // no station retries while the AP is up: a retry would move the AP to another channel
  }
  if (!_enabled) return;
  if (WiFi.status() != WL_CONNECTED && millis() - _last_attempt > RETRY_MS) {
    WiFi.disconnect();
    connect();
  }
}

void WifiLink::startSetupAP() {
  _ap_last_client = millis();
  if (_ap) return;
  snprintf(_ap_ssid, sizeof(_ap_ssid), "%s-%s", DIY_AP_PREFIX, _ap_suffix);
  for (int i = 0; i < 8; i++) _ap_pass[i] = '0' + (esp_random() % 10);
  _ap_pass[8] = 0;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);   // STA part is needed for scanning (and keeps a home link if there is one)
  WiFi.softAP(_ap_ssid, _ap_pass);
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(53, "*", WiFi.softAPIP());
  _ap = true;
  Serial.printf("Setup Wi-Fi: %s  password %s  open http://%s/\n", _ap_ssid, _ap_pass, WiFi.softAPIP().toString().c_str());
}

void WifiLink::stopSetupAP() {
  if (!_ap) return;
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  _ap = false;
  if (_enabled) connect();
  else WiFi.mode(WIFI_OFF);
}

String WifiLink::apIpString() const { return _ap ? WiFi.softAPIP().toString() : String(""); }

int WifiLink::scanNetworks(bool restart) {
  int n = WiFi.scanComplete();
  if (restart || n == WIFI_SCAN_FAILED) {
    if (n != WIFI_SCAN_RUNNING) { WiFi.scanDelete(); WiFi.scanNetworks(true, false); }
    return -1;
  }
  return n == WIFI_SCAN_RUNNING ? -1 : n;
}

bool WifiLink::isConnected() const { return _enabled && WiFi.status() == WL_CONNECTED; }
String WifiLink::ipString() const { return isConnected() ? WiFi.localIP().toString() : String(""); }
int WifiLink::rssi() const { return isConnected() ? WiFi.RSSI() : 0; }

bool WifiLink::handleCommand(const char* command, char* reply, size_t reply_len) {
  if (strncmp(command, "wifi", 4) != 0 || (command[4] != 0 && command[4] != ' ')) return false;
  const char* arg = command + 4;
  while (*arg == ' ') arg++;

  if (*arg == 0) {
    if (_ap) snprintf(reply, reply_len, "wifi: setup access point '%s' is on (password %s, http://%s/)%s", _ap_ssid, _ap_pass,
                      WiFi.softAPIP().toString().c_str(), _ssid[0] ? "; home network saved" : "");
    else if (!_enabled && _ssid[0]) snprintf(reply, reply_len, "wifi: '%s' saved, not connected (wifi connect)", _ssid);
    else if (!_enabled) snprintf(reply, reply_len, "wifi: off (set: wifi ssid <name>, wifi pwd <pwd>)");
    else if (isConnected()) snprintf(reply, reply_len, "wifi: connected to '%s', ip %s, rssi %d dBm, host %s",
                                     _ssid, WiFi.localIP().toString().c_str(), WiFi.RSSI(), _hostname);
    else snprintf(reply, reply_len, "wifi: connecting to '%s' (status %d)", _ssid, (int)WiFi.status());
  } else if (strncmp(arg, "ssid ", 5) == 0) {
    strncpy(_ssid, arg + 5, sizeof(_ssid) - 1);
    _ssid[sizeof(_ssid) - 1] = 0;
    save();
    _enabled = true;
    WiFi.disconnect();
    connect();
    snprintf(reply, reply_len, "OK - ssid saved, connecting (set the password with: wifi pwd <pwd>)");
  } else if (strncmp(arg, "pwd ", 4) == 0) {
    strncpy(_pwd, arg + 4, sizeof(_pwd) - 1);
    _pwd[sizeof(_pwd) - 1] = 0;
    save();
    if (_ssid[0]) {
      _enabled = true;
      WiFi.disconnect();
      connect();
      snprintf(reply, reply_len, "OK - password saved, connecting to '%s'", _ssid);
    } else {
      snprintf(reply, reply_len, "OK - password saved, now: wifi ssid <name>");
    }
  } else if (strcmp(arg, "connect") == 0) {
    if (_ssid[0] == 0) {
      snprintf(reply, reply_len, "Err - set ssid first");
    } else {
      _enabled = true;
      WiFi.disconnect();
      connect();
      snprintf(reply, reply_len, "OK - connecting to '%s'", _ssid);
    }
  } else if (strcmp(arg, "setup") == 0) {
    startSetupAP();
    snprintf(reply, reply_len, "OK - setup Wi-Fi '%s', password %s, open http://%s/", _ap_ssid, _ap_pass, WiFi.softAPIP().toString().c_str());
  } else if (strcmp(arg, "off") == 0) {
    stopSetupAP();
    _ssid[0] = 0;
    _pwd[0] = 0;
    save();
    _enabled = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    snprintf(reply, reply_len, "OK - wifi off, credentials erased");
  } else {
    snprintf(reply, reply_len, "Err - wifi [ssid <name>|pwd <pwd>|connect|setup|off]");
  }
  return true;
}
