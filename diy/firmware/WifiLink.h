#pragma once

#include <Arduino.h>

// Wi-Fi station link for the DIY repeater.
// Credentials live in NVS (namespace "diy"), never in source code.
// CLI (USB serial and web console):
//   wifi                 - status
//   wifi ssid <name>     - set SSID (spaces allowed)
//   wifi pwd <password>  - set password
//   wifi connect         - (re)connect with stored credentials
//   wifi off             - forget credentials and switch Wi-Fi off
//   wifi setup           - start the setup access point (first-run wizard)
//
// Setup access point: "<prefix>-XXXX" with a random 8-digit password shown on the OLED.
// Starts by itself when no Wi-Fi is configured, by triple click or by `wifi setup`;
// all DNS names point to the board (captive portal); stops after 15 min without clients.
class WifiLink {
public:
  void begin(const char* hostname, const char* ap_suffix);
  void loop();
  bool isConnected() const;
  String ipString() const;
  int rssi() const;
  const char* ssid() const { return _ssid; }

  void startSetupAP();
  void stopSetupAP();
  bool setupAPActive() const { return _ap; }
  const char* apSsid() const { return _ap_ssid; }
  const char* apPass() const { return _ap_pass; }
  String apIpString() const;
  // async scan for the wizard: returns -1 while scanning, otherwise the number of networks
  int scanNetworks(bool restart);


  // Returns true if the command was a wifi command (reply filled).
  bool handleCommand(const char* command, char* reply, size_t reply_len);

private:
  void load();
  void save();
  void connect();

  char _ssid[33] = {0};
  char _pwd[65] = {0};
  char _hostname[32] = {0};
  bool _enabled = false;
  uint32_t _last_attempt = 0;
  bool _ap = false;
  uint32_t _ap_last_client = 0;
  char _ap_suffix[8] = {0};
  char _ap_ssid[33] = {0};
  char _ap_pass[9] = {0};
};
