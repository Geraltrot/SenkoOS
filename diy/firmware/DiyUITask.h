#pragma once

#include <helpers/ui/DisplayDriver.h>
#include <helpers/CommonCLI.h>

class DiyMesh;
class WifiLink;
class MqttLink;
class Updater;

// OLED screen for the DIY repeater. Based on upstream examples/simple_repeater/UITask
// (same boot screen, button and auto-off behaviour); the main screen also shows
// Wi-Fi state / IP address, packet counters, noise floor, uptime and our version.
class DiyUITask {
  mesh::MainBoard* _board;
  DisplayDriver* _display;
  DiyMesh* _mesh = nullptr;
  WifiLink* _wifi = nullptr;
  MqttLink* _mqtt = nullptr;
  Updater* _upd = nullptr;
  unsigned long _next_refresh = 0, _auto_off = 0;
  NodePrefs* _node_prefs = nullptr;
  char _version_info[40];
  char _diy_short[24];
  unsigned long _powering_off_at = 0;
  unsigned long _started_at = 0;

  uint8_t _page = 0;
  // 128x128 (SenkoPhone): main, radio, system. 128x64 (Heltec): the system page is split in two.
  bool _small = false;          // display height < 100
  uint8_t _pages = 3;

  void renderCurrScreen();
  void renderMain(char* tmp, size_t sz);
  void renderRadio(char* tmp, size_t sz);
  void renderSystem(char* tmp, size_t sz);
  void renderSmallMain(char* tmp, size_t sz);
  void renderSmallRadio(char* tmp, size_t sz);
  void renderSmallSystem(char* tmp, size_t sz);
  void renderSmallInfo(char* tmp, size_t sz);
  int lastRxText(char* tmp, size_t sz);
  void drawHeader(const char* title);
  void drawWifiBars(int x, int y);
  void drawPageDots();
public:
  DiyUITask(mesh::MainBoard& board, DisplayDriver& display) : _board(&board), _display(&display) {}
  void begin(NodePrefs* node_prefs, DiyMesh* mesh, WifiLink* wifi, const char* diy_version, const char* fw_version);
  void setMqtt(MqttLink* m) { _mqtt = m; }
  void setUpdater(Updater* u) { _upd = u; }
  void wake();   // turn the screen on for a while (e.g. when Wi-Fi gets an address)
  void loop();
};
