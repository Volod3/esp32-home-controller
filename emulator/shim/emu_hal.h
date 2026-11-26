// Емулятор: заміна Arduino/ESP32 API для збірки справжнього src/main.cpp на комп'ютері.
// Усе, чого нема на ПК (Wi-Fi, I2C, UART, NVS, FreeRTOS), імітується тут.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <algorithm>

using std::min;
using std::max;

// ---------------- стан емулятора (керується з emu.cpp / Python) ----------------
struct EmuState {
  std::atomic<int>   ldrMv{1800};
  std::atomic<int>   gridOn{1};
  std::atomic<int>   wifiOn{1};
  float              inaV[3] = {5.05f, 3.95f, 4.10f};   // напруга каналів INA3221 1..3, В (вікно емулятора виставляє свої)
  std::mutex         rxMu;
  std::deque<char>   rx;                                // UART від "Pro Micro"
  std::atomic<int>   ledR{0}, ledG{0}, ledB{0}, ledOn{0};
  std::atomic<int>   contrast{255}, powerSave{0};
  std::string        lastBody;                          // остання відповідь веб-обробника
  std::string        prefsPath = "emu_prefs.txt";
};
extern EmuState emu;

struct EmuRestart {};                                   // ESP.restart() у емуляторі

// ---------------- час ----------------
inline uint64_t emuMicros() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}
inline uint32_t millis() { return (uint32_t)(emuMicros() / 1000); }
inline uint32_t micros() { return (uint32_t)emuMicros(); }
inline void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline void delayMicroseconds(uint32_t us) { (void)us; }
inline void yield() {}

// ---------------- Arduino ----------------
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define OUTPUT_OPEN_DRAIN 3
#define ADC_11db 3
#define SERIAL_8N1 0
#define PROGMEM
#define PGM_P const char*
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return HIGH; }
inline void analogSetPinAttenuation(int, int) {}
uint32_t analogReadMilliVolts(int pin);
inline long random(long a, long b) { return a + (b > a ? rand() % (b - a) : 0); }
inline long random(long b) { return b > 0 ? rand() % b : 0; }

class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(int v) : s(std::to_string(v)) {}
  const char* c_str() const { return s.c_str(); }
  unsigned length() const { return (unsigned)s.size(); }
  void reserve(size_t n) { s.reserve(n); }
  int indexOf(const char* p, int from = 0) const { size_t r = s.find(p, from); return r == std::string::npos ? -1 : (int)r; }
  int indexOf(char c, int from = 0) const { size_t r = s.find(c, from); return r == std::string::npos ? -1 : (int)r; }
  bool startsWith(const char* p) const { return s.rfind(p, 0) == 0; }
  bool endsWith(const char* p) const { size_t n = strlen(p); return s.size() >= n && s.compare(s.size() - n, n, p) == 0; }
  void remove(unsigned i) { if (i < s.size()) s.erase(i); }
  void remove(unsigned i, unsigned n) { if (i < s.size()) s.erase(i, n); }
  void trim() {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    s = s.substr(a, b - a);
  }
  long toInt() const { return atol(s.c_str()); }
  char operator[](unsigned i) const { return i < s.size() ? s[i] : 0; }
  String& operator+=(const char* c) { s += c; return *this; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(char c) { s += c; return *this; }
  String& operator+=(int v) { s += std::to_string(v); return *this; }
  bool operator==(const char* c) const { return s == c; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const char* c) const { return s != c; }
  explicit operator bool() const { return true; }
};

class Print {
 public:
  virtual ~Print() {}
};

class HardwareSerial {
 public:
  bool isLink;
  explicit HardwareSerial(bool link) : isLink(link) {}
  void begin(long, int = 0, int = 0, int = 0) {}
  void setRxBufferSize(size_t) {}
  int available() { if (!isLink) return 0; std::lock_guard<std::mutex> g(emu.rxMu); return (int)emu.rx.size(); }
  int read() {
    if (!isLink) return -1;
    std::lock_guard<std::mutex> g(emu.rxMu);
    if (emu.rx.empty()) return -1;
    char c = emu.rx.front(); emu.rx.pop_front(); return (uint8_t)c;
  }
  void println(const char* s);
  void println(const String& s) { println(s.c_str()); }
  void println() {}
  void print(const char*) {}
  int printf(const char* fmt, ...) {
    if (isLink) return 0;
    va_list ap; va_start(ap, fmt); int n = vprintf(fmt, ap); va_end(ap); return n;
  }
};
extern HardwareSerial Serial, Serial2;

