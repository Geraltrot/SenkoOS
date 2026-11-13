#include "OtaServer.h"

#include <WebServer.h>
#include <Update.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <Preferences.h>

OtaServer ota_server;

static const uint32_t TOKEN_TTL_MS = 5UL * 60UL * 1000UL;
static const uint32_t IDLE_STOP_MS = 6UL * 60UL * 1000UL;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_token[33] = {0};          // guarded by s_mux
static uint32_t s_token_until = 0;      // guarded by s_mux
static volatile bool s_running = false;
static volatile bool s_done = false;    // image written, reboot please
static WebServer* s_srv = nullptr;      // owned by the task

// upload state, touched only by the task
static bool s_auth = false, s_ok = false;
static char s_err[64];
static uint32_t s_last_activity = 0;
static bool s_first_chunk = false;

// The upload must be an application image (<tag>.bin) for a classic ESP32:
// esp_image_header_t starts with magic 0xE9, chip_id is at bytes 12..13 (0 = ESP32).
// A <tag>-merged.bin starts with 0xFF padding before the bootloader, so it is rejected here.
static const char* checkImageHeader(const uint8_t* b, size_t n) {
  if (n < 16) return "file too short";
  if (b[0] == 0xFF) return "merged.bin: flash it over USB at 0x0, OTA needs <tag>.bin";
  if (b[0] != 0xE9) return "not an ESP32 application image";
  if ((b[12] | (b[13] << 8)) != 0) return "image is for another chip, not ESP32";
  return nullptr;
}

static bool takeToken(const String& t) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s_token[0] && (int32_t)(s_token_until - millis()) > 0 && t.length() == 32) {
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= s_token[i] ^ t[i];
    ok = diff == 0;
    if (ok) s_token[0] = 0;   // one upload per token
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

static void sendJson(int code, const char* body) {
  s_srv->sendHeader("Access-Control-Allow-Origin", "*");
  s_srv->sendHeader("Cache-Control", "no-store");
  s_srv->send(code, "application/json", body);
}

static void onUpload() {
  HTTPUpload& up = s_srv->upload();
  s_last_activity = millis();
  if (up.status == UPLOAD_FILE_START) {
    s_ok = false;
    s_err[0] = 0;
    s_auth = takeToken(s_srv->arg("t"));
    if (!s_auth) { strcpy(s_err, "token is wrong or expired"); return; }
    s_first_chunk = true;   // Update.begin() waits for the first chunk: nothing is erased before the header check
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!s_auth || s_err[0]) return;
    if (s_first_chunk) {
      s_first_chunk = false;
      const char* bad = checkImageHeader(up.buf, up.currentSize);
      if (bad) { strlcpy(s_err, bad, sizeof(s_err)); return; }
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { snprintf(s_err, sizeof(s_err), "begin: %s", Update.errorString()); return; }
    }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      snprintf(s_err, sizeof(s_err), "write: %s", Update.errorString());
      Update.abort();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!s_auth || s_err[0]) return;
    if (s_first_chunk) { strcpy(s_err, "empty file"); return; }
    // trial, as for auto-updates: a new image that keeps restarting goes back to this slot (Updater::earlyBoot)
    const esp_partition_t* running = esp_ota_get_running_partition();
    Preferences p;
    if (p.begin("diy", false)) {
      p.putString("up_prev", running ? running->label : "");
      p.putUChar("up_boots", 0);
      p.putUChar("up_trial", 1);
      p.putUChar("up_rb", 0);
      p.end();
    }
    if (Update.end(true)) s_ok = true;
    else if (p.begin("diy", false)) { p.putUChar("up_trial", 0); p.end(); }
    else snprintf(s_err, sizeof(s_err), "end: %s", Update.errorString());
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (s_auth && !s_err[0] && !s_first_chunk) Update.abort();
    strcpy(s_err, "upload aborted");
  }
}

static void onDone() {
  char body[96];
  if (!s_auth) { snprintf(body, sizeof(body), "{\"error\":\"%s\"}", s_err[0] ? s_err : "token required"); sendJson(401, body); return; }
  if (!s_ok) { snprintf(body, sizeof(body), "{\"error\":\"%s\"}", s_err[0] ? s_err : "no file"); sendJson(400, body); return; }
  sendJson(200, "{\"ok\":true,\"msg\":\"firmware written, rebooting\"}");
  s_done = true;
}

static void otaTask(void*) {
  s_srv = new WebServer(OtaServer::PORT);
  s_srv->on("/update", HTTP_POST, onDone, onUpload);
  s_srv->on("/update", HTTP_OPTIONS, [] {
    s_srv->sendHeader("Access-Control-Allow-Origin", "*");
    s_srv->sendHeader("Access-Control-Allow-Methods", "POST");
    s_srv->send(204);
  });
  s_srv->onNotFound([] { sendJson(404, "{\"error\":\"not found\"}"); });
  s_srv->begin();
  s_last_activity = millis();

  while (!s_done && millis() - s_last_activity < IDLE_STOP_MS) {
    s_srv->handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  vTaskDelay(pdMS_TO_TICKS(300));   // let the last response go out
  s_srv->stop();
  delete s_srv;
  s_srv = nullptr;
  s_running = false;
  vTaskDelete(nullptr);
}

bool OtaServer::issueToken(char* token_out) {
  char t[33];
  for (int i = 0; i < 16; i++) sprintf(&t[i * 2], "%02x", (unsigned)(esp_random() & 0xFF));
  portENTER_CRITICAL(&s_mux);
  memcpy(s_token, t, sizeof(t));
  s_token_until = millis() + TOKEN_TTL_MS;
  portEXIT_CRITICAL(&s_mux);
  strcpy(token_out, t);

  if (!s_running) {
    s_running = true;
    s_last_activity = millis();
    // same core and priority as loop() -> time-sliced with the mesh
    if (xTaskCreatePinnedToCore(otaTask, "diy_ota", 6144, nullptr, 1, nullptr, ARDUINO_RUNNING_CORE) != pdPASS) {
      s_running = false;
      return false;
    }
  }
  return true;
}

bool OtaServer::rebootRequested() const { return s_done; }
bool OtaServer::running() const { return s_running; }
