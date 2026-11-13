#pragma once

#include <Arduino.h>

// Readings of I2C sensors found by the upstream EnvironmentSensorManager (BMP/BME280, AHT, SHT, INA, ...).
// The manager scans the bus at boot and starts only what it finds; here its CayenneLPP telemetry is
// decoded into plain values for MQTT / Home Assistant, the panel and the CLI.
struct DiySensorValue {
  uint8_t channel;       // LPP channel: one per detected sensor
  uint8_t type;          // LPP type (103 = temperature, ...)
  float value;
  char key[12];          // "s2_103": stable id for MQTT / HA
  const char* name;      // Russian name of the quantity
  const char* unit;      // HA unit, nullptr = none
  const char* dev_class; // HA device class, nullptr = none
};

// fills out[] with the current readings, returns how many
int diyReadSensors(DiySensorValue out[], int max);
