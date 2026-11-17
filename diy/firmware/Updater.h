#pragma once

#include <Arduino.h>

class WifiLink;

// clock for the night window of the auto mode: epoch seconds, 0 = unknown
void setUpdaterClock(uint32_t (*now_epoch)());

// Firmware auto-update from published releases (GitHub).
//
// The board reads a small manifest next to the release files:
//   manifest.json  {"version":"v1.4.0-mc1.18.0-20261120","board":"e22wroom","channel":"release",
//                   "url":"https://.../v1.4.0-mc1.18.0-20261120-e22wroom.bin","size":1234567,"sha256":"<64 hex>"}
//   manifest.sig   Ed25519 signature of manifest.json, 128 hex chars
// One manifest per board in the same release: manifest.json (SenkoPhone, e22wroom; boards up to v1.3.x
// read it), manifest-heltec_v3.json, manifest-heltec_v4.json. A board takes only a manifest whose
// "board" equals its DIY_BOARD_ID and an image for its own chip.
// The signature is checked with the public key built into the firmware (DIY_UPDATE_PUBKEY),
// so a release is accepted only if it was signed by our CI; TLS certificates are not needed
// for that. The image is written to the other OTA slot while its SHA-256 is computed, and the
// slot is switched only if the hash matches the signed manifest.
//
// After an update the new firmware is "on trial": if it restarts more than 3 times before
// running 10 minutes, the board boots the previous slot again.
//
// CLI (USB and web console):
//   update                         status
//   update check                   read the manifest now
//   update install                 install the version found by the last check
//   update mode off|notify|auto    auto installs at night (00:00-02:59 UTC = 03-06 MSK)
//   update beta on|off             accept pre-releases
//   update url <manifest url>      where manifest.json lives (manifest.sig next to it)
class Updater {
public:
  // Call first thing in setup(): handles the trial / rollback of a fresh update.
  static void earlyBoot();

  void begin(WifiLink* wifi);
  void loop();
  bool handleCommand(const char* command, char* reply, size_t reply_len);

  // state for the panel / OLED / MQTT
  uint8_t mode() const { return _mode; }            // 0 off, 1 notify, 2 auto
  bool beta() const { return _beta; }
  const char* url() const { return _url; }
  const char* available() const { return _avail_version; }   // "" = nothing newer
  const char* availableChannel() const { return _avail_channel; }
  const char* stateText() const;                     // idle, checking, downloading, ...
  const char* lastError() const { return _err; }
  int progress() const { return _progress; }          // 0..100 while downloading
  uint32_t lastCheckAgoS() const;
  bool rolledBack() const { return _rolled_back; }
  bool hasKey() const;

  void requestCheck() { _want_check = true; }
  void requestInstall() { _want_install = true; }

private:
  void load();
  void save();
  static void task(void* arg);
  bool doCheck();
  bool doInstall();
  bool fetch(const char* url, char* out, size_t out_size, size_t* out_len);

  WifiLink* _wifi = nullptr;
  uint8_t _mode = 1;
  bool _beta = false;
  char _url[160] = {0};

  // results (written by the task)
  volatile uint8_t _state = 0;
  volatile int _progress = 0;
  char _err[64] = {0};
  char _avail_version[48] = {0};
  char _avail_channel[12] = {0};
  char _avail_url[200] = {0};
  uint32_t _avail_size = 0;
  uint8_t _avail_sha[32] = {0};
  uint32_t _last_check_ms = 0;
  bool _checked_once = false;
  bool _rolled_back = false;

  volatile bool _want_check = false, _want_install = false, _busy = false;
  bool _trial_cleared = false;
};
