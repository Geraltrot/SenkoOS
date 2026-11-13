#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Streams a JSON response in ~1 KB chunks (chunked transfer encoding), so big
// answers (packet log, path graph) never need one large heap block.
class JsonOut {
public:
  explicit JsonOut(WebServer& server) : _s(server) {}

  void begin(int code = 200) {
    _s.sendHeader("Cache-Control", "no-store");
    _s.setContentLength(CONTENT_LENGTH_UNKNOWN);
    _s.send(code, "application/json", "");
  }
  void end() {
    flush();
    _s.sendContent("");   // terminating chunk
  }

  JsonOut& raw(const char* s) {
    while (*s) put(*s++);
    return *this;
  }
  JsonOut& f(const char* fmt, ...) {
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    return raw(tmp);
  }
  JsonOut& str(const char* s) {
    put('"');
    for (; *s; s++) {
      unsigned char c = (unsigned char)*s;
      if (c == '"' || c == '\\') { put('\\'); put(c); }
      else if (c == '\n') raw("\\n");
      else if (c < 0x20) f("\\u%04x", c);
      else put(c);
    }
    put('"');
    return *this;
  }
  JsonOut& hex(const uint8_t* b, int n) {
    static const char* H = "0123456789ABCDEF";
    put('"');
    for (int i = 0; i < n; i++) { put(H[b[i] >> 4]); put(H[b[i] & 15]); }
    put('"');
    return *this;
  }
  // comma helper for arrays/objects: call before every element
  JsonOut& sep(bool& first) {
    if (!first) put(',');
    first = false;
    return *this;
  }

private:
  void put(char c) {
    _buf[_n++] = c;
    if (_n == sizeof(_buf)) flush();
  }
  void flush() {
    if (_n) { _s.sendContent(_buf, _n); _n = 0; }
  }

  WebServer& _s;
  char _buf[1024];
  size_t _n = 0;
};
