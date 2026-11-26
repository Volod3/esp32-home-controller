// Емулятор контролера: справжня прошивка src/main.cpp, зібрана для ПК.
// Python-вікно (emulator.py) керує нею через C-функції emu_* нижче.
#include "emu_hal.h"

EmuState        emu;
HardwareSerial  Serial(false), Serial2(true);
TwoWire         Wire;
WiFiClass       WiFi;
ArduinoOTAClass ArduinoOTA;
MDNSClass       MDNS;
EspClass        ESP;

#include "../src/main.cpp"

// ---------------- реалізація заглушок ----------------
uint32_t analogReadMilliVolts(int pin) {
  int n = (rand() % 7) - 3;                                   // трохи "шуму", як у справжнього АЦП
  if (pin == PIN_LDR)          return (uint32_t)max(0, emu.ldrMv + n);
  if (pin == PIN_POWER_DETECT) return emu.gridOn ? 1850 + n : 40 + n;
  return 0;
}

void HardwareSerial::println(const char* s) {
  if (!isLink) { puts(s); return; }
  int r, g, b;                                                  // команди стрічки для Pro Micro
  if (!strcmp(s, "LED,OFF")) emu.ledOn = 0;
  else if (sscanf(s, "LED,%d,%d,%d", &r, &g, &b) == 3) { emu.ledR = r; emu.ledG = g; emu.ledB = b; emu.ledOn = 1; }
}

static uint8_t inaReg = 0;
uint8_t TwoWire::endTransmission(bool) {
  if (addr == 0x3C) return 0;                                   // екран є
  if (addr == 0x40) { if (!tx.empty()) inaReg = tx[0]; return 0; }   // INA3221
  return 2;                                                     // інших пристроїв нема
}
uint8_t TwoWire::requestFrom(int a, int n) {
  rxb.clear(); rxi = 0;
  if (a != 0x40) return 0;
  uint16_t v = 0;
  if (inaReg == 0xFE) v = 0x5449;
  else if (inaReg == 0x00) v = INA_CONFIG_VAL;
  else if (inaReg == 0x02 || inaReg == 0x04 || inaReg == 0x06) {
    int ch = inaReg / 2 - 1;
    int mv = (int)lroundf(emu.inaV[ch] * 1000.0f);
    v = (uint16_t)((max(0, mv) / 8) << 3);
  }
  rxb.push_back(v >> 8); rxb.push_back(v & 0xFF);
  return (uint8_t)min(n, 2);
}

static std::map<std::string, std::map<std::string, std::string>> gPrefs;
static std::mutex gPrefsMu;
static bool gPrefsLoaded = false, gPrefsDirty = false;
static void prefsLoad() {
  if (gPrefsLoaded) return;
  gPrefsLoaded = true;
  FILE* f = fopen(emu.prefsPath.c_str(), "r");
  if (!f) return;
  char ns[64], key[64]; static char hex[8192];
  while (fscanf(f, "%63s %63s %8191s", ns, key, hex) == 3) {
    std::string v;
    for (size_t i = 0; hex[i] && hex[i + 1] && strcmp(hex, "-"); i += 2) { unsigned b; sscanf(hex + i, "%2x", &b); v += (char)b; }
    gPrefs[ns][key] = v;
  }
  fclose(f);
}
static void prefsSave() {
  FILE* f = fopen(emu.prefsPath.c_str(), "w");
  if (!f) return;
  for (auto& n : gPrefs) for (auto& k : n.second) {
    fprintf(f, "%s %s ", n.first.c_str(), k.first.c_str());
    if (k.second.empty()) fputc('-', f);
    for (unsigned char c : k.second) fprintf(f, "%02x", c);
    fputc('\n', f);
  }
  fclose(f);
  gPrefsDirty = false;
}
std::string* Preferences::raw(const char* k) {
  std::lock_guard<std::mutex> g(gPrefsMu); prefsLoad();
  auto n = gPrefs.find(ns); if (n == gPrefs.end()) return nullptr;
  auto i = n->second.find(k); return i == n->second.end() ? nullptr : &i->second;
}
bool Preferences::isKey(const char* k) { return raw(k) != nullptr; }
bool Preferences::remove(const char* k) { std::lock_guard<std::mutex> g(gPrefsMu); prefsLoad(); gPrefsDirty = true; return gPrefs[ns].erase(k) > 0; }
void Preferences::put(const char* k, const void* p, size_t n) {
  std::lock_guard<std::mutex> g(gPrefsMu); prefsLoad();
  gPrefs[ns][k] = std::string((const char*)p, n); gPrefsDirty = true;
}
void Preferences::end() { std::lock_guard<std::mutex> g(gPrefsMu); if (gPrefsDirty) prefsSave(); }

void xTaskCreatePinnedToCore(void (*fn)(void*), const char* name, int, void* arg, int, TaskHandle_t*, int) {
  static std::vector<std::string> started;                      // після "перезапуску" задачі вже працюють
  if (std::find(started.begin(), started.end(), name) != started.end()) return;
  started.push_back(name);
  std::thread([fn, arg] { fn(arg); }).detach();
}

