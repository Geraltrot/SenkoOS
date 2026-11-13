#include "DiySensors.h"
#include "target.h"

#include <CayenneLPP.h>

struct LppType {
  uint8_t type, size;
  float scale;
  bool is_signed;
  const char* name;
  const char* unit;
  const char* dev_class;
};

// the CayenneLPP types the upstream sensor drivers produce
static const LppType TYPES[] = {
  {LPP_TEMPERATURE,         2, 0.1f,   true,  "температура",  "°C",  "temperature"},
  {LPP_RELATIVE_HUMIDITY,   1, 0.5f,   false, "влажность",    "%",   "humidity"},
  {LPP_BAROMETRIC_PRESSURE, 2, 0.1f,   false, "давление",     "hPa", "atmospheric_pressure"},
  {LPP_ALTITUDE,            2, 1.0f,   true,  "высота",       "m",   "distance"},
  {LPP_VOLTAGE,             2, 0.01f,  false, "напряжение",   "V",   "voltage"},
  {LPP_CURRENT,             2, 0.001f, false, "ток",          "A",   "current"},
  {LPP_POWER,               2, 1.0f,   false, "мощность",     "W",   "power"},
  {LPP_LUMINOSITY,          2, 1.0f,   false, "освещённость", "lx",  "illuminance"},
  {LPP_DISTANCE,            4, 0.001f, false, "расстояние",   "m",   "distance"},
  {LPP_FREQUENCY,           4, 1.0f,   false, "частота",      "Hz",  "frequency"},
  {LPP_PERCENTAGE,          1, 1.0f,   false, "процент",      "%",   nullptr},
  {LPP_CONCENTRATION,       2, 1.0f,   false, "концентрация", "ppm", nullptr},
  {LPP_ANALOG_INPUT,        2, 0.01f,  true,  "аналоговый вход", nullptr, nullptr},
  {LPP_GENERIC_SENSOR,      4, 1.0f,   false, "значение",     nullptr, nullptr},
};

int diyReadSensors(DiySensorValue out[], int max) {
  CayenneLPP lpp(160);
  lpp.reset();
  if (!sensors.querySensors(0xFF, lpp)) return 0;
  const uint8_t* b = lpp.getBuffer();
  int len = lpp.getSize(), i = 0, n = 0;
  while (i + 2 <= len && n < max) {
    uint8_t ch = b[i], type = b[i + 1];
    const LppType* t = nullptr;
    for (const LppType& x : TYPES) if (x.type == type) { t = &x; break; }
    if (!t) break;   // unknown size: cannot continue safely
    if (i + 2 + t->size > len) break;
    uint32_t raw = 0;
    for (int k = 0; k < t->size; k++) raw = (raw << 8) | b[i + 2 + k];
    float v;
    if (t->is_signed && t->size == 2) v = (int16_t)raw * t->scale;
    else if (t->is_signed && t->size == 4) v = (int32_t)raw * t->scale;
    else v = raw * t->scale;
    DiySensorValue& o = out[n++];
    o.channel = ch;
    o.type = type;
    o.value = v;
    snprintf(o.key, sizeof(o.key), "s%u_%u", ch, type);
    o.name = t->name;
    o.unit = t->unit;
    o.dev_class = t->dev_class;
    i += 2 + t->size;
  }
  return n;
}
