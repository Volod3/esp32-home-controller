// =====================================================================
//  Pro Micro (ATmega32U4) — прошивка периферії для ESP32 (v1.1, стабільна)
//  Зв'язок з ESP32: Serial1 (пін 0 = RX, пін 1 = TX) + USB Serial.
//  Протокол НЕ змінено відносно v1.0: рядкові команди, кома або пробіл,
//  регістр не важливий. Відповіді йдуть лише в той порт, звідки прийшла команда.
//  Події (ENC / BTN / IR) йдуть на ESP32 завжди, а в USB — лише якщо
//  хост читає (USB більше ніколи не гальмує роботу).
//
//  platformio.ini -> lib_deps:
//    adafruit/Adafruit NeoPixel
//    z3t0/IRremote            (потрібна, лише якщо ENABLE_IR 1)
//
//  КОМАНДИ
//    PING                      -> PONG
//    HELP                      -> список команд
//    VER                       -> VER,PROMICRO,<версія>
//    STATUS                    -> стан кнопок, світлодіода, подій, uptime
//    LED,r,g,b                 -> колір світлодіода (0..255)
//    LED,OFF                   -> вимкнути світлодіод
//    BRIGHT,n                  -> яскравість світлодіода (0..255)
//    EVENTS,ENC|BTN|IR,0|1     -> вмикає чи вимикає відправку подій
//    PINMODE,n,IN|PULLUP|OUT   -> режим вільного піна
//    DREAD,n                   -> DREAD,n,0|1
//    DWRITE,n,0|1              -> вивести 0 або 1
//    AREAD,n                   -> AREAD,n,0..1023 (піни A0..A3)
//    PWM,n,0..255              -> ШІМ (піни 5 і 6)
//    RESET                     -> скинути світлодіод і події до початкових
//
//  СВІТЛОДІОДИ З ESP32 (ефекти рахує ESP, Pro Micro лише показує)
//    LEDS,n                    -> скільки світлодіодів у стрічці (1..32)
//    PX,i,r,g,b                -> задати піксель i (без показу)
//    FILL,r,g,b                -> залити всі (без показу)
//    SHOW                      -> показати накопичене
//    FRAME,<hex>               -> весь кадр: RRGGBB на піксель, одразу показ
//    ACK,0|1                   -> вимкнути/увімкнути відповіді OK на команди світла
//
//  ПОДІЇ
//    ENC,+1 | ENC,-1
//    BTN,BACK|CONF|OK,1|0
//    IR,<протокол>,0x<адреса>,0x<команда>
//    READY,PROMICRO,<версія>   (раз при старті)
//
//  ВІЛЬНІ ПІНИ для PINMODE/DREAD/DWRITE/AREAD/PWM:
//    5, 6, 14, 15, 16, A0..A3 (18..21)
//
//  ВАЖЛИВО: ESP32 має надсилати команди з паузою ~10-15 мс між рядками.
//  Під час show() стрічки переривання вимкнені ~0.3 мс, і байти, що
//  прийшли одним пакетом, можуть загубитись.
// =====================================================================

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

#define ENABLE_IR          1   // 0 = не збирати код ІЧ (якщо приймача нема)
#define IR_REPORT_UNKNOWN  0   // 1 = слати IR,UNKNOWN (при шумі на піні може засмічувати лінію)
#define IR_MIN_GAP_MS      150 // не частіше одного ІЧ-події на 150 мс

#if ENABLE_IR
#include <IRremote.hpp>
#endif

#define FW_VERSION "1.1"

// --- ПІНИ ПЕРИФЕРІЇ ---
#define PIN_ENC_TRA   3
#define PIN_ENC_TRB   7
#define PIN_BTN_BACK  8
#define PIN_BTN_CONF  2
#define PIN_BTN_OK    4
#define PIN_IR        9
#define PIN_LED_DATA  10
#define NUM_LEDS      8     // стартова кількість (ESP32 все одно задає LEDS,n)
#define MAX_LEDS      32

#define LINK_BAUD     115200
#define DEBOUNCE_MS   30
#define LINE_MAX      200
#define MAX_TOKENS    6