// ---------------- тестова погода (в емуляторі нема інтернету) ----------------
static void emuFakeWeather() {
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  t.tm_hour = t.tm_min = t.tm_sec = 0;
  time_t midnight = mktime(&t);
  static Forecast nf;
  memset(&nf, 0, sizeof(nf));
  nf.magic = FC_MAGIC;
  nf.start = (uint32_t)midnight;
  nf.fetched = (uint32_t)now;
  static const uint8_t CODES[7] = {2, 61, 3, 0, 71, 95, 1};
  for (int i = 0; i < FC_HOURS; i++) {
    int d = i / 24, h = i % 24;
    float base = 2.0f - d * 1.3f + (d == 3 ? 4 : 0);
    nf.temp[i] = (int8_t)lroundf(base + 4.5f * sinf((h - 9) / 24.0f * 6.2832f));
    nf.code[i] = (h >= 8 && h <= 20) ? CODES[d] : (CODES[d] >= 61 ? 3 : 0);
    nf.pop[i]  = (uint8_t)(CODES[d] >= 61 ? 40 + (h * 7) % 55 : (h * 3) % 20);
    nf.hum[i]  = (uint8_t)(70 + (h * 5) % 25);
    nf.wind[i] = (uint8_t)(8 + (h + d * 5) % 15);
  }
  for (int d = 0; d < FC_DAYS; d++) {
    struct tm dt; time_t day = midnight + d * 86400; localtime_r(&day, &dt);
    int r, s; saverSun(dt, r, s);
    nf.rise[d] = (uint32_t)(day + r * 60);
    nf.set[d]  = (uint32_t)(day + s * 60);
  }
  portENTER_CRITICAL(&fcMux);
  fc = nf; fcOk = true; fcVer++;
  portEXIT_CRITICAL(&fcMux);
  int idx = (int)((now - midnight) / 3600);
  wx.temp = fc.temp[idx]; wx.code = fc.code[idx]; wx.hum = fc.hum[idx];
  wx.feel = fc.temp[idx] - 4.0f; wx.wind = fc.wind[idx];
  wx.at = millis(); wx.ok = true; wxLiveAt = millis(); wxFromCache = false; wxStatus = 200;
}

// ---------------- потік прошивки ----------------
static std::mutex gStateMu;
static std::string gState;

static void fwThread() {
  for (;;) {
    try {
      setup();
      emuFakeWeather();
      uint32_t tState = 0;
      for (;;) {
        loop();
        if (millis() - tState >= 250) {                         // стан для бічної панелі (той самий JSON, що в веб-меню)
          tState = millis();
          webState();
          std::lock_guard<std::mutex> g(gStateMu);
          gState = emu.lastBody;
        }
      }
    } catch (EmuRestart&) {
      puts("[EMU] перезапуск");
      delay(400);
    }
  }
}

static void pushLine(const char* l) {
  std::lock_guard<std::mutex> g(emu.rxMu);
  for (const char* p = l; *p; p++) emu.rx.push_back(*p);
  emu.rx.push_back('\n');
}

extern "C" {
void emu_start(const char* prefsPath) {
  static bool started = false;
  if (started) return;
  started = true;
  if (prefsPath && *prefsPath) emu.prefsPath = prefsPath;
  std::thread(fwThread).detach();
}

void emu_frame(uint8_t* out) { memcpy(out, scrShot, sizeof(scrShot)); }
int  emu_contrast() { return emu.contrast; }
int  emu_power_save() { return emu.powerSave || deepOff; }

// k: "left" / "right" — крок енкодера; "ok" / "back" / "conf" — кнопка (down = 1 натиснута, 0 відпущена)
void emu_key(const char* k, int down) {
  if (!strcmp(k, "left"))  { pushLine("ENC,-1"); return; }
  if (!strcmp(k, "right")) { pushLine("ENC,+1"); return; }
  char b[24];
  const char* n = !strcmp(k, "ok") ? "OK" : !strcmp(k, "back") ? "BACK" : !strcmp(k, "conf") ? "CONF" : nullptr;
  if (!n) return;
  snprintf(b, sizeof(b), "BTN,%s,%d", n, down ? 1 : 0);
  pushLine(b);
}

// name: ldr (мВ), grid (0/1), wifi (0/1), bat1 / bat2 / line (В, як показує екран)
void emu_set(const char* name, double v) {
  if (!strcmp(name, "ldr"))  emu.ldrMv = (int)v;
  else if (!strcmp(name, "grid")) emu.gridOn = v != 0;
  else if (!strcmp(name, "wifi")) emu.wifiOn = v != 0;
  else if (!strcmp(name, "bat1")) emu.inaV[BAT1_INA_CH - 1] = (float)v - BAT1_OFFSET_MV / 1000.0f;
  else if (!strcmp(name, "bat2")) emu.inaV[BAT2_INA_CH - 1] = (float)v - BAT2_OFFSET_MV / 1000.0f;
  else if (!strcmp(name, "line")) emu.inaV[LINE_INA_CH - 1] = (float)v - LINE_OFFSET_MV / 1000.0f;
}

// стан у JSON (як /api/state) — для панелі емулятора
int emu_state(char* out, int n) {
  std::lock_guard<std::mutex> g(gStateMu);
  snprintf(out, n, "%s", gState.c_str());
  return (int)gState.size();
}

// колір стрічки: повертає 1, якщо світиться
int emu_led(int* rgb) { rgb[0] = emu.ledR; rgb[1] = emu.ledG; rgb[2] = emu.ledB; return emu.ledOn; }
}
