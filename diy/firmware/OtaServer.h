#pragma once

#include <Arduino.h>

// Firmware upload that does not stop the repeater.
//
// The upload is received by a separate FreeRTOS task with its own small WebServer
// on port 8081, pinned to the same core and priority as loop(): FreeRTOS time-slices
// the two, so the mesh keeps running while the image is written to the spare slot.
//
// Access: the main panel (logged in) asks for a one-time token (valid 5 minutes,
// one upload). The task shares nothing else with the main panel.
class OtaServer {
public:
  static const uint16_t PORT = 8081;

  // Called from the main task. Starts the upload task if needed; fills token (33 chars).
  bool issueToken(char* token_out);
  // Called from loop(): true once a new image was written and the board should reboot.
  bool rebootRequested() const;
  bool running() const;
};

extern OtaServer ota_server;
