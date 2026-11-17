#pragma once

#include <Arduino.h>

// chip_id in the application image header (esp_image_header_t bytes 12..13) of the chip this firmware
// is built for: an upload or an auto-update for another chip is refused
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  #define DIY_IMAGE_CHIP_ID 9    // ESP_CHIP_ID_ESP32S3 (Heltec V3 / V4)
#elif defined(CONFIG_IDF_TARGET_ESP32)
  #define DIY_IMAGE_CHIP_ID 0    // ESP_CHIP_ID_ESP32 (SenkoPhone, WROOM-32)
#else
  #error "SenkoOS: unknown ESP32 target, add its image chip id here"
#endif

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