// ---------------- I2C: імітація SH1106 (0x3C) і INA3221 (0x40) ----------------
class TwoWire {
  uint8_t addr = 0, reg = 0;
  std::vector<uint8_t> tx, rxb;
  size_t rxi = 0;
 public:
  bool begin(int = -1, int = -1, uint32_t = 0) { return true; }
  void end() {}
  void setClock(uint32_t) {}
  void setTimeOut(uint16_t) {}
  size_t setBufferSize(size_t n) { return n; }
  void beginTransmission(int a) { addr = (uint8_t)a; tx.clear(); }
  size_t write(uint8_t b) { tx.push_back(b); return 1; }
  size_t write(const uint8_t* b, size_t n) { tx.insert(tx.end(), b, b + n); return n; }
  uint8_t endTransmission(bool = true);
  uint8_t requestFrom(int a, int n);
  int read() { return rxi < rxb.size() ? rxb[rxi++] : -1; }
  int available() { return (int)(rxb.size() - rxi); }
};
extern TwoWire Wire;

// ---------------- NVS ----------------
class Preferences {
  std::string ns;
 public:
  bool begin(const char* n, bool = false) { ns = n; return true; }
  void end();
  bool isKey(const char* k);
  bool remove(const char* k);
  std::string* raw(const char* k);
  void put(const char* k, const void* p, size_t n);
  uint8_t getUChar(const char* k, uint8_t d = 0) { auto r = raw(k); return r && r->size() == 1 ? (uint8_t)(*r)[0] : d; }
  uint16_t getUShort(const char* k, uint16_t d = 0) { auto r = raw(k); uint16_t v; if (r && r->size() == 2) { memcpy(&v, r->data(), 2); return v; } return d; }
  bool getBool(const char* k, bool d = false) { auto r = raw(k); return r && r->size() == 1 ? (*r)[0] != 0 : d; }
  float getFloat(const char* k, float d = NAN) { auto r = raw(k); float v; if (r && r->size() == 4) { memcpy(&v, r->data(), 4); return v; } return d; }
  size_t getString(const char* k, char* out, size_t mx) { auto r = raw(k); if (!r) return 0; snprintf(out, mx, "%s", r->c_str()); return r->size(); }
  size_t getBytes(const char* k, void* out, size_t mx) { auto r = raw(k); if (!r || r->size() > mx) return 0; memcpy(out, r->data(), r->size()); return r->size(); }
  void putUChar(const char* k, uint8_t v) { put(k, &v, 1); }
  void putUShort(const char* k, uint16_t v) { put(k, &v, 2); }
  void putBool(const char* k, bool v) { uint8_t b = v; put(k, &b, 1); }
  void putFloat(const char* k, float v) { put(k, &v, 4); }
  void putString(const char* k, const char* v) { put(k, v, strlen(v)); }
  void putBytes(const char* k, const void* p, size_t n) { put(k, p, n); }
};

// ---------------- мережа ----------------
typedef enum { WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_SCAN_COMPLETED = 2, WL_CONNECTED = 3,
               WL_CONNECT_FAILED = 4, WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6 } wl_status_t;
#define WIFI_OFF 0
#define WIFI_STA 1
#define WIFI_AP 2
#define WIFI_AP_STA 3
#define WIFI_AUTH_OPEN 0
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)

class IPAddress {
  std::string s;
 public:
  IPAddress(const char* x = "0.0.0.0") : s(x) {}
  String toString() const { return String(s); }
};