Adafruit_NeoPixel led(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------------
//  Стан
// ---------------------------------------------------------------------
struct Btn {
  uint8_t pin;
  const char* name;
  bool state;
  bool lastRaw;
  uint32_t t;
};
Btn btns[] = {
  {PIN_BTN_BACK, "BACK", false, false, 0},
  {PIN_BTN_CONF, "CONF", false, false, 0},
  {PIN_BTN_OK,   "OK",   false, false, 0},
};
const uint8_t NUM_BTNS = sizeof(btns) / sizeof(btns[0]);

struct Line {
  char buf[LINE_MAX];
  uint8_t len;
  bool overflow;
};
Line usbLine = {{0}, 0, false};
Line linkLine = {{0}, 0, false};

uint8_t encPrev = 0;
int8_t encAcc = 0;

uint8_t ledR = 0, ledG = 0, ledB = 0, ledBright = 60;
bool evEnc = true, evBtn = true, evIr = true;
bool ackOn = true;

// ---------------------------------------------------------------------
//  Вивід
// ---------------------------------------------------------------------

// Подія: спочатку на ESP32, у USB — лише якщо там є місце (не блокує)
void emitEvent(const char* s) {
  Serial1.println(s);
  if (Serial.availableForWrite() > (int)strlen(s) + 2) Serial.println(s);
}

void replyf(Print& out, const char* fmt, ...) {
  char b[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  out.println(b);
}

// Збирає текст у буфер (для рядка події IR)
class BufPrint : public Print {
 public:
  char b[48];
  uint8_t n;
  BufPrint() : n(0) { b[0] = 0; }
  size_t write(uint8_t c) override {
    if (n < sizeof(b) - 1) { b[n++] = c; b[n] = 0; }
    return 1;
  }
};

// ---------------------------------------------------------------------
//  Світлодіод
// ---------------------------------------------------------------------
void setLed(uint8_t r, uint8_t g, uint8_t b) {
  ledR = r; ledG = g; ledB = b;
  led.fill(led.Color(r, g, b), 0, led.numPixels());
  led.show();
}

// ---------------------------------------------------------------------
//  Допоміжні функції для піна
// ---------------------------------------------------------------------
bool isFreePin(int p) {
  switch (p) {
    case 5: case 6: case 14: case 15: case 16:
    case 18: case 19: case 20: case 21:
      return true;
  }
  return false;
}

// приймає "5", "14", "A0".."A3"
int parsePin(const char* t) {
  if ((t[0] == 'A') && t[1] >= '0' && t[1] <= '3' && t[2] == 0) return 18 + (t[1] - '0');
  if (t[0] < '0' || t[0] > '9') return -1;
  return atoi(t);
}

bool parseInt(const char* t, int lo, int hi, int& out) {
  if (!t) return false;
  const char* p = t;
  if (*p == '-') p++;
  if (*p == 0) return false;
  for (const char* q = p; *q; q++) if (*q < '0' || *q > '9') return false;
  long v = atol(t);
  if (v < lo || v > hi) return false;
  out = (int)v;
  return true;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ---------------------------------------------------------------------
//  Команди
// ---------------------------------------------------------------------
void printHelp(Print& o) {
  o.println(F("Commands:"));
  o.println(F(" PING | VER | STATUS | RESET | HELP"));
  o.println(F(" LED,r,g,b | LED,OFF | BRIGHT,n"));
  o.println(F(" EVENTS,ENC|BTN|IR,0|1"));
  o.println(F(" PINMODE,n,IN|PULLUP|OUT | DREAD,n | DWRITE,n,0|1"));
  o.println(F(" AREAD,A0..A3 | PWM,5|6,0..255"));
  o.println(F(" LEDS,n | PX,i,r,g,b | FILL,r,g,b | SHOW | FRAME,hex | ACK,0|1"));
  o.println(F("Free pins: 5 6 14 15 16 A0-A3"));
}

void handleCmd(Print& out, char* line) {
  // верхній регістр, пробіли в коми
  for (char* p = line; *p; p++) {
    if (*p == ' ' || *p == '\t') *p = ',';
    else if (*p >= 'a' && *p <= 'z') *p -= 32;
  }
  char* tok[MAX_TOKENS];
  uint8_t n = 0;
  char* save = nullptr;
  for (char* t = strtok_r(line, ",", &save); t && n < MAX_TOKENS; t = strtok_r(nullptr, ",", &save)) {
    tok[n++] = t;
  }
  if (!n) return;

  const char* c = tok[0];

  if (!strcmp(c, "PING")) {
    out.println(F("PONG"));

  } else if (!strcmp(c, "HELP")) {
    printHelp(out);

  } else if (!strcmp(c, "VER")) {
    replyf(out, "VER,PROMICRO,%s", FW_VERSION);

  } else if (!strcmp(c, "STATUS")) {
    replyf(out, "STATUS,UP,%lu,BTN,%d%d%d,LED,%u,%u,%u,BR,%u,EV,%d%d%d",
           millis() / 1000UL,
           btns[0].state, btns[1].state, btns[2].state,
           ledR, ledG, ledB, ledBright,
           evEnc, evBtn, evIr);

  } else if (!strcmp(c, "RESET")) {
    setLed(0, 0, 0);
    evEnc = evBtn = evIr = true;
    out.println(F("OK,RESET"));

  } else if (!strcmp(c, "LED")) {
    if (n == 2 && !strcmp(tok[1], "OFF")) {
      setLed(0, 0, 0);
      if (ackOn) out.println(F("OK,LED,0,0,0"));
    } else {
      int r, g, b;
      if (n == 4 && parseInt(tok[1], 0, 255, r) && parseInt(tok[2], 0, 255, g) && parseInt(tok[3], 0, 255, b)) {
        setLed(r, g, b);
        if (ackOn) replyf(out, "OK,LED,%d,%d,%d", r, g, b);
      } else {
        out.println(F("ERR,LED_FORMAT"));
      }
    }

  } else if (!strcmp(c, "BRIGHT")) {
    int v;
    if (n == 2 && parseInt(tok[1], 0, 255, v)) {
      ledBright = v;
      led.setBrightness(v);
      led.show();
      replyf(out, "OK,BRIGHT,%d", v);
    } else {
      out.println(F("ERR,BRIGHT_FORMAT"));
    }

  } else if (!strcmp(c, "EVENTS")) {
    int v;
    if (n == 3 && parseInt(tok[2], 0, 1, v)) {
      if (!strcmp(tok[1], "ENC"))      evEnc = v;
      else if (!strcmp(tok[1], "BTN")) evBtn = v;
      else if (!strcmp(tok[1], "IR"))  evIr = v;
      else { out.println(F("ERR,EVENTS_TYPE")); return; }
      replyf(out, "OK,EVENTS,%s,%d", tok[1], v);
    } else {
      out.println(F("ERR,EVENTS_FORMAT"));
    }

  } else if (!strcmp(c, "PINMODE")) {
    int p = (n == 3) ? parsePin(tok[1]) : -1;
    if (!isFreePin(p)) { out.println(F("ERR,PIN")); return; }
    if (!strcmp(tok[2], "IN"))          pinMode(p, INPUT);
    else if (!strcmp(tok[2], "PULLUP")) pinMode(p, INPUT_PULLUP);
    else if (!strcmp(tok[2], "OUT"))    pinMode(p, OUTPUT);
    else { out.println(F("ERR,PINMODE_FORMAT")); return; }
    replyf(out, "OK,PINMODE,%d,%s", p, tok[2]);

  } else if (!strcmp(c, "DREAD")) {
    int p = (n == 2) ? parsePin(tok[1]) : -1;
    if (!isFreePin(p)) { out.println(F("ERR,PIN")); return; }
    replyf(out, "DREAD,%d,%d", p, digitalRead(p));

  } else if (!strcmp(c, "DWRITE")) {
    int p = (n == 3) ? parsePin(tok[1]) : -1;
    int v;
    if (!isFreePin(p) || !parseInt(tok[2], 0, 1, v)) { out.println(F("ERR,DWRITE_FORMAT")); return; }
    digitalWrite(p, v);
    replyf(out, "OK,DWRITE,%d,%d", p, v);

  } else if (!strcmp(c, "AREAD")) {
    int p = (n == 2) ? parsePin(tok[1]) : -1;
    if (p < 18 || p > 21) { out.println(F("ERR,PIN")); return; }
    replyf(out, "AREAD,%d,%d", p, analogRead(p));

  } else if (!strcmp(c, "PWM")) {
    int p = (n == 3) ? parsePin(tok[1]) : -1;
    int v;
    if ((p != 5 && p != 6) || !parseInt(tok[2], 0, 255, v)) { out.println(F("ERR,PWM_FORMAT")); return; }
    analogWrite(p, v);
    replyf(out, "OK,PWM,%d,%d", p, v);

  } else if (!strcmp(c, "ACK")) {
    int v;
    if (n == 2 && parseInt(tok[1], 0, 1, v)) {
      ackOn = v;
      replyf(out, "OK,ACK,%d", v);
    } else {
      out.println(F("ERR,ACK_FORMAT"));
    }

  } else if (!strcmp(c, "LEDS")) {
    int v;
    if (n == 2 && parseInt(tok[1], 1, MAX_LEDS, v)) {
      if (v != led.numPixels()) {                 // не чіпаємо купу, якщо кількість та сама
        led.updateLength(v);
        if (led.numPixels() != v) { out.println(F("ERR,LEDS_ALLOC")); return; }
        led.clear();
        led.show();
      }
      replyf(out, "OK,LEDS,%d", v);
    } else {
      out.println(F("ERR,LEDS_FORMAT"));
    }

  } else if (!strcmp(c, "PX")) {
    int i, r, g, b;
    if (n == 5 && parseInt(tok[1], 0, MAX_LEDS - 1, i) && parseInt(tok[2], 0, 255, r) &&
        parseInt(tok[3], 0, 255, g) && parseInt(tok[4], 0, 255, b)) {
      if (i >= led.numPixels()) { out.println(F("ERR,PX_RANGE")); return; }
      led.setPixelColor(i, led.Color(r, g, b));
      if (ackOn) out.println(F("OK,PX"));
    } else {
      out.println(F("ERR,PX_FORMAT"));
    }

  } else if (!strcmp(c, "FILL")) {
    int r, g, b;
    if (n == 4 && parseInt(tok[1], 0, 255, r) && parseInt(tok[2], 0, 255, g) && parseInt(tok[3], 0, 255, b)) {
      led.fill(led.Color(r, g, b), 0, led.numPixels());
      if (ackOn) out.println(F("OK,FILL"));
    } else {
      out.println(F("ERR,FILL_FORMAT"));
    }

  } else if (!strcmp(c, "SHOW")) {
    led.show();
    if (ackOn) out.println(F("OK,SHOW"));

  } else if (!strcmp(c, "FRAME")) {
    uint16_t np = led.numPixels();
    if (n != 2 || strlen(tok[1]) != (size_t)np * 6) { out.println(F("ERR,FRAME_LEN")); return; }
    const char* h = tok[1];
    for (uint16_t i = 0; i < np; i++) {
      int v[6];
      for (uint8_t k = 0; k < 6; k++) {
        v[k] = hexVal(h[i * 6 + k]);
        if (v[k] < 0) { out.println(F("ERR,FRAME_HEX")); return; }
      }
      led.setPixelColor(i, led.Color((v[0] << 4) | v[1], (v[2] << 4) | v[3], (v[4] << 4) | v[5]));
    }
    led.show();
    if (ackOn) out.println(F("OK,FRAME"));

  } else {
    out.println(F("ERR,UNKNOWN"));
  }
}

// ---------------------------------------------------------------------
//  Читання рядків без String (без фрагментації купи)
// ---------------------------------------------------------------------
void readLines(Stream& st, Line& ln) {
  while (st.available()) {
    char ch = st.read();
    if (ch == '\n' || ch == '\r') {
      if (ln.overflow) {
        st.println(F("ERR,TOO_LONG"));
      } else if (ln.len) {
        ln.buf[ln.len] = 0;
        handleCmd(st, ln.buf);
      }
      ln.len = 0;
      ln.overflow = false;
    } else if (ln.len < LINE_MAX - 1) {
      ln.buf[ln.len++] = ch;
    } else {
      ln.overflow = true;
    }
  }
}

// ---------------------------------------------------------------------
//  Енкодер
// ---------------------------------------------------------------------
void pollEncoder() {
  static const int8_t tbl[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
  uint8_t cur = (digitalRead(PIN_ENC_TRA) << 1) | digitalRead(PIN_ENC_TRB);
  if (cur == (encPrev & 3)) return;
  encPrev = ((encPrev << 2) | cur) & 0x0F;
  encAcc += tbl[encPrev];
  if (encAcc >= 4)       { encAcc = 0; if (evEnc) emitEvent("ENC,+1"); }
  else if (encAcc <= -4) { encAcc = 0; if (evEnc) emitEvent("ENC,-1"); }
}

// ---------------------------------------------------------------------
//  Кнопки
// ---------------------------------------------------------------------
void pollButtons() {
  for (uint8_t i = 0; i < NUM_BTNS; i++) {
    Btn& b = btns[i];
    bool raw = (digitalRead(b.pin) == LOW);
    if (raw != b.lastRaw) { b.lastRaw = raw; b.t = millis(); }
    if (millis() - b.t > DEBOUNCE_MS && raw != b.state) {
      b.state = raw;
      if (evBtn) {
        char m[24];
        snprintf(m, sizeof(m), "BTN,%s,%d", b.name, raw ? 1 : 0);
        emitEvent(m);
      }
    }
  }
}

// ---------------------------------------------------------------------
//  ІЧ-приймач
// ---------------------------------------------------------------------
#if ENABLE_IR
void pollIR() {
  if (!IrReceiver.decode()) return;
  auto& d = IrReceiver.decodedIRData;
  static uint32_t lastIr = 0;

  if (evIr && (millis() - lastIr > IR_MIN_GAP_MS)) {
    if (d.protocol == UNKNOWN) {
#if IR_REPORT_UNKNOWN
      lastIr = millis();
      emitEvent("IR,UNKNOWN");
#endif
    } else if (!(d.flags & IRDATA_FLAGS_IS_REPEAT)) {
      lastIr = millis();
      BufPrint bp;
      bp.print(F("IR,"));
      bp.print(getProtocolString(d.protocol));
      bp.print(F(",0x"));
      bp.print((uint32_t)d.address, HEX);
      bp.print(F(",0x"));
      bp.print((uint32_t)d.command, HEX);
      emitEvent(bp.b);
    }
  }
  IrReceiver.resume();
}
#else
void pollIR() {}
#endif

// ---------------------------------------------------------------------
//  setup / loop
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);   // USB CDC, не чекаємо на підключення
  Serial1.begin(LINK_BAUD);

  pinMode(PIN_ENC_TRA, INPUT_PULLUP);
  pinMode(PIN_ENC_TRB, INPUT_PULLUP);
  for (uint8_t i = 0; i < NUM_BTNS; i++) {
    pinMode(btns[i].pin, INPUT_PULLUP);
    btns[i].state = btns[i].lastRaw = (digitalRead(btns[i].pin) == LOW);   // без хибних подій при старті
  }

  led.begin();
  led.setBrightness(ledBright);
  setLed(255, 0, 0); delay(120);
  setLed(0, 255, 0); delay(120);
  setLed(0, 0, 255); delay(120);
  setLed(0, 0, 0);

#if ENABLE_IR
  IrReceiver.begin(PIN_IR, DISABLE_LED_FEEDBACK);
#endif
  encPrev = (digitalRead(PIN_ENC_TRA) << 1) | digitalRead(PIN_ENC_TRB);

  char m[32];
  snprintf(m, sizeof(m), "READY,PROMICRO,%s", FW_VERSION);
  emitEvent(m);
}

void loop() {
  pollEncoder();
  pollButtons();
  pollIR();
  readLines(Serial, usbLine);
  readLines(Serial1, linkLine);
}