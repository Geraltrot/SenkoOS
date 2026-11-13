#pragma once

#include <Arduino.h>

class DiyMesh;

// short reason of the last reset: power, pin, software, panic, watchdog, deepsleep, brownout, other
const char* diyResetReason();
class WifiLink;
class MqttLink;
class Updater;

// Local web panel (plain HTTP, home LAN only).
// Runs a synchronous WebServer polled from loop(): every handler executes in the
// same task as the mesh, so it may call DiyMesh directly without locking.
//
// Login: dedicated web password (NVS) if set, otherwise the repeater admin
// password. The session lives in an HttpOnly cookie.
class WebPanel {
public:
  typedef void (*CommandFn)(char* command, char* reply, size_t reply_len);

  void begin(DiyMesh* mesh, WifiLink* wifi, CommandFn run_command);
  void loop();
  void setMqtt(MqttLink* mqtt) { _mqtt = mqtt; }
  void setUpdater(Updater* u) { _updater = u; }

private:
  static const int MAX_SESSIONS = 4;
  struct Session {
    char token[33];
    uint32_t last_used;
  };

  bool authorized();
  bool requireAuth();
  const char* loginPassword();
  void dropSessions();

  void handlePage();
  void handleLogin();
  void handleLogout();
  void handleStatus();
  void handleStats();
  void handleLog();
  void handleAirtime();
  void handleChannels();
  void handleChannelAction();
  void handleNodes();
  void handleNodeAction();
  void handleGraph();
  void handleTrace();
  void handleTraceStart();
  void handlePrefs();
  void handleCli();
  void handleWebPass();
  void handleReboot();
  void handleUpdateToken();
  // first-run wizard (setup access point)
  bool viaSetupAP();
  bool factoryPassword();
  bool setupAllowed(bool ap);
  void handleSetupState();
  void handleSetupScan();
  void handleSetupSave();
  void handleNotFound();
  void handleUpdateState();
  void sendOk(const char* msg = "OK");
  void sendError(int code, const char* msg);

  DiyMesh* _mesh = nullptr;
  WifiLink* _wifi = nullptr;
  MqttLink* _mqtt = nullptr;
  Updater* _updater = nullptr;
  CommandFn _run_command = nullptr;
  bool _started = false;
  Session _sessions[MAX_SESSIONS] = {};
  uint32_t _last_failed_login = 0;
  uint8_t _failed_logins = 0;
  char _web_pwd[33] = {0};
  uint32_t _reboot_at = 0;
  // wizard: Wi-Fi is switched a moment after the reply, so the phone still gets it over the AP
  uint32_t _setup_apply_at = 0;
  bool _setup_reboot = false;
  char _setup_ssid[33] = {0};
  char _setup_wpwd[65] = {0};
};