class WiFiClass {
  int mode_ = WIFI_STA;
  int scanState = WIFI_SCAN_FAILED;
 public:
  wl_status_t status() { return emu.wifiOn ? WL_CONNECTED : WL_DISCONNECTED; }
  void mode(int m) { mode_ = m; }
  int getMode() { return mode_; }
  void setHostname(const char*) {}
  void setAutoReconnect(bool) {}
  void setSleep(bool) {}
  void begin(const char*, const char* = nullptr) {}
  void disconnect(bool = false) {}
  void softAP(const char*, const char* = nullptr) {}
  void softAPdisconnect(bool = false) {}
  int softAPgetStationNum() { return 0; }
  IPAddress softAPIP() { return IPAddress("192.168.4.1"); }
  IPAddress localIP() { return IPAddress(emu.wifiOn ? "192.168.0.123" : "0.0.0.0"); }
  int RSSI() { return -54; }
  String SSID() { return String("Emu-WiFi"); }
  void scanNetworks(bool = false, bool = false) { scanState = 4; }
  int scanComplete() { return scanState; }
  void scanDelete() { scanState = WIFI_SCAN_FAILED; }
  String SSID(int i) { static const char* N[] = {"Emu-WiFi", "Сусіди 5G", "TP-Link_0000", "Kyivstar"}; return String(N[i & 3]); }
  int RSSI(int i) { return -50 - i * 9; }
  int channel(int i) { return 1 + i * 3; }
  int encryptionType(int i) { return i == 3 ? WIFI_AUTH_OPEN : 3; }
};
extern WiFiClass WiFi;

class WiFiClient {
 public:
  bool connected() { return false; }
  void stop() {}
  int fd() { return -1; }
  void setNoDelay(bool) {}
  void setTimeout(int) {}
  void setInsecure() {}
  size_t print(const char*) { return 0; }
  explicit operator bool() { return false; }
};
typedef WiFiClient WiFiClientSecure;

#define HTTP_CODE_OK 200
class HTTPClient {
 public:
  void useHTTP10(bool) {}
  void setTimeout(int) {}
  bool begin(WiFiClient&, const char*) { return false; }    // емулятор без інтернету: погода — тестова
  int GET() { return -1; }
  String getString() { return String(""); }
  void end() {}
};

typedef int ota_error_t;
class ArduinoOTAClass {
 public:
  void setHostname(const char*) {}
  void setPassword(const char*) {}
  void onStart(std::function<void()>) {}
  void onProgress(std::function<void(unsigned, unsigned)>) {}
  void onError(std::function<void(ota_error_t)>) {}
  void begin() {}
  void handle() {}
};
extern ArduinoOTAClass ArduinoOTA;

class MDNSClass { public: void addService(const char*, const char*, int) {} };
extern MDNSClass MDNS;

enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST };
class WebServer {
  std::map<std::string, std::string> args;
 public:
  explicit WebServer(int) {}
  void on(const char*, HTTPMethod, std::function<void()>) {}
  void onNotFound(std::function<void()>) {}
  void begin() {}
  void enableDelay(bool) {}
  void handleClient() {}
  void setArgs(const std::map<std::string, std::string>& a) { args = a; }
  String arg(const char* k) { auto i = args.find(k); return i == args.end() ? String("") : String(i->second); }
  bool hasArg(const char* k) { return args.count(k) > 0; }
  void sendHeader(const char*, const char*) {}
  void send(int, const char*, const String& b) { emu.lastBody = b.s; }
  void send(int, const char*, const char* b) { emu.lastBody = b; }
  void send_P(int, const char*, const char* b, size_t n = 0) { emu.lastBody = n ? std::string(b, n) : std::string(b); }
  WiFiClient client() { return WiFiClient(); }
};

class EspClass {
 public:
  uint32_t getFreeHeap() { return 200000; }
  [[noreturn]] void restart() { throw EmuRestart(); }
};
extern EspClass ESP;

// ---------------- FreeRTOS / ESP-IDF ----------------
struct portMUX_TYPE { std::recursive_mutex m; };
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(x) (x)->m.lock()
#define portEXIT_CRITICAL(x) (x)->m.unlock()
#define pdMS_TO_TICKS(x) (x)
typedef void* TaskHandle_t;
inline void vTaskDelay(uint32_t ms) { delay(ms); }
void xTaskCreatePinnedToCore(void (*fn)(void*), const char* name, int, void* arg, int, TaskHandle_t*, int);
inline void esp_task_wdt_init(int, bool) {}
inline void esp_task_wdt_add(void*) {}
inline void esp_task_wdt_reset() {}
inline int64_t esp_timer_get_time() { return (int64_t)emuMicros(); }
inline void sntp_set_time_sync_notification_cb(void (*)(struct timeval*)) {}
inline void configTzTime(const char* tz, const char*, const char* = nullptr, const char* = nullptr) { setenv("TZ", tz, 1); tzset(); }
#define settimeofday(tv, tz) 0                                  // годинник ПК не чіпаємо

// ---------------- U8g2: справжня бібліотека, буфер у пам'яті ----------------
extern "C" {
#include "u8g2.h"
}
#define U8X8_PROGMEM
class U8G2_SH1106_128X64_NONAME_F_HW_I2C {
  u8g2_t u;
 public:
  U8G2_SH1106_128X64_NONAME_F_HW_I2C(const u8g2_cb_t* r, uint8_t) {
    u8g2_Setup_sh1106_128x64_noname_f(&u, r, u8x8_byte_empty, u8x8_dummy_cb);
  }
  bool begin() { u8g2_InitDisplay(&u); u8g2_ClearBuffer(&u); return true; }
  void setBusClock(uint32_t) {}
  void setFontMode(int m) { u8g2_SetFontMode(&u, m); }
  void setBitmapMode(int m) { u8g2_SetBitmapMode(&u, m); }
  void setFont(const uint8_t* f) { u8g2_SetFont(&u, f); }
  int drawUTF8(int x, int y, const char* s) { return u8g2_DrawUTF8(&u, x, y, s); }
  int drawStr(int x, int y, const char* s) { return u8g2_DrawStr(&u, x, y, s); }
  int getUTF8Width(const char* s) { return u8g2_GetUTF8Width(&u, s); }
  int getStrWidth(const char* s) { return u8g2_GetStrWidth(&u, s); }
  int getAscent() { return u8g2_GetAscent(&u); }
  void drawLine(int a, int b, int c, int d) { u8g2_DrawLine(&u, a, b, c, d); }
  void drawHLine(int x, int y, int w) { u8g2_DrawHLine(&u, x, y, w); }
  void drawVLine(int x, int y, int h) { u8g2_DrawVLine(&u, x, y, h); }
  void drawPixel(int x, int y) { u8g2_DrawPixel(&u, x, y); }
  void drawDisc(int x, int y, int r, int o = U8G2_DRAW_ALL) { u8g2_DrawDisc(&u, x, y, r, o); }
  void drawCircle(int x, int y, int r, int o = U8G2_DRAW_ALL) { u8g2_DrawCircle(&u, x, y, r, o); }
  void drawBox(int x, int y, int w, int h) { u8g2_DrawBox(&u, x, y, w, h); }
  void drawFrame(int x, int y, int w, int h) { u8g2_DrawFrame(&u, x, y, w, h); }
  void drawRBox(int x, int y, int w, int h, int r) { u8g2_DrawRBox(&u, x, y, w, h, r); }
  void drawRFrame(int x, int y, int w, int h, int r) { u8g2_DrawRFrame(&u, x, y, w, h, r); }
  void drawTriangle(int a, int b, int c, int d, int e, int f) { u8g2_DrawTriangle(&u, a, b, c, d, e, f); }
  void drawXBMP(int x, int y, int w, int h, const uint8_t* b) { u8g2_DrawXBMP(&u, x, y, w, h, b); }
  void drawXBM(int x, int y, int w, int h, const uint8_t* b) { u8g2_DrawXBM(&u, x, y, w, h, b); }
  void setDrawColor(int c) { u8g2_SetDrawColor(&u, c); }
  void setClipWindow(int a, int b, int c, int d) { u8g2_SetClipWindow(&u, a, b, c, d); }
  void setMaxClipWindow() { u8g2_SetMaxClipWindow(&u); }
  void clearBuffer() { u8g2_ClearBuffer(&u); }
  void sendBuffer() {}
  uint8_t* getBufferPtr() { return u8g2_GetBufferPtr(&u); }
  void setContrast(uint8_t c) { emu.contrast = c; }
  void setPowerSave(int p) { emu.powerSave = p; }
};
