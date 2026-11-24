/**
 * =====================================================================
 *  ESP32 контролер освітлення (SH1106 128x64, INA3221, фоторезистор, 3 реле,
 *  адресна LED-стрічка і кнопки/енкодер/ІЧ через Pro Micro по UART)
 *
 *  Екрани (енкодер гортає): ГОЛОВНИЙ -> РЕЛЕ -> МЕНЮ
 *    ГОЛОВНИЙ — час, погода, 2 АКБ, лінія 5V, є світло чи нема, стан реле, LDR. OK — режим RGB
 *    РЕЛЕ     — OK зайти, крутилка — вибір реле, OK — вибрати; далі OK — режим
 *               Авто/Вимк/Увімк, крутилка — поріг, BACK — зберегти і вийти
 *    МЕНЮ     — Settings / Info / Wifi / Weather / Light / Power (той самий принцип)
 *  Скрінсейвер — після бездіяльності (час, дата, погода, галактика на фоні).
 *
 *  Кнопки (Pro Micro -> UART2):
 *    CONF  — ВСЕ ВИМКНУТИ і заставка; ще раз — повернути. Поки вимкнено, діють лише CONF
 *            і кнопка пульта 0x45. Стан переживає перезапуск.
 *    ІЧ-пульт: 0x45 — увімкнути все, 0x47 — вимкнути все.
 *    Глибокий сон (меню Power або веб) — вимкнено все, і екран, і Wi-Fi; увімкнути — утримати OK 2 с.
 *
 *  Веб-меню: http://esp32-test.local (або IP), через власну точку доступу — http://192.168.4.1.
 *    Усі налаштування, стани, прогноз на 7 днів, вибір мережі Wi-Fi.
 *    Нема мережі (не задана або недоступна 2 хв) — сама вмикається тимчасова точка доступу.
 *
 *  Паролі й координати — include/secrets.h (шаблон: include/secrets.example.h).
 *  ВАЖЛИВО: фоторезистор не має дивитись прямо на лампу, яку він вмикає.
 * =====================================================================
 */

// =====================================================================
//  ★ ПЕРЕМИКАЧ РЕЖИМУ ДІАГНОСТИКИ ★
//    1 = діагностика, 0 = звичайна робота
// =====================================================================
#define DEBUG_MODE             0

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <esp_task_wdt.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <stdarg.h>
#include "web_page.h"

// =====================================================================
//  1. НАЛАШТУВАННЯ
// =====================================================================

// --- Мережа ---
// Паролі й координати — у include/secrets.h (не в git). Шаблон: include/secrets.example.h
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#define OTA_HOST               "esp32-test"
#define AP_TEMP_AFTER_MS       120000   // нема Wi-Fi 2 хв (або мережа не задана) -> тимчасова точка доступу для налаштування
#define DEEP_WAKE_HOLD_MS      2000     // глибокий сон: стільки тримати OK, щоб увімкнути
#define WIFI_RETRY_MS          15000    // як часто (мс) повторювати підключення, поки нема Wi-Fi
#define NTP_SERVER             "pool.ntp.org"
#define TZ_INFO                "EET-2EEST,M3.5.0/3,M10.5.0/4"

// --- Погода (Open-Meteo, без ключів) ---
#define WEATHER_PERIOD_MS      (10UL * 60UL * 1000UL)
#define WEATHER_LIVE_MS        (20UL * 60UL * 1000UL)   // скільки "живі" дані вважаються свіжими; далі — з прогнозу
#define FC_PERIOD_S            (3UL * 3600UL)           // прогноз на 7 днів — раз на 3 години (і зберігається у флеш)
#define FC_RETRY_MS            (5UL * 60UL * 1000UL)    // якщо прогноз не скачався — повтор через 5 хв

// --- Реле через NPN-транзистор (HIGH на піні = реле увімкнене) ---
#define RELAY1_NAME            "RGB під столом"
#define RELAY2_NAME            "Верхня LED"
#define RELAY3_NAME            "Гірлянда"     // на головному екрані — "GRL"
#define RELAY_ACTIVE_LOW       0
// 1 = вихід open-drain (потрібен з діодом: анод до IN реле, катод до піна ESP32);
// 0 = звичайний OUTPUT (без діода)
#define RELAY_OPEN_DRAIN       0

// --- Батареї (INA3221). Який канал мікросхеми (1, 2 або 3) на якій АКБ — міняй тут ---
#define BAT1_INA_CH            2
#define BAT2_INA_CH            3
#define LINE_INA_CH            1      // канал з лінією 5V
#define BAT1_LABEL             "РЕЗ.АКУМ"   // верхня на головному екрані
#define BAT2_LABEL             "ОСН.АКУМ"   // нижня
// Поправка напруги, мВ (додається до показу кожного каналу окремо; можна й від'ємну)
#define BAT1_OFFSET_MV         110
#define BAT2_OFFSET_MV         10
#define LINE_OFFSET_MV         0        // поправка для лінії 5V
#define BAT_MIN_V              2.0f     // нижче = АКБ не підключена, показуємо "--"

// --- Датчик наявності світла в домі (>= 1 В = світло є) ---
#define PIN_POWER_DETECT       35
#define POWER_DETECT_MV        1000

// --- Фоторезистор ---
#define PIN_LDR                34
#define LDR_MIN_RANGE_MV       400      // менший діапазон = ще не відкалібровано
#define LDR_ABSENT_MV          100      // нижче цього довше LDR_ABSENT_MS = фотоопір не підключено (у темряві LDR дає <500 мВ, тож поріг низький)
#define LDR_ABSENT_MS          10000
#define LDR_UNCAL_MAX_MV       3300     // шкала, поки калібрування не набрало даних
#define DEF_THR_ON_PM          300      // за замовч.: вмикати нижче 30.0% діапазону
#define DEF_THR_OFF_PM         450      // за замовч.: вимикати вище 45.0% діапазону
#define THR_HYST_PM            20       // гістерезис: вмикається на 2.0% нижче порогу вимкнення

// --- Сонце за прогнозом (Open-Meteo: схід/захід) ---
// Вдень (від сходу до заходу) реле в режимі "Авто" НЕ вмикається і фоторезистор ігнорується.
// Від заходу до наступного сходу працює фоторезистор. Нема даних із мережі — працює тільки фоторезистор.
#define DEF_SUN_GATE           1        // за замовч. функцію увімкнено
#define SUN_GATE_RELAYS        0x07     // до яких реле застосовувати: біт0 = Р1, біт1 = Р2, біт2 = Р3
#define SUN_DATA_MAX_AGE_MS    (36UL * 3600UL * 1000UL)   // скільки діють старі дані, якщо нема інтернету
#define DEF_SWITCH_DELAY_SEC   8        // стан має триматись стільки секунд

// --- Нічне вимкнення (тільки режим "Авто") ---
#define NIGHT_OFF_RELAYS       0x01     // до яких реле застосовувати: біт0 = Р1, біт1 = Р2, біт2 = Р3
#define DEF_NIGHT_OFF          0        // за замовч.: 1 = увімкнено, 0 = вимкнено
#define DEF_NIGHT_OFF_HOUR     0        // година вимкнення (0 = опівночі)
#define NIGHT_RESET_MIN        30       // скільки хвилин має бути світло, щоб скинути нічне вимкнення

// --- Світлодіодна стрічка (Pro Micro). Ефекти рахує ESP32, Pro Micro отримує "LED,r,g,b" ---
#define LED_ENABLE             1
#define LED_COUNT              8
#define LED_DEF_BRIGHT_PCT     47       // за замовч. яскравість, %
#define LED_ANIM_MS            80       // період оновлення кольору для анімованих ефектів
#define LED_EDIT_GAP_MS        40       // мін. пауза між командами при зміні яскравості/ефекту (щоб не забивати Pro Micro)

// --- Яскравість екрана, % (за замовч.) ---
#define DEF_SCR_BRIGHT_PCT     100      // звичайна робота
#define DEF_SAVER_BRIGHT_PCT   4        // скрінсейвер
#define DEF_SAVER_OFF_PCT      0        // скрінсейвер, коли "все вимкнено" (CONF)

// --- Піни ---
#define UART_RX_PIN            16
#define UART_TX_PIN            17
#define LINK_BAUD              115200
#define I2C_SDA_PIN            21
#define I2C_SCL_PIN            22
#define PIN_RELAY1             25
#define PIN_RELAY2             26
#define PIN_RELAY3             27       // третє реле (гірлянда)
#define NUM_RELAYS             3

#define DEFAULT_SLEEP_SEC      15
#define TRANS_MS               240      // тривалість анімації переходу між екранами
#define DEBUG_LINK             DEBUG_MODE

// Сторожовий таймер: якщо loop() завис довше WDT_TIMEOUT_S — плата сама перезавантажиться
#define WDT_ENABLE             1
#define WDT_TIMEOUT_S          20

// --- ІЧ-пульт (NEC): повне вимкнення / увімкнення, як кнопка CONF ---
#define IR_CMD_ALL_ON          0x45     // кнопка "увімкнути все"
#define IR_CMD_ALL_OFF         0x47     // кнопка "вимкнути все"


// =====================================================================
//  2. СТАН
// =====================================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
Preferences prefs;

#define FONT_S u8g2_font_5x8_t_cyrillic
#define FONT_M u8g2_font_6x12_t_cyrillic
#define FONT_L u8g2_font_logisoso16_tn

enum RelayMode : uint8_t { MODE_AUTO = 0, MODE_OFF, MODE_ON, MODE_COUNT };
static const char* const MODE_NAMES[] = {"Авто", "Вимк", "Увімк"};
static const char MODE_CHAR[]         = {'A', '-', '+'};

enum Page : uint8_t {
  P_MAIN = 0, P_REL, P_MENU,  // три екрани: головний, реле, меню
  P_COUNT
};

static const uint8_t      RELAY_PIN[NUM_RELAYS]  = {PIN_RELAY1, PIN_RELAY2, PIN_RELAY3};
static const char* const  RELAY_NAME[NUM_RELAYS] = {RELAY1_NAME, RELAY2_NAME, RELAY3_NAME};
static const char* const  RELAY_SHORT[NUM_RELAYS] = {"RGB", "LED", "GRL"};
#define RELAY_ON_LEVEL   (RELAY_ACTIVE_LOW ? LOW : HIGH)
#define RELAY_OFF_LEVEL  (RELAY_ACTIVE_LOW ? HIGH : LOW)

// Налаштування (зберігаються в пам'яті)
static uint8_t  relayMode[NUM_RELAYS]  = {MODE_AUTO, MODE_OFF, MODE_OFF};
static uint16_t sleepSec      = DEFAULT_SLEEP_SEC;
// поріг вмикання не зберігається: рахується з порогу вимкнення мінус гістерезис
static uint16_t thrOff[NUM_RELAYS]     = {DEF_THR_OFF_PM, DEF_THR_OFF_PM, DEF_THR_OFF_PM};   // ЄДИНИЙ поріг (проміле діапазону LDR): вище = вимкнено
static inline uint16_t thrOnPm(int i) { return thrOff[i] - THR_HYST_PM; }
static uint16_t switchDelaySec = DEF_SWITCH_DELAY_SEC;
static bool     calAuto       = true;     // true = діапазон LDR підлаштовується сам
static bool     sunGate       = DEF_SUN_GATE;   // true = блокувати вмикання вдень за прогнозом
static bool     nightOff      = DEF_NIGHT_OFF;  // true = нічне вимкнення Авто-реле
static uint8_t  nightHour     = DEF_NIGHT_OFF_HOUR;
static bool     nightLatch[NUM_RELAYS] = {false, false, false}; // true = реле вже вимкнене на цю ніч

// LED-стрічка: mode = MODE_AUTO/OFF/ON. "Увімк" = стан День (brN/fxN).
// "Авто": коли світло — День (brN/fxN), коли темно — Темно (brD/fxD). Яскравість 0 = вимкнено.
// "Темно" визначає фоторезистор (як для Реле 1); якщо його не підключено (<1 В) — схід/захід за прогнозом.
enum LedFx : uint8_t { FX_WARM = 0, FX_COOL, FX_BREATH, FX_RAINBOW, FX_FIRE,
                       FX_RED, FX_GREEN, FX_BLUE, FX_PURPLE, FX_OCEAN, FX_SUNSET, FX_PULSE, FX_AURORA, FX_COUNT };
static const char* const FX_NAMES[] = {"Тепле", "Холодне", "Дихання", "Веселка", "Вогонь",
                                       "Червоне", "Зелене", "Синє", "Фіолет", "Океан", "Захід", "Пульс", "Сяйво"};
struct LedCfg { uint8_t mode, brN, fxN, brD, fxD; };
static LedCfg ledCfg  = {MODE_AUTO, LED_DEF_BRIGHT_PCT, FX_WARM, LED_DEF_BRIGHT_PCT, FX_WARM};

// Яскравість екрана, %: [0] звичайна, [1] скрінсейвер, [2] скрінсейвер при "все вимк"
static uint8_t scrBr[3]   = {DEF_SCR_BRIGHT_PCT, DEF_SAVER_BRIGHT_PCT, DEF_SAVER_OFF_PCT};

// Реле
static bool relayOn[NUM_RELAYS] = {false, false, false};
static bool allOff     = false;           // "все вимкнено" (CONF або пульт): жорстко, зберігається в пам'яті

// Фоторезистор
static float ldrMv = 0, ldrLo = 0, ldrHi = 0;
static float ldrFast = -1;                      // слабко згладжений LDR — для слова "Темно/Світло..." (швидка реакція)
static bool  ldrReady = false;
static bool  ldrAbsent = false;                 // true = фоторезистор не підключено
static float savedLo = 0, savedHi = 0;
static bool     relayDark[NUM_RELAYS] = {false, false, false};   // умова "темно" для кожного реле окремо
static bool     pending[NUM_RELAYS]   = {false, false, false};
static uint32_t pendT[NUM_RELAYS]     = {0, 0, 0};
static uint16_t pendLeft[NUM_RELAYS]  = {0, 0, 0};

// Інші датчики
static uint32_t gridMv   = 0;
static uint8_t  inaAddr  = 0;
static float    batV[3]  = {-1, -1, -1};   // [0] АКБ 1, [1] АКБ 2, [2] лінія 5V
static bool     batOk    = false;

// Погода (оновлюється в окремій задачі)
struct Weather {
  volatile float    temp;
  volatile uint8_t  hum;
  volatile int      code;
  volatile bool     ok;
  volatile uint32_t at;
  volatile float    feel;
  volatile float    wind;
};
static Weather wx = {0, 0, 0, false, 0, 0, 0};
static volatile int wxStatus = 0;      // останній HTTP-код (-100 = не розібрано, -101 = не відкрилось)
static volatile int16_t  sunRiseMin = -1, sunSetMin = -1;   // хвилини від півночі (місцевий час)
static volatile uint32_t sunAt = 0;
static volatile bool     sunOk = false;

// Годинник без інтернету: похибка кварцу (див. clockKeep)
static volatile int64_t ntpMono = 0, ntpEpochUs = 0;   // монотонний час і точний час останньої синхронізації
static volatile float   driftPpm = 0;
static volatile bool    driftOk = false, driftDirty = false;
static int64_t          corrMono = 0;                   // коли востаннє підправляли

// Інтерфейс
static bool     oledOk = false, otaBusy = false;
static bool     asleep = false, dirty = true;
static uint32_t lastActivity = 0;
static uint8_t  page = P_MAIN, fromPage = P_MAIN;
static int8_t   transDir = 0;
static float    transT = 1.0f;
static uint32_t transStart = 0;

// Правка (екрани порогів і налаштувань)
static bool     editing = false;
// Екран реле: relLevel 0 = екран показано (крутилка гортає екрани), 1 = всередині, вибір реле,
// 2 = реле вибране (OK — режим, крутилка — поріг, BACK — зберегти і вийти). editing == (relLevel == 2)
static uint8_t  relLevel = 0;
static uint8_t  relSel   = 0;

// Третій екран — меню (сітка 3x2). menuLevel: 0 = показано (крутилка гортає екрани),
// 1 = вибір пункту, 2 = відкрито підменю
static uint8_t  menuLevel = 0, menuSel = 0;
static float    menuR[4] = {0, 0, 128, 64};   // рамка-курсор меню (плавно перетікає)
static float    wxSelF = 1;                 // виділена картка погоди (плавно)
static bool     wxDetClose = false;         // день прогнозу закривається (анімація)
static int8_t   subAnimDir = 0;            // +1 підменю відкривається, -1 закривається
static uint32_t subAnimT0 = 0;
static uint8_t  subSel = 0;
static float    subScroll = 0, subBarY = -1;
static bool     subEdit = false;
static uint8_t  wxCard = 1;                // 0 = "Оновити", 1.. = дні
static float    wxCardX = 0;
static bool     wxDetail = false;
static uint8_t  wxHour = 12;
static float    wxDetT = 0;
static uint8_t  pwSel = 0, pwAct = 0;      // pwAct: 1 = відлік перезапуску, 2 = глибокого сну
static float    pwCurX = -1;
static uint32_t pwActT = 0;
static char     toastMsg[28] = "";
static uint32_t toastT = 0;
static int8_t   ledPreview = -1;           // правка LED у меню: 0 = показати "День", 1 = "Темно"
static bool     deepOff = false;           // глибокий сон: усе вимкнено, увімкнути — утримати OK 2 с
static bool     okHeld = false;
static uint32_t okDownT = 0;
static bool     apOn = false;              // власна точка доступу ESP32 (увімкнена користувачем)
static bool     apTemp = false;            // тимчасова точка: мережа не задана або недоступна
static char     netSsid[33] = WIFI_SSID;   // мережа, до якої підключаємось (вибирається у веб-меню)
static char     netPass[65] = WIFI_PASS;
static uint32_t netApplyAt = 0;            // відкладене перепідключення після зміни мережі
static volatile bool wxForce = false, wxBusy = false;

// Плавні значення для малювання
static float frameDt  = 0.016f;
static float ldrShown = -1;

static char    lineBuf[64];
static uint8_t lineLen = 0;

// Зв'язок з Pro Micro (для діагностики)
static uint32_t rxBytes = 0, lastRxMs = 0;
static uint16_t pingSent = 0, pongRcv = 0, evCount = 0;
static char     lastEvt[22] = "-";
static char     proVer[12]  = "";
static char     proBtn[4]   = "", proEv[4] = "";
static bool     ledForce = true;

// =====================================================================
//  3. ДОПОМІЖНІ ФУНКЦІЇ МАЛЮВАННЯ
// =====================================================================

static void txt(const uint8_t* f, int x, int y, const char* s) {
  oled.setFont(f);
  oled.drawUTF8(x, y, s);
}
static int txtW(const uint8_t* f, const char* s) {
  oled.setFont(f);
  return oled.getUTF8Width(s);
}
static void txtR(const uint8_t* f, int xr, int y, const char* s) { txt(f, xr - txtW(f, s), y, s); }
static void txtC(const uint8_t* f, int xc, int y, const char* s) { txt(f, xc - txtW(f, s) / 2, y, s); }

static bool localNow(struct tm& t) {
  time_t n = time(nullptr);
  if (n < 1700000000) return false;   // NTP ще не синхронізовано
  localtime_r(&n, &t);
  return true;
}

// Рядок "ліве ........ праве"; при hl = true — інвертований (рядок правки)
static void row(int ox, int y, bool hl, const char* l, const char* r) {
  if (hl) {
    oled.drawBox(ox + 1, y - 8, 126, 10);
    oled.setDrawColor(2);                 // XOR: текст на білому стає чорним
  }
  txt(FONT_S, ox + 3, y, l);
  txtR(FONT_S, ox + 125, y, r);
  oled.setDrawColor(1);
}

// =====================================================================
//  4. ЗБЕРЕЖЕННЯ НАЛАШТУВАНЬ
// =====================================================================

static void fixThresholds(int i) {
  thrOff[i] = constrain((int)thrOff[i], 10 + THR_HYST_PM, 990);
}

static void loadPrefs() {
  prefs.begin("ctl", false);
  relayMode[0]   = prefs.getUChar("m0", MODE_AUTO);
  relayMode[1]   = prefs.getUChar("m1", MODE_OFF);
  relayMode[2]   = prefs.getUChar("m2", MODE_OFF);
  sleepSec       = prefs.getUShort("sleep", DEFAULT_SLEEP_SEC);
  thrOff[0]      = prefs.getUShort("off0", DEF_THR_OFF_PM);
  thrOff[1]      = prefs.getUShort("off1", DEF_THR_OFF_PM);
  thrOff[2]      = prefs.getUShort("off2", DEF_THR_OFF_PM);
  switchDelaySec = prefs.getUShort("dly",  DEF_SWITCH_DELAY_SEC);
  calAuto        = prefs.getBool("cal", true);
  sunGate        = prefs.getBool("sun", DEF_SUN_GATE);
  nightOff       = prefs.getBool("nof", DEF_NIGHT_OFF);
  nightHour      = prefs.getUChar("noh", DEF_NIGHT_OFF_HOUR);
  ldrLo          = prefs.getFloat("lo", 0);
  ldrHi          = prefs.getFloat("hi", 0);
  ledCfg.mode    = prefs.getUChar("lmode", MODE_AUTO);
  ledCfg.brN     = prefs.getUChar("lbrn",  LED_DEF_BRIGHT_PCT);
  ledCfg.fxN     = prefs.getUChar("lfxn",  FX_WARM);
  ledCfg.brD     = prefs.getUChar("lbrd",  LED_DEF_BRIGHT_PCT);
  ledCfg.fxD     = prefs.getUChar("lfxd",  FX_WARM);
  scrBr[0]       = prefs.getUChar("scr0",  DEF_SCR_BRIGHT_PCT);
  scrBr[1]       = prefs.getUChar("scr1",  DEF_SAVER_BRIGHT_PCT);
  scrBr[2]       = prefs.getUChar("scr2",  DEF_SAVER_OFF_PCT);
  allOff         = prefs.getBool("aoff", false);
  apOn           = prefs.getBool("ap", false);
  if (prefs.isKey("wssid")) {                 // мережа, вибрана у веб-меню, важливіша за вбудовану
    prefs.getString("wssid", netSsid, sizeof(netSsid));
    prefs.getString("wpass", netPass, sizeof(netPass));
  }
  { float pp = prefs.getFloat("ppm", NAN); if (!isnan(pp) && fabsf(pp) < 150.0f) { driftPpm = pp; driftOk = true; } }
  prefs.end();
  asleep         = allOff;       // жорстке вимкнення переживає перезапуск: стартуємо вимкненими
  if (ledCfg.mode >= MODE_COUNT) ledCfg.mode = MODE_AUTO;
  if (ledCfg.fxN  >= FX_COUNT)   ledCfg.fxN  = FX_WARM;
  if (ledCfg.fxD  >= FX_COUNT)   ledCfg.fxD  = FX_WARM;
  if (ledCfg.brN  > 100) ledCfg.brN = 100;
  if (ledCfg.brD  > 100) ledCfg.brD = 100;
  scrBr[0] = constrain((int)scrBr[0], 5, 100);      // звичайна яскравість не менше 5%, щоб не "втратити" екран
  scrBr[1] = constrain((int)scrBr[1], 0, 100);
  scrBr[2] = constrain((int)scrBr[2], 0, 100);
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (relayMode[i] >= MODE_COUNT) relayMode[i] = MODE_OFF;
    fixThresholds(i);
  }
  if (nightHour > 23) nightHour = DEF_NIGHT_OFF_HOUR;
  sleepSec       = constrain(sleepSec, 10, 600);
  switchDelaySec = constrain(switchDelaySec, 1, 120);
  savedLo = ldrLo;
  savedHi = ldrHi;
}

static void saveSettings() {
  prefs.begin("ctl", false);
  prefs.putUChar("m0", relayMode[0]);
  prefs.putUChar("m1", relayMode[1]);
  prefs.putUChar("m2", relayMode[2]);
  prefs.putUShort("sleep", sleepSec);
  prefs.putUShort("off0", thrOff[0]);
  prefs.putUShort("off1", thrOff[1]);
  prefs.putUShort("off2", thrOff[2]);
  prefs.putUShort("dly",  switchDelaySec);
  prefs.putBool("cal", calAuto);
  prefs.putBool("sun", sunGate);
  prefs.putBool("nof", nightOff);
  prefs.putUChar("noh", nightHour);
  prefs.putUChar("lmode", ledCfg.mode);
  prefs.putUChar("lbrn",  ledCfg.brN);
  prefs.putUChar("lfxn",  ledCfg.fxN);
  prefs.putUChar("lbrd",  ledCfg.brD);
  prefs.putUChar("lfxd",  ledCfg.fxD);
  prefs.putUChar("scr0",  scrBr[0]);
  prefs.putUChar("scr1",  scrBr[1]);
  prefs.putUChar("scr2",  scrBr[2]);
  prefs.end();
}

static void saveCalib() {
  prefs.begin("ctl", false);
  prefs.putFloat("lo", ldrLo);
  prefs.putFloat("hi", ldrHi);
  prefs.end();
  savedLo = ldrLo;
  savedHi = ldrHi;
}

// =====================================================================
//  5. ПОГОДА (окрема задача на ядрі 0, щоб TLS не гальмував інтерфейс)
// =====================================================================

static bool jsonNum(const String& s, int from, const char* key, float& out) {
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  int i = s.indexOf(pat, from);
  if (i < 0) return false;
  out = atof(s.c_str() + i + strlen(pat));
  return true;
}

// ---- Прогноз на 7 днів: погодинно + схід/захід. Зберігається у флеші (Preferences "wx"),
// тож без інтернету погода береться з нього, навіть після перезапуску (якщо час уже відомий з NTP).
#define FC_HOURS  168
#define FC_DAYS   7
#define FC_MAGIC  0x46433733UL                 // "FC73": версія формату
struct Forecast {
  uint32_t magic;
  uint32_t start;                              // unix-час першої години
  int8_t   temp[FC_HOURS];                     // °C, -128 = нема
  uint8_t  code[FC_HOURS];                     // WMO, 255 = нема
  uint8_t  pop[FC_HOURS];                      // ймовірність опадів, %, 255 = нема
  uint8_t  hum[FC_HOURS];                      // вологість, %
  uint8_t  wind[FC_HOURS];                     // вітер, км/год
  uint32_t rise[FC_DAYS], set[FC_DAYS];        // unix-час сходу/заходу
  uint32_t fetched;                            // unix-час, коли прогноз скачано
};
static Forecast  fc;                           // пише і читає тільки задача погоди (+ setup до її запуску)
static bool      fcOk = false;
static uint32_t  fcTryMs = 0;                  // остання спроба скачати прогноз (millis)
static bool      fcTried = false;
static volatile uint32_t wxLiveAt = 0;         // коли востаннє прийшли "живі" дані
static volatile bool     wxFromCache = false;
static Preferences wxPrefs;
static portMUX_TYPE fcMux = portMUX_INITIALIZER_UNLOCKED;   // fc читає інтерфейс на іншому ядрі
static volatile uint32_t fcVer = 0;                    // окремий об'єкт: prefs використовує loop() на іншому ядрі

static void fcLoad() {
  if (!wxPrefs.begin("wx", true)) return;
  size_t n = wxPrefs.getBytes("fc", &fc, sizeof(fc));
  wxPrefs.end();
  fcOk = (n == sizeof(fc) && fc.magic == FC_MAGIC);
  fcVer++;
}

static void fcSave() {
  if (!wxPrefs.begin("wx", false)) return;
  wxPrefs.putBytes("fc", &fc, sizeof(fc));
  wxPrefs.end();
}

// Масив чисел "key":[1,2,null,...] після позиції from. Повертає кількість (null -> NAN)
static int jsonArr(const String& s, int from, const char* key, double* out, int maxN) {
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\":[", key);
  int i = s.indexOf(pat, from);
  if (i < 0) return 0;
  const char* c = s.c_str() + i + strlen(pat);
  int n = 0;
  while (*c && n < maxN) {
    while (*c == ' ' || *c == ',') c++;
    if (*c == ']' || !*c) break;
    if (!strncmp(c, "null", 4)) { out[n++] = NAN; c += 4; continue; }
    char* e;
    double v = strtod(c, &e);
    if (e == c) break;
    out[n++] = v;
    c = e;
  }
  return n;
}

// Схід/захід на сьогодні і (якщо живі дані застаріли) погода поточної години — з прогнозу
static void fcApply() {
  if (!fcOk) return;
  time_t n = time(nullptr);
  if (n < 1700000000) return;                  // час ще невідомий
  struct tm now;
  localtime_r(&n, &now);
  for (int d = 0; d < FC_DAYS; d++) {
    if (!fc.rise[d] || !fc.set[d]) continue;
    time_t r = fc.rise[d], st = fc.set[d];
    struct tm tr, ts;
    localtime_r(&r, &tr);
    localtime_r(&st, &ts);
    if (tr.tm_yday != now.tm_yday || tr.tm_year != now.tm_year) continue;
    int rm = tr.tm_hour * 60 + tr.tm_min, sm = ts.tm_hour * 60 + ts.tm_min;
    if (sm > rm) { sunRiseMin = rm; sunSetMin = sm; sunAt = millis(); sunOk = true; }
    break;
  }
  bool liveFresh = wxLiveAt && (millis() - wxLiveAt) < WEATHER_LIVE_MS;
  if (liveFresh || (uint32_t)n < fc.start) return;
  uint32_t idx = ((uint32_t)n - fc.start) / 3600UL;
  if (idx >= FC_HOURS || fc.code[idx] == 255 || fc.temp[idx] == -128) return;
  wx.temp = fc.temp[idx];
  wx.code = fc.code[idx];
  wx.at   = millis();
  wx.ok   = true;
  wxFromCache = true;
}

// GET через HTTPS; тіло відповіді в out. Повертає HTTP-код (або <0)
static int httpsGet(const char* url, String& out) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  if (!http.begin(client, url)) return -101;
  int code = http.GET();
  if (code == HTTP_CODE_OK) out = http.getString();
  http.end();
  return code;
}

// Поточна погода: маленький швидкий запит (раз на 10 хв)
static bool fetchCurrent() {
  char url[220];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,apparent_temperature,wind_speed_10m&timezone=auto",
           WEATHER_LAT, WEATHER_LON);
  String p;
  int code = httpsGet(url, p);
  wxStatus = code;
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    int cur = p.indexOf("\"current\":{");
    float t, h, c;
    if (cur >= 0 &&
        jsonNum(p, cur, "temperature_2m", t) &&
        jsonNum(p, cur, "relative_humidity_2m", h) &&
        jsonNum(p, cur, "weather_code", c)) {
      wx.temp = t;
      wx.hum  = (uint8_t)constrain((int)h, 0, 100);
      wx.code = (int)c;
      float fl, wd;
      wx.feel = jsonNum(p, cur, "apparent_temperature", fl) ? fl : t;
      wx.wind = jsonNum(p, cur, "wind_speed_10m", wd) ? wd : 0;
      wx.at   = millis();
      wx.ok   = true;
      wxLiveAt = millis();
      wxFromCache = false;
      ok = true;
    } else {
      wxStatus = -100;
    }
  }
  Serial.printf("[WX] current HTTP %d %s\n", code, ok ? "ok" : "fail");
  return ok;
}

// Прогноз на 7 днів (раз на 3 години): погодинно + схід/захід, одразу у флеш
static bool fetchForecast() {
  char url[340];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&hourly=temperature_2m,weather_code,precipitation_probability,relative_humidity_2m,wind_speed_10m&daily=sunrise,sunset"
           "&forecast_days=7&timezone=auto&timeformat=unixtime",
           WEATHER_LAT, WEATHER_LON);
  String p;
  int code = httpsGet(url, p);
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    static double buf[FC_HOURS];               // static: не тримаємо 1.3 КБ на стеку задачі
    int hi = p.indexOf("\"hourly\":{");
    int di = p.indexOf("\"daily\":{");
    if (hi >= 0 && di >= 0) {
      static Forecast nf;                      // static: не на стеку задачі
      memset(&nf, 0, sizeof(nf));
      nf.magic = FC_MAGIC;
      bool good = jsonArr(p, hi, "time", buf, 1) == 1;
      nf.start = good ? (uint32_t)buf[0] : 0;
      int nt = jsonArr(p, hi, "temperature_2m", buf, FC_HOURS);
      for (int i = 0; i < FC_HOURS; i++) nf.temp[i] = (i < nt && !isnan(buf[i])) ? (int8_t)constrain((int)lround(buf[i]), -60, 60) : -128;
      int nc = jsonArr(p, hi, "weather_code", buf, FC_HOURS);
      for (int i = 0; i < FC_HOURS; i++) nf.code[i] = (i < nc && !isnan(buf[i])) ? (uint8_t)constrain((int)buf[i], 0, 254) : 255;
      auto u8arr = [&](const char* key, uint8_t* out) {
        int k = jsonArr(p, hi, key, buf, FC_HOURS);
        for (int i = 0; i < FC_HOURS; i++) out[i] = (i < k && !isnan(buf[i])) ? (uint8_t)constrain((int)lround(buf[i]), 0, 254) : 255;
      };
      u8arr("precipitation_probability", nf.pop);
      u8arr("relative_humidity_2m", nf.hum);
      u8arr("wind_speed_10m", nf.wind);
      int nr = jsonArr(p, di, "sunrise", buf, FC_DAYS);
      for (int i = 0; i < FC_DAYS; i++) nf.rise[i] = (i < nr && !isnan(buf[i])) ? (uint32_t)buf[i] : 0;
      int ns = jsonArr(p, di, "sunset", buf, FC_DAYS);
      for (int i = 0; i < FC_DAYS; i++) nf.set[i] = (i < ns && !isnan(buf[i])) ? (uint32_t)buf[i] : 0;
      time_t n = time(nullptr);
      nf.fetched = (n > 1700000000) ? (uint32_t)n : 0;
      if (good && nt >= 24 && nc >= 24) {
        portENTER_CRITICAL(&fcMux);
        fc = nf;
        fcOk = true;
        fcVer++;
        portEXIT_CRITICAL(&fcMux);
        fcSave();
        ok = true;
      }
      Serial.printf("[WX] forecast %d h, %d days\n", nt, nr);
    }
  }
  Serial.printf("[WX] forecast HTTP %d %s\n", code, ok ? "ok" : "fail");
  return ok;
}

// Чи пора качати прогноз: нема його, йому >= 3 год, або час невідомий і цього запуску ще не пробували
static bool forecastDue() {
  if (fcTried && millis() - fcTryMs < FC_RETRY_MS) return false;
  if (!fcOk) return true;
  time_t n = time(nullptr);
  if (n < 1700000000) return !fcTried;        // час ще невідомий: одна спроба на запуск
  if (!fc.fetched) return true;                 // скачаний до NTP — оновити, щойно відомий час
  return (uint32_t)n - fc.fetched >= FC_PERIOD_S;
}

static void weatherTask(void*) {
  uint32_t lastCur = 0;
  bool curOk = false, curTried = false;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && wxForce) {    // кнопка "Оновити" (меню або веб)
      wxBusy = true;
      curOk = fetchCurrent();
      curTried = true;
      lastCur = millis();
      fcTried = true;
      fcTryMs = millis();
      fetchForecast();
      wxForce = false;
      wxBusy = false;
    }
    if (WiFi.status() == WL_CONNECTED) {
      // поточна: одразу після підключення, далі раз на 10 хв (після збою — через 30 с)
      uint32_t gap = curOk ? WEATHER_PERIOD_MS : 30000UL;
      if (!curTried || millis() - lastCur >= gap) {
        curOk = fetchCurrent();
        curTried = true;
        lastCur = millis();
      }
      if (forecastDue()) {
        fcTried = true;
        fcTryMs = millis();
        fetchForecast();
      }
    }
    fcApply();                                 // без інтернету — з прогнозу; схід/захід на сьогодні
    for (int k = 0; k < 10 && !wxForce; k++) vTaskDelay(pdMS_TO_TICKS(200));   // мережу перевіряємо часто; "Оновити" — одразу
  }
}

// ---- Точність годинника без інтернету ----
// Між синхронізаціями NTP міряємо, наскільки кварц ESP32 спішить/відстає (ppm), і поки NTP недоступний
// (нема інтернету), щохвилини підправляємо системний час на цю похибку. Похибка зберігається в пам'яті.

static void onTimeSync(struct timeval* tv) {
  int64_t mono = esp_timer_get_time();
  int64_t ep = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
  if (ntpMono) {
    int64_t dm = mono - ntpMono;
    if (dm > 30LL * 60LL * 1000000LL) {                 // мінімум 30 хв між замірами
      float ppm = (float)((double)((ep - ntpEpochUs) - dm) / (double)dm * 1e6);
      if (fabsf(ppm) < 150.0f) {
        driftPpm = driftOk ? driftPpm * 0.7f + ppm * 0.3f : ppm;
        driftOk = true;
        driftDirty = true;
      }
    }
  }
  ntpMono = mono;
  ntpEpochUs = ep;
  corrMono = mono;
}

// Викликається з loop() раз на хвилину
static void clockKeep() {
  if (driftDirty) {                                     // зберегти похибку (з loop, не з задачі SNTP)
    driftDirty = false;
    prefs.begin("ctl", false);
    prefs.putFloat("ppm", driftPpm);
    prefs.end();
  }
  if (!driftOk || !ntpMono || fabsf(driftPpm) < 0.5f) return;
  int64_t mono = esp_timer_get_time();
  if (mono - ntpMono < 65LL * 60LL * 1000000LL) return; // NTP працює (оновлює раз на годину) — не втручаємось
  int64_t corr = (int64_t)((double)driftPpm * 1e-6 * (double)(mono - corrMono));
  if (corr > -2000 && corr < 2000) return;              // копимо до 2 мс
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  int64_t us = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec + corr;
  tv.tv_sec = (time_t)(us / 1000000LL);
  tv.tv_usec = (suseconds_t)(us % 1000000LL);
  settimeofday(&tv, nullptr);
  corrMono = mono;
}

static bool wxValid() { return wx.ok && (millis() - wx.at) < 3600000UL; }

// =====================================================================
//  6. ДАТЧИКИ
// =====================================================================

static uint32_t adcMv(uint8_t pin) {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(pin);
  return sum / 16;
}

// --- INA3221 (тільки напруга) ---
// Раніше канали "зависали": INA нікому не налаштовувався (працював на заводських
// значеннях), а при збої шини драйвер I2C лишався у зламаному стані й нічого не
// відновлювалось. Тепер: явне налаштування, читання зі STOP, відновлення шини,
// періодичне перезаписування конфігурації і контроль "застиглих" даних.
#define INA_REG_CONFIG   0x00
// канали 1-3 увімкнені, усереднення 16, 1.1 мс, безперервний режим
#define INA_CONFIG_VAL   0x7527
#define INA_CFG_REFRESH_MS   20000UL    // перезапис конфігурації (захист від тихого скидання)
#define INA_STUCK_READS      600        // стільки однакових відліків поспіль = "завис" (~5 хв; стабільна АКБ може довго давати те саме)

static bool inaRead(uint8_t reg, uint16_t& out) {
  Wire.beginTransmission(inaAddr);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;       // зі STOP, без repeated start
  if (Wire.requestFrom((int)inaAddr, 2) != 2) return false;
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  out = ((uint16_t)hi << 8) | lo;
  return true;
}

static bool inaWrite(uint8_t reg, uint16_t val) {
  Wire.beginTransmission(inaAddr);
  Wire.write(reg);
  Wire.write((uint8_t)(val >> 8));
  Wire.write((uint8_t)(val & 0xFF));
  return Wire.endTransmission(true) == 0;
}

// Відновлення шини I2C: якщо датчик утримує SDA внизу — "дотягуємо" його 9 тактами + STOP
static void i2cRecover() {
  Wire.end();
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SCL_PIN, HIGH);
  delayMicroseconds(10);
  for (int i = 0; i < 9 && digitalRead(I2C_SDA_PIN) == LOW; i++) {
    digitalWrite(I2C_SCL_PIN, LOW);  delayMicroseconds(10);
    digitalWrite(I2C_SCL_PIN, HIGH); delayMicroseconds(10);
  }
  pinMode(I2C_SDA_PIN, OUTPUT_OPEN_DRAIN);                 // STOP-умова
  digitalWrite(I2C_SDA_PIN, LOW);  delayMicroseconds(10);
  digitalWrite(I2C_SCL_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(I2C_SDA_PIN, HIGH); delayMicroseconds(10);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);
  Wire.setTimeOut(20);
}

static bool inaInit() {
  return inaAddr && inaWrite(INA_REG_CONFIG, INA_CONFIG_VAL);
}

static void inaDetect() {
  for (uint8_t a = 0x40; a <= 0x43; a++) {
    inaAddr = a;
    uint16_t id = 0;
    if (inaRead(0xFE, id) && id == 0x5449) { inaInit(); return; }
  }
  inaAddr = 0;
}

static void updateBattery() {
  static uint32_t lastRun = 0, lastTry = 0, lastCfg = 0;
  static uint8_t  fails = 0;
  static uint16_t lastRaw[3] = {0, 0, 0};
  static uint16_t sameCnt[3] = {0, 0, 0};
  uint32_t now = millis();
  if (now - lastRun < 500) return;             // I2C-шина спільна з екраном — не частимо
  lastRun = now;

  if (!inaAddr) {                              // шукаємо INA, поки не з'явиться
    if (now - lastTry < 3000) return;
    lastTry = now;
    i2cRecover();
    inaDetect();
    if (!inaAddr) { batOk = false; return; }
    lastCfg = now; fails = 0;
    batV[0] = batV[1] = batV[2] = -1;
    sameCnt[0] = sameCnt[1] = sameCnt[2] = 0;
  }

  if (now - lastCfg >= INA_CFG_REFRESH_MS) {   // профілактика: INA міг тихо скинутись
    lastCfg = now;
    if (!inaInit()) fails++;
  }

  bool good = true;
  static const uint8_t BAT_CH[3]      = {BAT1_INA_CH, BAT2_INA_CH, LINE_INA_CH};
  static const int16_t BAT_OFFS_MV[3] = {BAT1_OFFSET_MV, BAT2_OFFSET_MV, LINE_OFFSET_MV};
  for (int c = 0; c < 3; c++) {                // регістр напруги каналу N = 0x00 + 2*N (CH1=0x02, CH2=0x04, CH3=0x06)
    uint16_t raw;
    if (!inaRead(BAT_CH[c] * 2, raw)) { good = false; continue; }

    if (raw == lastRaw[c]) { if (sameCnt[c] < 65000) sameCnt[c]++; }
    else                   { lastRaw[c] = raw; sameCnt[c] = 0; }

    float v = (raw >> 3) * 0.008f + BAT_OFFS_MV[c] / 1000.0f;
    batV[c] = (batV[c] < 0) ? v : batV[c] + (v - batV[c]) * 0.3f;
  }

  if (good) {
    fails = 0;
    batOk = true;
    // дані застигли на обох каналах — перенастроюємо INA і шину
    if (sameCnt[0] >= INA_STUCK_READS && sameCnt[1] >= INA_STUCK_READS) {
      sameCnt[0] = sameCnt[1] = 0;
      inaInit();                               // лише переналаштувати INA, шину без потреби не чіпаємо
      lastCfg = millis();
    }
  } else if (++fails >= 3) {                   // кілька збоїв поспіль: відновити шину і знайти INA заново
    batOk = false;
    inaAddr = 0;
    lastTry = 0;
  }
}

static int batPercent(float v) {
  static const float V[] = {3.00f, 3.60f, 3.75f, 3.90f, 4.05f, 4.20f};
  static const float P[] = {0, 15, 40, 70, 90, 100};
  if (v <= V[0]) return 0;
  if (v >= V[5]) return 100;
  for (int i = 1; i < 6; i++) {
    if (v < V[i]) return (int)(P[i - 1] + (v - V[i - 1]) * (P[i] - P[i - 1]) / (V[i] - V[i - 1]));
  }
  return 100;
}

// --- Фоторезистор: шкала, пороги, автокалібрування ---

// Шкала, відносно якої рахуються відсотки порогів
static void ldrScale(float& lo, float& hi) {
  if (ldrHi - ldrLo >= LDR_MIN_RANGE_MV) { lo = ldrLo; hi = ldrHi; }
  else                                   { lo = 0;     hi = LDR_UNCAL_MAX_MV; }
}

// Частка освітленості 0..1 для показу (смужка, слова "Темно/Сутінки/..."). Фоторезистор у дільнику дає
// напругу, що дуже нелінійна до світла: у світлі майже не рухається. Тому рахуємо через провідність LDR
// q = V/(Vref-V) у логарифмі — це пропорційно log(люкс), і смужка рівномірно реагує в усьому діапазоні.
static float ldrFrac(float mv) {
  float lo, hi;
  ldrScale(lo, hi);
  auto lq = [](float v) { v = constrain(v, 30.0f, 3270.0f); return logf(v / (3300.0f - v)); };
  float a = lq(lo), b = lq(hi);
  if (b - a < 0.05f) return 0;
  return constrain((lq(mv) - a) / (b - a), 0.0f, 1.0f);
}

static float permToMv(uint16_t pm) {
  float lo, hi;
  ldrScale(lo, hi);
  return lo + (hi - lo) * pm / 1000.0f;
}

// Викликається раз на секунду
static void calibrateLdr() {
  const float ATTACK  = 0.02f;                 // ~50 с: швидко розширюємо діапазон
  const float RELEASE = 1.0f / (8 * 3600);     // ~8 год: повільно "забуваємо" старе
  float v = ldrMv;
  bool wide = (ldrHi - ldrLo) > LDR_MIN_RANGE_MV * 1.5f;

  if (v < ldrLo)      ldrLo += (v - ldrLo) * ATTACK;
  else if (wide)      ldrLo += (v - ldrLo) * RELEASE;
  if (v > ldrHi)      ldrHi += (v - ldrHi) * ATTACK;
  else if (wide)      ldrHi += (v - ldrHi) * RELEASE;
}

static int tuneRelay(int p) { return (p == P_REL && editing) ? relSel : -1; }

// Для кожного реле свій стан "темно" зі своїми порогами й гістерезисом
// Дані про схід/захід є і не застарілі
static bool sunDataValid() {
  return sunOk && (uint32_t)(millis() - sunAt) < SUN_DATA_MAX_AGE_MS;
}

// true = за прогнозом зараз ДЕНЬ (від сходу до заходу) і функцію увімкнено
static bool sunDay() {
  if (!sunGate || !sunDataValid()) return false;
  struct tm t;
  if (!localNow(t)) return false;
  int m = t.tm_hour * 60 + t.tm_min;
  return m >= sunRiseMin && m < sunSetMin;
}

// true = за прогнозом зараз НІЧ (до сходу / після заходу). valid = false — нема даних або часу
static bool sunNight(bool& valid) {
  valid = false;
  if (!sunDataValid()) return false;
  struct tm t;
  if (!localNow(t)) return false;
  valid = true;
  int m = t.tm_hour * 60 + t.tm_min;
  return m < sunRiseMin || m >= sunSetMin;
}

static void updateDark() {
  static bool init[NUM_RELAYS] = {false, false, false};
  uint32_t nowMs = millis();
  bool day = sunDay();

  for (int i = 0; i < NUM_RELAYS; i++) {
    // Фоторезистор не підключено (<1 В): "темно" лише за часом заходу/сходу (незалежно від "За сонцем")
    if (ldrAbsent) {
      bool v; bool n = sunNight(v);
      relayDark[i] = v && n; pending[i] = false; pendLeft[i] = 0; init[i] = true;
      continue;
    }
    // Вдень фоторезистор ігноруємо: "темно" немає. Під час правки порогу цього реле — не блокуємо
    if (day && ((SUN_GATE_RELAYS >> i) & 1) && !(editing && tuneRelay(page) == i)) {
      relayDark[i] = false; pending[i] = false; pendLeft[i] = 0; init[i] = true;
      continue;
    }
    float dk = permToMv(thrOnPm(i));
    float br = permToMv(thrOff[i]);

    if (!init[i]) { relayDark[i] = ldrMv < (dk + br) * 0.5f; init[i] = true; continue; }

    bool want = relayDark[i] ? !(ldrMv > br) : (ldrMv < dk);
    if (want == relayDark[i]) { pending[i] = false; pendLeft[i] = 0; continue; }

    if (!pending[i]) { pending[i] = true; pendT[i] = nowMs; }
    // під час правки порогу цього реле — майже без затримки, щоб було видно ефект
    uint32_t need = (editing && tuneRelay(page) == i) ? 800UL : (uint32_t)switchDelaySec * 1000UL;
    uint32_t el = nowMs - pendT[i];
    if (el >= need) { relayDark[i] = want; pending[i] = false; pendLeft[i] = 0; }
    else            { pendLeft[i] = (uint16_t)((need - el + 999) / 1000); }
  }
}

// Нічне вимкнення: реле в Авто вимкнене від nightHour протягом NIGHT_WINDOW_H годин (напр. 00:00..12:00).
// Раніше "защіпка" ставилась лише в ту хвилину, коли годинник переходив на nightHour, і тільки якщо
// саме тоді було темно. Якщо в цю мить LDR бачив світло (інша лампа, затримка перемикання) —
// реле вмикалось пізніше вночі і горіло до ранку. Тепер діє все вікно.
#define NIGHT_WINDOW_H 12
static void updateNightOff() {
  if (!nightOff) {
    for (int i = 0; i < NUM_RELAYS; i++) nightLatch[i] = false;
    return;
  }
  struct tm t;
  if (!localNow(t)) return;                    // час невідомий — лишаємо як було
  bool inWin = ((t.tm_hour - nightHour + 24) % 24) < NIGHT_WINDOW_H;
  for (int i = 0; i < NUM_RELAYS; i++)
    nightLatch[i] = ((NIGHT_OFF_RELAYS >> i) & 1) && inWin;
}

static void updateSensors() {
  float v = (float)adcMv(PIN_LDR);
  if (!ldrReady) {
    ldrMv = v;
    ldrFast = v;
    ldrReady = true;
    if (ldrHi - ldrLo < 1) ldrLo = ldrHi = v;     // перший запуск
  } else {
    ldrMv += (v - ldrMv) * 0.15f;
    ldrFast += (v - ldrFast) * 0.6f;
  }
  {
    static bool lowFlag = false;
    static uint32_t lowT = 0;
    uint32_t nowMs = millis();
    if (ldrMv < LDR_ABSENT_MV) {
      if (!lowFlag) { lowFlag = true; lowT = nowMs; }
      if (nowMs - lowT >= LDR_ABSENT_MS) ldrAbsent = true;
    } else {
      lowFlag = false;
      ldrAbsent = false;
    }
  }
  gridMv = adcMv(PIN_POWER_DETECT);
  updateBattery();
  updateDark();
  updateNightOff();
}

// =====================================================================
//  7. РЕЛЕ ТА СВІТЛОДІОДИ
// =====================================================================

static void relayWrite(int i, bool on) {
  relayOn[i] = on;
  digitalWrite(RELAY_PIN[i], on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

static uint8_t effectiveMode(int i) { return (allOff || deepOff) ? MODE_OFF : relayMode[i]; }

static void applyRelays() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    uint8_t m = effectiveMode(i);
    bool on = (m == MODE_ON) || (m == MODE_AUTO && relayDark[i] && !nightLatch[i]);
    if (on != relayOn[i]) relayWrite(i, on);
  }
}

static void hueRgb(float h, uint8_t& r, uint8_t& g, uint8_t& b) {
  h = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
  int   i  = (int)h;
  float f  = h - i;
  uint8_t up = (uint8_t)(f * 255), dn = (uint8_t)((1.0f - f) * 255);
  switch (i) {
    case 0:  r = 255; g = up;  b = 0;   break;
    case 1:  r = dn;  g = 255; b = 0;   break;
    case 2:  r = 0;   g = 255; b = up;  break;
    case 3:  r = 0;   g = dn;  b = 255; break;
    case 4:  r = up;  g = 0;   b = 255; break;
    default: r = 255; g = 0;   b = dn;  break;
  }
}

// Насичений колір за відтінком. Лінійне змішування на адресних діодах виглядає блідим,
// майже білим (жовтий і бірюзовий особливо, бо зелений діод найяскравіший).
// Тому: гамма-корекція (чисті кольори) + трохи приглушений зелений. Білого тут немає взагалі.
static void hueVivid(float h, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t R, G, B;
  hueRgb(h, R, G, B);
  r = (uint8_t)((uint16_t)R * R / 255);
  g = (uint8_t)((uint16_t)G * G / 255 * 200 / 255);
  b = (uint8_t)((uint16_t)B * B / 255);
}

// Колір ефекту в момент now (ефекти — на всю стрічку, бо Pro Micro приймає лише "LED,r,g,b")
static void effectColor(uint8_t fx, uint32_t now, uint8_t& r, uint8_t& g, uint8_t& b) {
  switch (fx) {
    case FX_COOL:                                       // холодний білий з синюватим відтінком
      r = 170; g = 210; b = 255;
      break;
    case FX_BREATH: {                                   // м'яке бурштинове "дихання", період 4 с
      float s = 0.5f - 0.5f * cosf((now % 4000UL) / 4000.0f * 6.2832f);
      float k = 0.04f + 0.96f * s * s;                  // квадрат — плавніше для ока
      r = (uint8_t)(255 * k); g = (uint8_t)(120 * k); b = (uint8_t)(35 * k);
      break;
    }
    case FX_RAINBOW:                                    // повне коло за 10 с, без білого
      hueVivid((now % 10000UL) * 360.0f / 10000.0f, r, g, b);
      break;
    case FX_RED:    r = 255; g = 0;   b = 0;   break;
    case FX_GREEN:  r = 0;   g = 255; b = 0;   break;
    case FX_BLUE:   r = 0;   g = 40;  b = 255; break;
    case FX_PURPLE: r = 170; g = 0;   b = 255; break;
    case FX_OCEAN: {                                    // синій <-> морська хвиля, період 10 с
      float k = 0.5f - 0.5f * cosf((now % 10000UL) / 10000.0f * 6.2832f);
      r = 0; g = (uint8_t)(60 + 110 * k); b = (uint8_t)(255 - 30 * k);
      break;
    }
    case FX_SUNSET: {                                   // оранжевий <-> малиновий, період 14 с
      float k = 0.5f - 0.5f * cosf((now % 14000UL) / 14000.0f * 6.2832f);
      r = (uint8_t)(255 - 55 * k); g = (uint8_t)(60 - 60 * k); b = (uint8_t)(120 * k);
      break;
    }
    case FX_PULSE: {                                    // "серцебиття": два удари за 1.3 с
      float ph = (now % 1300UL) / 1300.0f;
      float d1 = (ph - 0.12f) / 0.09f, d2 = (ph - 0.34f) / 0.09f;
      float k  = 0.08f + 0.92f * fmaxf(expf(-d1 * d1), 0.65f * expf(-d2 * d2));
      r = (uint8_t)(255 * k); g = (uint8_t)(6 * k); b = (uint8_t)(14 * k);
      break;
    }
    case FX_AURORA:                                     // зелений -> бірюзовий -> синій -> фіолетовий, період 16 с
      hueVivid(150.0f + 120.0f * (0.5f - 0.5f * cosf((now % 16000UL) / 16000.0f * 6.2832f)), r, g, b);
      break;
    case FX_FIRE: {                                     // мерехтіння свічки: слабше полум'я — червоніше
      static float f = 0.7f;
      f += (random(35, 101) / 100.0f - f) * 0.4f;
      r = (uint8_t)(255 * f); g = (uint8_t)(90 * f * f); b = (uint8_t)(6 * f * f);
      break;
    }
    default:                                            // FX_WARM: тепле світло (на WS2812 це справжнє "тепле")
      r = 255; g = 150; b = 45;
      break;
  }
}

// Яскравість у %: з округленням, щоб на малій яскравості колір не "біліє" від відсікання
static uint8_t scaleCh(uint8_t v, uint8_t pct) {
  if (!v || !pct) return 0;
  uint16_t x = ((uint16_t)v * pct + 50) / 100;
  if (!x && v >= 64) x = 1;                             // помітний канал не губимо
  return (uint8_t)x;
}

static bool ledFxAnimated(uint8_t fx) {
  return fx == FX_BREATH || fx == FX_RAINBOW || fx == FX_FIRE || fx == FX_OCEAN ||
         fx == FX_SUNSET || fx == FX_PULSE || fx == FX_AURORA;
}

// "Темно" для стрічки: фоторезистор; якщо його нема (<1 В на піні) — за прогнозом схід/захід
static bool ledIsDark() {
  if (!ldrAbsent) return relayDark[0];
  bool v; bool n = sunNight(v);
  return v && n;
}

// Що має робити стрічка зараз. Під час правки на екрані LED показує те, що редагується:
// поля яскравості/ефекту — одразу видно результат (поля "Темно" — стан для темряви).
static void ledTarget(bool& on, uint8_t& pct, uint8_t& fx) {
  on = false; pct = 0; fx = 0;
  if (allOff || deepOff) return;
  const LedCfg& c = ledCfg;
  bool dark;
  if (ledPreview >= 0)             { dark = ledPreview == 1; }
  else if (c.mode == MODE_ON)           { dark = false; }
  else if (c.mode == MODE_AUTO)         { dark = ledIsDark(); }
  else return;
  pct = dark ? c.brD : c.brN;
  fx  = dark ? c.fxD : c.fxN;
  on  = pct > 0;
}

// Яскравість рахуємо тут і шлемо готовий колір (BRIGHT на Pro Micro стоїть 255).
// Раніше при кожному кроці яскравості йшли BRIGHT + LED, на швидкому обертанні
// енкодера Pro Micro губив байти й показував випадкові кольори.
static void ledSync() {
#if LED_ENABLE
  static int8_t   lastOn = -1;
  static int16_t  lastBr = -1;
  static uint8_t  lastFx = 255;
  static uint32_t t = 0;

  bool on; uint8_t pct, fx;
  ledTarget(on, pct, fx);
  uint32_t now = millis();

  if (ledForce) { ledForce = false; lastOn = -1; }
  bool anim    = on && ledFxAnimated(fx);
  bool changed = ((int8_t)on != lastOn) || (on && (pct != lastBr || fx != lastFx));
  if (!changed && now - t < (anim ? (uint32_t)LED_ANIM_MS : 10000UL)) return;
  if (changed && lastOn != -1 && now - t < (uint32_t)LED_EDIT_GAP_MS) return;   // не частимо: візьмемо останнє значення

  char b[24];
  if (lastOn == -1) {                                  // старт або перезапуск Pro Micro
    snprintf(b, sizeof(b), "LEDS,%d", LED_COUNT);
    Serial2.println(b);
    Serial2.println("BRIGHT,255");
  }
  if (on) {
    uint8_t r, g, bl;
    effectColor(fx, now, r, g, bl);
    r  = scaleCh(r,  pct);
    g  = scaleCh(g,  pct);
    bl = scaleCh(bl, pct);
    snprintf(b, sizeof(b), "LED,%u,%u,%u", r, g, bl);
    Serial2.println(b);
  } else {
    Serial2.println("LED,OFF");
  }
  lastOn = on;
  lastBr = pct;
  lastFx = fx;
  t = now;
#endif
}

// =====================================================================
//  8. КЕРУВАННЯ
// =====================================================================

static void gotoPage(int p, int dir) {
  if (p == page) return;
  fromPage = page;
  page = p;
  transDir = dir;
  transT = 0;
  transStart = millis();
  dirty = true;
}

// Яскравість екрана за поточним станом: звичайна / скрінсейвер / скрінсейвер при "все вимк"
static void applyContrast() {
  if (!oledOk) return;
  uint8_t p;
  if (!asleep) p = scrBr[0];
  else         p = allOff ? scrBr[2] : scrBr[1];
  oled.setContrast((uint8_t)((uint16_t)p * 255 / 100));
}

static void beginEdit() { editing = true; relLevel = 2; }

// Вихід із правки порогу — завжди зі збереженням (BACK або сон екрана)
static void commitEdit() {
  if (!editing) return;
  editing = false;
  relLevel = 1;
  saveSettings();
}

static void menuReset();
static void toast(const char* m);
static void menuEncoder(int dir);
static void menuOk();
static void menuBack();

static void cancelEdit() {
  commitEdit();
  relLevel = 0;
  menuReset();
}

static void wake() {
  asleep = false;
  lastActivity = millis();
  applyContrast();
  dirty = true;
}

static void sleepNow() {
  cancelEdit();
  asleep = true;
  applyContrast();
  dirty = true;
}

// Жорстке вимкнення/увімкнення всього (CONF або пульт). Стан зберігається в пам'яті:
// після перезапуску система лишається вимкненою, доки її не ввімкнуть самі.
static void setAllOff(bool off) {
  if (off == allOff) {
    if (!off && asleep) wake();
    return;
  }
  allOff = off;
  prefs.begin("ctl", false);
  prefs.putBool("aoff", allOff);
  prefs.end();
  if (allOff) sleepNow(); else wake();
}

static void cycleMode(int i) {
  relayMode[i] = (relayMode[i] + 1) % MODE_COUNT;
  saveSettings();
}

static void onEncoder(int dir) {
  if (page == P_MENU && menuLevel > 0) { menuEncoder(dir); return; }
  if (page == P_REL && relLevel == 1) {                     // вибір реле всередині екрана
    relSel = (relSel + dir + NUM_RELAYS) % NUM_RELAYS;
    return;
  }
  if (page == P_REL && relLevel == 2) {                     // поріг вибраного реле
    static uint32_t lastTick = 0;                           // чим швидше крутимо, тим грубіший крок
    uint32_t n = millis();
    uint32_t gap = n - lastTick;
    lastTick = n;
    int step = gap < 45 ? 40 : (gap < 110 ? 10 : 2);       // проміле: 4% / 1% / 0.2%
    thrOff[relSel] = constrain((int)thrOff[relSel] + dir * step, 10 + THR_HYST_PM, 990);
    return;
  }
  gotoPage((page + dir + P_COUNT) % P_COUNT, dir);
}

static void onOk() {
  if (page == P_MAIN) {
    cycleMode(0);
    char b[24];
    snprintf(b, sizeof(b), "RGB: %s", MODE_NAMES[relayMode[0]]);
    toast(b);
    return;
  }
  if (page == P_MENU) { menuOk(); return; }
  if (relLevel == 0)      relLevel = 1;                     // зайти на екран
  else if (relLevel == 1) beginEdit();                      // вибрати реле
  else                    cycleMode(relSel);                // Авто -> Вимк -> Увімк
}

static void onBack() {
  if (page == P_MENU) { menuBack(); return; }
  if (page != P_REL) return;
  if (relLevel == 2)      commitEdit();                     // зберегти і вийти з реле
  else if (relLevel == 1) relLevel = 0;                     // вийти з екрана
  else                    gotoPage(P_MAIN, -1);
}

// Pro Micro міг отримати EVENTS,x,0 раніше — вмикаємо події явно
static void proInit() {
  Serial2.println("EVENTS,ENC,1");
  Serial2.println("EVENTS,BTN,1");
  Serial2.println("EVENTS,IR,1");
  if (!proVer[0]) Serial2.println("VER");          // версія для Info / веб-меню
}

static void handleLine(char* s) {
  while (*s && !isalpha((unsigned char)*s)) s++;   // пропускаємо сміття на початку
  if (!*s) return;
#if DEBUG_LINK
  Serial.printf("[LINK] %s\n", s);
#endif

  // Відповіді Pro Micro на наші команди
  if (!strncmp(s, "PONG", 4)) { pongRcv++; return; }
  if (!strncmp(s, "STATUS,", 7)) {
    const char* b = strstr(s, "BTN,");
    const char* e = strstr(s, "EV,");
    if (b) snprintf(proBtn, sizeof(proBtn), "%.3s", b + 4);
    if (e) snprintf(proEv,  sizeof(proEv),  "%.3s", e + 3);
    return;
  }
  if (!strncmp(s, "READY,PROMICRO", 14) || !strncmp(s, "VER,PROMICRO", 12)) {
    const char* v = strrchr(s, ',');
    snprintf(proVer, sizeof(proVer), "%s", v ? v + 1 : "?");
    if (s[0] == 'R') { ledForce = true; proInit(); } // Pro Micro перезавантажився — налаштувати знову
    return;
  }

  bool enc = false, ok = false, back = false, conf = false, ir = false;
  int dir = 0, irCmd = -1;

  bool isEvt = !strncmp(s, "ENC,", 4) || !strncmp(s, "BTN,", 4) || !strncmp(s, "IR,", 3);
  if (isEvt) { snprintf(lastEvt, sizeof(lastEvt), "%s", s); evCount++; }

  char* p = s;
  if (!strncmp(p, "ENC,", 4)) {
    enc = true;
    dir = (p[4] == '-') ? -1 : 1;
  } else if (!strncmp(p, "BTN,", 4)) {
    char* name = p + 4;
    char* c = strchr(name, ',');
    if (c && !strncmp(name, "OK,", 3)) {     // утримання OK (вихід із глибокого сну)
      okHeld = c[1] == '1';
      if (okHeld) okDownT = millis();
    }
    if (deepOff) return;                     // глибокий сон: реагуємо лише на утримання OK
    if (!c || c[1] != '1') return;           // відпускання кнопки ігноруємо
    *c = 0;
    if      (!strcmp(name, "OK"))   ok = true;
    else if (!strcmp(name, "BACK")) back = true;
    else if (!strcmp(name, "CONF")) conf = true;
    else return;

    static uint32_t lastBtn = 0;             // антидеребезг
    if (millis() - lastBtn < 200) return;
    lastBtn = millis();
  } else if (!strncmp(p, "IR,", 3)) {
    ir = true;
    // формат: IR,NEC,<addr>,<cmd> — команда після третьої коми
    const char* c1 = strchr(p + 3, ',');
    const char* c2 = c1 ? strchr(c1 + 1, ',') : nullptr;
    if (c2) irCmd = (int)strtoul(c2 + 1, nullptr, 0);
  } else {
    return;
  }

  if (deepOff) return;
  if (ir && irCmd != IR_CMD_ALL_ON && irCmd != IR_CMD_ALL_OFF) return;   // інші кнопки пульта ігноруємо

  lastActivity = millis();
  dirty = true;

  if (conf) { setAllOff(!allOff); return; }  // CONF: вимкнути все / повернути
  if (ir && (irCmd == IR_CMD_ALL_ON || irCmd == IR_CMD_ALL_OFF)) {   // пульт: 0x45 — все увімк., 0x47 — все вимк.
    setAllOff(irCmd == IR_CMD_ALL_OFF);      // повтори тієї ж кнопки нічого не міняють
    return;
  }
  if (allOff) return;                        // "жорстко вимк": усе інше (енкодер, кнопки, пульт) ігнорується
  if (asleep) { wake(); return; }            // перше натискання тільки будить
  if (ir) return;

  if (enc)       onEncoder(dir);
  else if (ok)   onOk();
  else if (back) onBack();
}

static void flushLine() {
  lineBuf[lineLen] = 0;
  handleLine(lineBuf);
  lineLen = 0;
}

static void readLink() {
  while (Serial2.available()) {
    char c = Serial2.read();
    rxBytes++;
    lastRxMs = millis();
    if (c == '\n' || c == '\r') {
      if (lineLen) flushLine();
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
  if (lineLen && millis() - lastRxMs > 40) flushLine();   // рядок без \n
}

// Плавні значення: викликається один раз на кадр
static void stepAnim() {
  float k = 1.0f - expf(-frameDt * 14.0f);
  if (ldrShown < 0) ldrShown = ldrMv;
  else              ldrShown += (ldrMv - ldrShown) * k;

}

// =====================================================================
//  9. ВІДРИСОВКА
// =====================================================================
//  Координати невід'ємні (U8g2 для цього екрана працює з uint8), тому перехід
//  між екранами зроблений "накриттям", а не зсувом у мінус.

// ---- Плавні анімації: спільні помічники ----
static bool animMoving = false;   // за кадр щось рухалось -> наступний кадр швидко (інакше рідше, економимо I2C)
static float easeOut(float t) { t = 1.0f - constrain(t, 0.0f, 1.0f); return 1.0f - t * t * t; }
static float approach(float cur, float tgt, float rate) {
  if (fabsf(tgt - cur) < 0.05f) return tgt;
  animMoving = true;                                  // ще рухається — малюємо часто
  return cur + (tgt - cur) * (1.0f - expf(-frameDt * rate));
}
// Вікно обрізання з урахуванням зовнішнього (під час анімації відкриття підменю)
static int clipB[4] = {0, 0, 128, 64};
static void clipSet(int x0, int y0, int x1, int y1) {
  x0 = max(x0, clipB[0]); y0 = max(y0, clipB[1]);
  x1 = max(x0, min(x1, clipB[2])); y1 = max(y0, min(y1, clipB[3]));
  oled.setClipWindow(x0, y0, x1, y1);
}
static void clipReset() {
  if (!clipB[0] && !clipB[1] && clipB[2] == 128 && clipB[3] == 64) oled.setMaxClipWindow();
  else oled.setClipWindow(clipB[0], clipB[1], clipB[2], clipB[3]);
}

// Зміна режиму реле: старий текст їде вгору, новий випливає знизу
static uint8_t  modeCur[NUM_RELAYS]  = {255, 255, 255}, modePrev[NUM_RELAYS] = {255, 255, 255};
static uint32_t modeT[NUM_RELAYS]    = {0, 0, 0};
static float modeAnim(int i, uint8_t m) {
  if (modeCur[i] == 255) modeCur[i] = modePrev[i] = m;
  if (m != modeCur[i]) { modePrev[i] = modeCur[i]; modeCur[i] = m; modeT[i] = millis(); }
  float k = (millis() - modeT[i]) / 240.0f;
  if (k >= 1.0f) return 1.0f;
  animMoving = true;
  return easeOut(k);
}
// Текст режиму в межах (x0,y0)-(x1,y1); центр xc або лівий край x (xc < 0), базова лінія y
static void modeText(int i, const char* const* names, int x, int xc, int y, int x0, int y0, int x1, int y1) {
  float k = modeAnim(i, modeCur[i] == 255 ? 0 : modeCur[i]);
  int sh = y1 - y0;
  auto put = [&](const char* t, int yy) {
    int xx = xc >= 0 ? xc - oled.getStrWidth(t) / 2 : x;
    oled.drawStr(xx, yy, t);
  };
  if (k >= 1.0f) { put(names[modeCur[i]], y); return; }
  clipSet(x0, y0, x1, y1);
  put(names[modePrev[i]], y - (int)lroundf(k * sh));
  put(names[modeCur[i]],  y + (int)lroundf((1.0f - k) * sh));
  clipReset();
}

// ---- Головний екран (дизайн з Lopaka) ----
// Функції скрінсейвера, які використовує і головний екран (визначені нижче)
static void        saverWeatherIcon(int x, int y, int c, bool day);
static void        saverSun(const struct tm& t, int& rise, int& set);
static const char* saverDayPart(const struct tm& t);
static const char* saverLightWord(const struct tm* t);

// Значок Wi-Fi 15x12: 4 дуги + крапка
static const unsigned char ico_wifi[] U8X8_PROGMEM = {0xe0,0x03,0x18,0x0c,0x04,0x10,0xe2,0x23,0x11,0x44,0xc8,0x09,0x24,0x12,0x10,0x04,0xc0,0x01,0x20,0x02,0x00,0x00,0x80,0x00};

// Значок показується тільки коли Wi-Fi підключено
static void drawWifi(int x, int y) {
  if (WiFi.status() != WL_CONNECTED) return;
  oled.drawXBMP(x, y, 15, 12, ico_wifi);
}

// Кружечок-індикатор 6x6: заповнений центр = увімкнено
static void drawDot(int x, int y, bool on) {
  oled.drawHLine(x + 1, y, 4);
  oled.drawHLine(x + 1, y + 5, 4);
  oled.drawVLine(x, y + 1, 4);
  oled.drawVLine(x + 5, y + 1, 4);
  if (on) oled.drawBox(x + 2, y + 2, 2, 2);
}

// Батарея: корпус 26x13 з контактом, заливка за %, справа — назва і "98% 4.11v"
static void drawBatRow(int ox, int y0, const char* label, int ch) {
  oled.drawFrame(ox + 2, y0, 26, 13);
  oled.drawBox(ox + 28, y0 + 3, 2, 7);
  oled.setFont(u8g2_font_4x6_t_cyrillic);
  oled.drawUTF8(ox + 34, y0 + 5, label);
  char b[16];
  if (!batOk || batV[ch] < BAT_MIN_V) {
    strcpy(b, "--");
  } else {
    int pct = batPercent(batV[ch]);
    static float shown[2] = {-1, -1};                      // заливка плавно доїжджає до заряду
    shown[ch] = shown[ch] < 0 ? pct : approach(shown[ch], pct, 6);
    int w = (int)lroundf(shown[ch] * 22 / 100);            // заливка x 4..25, по 1 px відступу
    if (w > 0) oled.drawBox(ox + 4, y0 + 2, w, 9);
    if (pct >= 100) snprintf(b, sizeof(b), "%d%% %.1fv", pct, batV[ch]);   // щоб не налізло на лінію x=72
    else            snprintf(b, sizeof(b), "%d%% %.2fv", pct, batV[ch]);
  }
  oled.setFont(u8g2_font_4x6_tr);
  oled.drawStr(ox + 34, y0 + 13, b);
}

// Рядок реле: назва, стан ON/OFF, індикатор (блимає, поки чекає перемикання), режим
static void drawRelayRow(int ox, int y, int i, const char* name) {
  oled.setFont(u8g2_font_4x6_tr);
  oled.drawStr(ox + 74, y, name);
  oled.drawStr(ox + (relayOn[i] ? 88 : 87), y, relayOn[i] ? "ON" : "OFF");   // OFF ширший — на 1 px лівіше
  bool dot = relayOn[i];
  if (pending[i] && (millis() / 300) % 2) dot = !dot;
  drawDot(ox + 101, y - 6, dot);
  static const char* const M[] = {"AUTO", "OFF", "ON"};
  modeAnim(i, effectiveMode(i));
  modeText(i, M, ox + 111, -1, y, ox + 110, y - 7, ox + 128, y + 1);
}

// Опис погоди під температурою (до 8 літер — щоб не налазило на значок Wi-Fi)
static const char* wxTextUa(int c) {
  if (c <= 1)  return "Ясно";
  if (c == 2)  return "Мінлива";
  if (c == 3)  return "Хмарно";
  if (c <= 48) return "Туман";
  if (c <= 57) return "Мряка";
  if (c <= 63) return "Дощ";
  if (c <= 67) return "Злива";
  if (c <= 77) return "Сніг";
  if (c <= 82) return "Злива";
  if (c <= 86) return "Сніг";
  return "Гроза";
}

static void drawMain(int ox) {
  char b[28];
  struct tm t;
  bool ok = localNow(t);

  oled.setFontMode(1);
  oled.setBitmapMode(1);

  // ---- верх: час, Wi-Fi, температура, погода ----
  // час: цифри шрифтом, двокрапку малюємо самі (дві крапки 3x3), щоб вона плавно "дихала"
  oled.setFont(u8g2_font_profont17_tr);
  if (ok) snprintf(b, sizeof(b), "%02d %02d", t.tm_hour, t.tm_min);
  else    strcpy(b, "-- --");
  oled.drawStr(ox + 3, 14, b);
  {
    // яскравість 0..1 за косинусом (період 2 с), на 1-бітному екрані — кількість світних пікселів крапки:
    // спершу центр, потім хрестик, потім кути — крапка плавно "росте" і "тане"
    static const uint8_t ORD[9][2] = {{1,1},{0,1},{2,1},{1,0},{1,2},{0,0},{2,2},{2,0},{0,2}};
    float k = ok ? 0.5f + 0.5f * cosf(6.2831853f * (millis() % 2000UL) / 2000.0f) : 1.0f;
    int n = (int)lroundf(k * 9);
    for (int i = 0; i < n; i++) {
      oled.drawPixel(ox + 23 + ORD[i][0], 6 + ORD[i][1]);
      oled.drawPixel(ox + 23 + ORD[i][0], 11 + ORD[i][1]);
    }
  }

  drawWifi(ox + 54, 2);

  const char* wtxt;
  if (wxValid()) {
    snprintf(b, sizeof(b), "%+d°", (int)lroundf(wx.temp));
    wtxt = wxTextUa(wx.code);
    bool day = true;
    if (ok) { int rise, set; saverSun(t, rise, set); int m = t.tm_hour * 60 + t.tm_min; day = (m >= rise && m < set); }
    saverWeatherIcon(ox + 107, 0, wx.code, day);
  } else {
    strcpy(b, "--°");
    if (WiFi.status() != WL_CONNECTED) wtxt = "нема WiFi";
    else if (wxStatus == 0)            wtxt = "чекаю...";
    else                               wtxt = "збій";      // код помилки — на екрані діагностики
  }
  oled.setFont(u8g2_font_profont11_tf);
  oled.drawUTF8(ox + 106 - oled.getUTF8Width(b), 9, b);    // вирівняно вправо до значка
  oled.setFont(u8g2_font_4x6_t_cyrillic);
  oled.drawUTF8(ox + 104 - oled.getUTF8Width(wtxt), 15, wtxt);

  oled.drawHLine(ox, 17, 128);

  // ---- ліва колонка: дві АКБ і лінія 5V ----
  drawBatRow(ox, 20, BAT1_LABEL, 0);
  drawBatRow(ox, 39, BAT2_LABEL, 1);
  oled.drawHLine(ox, 55, 72);
  if (batOk && batV[2] >= 0) snprintf(b, sizeof(b), "Лінія 5v - %.2fv", batV[2]);
  else                       strcpy(b, "Лінія 5v - --");
  oled.setFont(u8g2_font_4x6_t_cyrillic);
  oled.drawUTF8(ox + 3, 62, b);

  oled.drawVLine(ox + 72, 18, 46);

  // ---- права колонка ----
  bool grid = gridMv >= POWER_DETECT_MV;
  oled.setFont(u8g2_font_4x6_t_cyrillic);
  oled.drawUTF8(ox + 74, 25, allOff ? "Все вимкнено" : (grid ? "Світло є" : "Без світла"));
  if (!allOff) drawDot(ox + 118, 19, grid);
  oled.drawHLine(ox + 72, 26, 56);

  drawRelayRow(ox, 34, 0, "RGB");
  oled.drawHLine(ox + 72, 35, 56);
  drawRelayRow(ox, 43, 1, "LED");
  oled.drawHLine(ox + 72, 44, 56);

  drawRelayRow(ox, 51, 2, "GRL");                         // третє реле (гірлянда)
  oled.drawHLine(ox + 72, 52, 56);

  // шкала фоторезистора: заливка = освітленість зараз (логарифмічна шкала)
  oled.drawFrame(ox + 75, 55, 37, 8);
  if (!ldrAbsent) {
    int w = (int)lroundf(ldrFrac(ldrShown) * 33);
    if (w > 0) oled.drawBox(ox + 77, 57, w, 4);
  }
  oled.setFont(u8g2_font_4x6_tr);
  oled.drawStr(ox + 115, 61, "LDR");
}

// ---- Екран реле (дизайн з Lopaka): три блоки по 18 px + рядок LDR ----
// Блок: шкала LDR з рискою порогу, режим у рамці, кружечок = реле увімкнене, підпис "RGB -- 1900mv поріг"
static float relFr[4] = {0, 0, 128, 64};          // рамка-курсор: плавно перетікає між блоками / на весь екран
static float thrShown[NUM_RELAYS] = {-1, -1, -1};

static void drawRelBlock(int ox, int i) {
  int y0 = i * 18;
  float lo, hi;
  ldrScale(lo, hi);
  float span = max(hi - lo, 1.0f);
  auto px = [&](float mv) -> int { return constrain((int)lroundf((mv - lo) * 79.0f / span), 0, 79); };

  oled.drawFrame(ox + 2, y0 + 2, 84, 9);
  if (!ldrAbsent) {
    int w = px(ldrShown) + 1;
    oled.drawBox(ox + 4, y0 + 4, w, 5);
  }
  thrShown[i] = thrShown[i] < 0 ? thrOff[i] : approach(thrShown[i], thrOff[i], 22);
  oled.setDrawColor(2);                                     // риска порогу видна і на заливці
  oled.drawVLine(ox + 4 + px(permToMv((uint16_t)lroundf(thrShown[i]))), y0 + 3, 7);
  oled.setDrawColor(1);

  // режим у рамці 24x9 зі зрізаними кутами; у вибраного реле — інвертований
  static const char* const M[] = {"Auto", "Off", "On"};
  bool sel = (relLevel == 2 && relSel == i);
  int bx = ox + 92, by = y0 + 5;
  if (sel) {
    oled.drawBox(bx + 1, by, 22, 9);
    oled.drawVLine(bx, by + 1, 7);
    oled.drawVLine(bx + 23, by + 1, 7);
    oled.setDrawColor(0);
  } else {
    oled.drawHLine(bx + 1, by, 22);
    oled.drawHLine(bx + 1, by + 8, 22);
    oled.drawVLine(bx, by + 1, 7);
    oled.drawVLine(bx + 23, by + 1, 7);
  }
  oled.setFont(u8g2_font_4x6_tr);
  modeAnim(i, effectiveMode(i));
  modeText(i, M, 0, bx + 12, y0 + 12, bx + 1, by + 1, bx + 23, by + 8);
  oled.setDrawColor(1);

  bool dot = relayOn[i];
  if (pending[i] && (millis() / 300) % 2) dot = !dot;     // блимає, поки чекає перемикання
  drawDot(ox + 119, y0 + 8, dot);

  char b[32];
  snprintf(b, sizeof(b), "%s -- %dmv поріг", RELAY_SHORT[i], (int)permToMv(thrOff[i]));
  oled.setFont(u8g2_font_4x6_t_cyrillic);
  oled.drawUTF8(ox + 2, y0 + 17, b);

}

static void drawRelays(int ox) {
  struct tm t;
  bool ok = localNow(t);
  oled.setFontMode(1);
  oled.setBitmapMode(1);

  for (int i = 0; i < NUM_RELAYS; i++) drawRelBlock(ox, i);
  oled.drawHLine(ox, 55, 128);

  // низ: LDR мВ, рівень %, пора доби -- освітленість
  float lo, hi;
  ldrScale(lo, hi);
  int pct = constrain((int)lroundf((ldrShown - lo) * 100.0f / max(hi - lo, 1.0f)), 0, 100);
  const char* lw = saverLightWord(ok ? &t : nullptr);
  if (!strcmp(lw, "--")) lw = "";
  char b[48], w[24];
  if (ok && *lw) snprintf(w, sizeof(w), "%s--%s", saverDayPart(t), lw);
  else if (ok)   snprintf(w, sizeof(w), "%s", saverDayPart(t));
  else           snprintf(w, sizeof(w), "%s", lw);
  if (ldrAbsent) snprintf(b, sizeof(b), "LDR -- %s", w);
  else           snprintf(b, sizeof(b), "LDR-%dmv L%d%% %s", (int)ldrShown, pct, w);
  oled.setFont(u8g2_font_4x6_t_cyrillic);
  oled.drawUTF8(ox + 2, 62, b);

  // рамка: рівень 0 — краї всього екрана (OK — зайти), далі — навколо вибраного реле.
  // Між станами вона плавно перетікає (стискається до блока, їде між блоками)
  float tg[4] = {0, 0, 128, 64};
  if (relLevel >= 1) { tg[1] = relSel * 18; tg[3] = (relSel == NUM_RELAYS - 1) ? 20 : 19; }
  for (int k = 0; k < 4; k++) relFr[k] = approach(relFr[k], tg[k], 18);
  oled.drawFrame(ox + (int)lroundf(relFr[0]), (int)lroundf(relFr[1]), (int)lroundf(relFr[2]), (int)lroundf(relFr[3]));
}


static void drawMenu(int ox);
static void drawToast();

static void drawPage(int p, int ox) {
  switch (p) {
    case P_MAIN: drawMain(ox); break;
    case P_REL:  drawRelays(ox); break;
    case P_MENU: drawMenu(ox); break;
  }
}

static void drawPages() {
  if (transT >= 1.0f) { drawPage(page, 0); return; }

  // Анімація прив'язана до часу, а не до кількості кадрів — однакова швидкість завжди
  transT = min(1.0f, (float)(millis() - transStart) / (float)TRANS_MS);
  float inv = 1.0f - transT;
  float e = 1.0f - inv * inv * inv;                // ease-out cubic

  if (transDir > 0) {                       // новий екран виїжджає справа
    drawPage(fromPage, 0);
    int off = (int)(128 * (1.0f - e));
    oled.setDrawColor(0);
    oled.drawBox(off, 0, 128 - off, 64);
    oled.setDrawColor(1);
    drawPage(page, off);
  } else {                                  // старий екран від'їжджає вправо
    drawPage(page, 0);
    int off = (int)(128 * e);
    if (off < 128) {
      oled.setDrawColor(0);
      oled.drawBox(off, 0, 128 - off, 64);
      oled.setDrawColor(1);
      drawPage(fromPage, off);
    }
  }
}

static void drawUi() {
  drawPages();
  drawToast();
}

// Скрінсейвер (дизайн з Lopaka): час, дата, день тижня, "освітленість--пора доби", погода.
// Освітленість — фоторезистор (4 рівні), пора доби — за годиною і сходом/заходом сонця (пора року).

// Іконки погоди 17x16 (XBM). Сонце/місяць залиті, хмари — контуром.
static const unsigned char wx_sun[] U8X8_PROGMEM = {0x00,0x01,0x00,0x00,0x01,0x00,0x08,0x21,0x00,0x10,0x10,0x00,0x80,0x03,0x00,0xc0,0x07,0x00,0xe0,0x0f,0x00,0xee,0xef,0x00,0xe0,0x0f,0x00,0xc0,0x07,0x00,0x80,0x03,0x00,0x10,0x10,0x00,0x08,0x21,0x00,0x00,0x01,0x00,0x00,0x01,0x00,0x00,0x00,0x00};
static const unsigned char wx_moon[] U8X8_PROGMEM = {0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x10,0x00,0x70,0x00,0x00,0x38,0x00,0x00,0x38,0x40,0x00,0x3c,0x00,0x00,0x7c,0x00,0x00,0x7c,0x00,0x00,0xfc,0x00,0x00,0xf8,0x23,0x00,0xf8,0x1f,0x00,0xf0,0x0f,0x00,0xc0,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const unsigned char wx_pcday[] U8X8_PROGMEM = {0x00,0x04,0x00,0x40,0x40,0x00,0x00,0x0e,0x00,0x80,0x31,0x00,0x90,0x20,0x01,0x40,0x40,0x00,0x40,0x40,0x00,0xe0,0x41,0x00,0x10,0x22,0x01,0x08,0x34,0x00,0x0c,0x0c,0x00,0x06,0x78,0x00,0x01,0xc0,0x00,0x01,0x80,0x00,0x01,0x80,0x00,0xfe,0x7f,0x00};
static const unsigned char wx_pcnight[] U8X8_PROGMEM = {0x00,0xe0,0x00,0x00,0x30,0x00,0x00,0x18,0x00,0x00,0x18,0x00,0x00,0x18,0x00,0x00,0x30,0x00,0xe0,0xe1,0x00,0x10,0x02,0x00,0x10,0x04,0x00,0x0c,0x18,0x00,0x02,0x20,0x00,0x02,0x20,0x00,0x02,0x20,0x00,0x0c,0x10,0x00,0xf0,0x0f,0x00,0x00,0x00,0x00};
static const unsigned char wx_cloudy[] U8X8_PROGMEM = {0x00,0x00,0x00,0x00,0x1f,0x00,0x00,0x11,0x00,0xc0,0x60,0x00,0x00,0x80,0x00,0xc0,0x81,0x00,0x20,0x86,0x00,0x10,0x44,0x00,0x10,0x18,0x00,0x0c,0x20,0x00,0x02,0x40,0x00,0x02,0x40,0x00,0x02,0x40,0x00,0x0e,0x30,0x00,0xf0,0x0f,0x00,0x00,0x00,0x00};
static const unsigned char wx_fog[] U8X8_PROGMEM = {0xc0,0x01,0x00,0x20,0x06,0x00,0x10,0x04,0x00,0x08,0x18,0x00,0x04,0x20,0x00,0x04,0x20,0x00,0x04,0x20,0x00,0x0c,0x30,0x00,0xf0,0x0f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf8,0x3f,0x00,0x00,0x00,0x00,0xfe,0x0f,0x00,0x00,0x00,0x00,0xf0,0xff,0x00};
static const unsigned char wx_drizzle[] U8X8_PROGMEM = {0x80,0x03,0x00,0x60,0x0c,0x00,0x20,0x08,0x00,0x10,0x10,0x00,0x0c,0x60,0x00,0x04,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x04,0x80,0x00,0xf8,0x7f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x11,0x00,0x00,0x00,0x00,0x40,0x04,0x00,0x00,0x00,0x00};
static const unsigned char wx_rain[] U8X8_PROGMEM = {0x80,0x03,0x00,0x60,0x0c,0x00,0x20,0x08,0x00,0x10,0x10,0x00,0x0c,0x60,0x00,0x04,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x04,0x80,0x00,0xf8,0x7f,0x00,0x00,0x00,0x00,0x20,0x22,0x00,0x10,0x11,0x00,0x00,0x00,0x00,0x80,0x08,0x00,0x40,0x04,0x00};
static const unsigned char wx_heavy[] U8X8_PROGMEM = {0x80,0x03,0x00,0x60,0x0c,0x00,0x20,0x08,0x00,0x10,0x10,0x00,0x0c,0x60,0x00,0x04,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x04,0x80,0x00,0xf8,0x7f,0x00,0x00,0x00,0x00,0x90,0x24,0x00,0x48,0x12,0x00,0x24,0x09,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const unsigned char wx_snow[] U8X8_PROGMEM = {0x80,0x03,0x00,0x60,0x0c,0x00,0x20,0x08,0x00,0x10,0x10,0x00,0x0c,0x60,0x00,0x04,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x04,0x80,0x00,0xf8,0x7f,0x00,0x00,0x00,0x00,0x10,0x10,0x00,0x38,0x38,0x00,0x10,0x11,0x00,0x80,0x03,0x00,0x00,0x01,0x00};
static const unsigned char wx_thunder[] U8X8_PROGMEM = {0x80,0x03,0x00,0x60,0x0c,0x00,0x20,0x08,0x00,0x10,0x10,0x00,0x0c,0x60,0x00,0x04,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x04,0x80,0x00,0xf8,0x7f,0x00,0x00,0x08,0x00,0x00,0x06,0x00,0x00,0x0f,0x00,0x00,0x06,0x00,0x00,0x02,0x00,0x00,0x01,0x00};

// Рух макета по екрану проти вигорання OLED: плавна траєкторія (синусоїди з різними періодами). 0 = стояти
#define SAVER_DRIFT      1
#define SAVER_PERIOD_X   17.0f     // с, повний цикл по X (менше = швидше)
#define SAVER_PERIOD_Y   11.0f     // с, повний цикл по Y
#define SAVER_TRAIL      0         // шлейф за панеллю, що рухається
#define SAVER_BG         1         // фон-галактика (смуга, зорі, метеори). Усе рухається — екран не вигорає
#define SAVER_FRAME_MS   33        // період кадрів скрінсейвера (~30 к/с)

// Іконка за WMO-кодом Open-Meteo, лівий верхній кут (x, y)
static void saverWeatherIcon(int x, int y, int c, bool day) {
  const unsigned char* bm;
  if      (c <= 1)                      bm = day ? wx_sun : wx_moon;
  else if (c == 2)                      bm = day ? wx_pcday : wx_pcnight;
  else if (c == 3)                      bm = wx_cloudy;
  else if (c <= 48)                     bm = wx_fog;
  else if (c <= 55)                     bm = wx_drizzle;            // мжичка
  else if (c <= 57)                     bm = wx_rain;               // паморозна мжичка
  else if (c == 61 || c == 80)          bm = wx_rain;
  else if (c <= 67 || c == 81 || c == 82) bm = wx_heavy;
  else if (c <= 77 || c == 85 || c == 86) bm = wx_snow;
  else                                  bm = wx_thunder;
  oled.drawXBMP(x, y, 17, 16, bm);
}

// Схід/захід (хвилини від півночі): з прогнозу, а якщо його нема — наближено для Львова за днем року
static void saverSun(const struct tm& t, int& rise, int& set) {
  if (sunDataValid()) { rise = sunRiseMin; set = sunSetMin; return; }
  float half = 6.0f + 2.0f * cosf(2.0f * 3.14159265f * (t.tm_yday - 172) / 365.0f);   // півтривалість дня, год
  rise = (int)((12.6f - half) * 60.0f);
  set  = (int)((12.6f + half) * 60.0f);
}

// Пора доби за годиною і порою року: межі рахуються від сходу/заходу сонця на сьогодні
// (точні дані Open-Meteo; без них — наближено для широти Львова за днем року).
//   Ранок  — від min(схід - 1 год, 06:00), але не раніше 04:00
//   День   — з 11:00
//   Вечір  — від min(захід - 30 хв, 18:00)
//   Ніч    — від min(захід + 3.5 год, 23:00)
// Приклад: кінець листопада (схід 07:49, захід 16:30): ранок 06:00, вечір 16:00, ніч 20:00;
//          червень (схід 05:07, захід 21:18): ранок 04:07, вечір 18:00, ніч 23:00.
static const char* saverDayPart(const struct tm& t) {
  int rise, set;
  saverSun(t, rise, set);
  int mStart = max(min(rise - 60, 6 * 60), 4 * 60);
  int eStart = min(set - 30, 18 * 60);
  int nStart = min(set + 210, 23 * 60);
  int m = t.tm_hour * 60 + t.tm_min;
  if (m < mStart || m >= nStart) return "Ніч";
  if (m >= eStart)               return "Вечір";
  if (m >= 11 * 60)              return "День";
  return "Ранок";
}

// 4 рівні освітленості з фоторезистора (за відкаліброваною шкалою) з гістерезисом, щоб слово не миготіло
static const char* saverLightWord(const struct tm* t) {
  static const char* const W[4] = {"Темно", "Сутінки", "Світло", "Яскраво"};
  static int8_t lv = -1;
  if (ldrAbsent) {                                       // фоторезистора нема — хоча б за сонцем
    if (!t) return "--";
    int rise, set;
    saverSun(*t, rise, set);
    int m = t->tm_hour * 60 + t->tm_min;
    return (m >= rise && m < set) ? W[2] : W[0];
  }
  // Рівень за швидким значенням; показ міняється, лише коли новий рівень тримається LIGHT_HOLD_MS,
  // і одразу стрибає на нього — без "перебігання" проміжних слів, поки фоторезистор доходить до темряви
  const uint32_t LIGHT_HOLD_MS = 1000;
  static int8_t   cand = -1;
  static uint32_t candT = 0;
  float f = ldrFrac(ldrFast < 0 ? ldrMv : ldrFast);
  int tgt = (lv < 0) ? (f < 0.25f ? 0 : f < 0.5f ? 1 : f < 0.75f ? 2 : 3) : lv;
  while (tgt < 3 && f > 0.25f * (tgt + 1) + 0.03f) tgt++;     // гістерезис 3% навколо меж
  while (tgt > 0 && f < 0.25f * tgt - 0.03f)       tgt--;
  if (lv < 0) lv = tgt;
  uint32_t now = millis();
  if (tgt == lv) {
    cand = -1;
  } else if (tgt != cand) {
    cand = tgt;                                          // новий кандидат: чекаємо, поки встоїться
    candT = now;
  } else if (now - candT >= LIGHT_HOLD_MS) {
    lv = tgt;
    cand = -1;
  }
  return W[lv];
}

// ---- Фон-галактика ----
// U8g2 працює з беззнаковими координатами: від'ємні "загортаються" і малюють лінії через увесь екран,
// тому все фонове малюємо з обрізанням по межах екрана.
static void bgPx(int x, int y) { if (x >= 0 && x < 128 && y >= 0 && y < 64) oled.drawPixel(x, y); }

#define GAL_DUST   80
#define GAL_STARS  20
struct GalDust { float t, off, ph; };         // t: 0..1 вздовж смуги, off: відхилення вбік, px
struct GalStar { float x, y, v, ph; };        // v: швидкість дрейфу вліво (px/с) — 3 шари паралаксу
static GalDust gd[GAL_DUST];
static GalStar gs[GAL_STARS];
static bool    galInit = false;
static bool    metOn = false;
static float   metX, metY, metVX, metVY, metLife;
static uint32_t metNext = 0;

// Смуга Чумацького шляху: від (-12,64) до (140,2)
static const float GAL_X0 = -12, GAL_Y0 = 64, GAL_DX = 152, GAL_DY = -62;

static void galReset(uint32_t now) {
  for (int i = 0; i < GAL_DUST; i++) {
    gd[i].t   = random(0, 1000) / 1000.0f;
    float g   = (random(-100, 101) + random(-100, 101) + random(-100, 101)) / 300.0f;   // ближче до центру смуги
    gd[i].off = g * 8.0f;
    gd[i].ph  = random(0, 628) / 100.0f;
  }
  for (int i = 0; i < GAL_STARS; i++) {
    gs[i].x  = random(0, 128);
    gs[i].y  = random(0, 64);
    gs[i].v  = (i % 3 == 0) ? 4.5f : ((i % 3 == 1) ? 2.5f : 1.2f);
    gs[i].ph = random(0, 628) / 100.0f;
  }
  metNext = now + 3000;
  galInit = true;
}

static void saverBgDraw(float dt, uint32_t now) {
  if (!galInit) galReset(now);
  float tt = now / 1000.0f;

  // смуга: частинки повільно течуть вздовж неї і мерехтять
  const float len = sqrtf(GAL_DX * GAL_DX + GAL_DY * GAL_DY);
  const float nx = -GAL_DY / len, ny = GAL_DX / len;      // нормаль
  for (int i = 0; i < GAL_DUST; i++) {
    GalDust& d = gd[i];
    d.t += 3.0f * dt / len;
    if (d.t > 1.0f) d.t -= 1.0f;
    float k = sinf(tt * 1.3f + d.ph);
    if (k > -0.25f) {
      int x = (int)(GAL_X0 + d.t * GAL_DX + d.off * nx);
      int y = (int)(GAL_Y0 + d.t * GAL_DY + d.off * ny);
      bgPx(x, y);
    }
  }

  // зорі: дрейф вліво з різною швидкістю, мерехтіння, у піку — хрестик
  for (int i = 0; i < GAL_STARS; i++) {
    GalStar& q = gs[i];
    q.x -= q.v * dt;
    if (q.x < -2) { q.x += 132; q.y = random(0, 64); }
    float k = sinf(tt * (0.9f + (i % 5) * 0.35f) + q.ph);
    int x = (int)q.x, y = (int)q.y;
    if (k > -0.4f) bgPx(x, y);
    if (k > 0.82f && q.v > 2.0f) { bgPx(x - 1, y); bgPx(x + 1, y); bgPx(x, y - 1); bgPx(x, y + 1); }
  }

  // метеор: раз на 7..15 с пролітає коротка риска з хвостом
  if (!metOn && (int32_t)(now - metNext) > 0) {
    metOn = true;
    metX = random(60, 150);  metY = random(0, 24);
    metVX = -(55 + random(0, 30));  metVY = 22 + random(0, 14);
    metLife = 0;
  }
  if (metOn) {
    metX += metVX * dt;  metY += metVY * dt;  metLife += dt;
    for (int k = 0; k < 6; k++) {                          // хвіст: чим далі, тим рідший
      if (k >= 3 && (k & 1)) continue;
      bgPx((int)(metX - metVX * 0.012f * k), (int)(metY - metVY * 0.012f * k));
    }
    if (metX < -10 || metY > 70 || metLife > 3.0f) { metOn = false; metNext = now + 7000 + random(0, 8000); }
  }
}

// Позиція макета в момент tsec (плавні синусоїди; макет займає x 24..102, y 16..45)
static void saverPos(float tsec, int& X, int& Y) {
#if SAVER_DRIFT
  const float TW = 6.2831853f;
  X = (int)lroundf(0.5f + 22.5f * sinf(TW * tsec / SAVER_PERIOD_X));
  Y = (int)lroundf(1.5f + 15.5f * sinf(TW * tsec / SAVER_PERIOD_Y + 1.3f));
#else
  X = 0; Y = 0;
#endif
}

// Привид контуру рамки (для шлейфу): крапками, чим старіший — тим рідше
static void saverGhost(int X, int Y, int mod) {
  for (int x = 26; x <= 100; x++) {
    if (((x + 16) % mod) == 0) bgPx(X + x, Y + 16);
    if (((x + 45) % mod) == 0) bgPx(X + x, Y + 45);
  }
  for (int y = 18; y <= 43; y++) {
    if (((24 + y) % mod) == 0)  bgPx(X + 24, Y + y);
    if (((102 + y) % mod) == 0) bgPx(X + 102, Y + y);
  }
  bgPx(X + 25, Y + 17);  bgPx(X + 101, Y + 17);
  bgPx(X + 25, Y + 44);  bgPx(X + 101, Y + 44);
}

static void drawSaver() {
  static const char* const WDAY[] = {"Неділя", "Понеділок", "Вівторок", "Середа", "Четвер", "П'ятниця", "Субота"};
  static uint32_t last = 0;

  uint32_t now = millis();
  float dt = last ? (now - last) / 1000.0f : 0;
  if (dt > 0.2f) dt = 0.2f;                               // після паузи не стрибаємо
  last = now;
  int X, Y;
  saverPos(now / 1000.0f, X, Y);       // від абсолютного часу: рух не залежить від затримок кадрів

  struct tm t;
  bool ok = localNow(t);
  char b[28];

  oled.setFontMode(1);
  oled.setBitmapMode(1);
  oled.setDrawColor(1);

#if SAVER_BG
  if (!allOff) saverBgDraw(dt, now);
#endif
#if SAVER_TRAIL
  if (!allOff) {                                           // шлейф: 4 привиди з позицій у минулому
    static const uint8_t MODS[4] = {2, 3, 4, 6};
    int px = X, py = Y;
    for (int k = 1; k <= 4; k++) {
      int gx, gy;
      saverPos(now / 1000.0f - k * 0.12f, gx, gy);
      if (gx == px && gy == py) continue;                  // стоїмо на місці — шлейфу нема
      saverGhost(gx, gy, MODS[k - 1]);
      px = gx; py = gy;
    }
  }
#endif
  if (!allOff) {                                           // панель непрозора, з відступом 1 px
    oled.setDrawColor(0);
    oled.drawBox(X + 23, Y + 15, 81, 32);
    oled.setDrawColor(1);
  }

  // рамка 24..102 x 16..45 із симетричними заокругленими кутами
  oled.drawHLine(X + 26, Y + 16, 75);                     // верх
  oled.drawHLine(X + 26, Y + 45, 75);                     // низ
  oled.drawVLine(X + 24,  Y + 18, 26);                    // ліво
  oled.drawVLine(X + 102, Y + 18, 26);                    // право
  oled.drawPixel(X + 25,  Y + 17);  oled.drawPixel(X + 101, Y + 17);   // кути зверху
  oled.drawPixel(X + 25,  Y + 44);  oled.drawPixel(X + 101, Y + 44);   // кути знизу
  oled.drawVLine(X + 80, Y + 17, 28);                     // розділювач ліво/право
  oled.drawHLine(X + 25, Y + 36, 55);                     // розділювач час/опис

  // час (двокрапка постійна)
  oled.setFont(u8g2_font_t0_11b_tr);
  if (ok) snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
  else    strcpy(b, "--:--");
  oled.drawStr(X + 27, Y + 26, b);

  // дата: притиснута до правого краю лівої клітинки
  oled.setFont(u8g2_font_4x6_tr);
  if (ok) snprintf(b, sizeof(b), "%02d/%02d", t.tm_mday, t.tm_mon + 1);
  else    strcpy(b, "--/--");
  oled.drawStr(X + 78 - oled.getUTF8Width(b), Y + 26, b);

  // день тижня (або причина, чому часу нема)
  oled.setFont(u8g2_font_5x8_t_cyrillic);
  const char* wd = ok ? WDAY[t.tm_wday] : (WiFi.status() != WL_CONNECTED ? "Нема Wi-Fi" : "Чекаю...");
  oled.drawUTF8(X + 27, Y + 34, wd);

  // "освітленість--пора доби"; при "все вимкнено" — просто підпис
  oled.setFont(u8g2_font_4x6_t_cyrillic);
  if (allOff) {
    strcpy(b, "Вимкнено");
  } else {
    const char* a = saverLightWord(ok ? &t : nullptr);
    if (!strcmp(a, "--")) a = "";
    if (ok && *a) {                                        // пора доби — лише коли відомий час
      snprintf(b, sizeof(b), "%s--%s", a, saverDayPart(t));
      if (oled.getUTF8Width(b) > 52) snprintf(b, sizeof(b), "%s-%s", a, saverDayPart(t));   // не вміщається — одне тире
    } else if (ok) {
      snprintf(b, sizeof(b), "%s", saverDayPart(t));
    } else {
      snprintf(b, sizeof(b), "%s", a);                    // часу нема — тільки освітленість
    }
  }
  oled.drawUTF8(X + 27, Y + 43, b);

  // погода: права клітинка 81..101, центр x=91
  if (wxValid()) {
    bool day = true;
    if (ok) { int rise, set; saverSun(t, rise, set); int m = t.tm_hour * 60 + t.tm_min; day = (m >= rise && m < set); }
    saverWeatherIcon(X + 83, Y + 18, wx.code, day);
    snprintf(b, sizeof(b), "%+d°", (int)lroundf(wx.temp));
  } else {
    strcpy(b, "--°");
  }
  oled.setFont(u8g2_font_4x6_tf);
  oled.drawUTF8(X + 91 - oled.getUTF8Width(b) / 2, Y + 42, b);
}

// =====================================================================
//  9b. МЕНЮ (третій екран): Settings / Info / Wifi / Weather / Light / Power
// =====================================================================
//  Крутилка на екрані меню: рівень 0 — гортає екрани (підсвічені краї), OK — зайти;
//  рівень 1 — рамка бігає по пунктах, OK — відкрити, BACK — вийти; рівень 2 — відкрите підменю.

static const unsigned char mi_gear[] U8X8_PROGMEM = {0xc0,0x03,0x48,0x12,0x34,0x2c,0x02,0x40,0xc4,0x23,0x24,0x24,0x13,0xc8,0x11,0x88,0x11,0x88,0x13,0xc8,0x24,0x24,0xc4,0x23,0x02,0x40,0x34,0x2c,0x48,0x12,0xc0,0x03};
static const unsigned char mi_info[] U8X8_PROGMEM = {0xe0,0x03,0x18,0x0c,0xc4,0x10,0x42,0x21,0xc2,0x20,0x01,0x40,0xc1,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x42,0x21,0xc2,0x21,0x04,0x10,0x18,0x0c,0xe0,0x03,0x00,0x00};
static const unsigned char mi_bars[] U8X8_PROGMEM = {0x00,0x70,0x00,0x50,0x00,0x50,0x00,0x50,0x00,0x57,0x00,0x57,0x00,0x57,0x00,0x57,0x70,0x57,0x70,0x57,0x70,0x57,0x70,0x57,0x77,0x57,0x77,0x57,0x77,0x77,0x00,0x00};
static const unsigned char mi_light[] U8X8_PROGMEM = {0x04,0x20,0xc8,0x13,0x20,0x04,0x10,0x08,0x95,0xa8,0x90,0x09,0x90,0x08,0x24,0x24,0x42,0x42,0x80,0x00,0xc0,0x03,0x00,0x00,0xc0,0x03,0x00,0x00,0x80,0x01,0x00,0x00};
static const unsigned char mi_power[] U8X8_PROGMEM = {0x80,0x00,0x80,0x00,0x98,0x0c,0xa4,0x12,0x92,0x24,0x8a,0x28,0x85,0x50,0x05,0x50,0x05,0x50,0x05,0x50,0x05,0x50,0x0a,0x28,0x12,0x24,0xe4,0x13,0x18,0x0c,0xe0,0x03};
// Рамка-курсор 20x21 (з "тінню" знизу)
static const unsigned char mi_frame[] U8X8_PROGMEM = {0xfc,0xff,0x03,0x02,0x00,0x04,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x01,0x00,0x08,0x03,0x00,0x0c,0xfe,0xff,0x07,0xfc,0xff,0x03};

struct MenuItem { const unsigned char* bm; uint8_t w, h, x, y; const char* label; uint8_t lx, ly; };
static const MenuItem MENU[6] = {
  {mi_gear,  16, 16, 16,  6, "Settings",  9, 31},
  {mi_info,  15, 16, 56,  7, "Info",     56, 31},
  {mi_bars,  15, 16, 95,  7, "Wifi",     96, 31},
  {wx_pcday, 17, 16, 15, 36, "Weather",  11, 61},
  {mi_light, 16, 16, 56, 37, "Light",    54, 61},
  {mi_power, 15, 16, 96, 37, "Power",    94, 61},
};
enum { M_SET = 0, M_INFO, M_WIFI, M_WX, M_LIGHT, M_POWER };

#define FONT_T u8g2_font_4x6_t_cyrillic


static void menuCell(int i, int& fx, int& fy) { fx = MENU[i].x + MENU[i].w / 2 - 10; fy = MENU[i].y - 2; }


// ---- Текст із символом "°" (його нема в кириличних шрифтах — малюємо кільце самі) ----
static int uW(const char* s) {
  int w = 0;
  char buf[64];
  while (*s) {
    const char* d = strstr(s, "°");
    int n = d ? (int)(d - s) : (int)strlen(s);
    if (n > 63) n = 63;
    if (n) { memcpy(buf, s, n); buf[n] = 0; w += oled.getUTF8Width(buf); }
    if (!d) break;
    w += 4;
    s = d + 2;
  }
  return w;
}
static void uT(int x, int y, const char* s) {
  char buf[64];
  while (*s) {
    const char* d = strstr(s, "°");
    int n = d ? (int)(d - s) : (int)strlen(s);
    if (n > 63) n = 63;
    if (n) { memcpy(buf, s, n); buf[n] = 0; x += oled.drawUTF8(x, y, buf); }
    if (!d) break;
    int top = y - oled.getAscent();
    oled.drawPixel(x + 1, top);
    oled.drawPixel(x, top + 1);
    oled.drawPixel(x + 2, top + 1);
    oled.drawPixel(x + 1, top + 2);
    x += 4;
    s = d + 2;
  }
}
static void uTR(int xr, int y, const char* s) { uT(xr - uW(s), y, s); }
static void uTC(int xc, int y, const char* s) { uT(xc - uW(s) / 2, y, s); }

// ---- Спливаюче повідомлення ----
static void toast(const char* m) {
  snprintf(toastMsg, sizeof(toastMsg), "%s", m);
  toastT = millis();
  dirty = true;
}
static void drawToast() {
  if (!toastMsg[0]) return;
  uint32_t el = millis() - toastT;
  const uint32_t DUR = 1400;
  if (el >= DUR) { toastMsg[0] = 0; return; }
  float in  = easeOut(el / 180.0f);
  float out = el > DUR - 180 ? easeOut((DUR - el) / 180.0f) : 1.0f;
  float k = min(in, out);
  oled.setFont(FONT_T);
  int w = uW(toastMsg) + 12;
  int x = 64 - w / 2;
  int y = 64 - (int)lroundf(14 * k);
  oled.setDrawColor(0);
  oled.drawRBox(x - 1, y - 1, w + 2, 14, 4);
  oled.setDrawColor(1);
  oled.drawRBox(x, y, w, 12, 3);
  oled.setDrawColor(0);
  uT(x + 6, y + 8, toastMsg);
  oled.setDrawColor(1);
}

// ---- Прогноз: копія для інтерфейсу (задача погоди пише fc на іншому ядрі) ----
static Forecast fcU;
static bool     fcUOk = false;
static uint32_t fcUVer = 0xFFFFFFFFUL;
static void fcSnap() {
  if (fcUVer == fcVer) return;
  portENTER_CRITICAL(&fcMux);
  fcU = fc;
  fcUOk = fcOk;
  fcUVer = fcVer;
  portEXIT_CRITICAL(&fcMux);
}

// Дні прогнозу, починаючи з сьогодні: idx[k] = номер дня в fc. Повертає кількість
static int wxDays(int* idx) {
  fcSnap();
  if (!fcUOk || !fcU.start) return 0;
  time_t n = time(nullptr);
  int first = 0;
  if (n > 1700000000 && (uint32_t)n > fcU.start) first = ((uint32_t)n - fcU.start) / 86400UL;
  int c = 0;
  for (int d = first; d < FC_DAYS; d++) idx[c++] = d;
  return c;
}

struct DaySum { int tmax, tmin, code, popMax, windMax; bool ok; };
static DaySum daySum(int d) {
  DaySum s = {-128, 127, 0, 0, 0, false};
  uint8_t cnt[100];
  memset(cnt, 0, sizeof(cnt));
  for (int h = 0; h < 24; h++) {
    int i = d * 24 + h;
    if (fcU.temp[i] == -128) continue;
    s.ok = true;
    s.tmax = max(s.tmax, (int)fcU.temp[i]);
    s.tmin = min(s.tmin, (int)fcU.temp[i]);
    if (fcU.pop[i] != 255)  s.popMax  = max(s.popMax, (int)fcU.pop[i]);
    if (fcU.wind[i] != 255) s.windMax = max(s.windMax, (int)fcU.wind[i]);
    if (h >= 8 && h <= 20 && fcU.code[i] < 100) cnt[fcU.code[i]]++;
  }
  // найчастіший стан удень; якщо опади йдуть хоча б 3 години — найчастіші опади
  int best = 0, wet = 0, bestWet = -1;
  for (int c = 0; c < 100; c++) {
    if (cnt[c] > cnt[best]) best = c;
    if (c >= 51) { wet += cnt[c]; if (cnt[c] && (bestWet < 0 || cnt[c] > cnt[bestWet])) bestWet = c; }
  }
  s.code = (wet >= 3 && bestWet >= 0) ? bestWet : best;
  return s;
}

static bool hourIsDay(int d, int h) {
  if (!fcU.rise[d] || !fcU.set[d]) return h >= 7 && h < 18;
  uint32_t t = fcU.start + d * 86400UL + h * 3600UL + 1800UL;
  return t >= fcU.rise[d] && t < fcU.set[d];
}

static const char* const WD_SHORT[7] = {"Нд", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
static const char* const WD_FULL[7]  = {"Неділя", "Понеділок", "Вівторок", "Середа", "Четвер", "П'ятниця", "Субота"};

// =====================================================================
//  Списки (Settings / Info / Wifi / Light)
// =====================================================================
enum RowKind : uint8_t { RK_INFO = 0, RK_EDIT, RK_ACTION, RK_TOGGLE };

static int wifiQuality() {
  if (WiFi.status() != WL_CONNECTED) return -1;
  return constrain(2 * (WiFi.RSSI() + 100), 0, 100);
}
static const char* wifiWord(int q) {
  if (q < 0)  return "--";
  if (q >= 80) return "Відмінний";
  if (q >= 60) return "Добрий";
  if (q >= 40) return "Середній";
  return "Слабкий";
}

static void setAp(bool on);

// ---- Settings ----
#define SET_N 8
static uint8_t kindSet(int i) {
  if (i <= 1 || i == 5) return RK_EDIT;
  if (i <= 4) return RK_TOGGLE;
  if (i == 6) return RK_ACTION;
  return RK_INFO;
}
static void rowSet(int i, char* l, char* v) {
  switch (i) {
    case 0: strcpy(l, "Екран спить");   sprintf(v, "%u с", (unsigned)sleepSec); break;
    case 1: strcpy(l, "Затримка реле"); sprintf(v, "%u с", (unsigned)switchDelaySec); break;
    case 2: strcpy(l, "Калібр. LDR");   strcpy(v, calAuto ? "Авто" : "Фікс."); break;
    case 3: strcpy(l, "За сонцем");     strcpy(v, sunGate ? "Вкл" : "Вимк"); break;
    case 4: strcpy(l, "Нічне вимк.");   strcpy(v, nightOff ? "Вкл" : "Вимк"); break;
    case 5: strcpy(l, "Година вимк.");  sprintf(v, "%02u:00", (unsigned)nightHour); break;
    case 6: strcpy(l, "Скинути LDR");   strcpy(v, "OK"); break;
    default: strcpy(l, "Прошивка");     strcpy(v, __DATE__); break;
  }
}
static void adjSet(int i, int dir, uint32_t gap) {
  switch (i) {
    case 0: sleepSec = constrain((int)sleepSec + dir * (gap < 110 ? 30 : 5), 10, 600); break;
    case 1: switchDelaySec = constrain((int)switchDelaySec + dir * (gap < 110 ? 5 : 1), 1, 120); break;
    case 5: nightHour = (nightHour + dir + 24) % 24; break;
  }
}
static void actSet(int i) {
  switch (i) {
    case 2: calAuto = !calAuto;   break;
    case 3: sunGate = !sunGate;   break;
    case 4: nightOff = !nightOff; break;
    case 6: ldrLo = ldrHi = ldrMv; saveCalib(); toast("LDR скинуто"); return;
  }
  saveSettings();
  toast("Збережено");
}

// ---- Light ----
#define LIGHT_N 9
static uint8_t kindLight(int i) { return i < 8 ? RK_EDIT : RK_INFO; }
static void rowLight(int i, char* l, char* v) {
  switch (i) {
    case 0: strcpy(l, "Екран");          sprintf(v, "%u%%", (unsigned)scrBr[0]); break;
    case 1: strcpy(l, "Заставка");       sprintf(v, "%u%%", (unsigned)scrBr[1]); break;
    case 2: strcpy(l, "Заставка вимк."); sprintf(v, "%u%%", (unsigned)scrBr[2]); break;
    case 3: strcpy(l, "LED режим");      strcpy(v, MODE_NAMES[ledCfg.mode]); break;
    case 4: strcpy(l, "День: яскр.");    sprintf(v, "%u%%", (unsigned)ledCfg.brN); break;
    case 5: strcpy(l, "День: ефект");    strcpy(v, FX_NAMES[ledCfg.fxN]); break;
    case 6: strcpy(l, "Темно: яскр.");   sprintf(v, "%u%%", (unsigned)ledCfg.brD); break;
    case 7: strcpy(l, "Темно: ефект");   strcpy(v, FX_NAMES[ledCfg.fxD]); break;
    default: {
      strcpy(l, "LED зараз");
      bool on; uint8_t pct, fx;
      ledTarget(on, pct, fx);
      if (on) sprintf(v, "%s %u%%", FX_NAMES[fx], (unsigned)pct);
      else    strcpy(v, "Вимк");
    }
  }
}
static void adjLight(int i, int dir, uint32_t gap) {
  int st = gap < 45 ? 10 : (gap < 110 ? 5 : 1);
  switch (i) {
    case 0: scrBr[0] = constrain((int)scrBr[0] + dir * st, 5, 100); applyContrast(); break;
    case 1: scrBr[1] = constrain((int)scrBr[1] + dir * st, 0, 100); break;
    case 2: scrBr[2] = constrain((int)scrBr[2] + dir * st, 0, 100); break;
    case 3: ledCfg.mode = (ledCfg.mode + dir + MODE_COUNT) % MODE_COUNT; break;
    case 4: ledCfg.brN = constrain((int)ledCfg.brN + dir * st, 0, 100); break;
    case 5: ledCfg.fxN = (ledCfg.fxN + dir + FX_COUNT) % FX_COUNT; break;
    case 6: ledCfg.brD = constrain((int)ledCfg.brD + dir * st, 0, 100); break;
    case 7: ledCfg.fxD = (ledCfg.fxD + dir + FX_COUNT) % FX_COUNT; break;
  }
}

// ---- Wifi ----
static bool apActive() { return apOn || apTemp; }
static int wifiN() { return apActive() ? 9 : 5; }
static uint8_t kindWifi(int i) { return i == 4 ? RK_TOGGLE : RK_INFO; }
static void rowWifi(int i, char* l, char* v) {
  bool c = WiFi.status() == WL_CONNECTED;
  switch (i) {
    case 0: strcpy(l, "Мережа"); snprintf(v, 28, "%s", c ? WiFi.SSID().c_str() : (netSsid[0] ? "шукаю..." : "не задана")); break;
    case 1: {
      strcpy(l, "Сигнал");
      int q = wifiQuality();
      if (q < 0) strcpy(v, "--");
      else       sprintf(v, "%s %d%%", wifiWord(q), q);
      break;
    }
    case 2: strcpy(l, "IP"); strcpy(v, c ? WiFi.localIP().toString().c_str() : "--"); break;
    case 3: strcpy(l, "Веб"); strcpy(v, c ? OTA_HOST ".local" : "--"); break;
    case 4: strcpy(l, "Точка доступу"); strcpy(v, apOn ? "Вкл" : (apTemp ? "Тимчас." : "Вимк")); break;
    case 5: strcpy(l, "Назва"); strcpy(v, AP_SSID); break;
    case 6: strcpy(l, "Пароль"); strcpy(v, AP_PASS); break;
    case 7: strcpy(l, "Адреса"); strcpy(v, WiFi.softAPIP().toString().c_str()); break;
    default: strcpy(l, "Підключено"); sprintf(v, "%d пристр.", (int)WiFi.softAPgetStationNum()); break;
  }
}
static void actWifi(int i) {
  if (i != 4) return;
  setAp(!apOn);
  toast(apOn ? "Точку ввімкнено" : (apTemp ? "Точка лишиться: нема Wi-Fi" : "Точку вимкнено"));
}

// ---- Info ----
#define INFO_N 32
static void rowInfo(int i, char* l, char* v) {
  struct tm t;
  bool ok = localNow(t);
  *v = 0;
  switch (i) {
    case 0: strcpy(l, "Час");  if (ok) sprintf(v, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec); else strcpy(v, "--"); break;
    case 1: strcpy(l, "Дата"); if (ok) sprintf(v, "%s %02d.%02d.%04d", WD_SHORT[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900); else strcpy(v, "--"); break;
    case 2: strcpy(l, "Пора доби"); strcpy(v, ok ? saverDayPart(t) : "--"); break;
    case 3: strcpy(l, "Світло в домі"); sprintf(v, "%s %.2fВ", gridMv >= POWER_DETECT_MV ? "Є" : "Нема", gridMv / 1000.0f); break;
    case 4: strcpy(l, "Лінія 5V"); if (batOk && batV[2] >= 0) sprintf(v, "%.2f В", batV[2]); else strcpy(v, "--"); break;
    case 5: case 6: {
      int c = i - 5;
      strcpy(l, c ? BAT2_LABEL : BAT1_LABEL);
      if (batOk && batV[c] >= BAT_MIN_V) sprintf(v, "%d%% %.2f В", batPercent(batV[c]), batV[c]);
      else strcpy(v, "--");
      break;
    }
    case 7: strcpy(l, "Фоторезистор"); if (ldrAbsent) strcpy(v, "нема"); else sprintf(v, "%d мВ", (int)ldrShown); break;
    case 8: {
      strcpy(l, "Освітленість");
      const char* w = saverLightWord(ok ? &t : nullptr);
      sprintf(v, "%s %d%%", w, (int)lroundf(ldrFrac(ldrShown) * 100));
      break;
    }
    case 9: { strcpy(l, "Шкала LDR"); float lo, hi; ldrScale(lo, hi); sprintf(v, "%d-%d мВ", (int)lo, (int)hi); break; }
    case 10: case 11: case 12: {
      int r = i - 10;
      sprintf(l, "Реле %s", RELAY_SHORT[r]);
      if (pending[r]) sprintf(v, "%s, чекає %uс", relayOn[r] ? "Увімк" : "Вимк", (unsigned)pendLeft[r]);
      else            sprintf(v, "%s, %s", relayOn[r] ? "Увімк" : "Вимк", MODE_NAMES[effectiveMode(r)]);
      break;
    }
    case 13: case 14: case 15: {
      int r = i - 13;
      sprintf(l, "Поріг %s", RELAY_SHORT[r]);
      sprintf(v, "%d мВ", (int)permToMv(thrOff[r]));
      break;
    }
    case 16: {
      strcpy(l, "Нічне вимк.");
      bool act = false;
      for (int r = 0; r < NUM_RELAYS; r++) act |= nightLatch[r];
      if (!nightOff) strcpy(v, "Вимк");
      else sprintf(v, "%s %02u:00", act ? "діє з" : "з", (unsigned)nightHour);
      break;
    }
    case 17: strcpy(l, "Схід / захід");
      if (sunDataValid()) sprintf(v, "%02d:%02d / %02d:%02d", sunRiseMin / 60, sunRiseMin % 60, sunSetMin / 60, sunSetMin % 60);
      else strcpy(v, "--");
      break;
    case 18: strcpy(l, "Погода"); if (wxValid()) sprintf(v, "%+d° %s", (int)lroundf(wx.temp), wxTextUa(wx.code)); else strcpy(v, "--"); break;
    case 19: strcpy(l, "Відчув., волог."); if (wxValid() && !wxFromCache) sprintf(v, "%+d° %u%%", (int)lroundf(wx.feel), (unsigned)wx.hum); else strcpy(v, "--"); break;
    case 20: strcpy(l, "Вітер"); if (wxValid() && !wxFromCache) sprintf(v, "%d км/г", (int)lroundf(wx.wind)); else strcpy(v, "--"); break;
    case 21: {
      strcpy(l, "Прогноз");
      fcSnap();
      if (fcUOk && fcU.fetched) {
        time_t f = fcU.fetched; struct tm tf; localtime_r(&f, &tf);
        sprintf(v, "%02d:%02d %02d.%02d", tf.tm_hour, tf.tm_min, tf.tm_mday, tf.tm_mon + 1);
      } else strcpy(v, fcUOk ? "є" : "нема");
      break;
    }
    case 22: strcpy(l, "Wi-Fi"); snprintf(v, 28, "%s", WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "нема"); break;
    case 23: strcpy(l, "IP"); strcpy(v, WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "--"); break;
    case 24: strcpy(l, "Сигнал"); if (WiFi.status() == WL_CONNECTED) sprintf(v, "%d дБм %d%%", (int)WiFi.RSSI(), wifiQuality()); else strcpy(v, "--"); break;
    case 25: strcpy(l, "Точка доступу"); strcpy(v, apActive() ? AP_SSID : "Вимк"); break;
    case 26: strcpy(l, "Годинник");
      if (ntpMono) sprintf(v, "NTP, %+.1f ppm", (float)driftPpm); else strcpy(v, ok ? "без NTP" : "--");
      break;
    case 27: strcpy(l, "Pro Micro");
      if (millis() - lastRxMs < 15000) sprintf(v, "v%s зв'язок є", proVer[0] ? proVer : "?"); else strcpy(v, "нема зв'язку");
      break;
    case 28: strcpy(l, "INA3221"); if (inaAddr) sprintf(v, "0x%02X OK", inaAddr); else strcpy(v, "нема"); break;
    case 29: strcpy(l, "Пам'ять"); sprintf(v, "%u КБ вільно", (unsigned)(ESP.getFreeHeap() / 1024)); break;
    case 30: {
      strcpy(l, "Працює");
      uint32_t s = millis() / 1000;
      sprintf(v, "%uд %02u:%02u", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60));
      break;
    }
    default: strcpy(l, "Прошивка"); strcpy(v, __DATE__); break;
  }
}

typedef void (*RowFn)(int, char*, char*);

static int subCount() {
  switch (menuSel) {
    case M_SET:   return SET_N;
    case M_INFO:  return INFO_N;
    case M_WIFI:  return wifiN();
    case M_LIGHT: return LIGHT_N;
  }
  return 0;
}
static RowFn subRowFn() {
  switch (menuSel) {
    case M_SET:   return rowSet;
    case M_INFO:  return rowInfo;
    case M_WIFI:  return rowWifi;
    default:      return rowLight;
  }
}
static uint8_t subKind(int i) {
  switch (menuSel) {
    case M_SET:   return kindSet(i);
    case M_WIFI:  return kindWifi(i);
    case M_LIGHT: return kindLight(i);
  }
  return RK_INFO;
}

static void subHeader(const char* title, const char* right) {
  oled.setFont(FONT_S);
  uT(2, 8, title);
  if (right && *right) {
    oled.setFont(FONT_T);
    uTR(125, 7, right);
  }
  oled.drawHLine(0, 10, 128);
}

// Список: 5 рядків по 10 px, плашка вибору плавно їде, рядки плавно прокручуються
static void drawList(int n, RowFn fn) {
  const int TOP = 12, RH = 10, VIS = 5;
  if (subSel >= n) subSel = n - 1;
  float first = constrain(subSel - 2, 0, max(0, n - VIS));
  subScroll = approach(subScroll, first, 16);
  float barT = TOP + (subSel - subScroll) * RH;
  subBarY = (subBarY < 0) ? barT : approach(subBarY, barT, 24);

  clipSet(0, 11, 128, 64);
  int by = (int)lroundf(subBarY);
  if (subEdit) oled.drawRFrame(1, by, 122, RH, 2);
  else         oled.drawRBox(1, by, 122, RH, 2);

  char l[32], v[40];
  for (int i = 0; i < n; i++) {
    float yt = TOP + (i - subScroll) * RH;
    if (yt < 2 || yt > 63) continue;
    int y = (int)lroundf(yt);
    fn(i, l, v);
    bool ed = subEdit && i == subSel;
    // підбір шрифтів, щоб нічого не налазило
    const uint8_t* lf = FONT_S;
    const uint8_t* vf = FONT_S;
    oled.setFont(lf); int lw = uW(l);
    oled.setFont(vf); int vw = uW(v) + (ed ? 12 : 0);
    if (lw + vw + 12 > 119) { vf = FONT_T; oled.setFont(vf); vw = uW(v) + (ed ? 12 : 0); }
    if (lw + vw + 12 > 119) { lf = FONT_T; oled.setFont(lf); lw = uW(l); }

    oled.setDrawColor(2);
    oled.setFont(lf);
    uT(4, y + (lf == FONT_S ? 8 : 7), l);
    oled.setFont(vf);
    int vy = y + (vf == FONT_S ? 8 : 7);
    if (ed) {
      // значення, що правиться: інвертоване, зі стрілками
      int tw = uW(v);
      int x = 119 - tw - 8;
      oled.setDrawColor(1);
      oled.drawRBox(x - 3, y + 1, tw + 12, RH - 2, 2);
      oled.setDrawColor(0);
      uT(x + 3, vy, v);
      oled.drawTriangle(x - 1, y + 5, x + 1, y + 3, x + 1, y + 7);
      oled.drawTriangle(x + tw + 7, y + 5, x + tw + 5, y + 3, x + tw + 5, y + 7);
    } else {
      uTR(119, vy, v);
    }
    oled.setDrawColor(1);
  }
  clipReset();

  // смуга прокрутки
  if (n > VIS) {
    for (int y = 12; y < 64; y += 2) oled.drawPixel(126, y);
    int th = max(6, 52 * VIS / n);
    int ty = 12 + (int)lroundf((52 - th) * subScroll / (float)(n - VIS));
    oled.drawBox(125, ty, 3, th);
  }
}

// =====================================================================
//  Погода: картки на 7 днів і детальний день
// =====================================================================

static void drawRefreshIcon(int cx, int cy, bool spin) {
  float a0 = spin ? millis() / 160.0f : -1.2f;
  for (int j = 0; j < 22; j++) {                     // дуга 270°
    float a = a0 + j * 0.215f;
    oled.drawPixel(cx + (int)lroundf(6.5f * cosf(a)), cy + (int)lroundf(6.5f * sinf(a)));
  }
  float ae = a0 + 21 * 0.215f;                      // стрілка на кінці дуги
  int ex = cx + (int)lroundf(6.5f * cosf(ae)), ey = cy + (int)lroundf(6.5f * sinf(ae));
  float tx = -sinf(ae), ty = cosf(ae);              // дотична (напрямок руху)
  float nx = cosf(ae), ny = sinf(ae);
  oled.drawTriangle(ex + (int)lroundf(tx * 3), ey + (int)lroundf(ty * 3),
                    ex + (int)lroundf(nx * 3 - tx), ey + (int)lroundf(ny * 3 - ty),
                    ex - (int)lroundf(nx * 3 + tx), ey - (int)lroundf(ny * 3 + ty));
}

static void drawWxCard(int k, int x, const int* idx, bool sel) {
  int cx = x + 20;
  char b[24];
  if (sel) {
    oled.drawRFrame(x, 12, 40, 52, 3);
    oled.drawRBox(x + 2, 14, 36, 8, 2);
  }
  oled.setDrawColor(2);
  oled.setFont(FONT_T);
  if (k == 0) {
    uTC(cx, 20, "Оновити");
    oled.setDrawColor(1);
    drawRefreshIcon(cx, 31, wxBusy);
    oled.setFont(FONT_T);
    if (WiFi.status() != WL_CONNECTED) { uTC(cx, 50, "нема"); uTC(cx, 57, "Wi-Fi"); }
    else if (wxBusy) {
      int nd = (millis() / 300) % 4;
      strcpy(b, "...");
      b[nd < 3 ? nd + 1 : 3] = 0;
      uTC(cx, 50, "качаю");
      uTC(cx, 57, b);
    } else {
      fcSnap();
      if (fcUOk && fcU.fetched) {
        time_t f = fcU.fetched; struct tm tf; localtime_r(&f, &tf);
        uTC(cx, 50, "було о");
        sprintf(b, "%02d:%02d", tf.tm_hour, tf.tm_min);
        uTC(cx, 57, b);
      } else uTC(cx, 50, "OK");
    }
    return;
  }
  int d = idx[k - 1];
  time_t t = fcU.start + d * 86400UL + 43200UL;
  struct tm tm;
  localtime_r(&t, &tm);
  struct tm now;
  bool today = localNow(now) && now.tm_yday == tm.tm_yday && now.tm_year == tm.tm_year;
  if (today) strcpy(b, "Сьогодні");
  else       sprintf(b, "%s %d", WD_SHORT[tm.tm_wday], tm.tm_mday);
  uTC(cx, 20, b);
  oled.setDrawColor(1);
  DaySum s = daySum(d);
  if (!s.ok) { uTC(cx, 40, "--"); return; }
  saverWeatherIcon(cx - 8, 23, s.code, true);
  oled.setFont(FONT_S);
  sprintf(b, "%+d°", s.tmax);
  uTC(cx, 48, b);
  oled.setFont(FONT_T);
  sprintf(b, "%+d°", s.tmin);
  uTC(cx, 55, b);
  sprintf(b, "%d%%", s.popMax);
  int w = uW(b);
  int x0 = cx - (w + 5) / 2;
  // крапля
  oled.drawPixel(x0 + 1, 58);
  oled.drawHLine(x0, 59, 3);
  oled.drawHLine(x0, 60, 3);
  oled.drawPixel(x0 + 1, 61);
  uT(x0 + 5, 62, b);
}

static void drawWxDetail(int oy) {
  int idx[FC_DAYS];
  int nd = wxDays(idx);
  if (wxCard < 1 || wxCard > nd) return;
  int d = idx[wxCard - 1];
  char b[40];
  time_t t0 = fcU.start + d * 86400UL + 43200UL;
  struct tm tm;
  localtime_r(&t0, &tm);

  oled.setFont(FONT_S);
  sprintf(b, "%s %02d.%02d", WD_FULL[tm.tm_wday], tm.tm_mday, tm.tm_mon + 1);
  uT(2, oy + 8, b);
  if (fcU.rise[d] && fcU.set[d]) {
    time_t r = fcU.rise[d], st = fcU.set[d];
    struct tm tr, ts;
    localtime_r(&r, &tr);
    localtime_r(&st, &ts);
    sprintf(b, "%02d:%02d-%02d:%02d", tr.tm_hour, tr.tm_min, ts.tm_hour, ts.tm_min);
    oled.setFont(FONT_T);
    uTR(126, oy + 7, b);
  }
  oled.drawHLine(0, oy + 10, 128);

  int h = wxHour, i = d * 24 + h;
  if (fcU.temp[i] == -128) { oled.setFont(FONT_S); uTC(64, oy + 38, "Нема даних"); return; }
  saverWeatherIcon(1, oy + 12, fcU.code[i], hourIsDay(d, h));
  oled.setFont(FONT_S);
  sprintf(b, "%02d:00 %+d°", h, fcU.temp[i]);
  uT(21, oy + 20, b);
  oled.setFont(FONT_T);
  uT(21, oy + 28, wxTextUa(fcU.code[i]));
  if (fcU.pop[i] != 255)  { sprintf(b, "опади %u%%", fcU.pop[i]);  uTR(126, oy + 17, b); }
  if (fcU.hum[i] != 255)  { sprintf(b, "волог. %u%%", fcU.hum[i]); uTR(126, oy + 23, b); }
  if (fcU.wind[i] != 255) { sprintf(b, "вітер %uкм/г", fcU.wind[i]); uTR(126, oy + 29, b); }

  // графік температури за добу
  DaySum s = daySum(d);
  int lo = s.tmin, hi = s.tmax;
  if (hi - lo < 4) { int m = (hi + lo) / 2; lo = m - 2; hi = m + 2; }
  const int GX = 16, GY0 = oy + 34, GY1 = oy + 55;
  auto X = [&](int hh) { return GX + hh * 110 / 23; };
  auto Y = [&](int tv) { return GY1 - (tv - lo) * (GY1 - GY0) / (hi - lo); };
  sprintf(b, "%+d", s.tmax); uTR(13, GY0 + 4, b);
  sprintf(b, "%+d", s.tmin); uTR(13, GY1, b);
  for (int x = GX; x < 128; x += 2) oled.drawPixel(x, GY1 + 1);       // вісь
  int px = -1, py = -1;
  for (int hh = 0; hh < 24; hh++) {
    int tv = fcU.temp[d * 24 + hh];
    if (tv == -128) { px = -1; continue; }
    int x = X(hh), y = Y(tv);
    if (px >= 0) {
      oled.drawLine(px, py, x, y);
      for (int xx = px + 1; xx <= x; xx++) {                           // штрихування під кривою
        if (xx % 2) continue;
        int yy = py + (y - py) * (xx - px) / (x - px);
        for (int k = yy + 2; k <= GY1; k += 2) oled.drawPixel(xx, k);
      }
    }
    px = x; py = y;
  }
  static const uint8_t TH[4] = {0, 6, 12, 18};
  oled.setFont(FONT_T);
  for (int k = 0; k < 4; k++) {
    sprintf(b, "%d", TH[k]);
    int x = X(TH[k]);
    uT(max(0, x - uW(b) / 2), oy + 63, b);
  }
  // курсор години
  int cx = X(h), cy = Y(fcU.temp[i]);
  for (int y = GY0 - 2; y <= GY1; y += 2) oled.drawPixel(cx, y);
  oled.setDrawColor(0);
  oled.drawDisc(cx, cy, 3);
  oled.setDrawColor(1);
  oled.drawDisc(cx, cy, 2);
}

static void drawWeather() {
  int idx[FC_DAYS];
  int nd = wxDays(idx);
  int nCards = nd + 1;
  if (wxCard >= nCards) wxCard = nCards - 1;

  if (wxDetT < 0.999f || !wxDetail) {
    char r[24] = "";
    fcSnap();
    if (wxValid()) sprintf(r, "зараз %+d°", (int)lroundf(wx.temp));
    subHeader("Прогноз", r);
    float tgt = constrain(wxCard - 1, 0, max(0, nCards - 3));
    wxCardX = approach(wxCardX, tgt, 14);
    clipSet(0, 11, 128, 64);
    wxSelF = approach(wxSelF, wxCard, 16);                 // рамка вибору плавно їде між картками
    {
      int sx = 1 + (int)lroundf((wxSelF - wxCardX) * 42);
      oled.drawRFrame(sx, 12, 40, 52, 3);
      oled.drawRBox(sx + 2, 14, 36, 8, 2);
    }
    for (int k = 0; k < nCards; k++) {
      int x = 1 + (int)lroundf((k - wxCardX) * 42);
      if (x < -40 || x > 127) continue;
      drawWxCard(k, x, idx, false);
    }
    clipReset();
    if (nd == 0) {
      oled.setFont(FONT_T);
      uT(46, 34, "Прогнозу ще нема.");
      uT(46, 42, "Потрібен Wi-Fi");
    }
  }
  if (wxDetail) {
    wxDetT = approach(wxDetT, wxDetClose ? 0.0f : 1.0f, 12);
    if (wxDetClose && wxDetT < 0.02f) { wxDetail = false; wxDetClose = false; wxDetT = 0; return; }
    int oy = (int)lroundf((1.0f - wxDetT) * 64);
    if (oy < 64) {
      oled.setDrawColor(0);
      oled.drawBox(0, oy, 128, 64 - oy);
      oled.setDrawColor(1);
      if (oy > 0) oled.drawHLine(0, oy, 128);
      clipSet(0, oy, 128, 64);
      drawWxDetail(oy);
      clipReset();
    }
  }
}

// =====================================================================
//  Power
// =====================================================================

static void drawRestartIcon(int cx, int cy) {
  oled.drawCircle(cx, cy, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  oled.drawTriangle(cx, cy - 9, cx, cy - 3, cx + 4, cy - 6);
}

static void drawSpinner(int cx, int cy, int r) {
  int head = (millis() / 90) % 8;
  for (int j = 0; j < 8; j++) {
    float a = j * 0.785398f;
    int x = cx + (int)lroundf(r * cosf(a)), y = cy + (int)lroundf(r * sinf(a));
    int age = (head - j + 8) % 8;
    if (age == 0)      oled.drawDisc(x, y, 2);
    else if (age < 3)  oled.drawDisc(x, y, 1);
    else               oled.drawPixel(x, y);
  }
}

#define PW_RESTART_MS 1600
#define PW_DEEP_MS    2600

static void drawPower() {
  static const int CX[3] = {20, 62, 107};
  static const char* const L[3] = {"Сон", "Перезапуск", "Глиб. сон"};
  static const char* const D1[3] = {"Все вимкнено, заставка", "Перезавантажити ESP32", "Вимк. все + екран і Wi-Fi"};
  static const char* const D2[3] = {"CONF - увімкнути", "OK - підтвердити", "Утримай OK 2с - увімкн."};

  if (pwAct) {
    uint32_t dur = pwAct == 1 ? PW_RESTART_MS : PW_DEEP_MS;
    float p = constrain((millis() - pwActT) / (float)dur, 0.0f, 1.0f);
    oled.setFont(FONT_S);
    uTC(64, 10, pwAct == 1 ? "Перезавантаження" : "Глибокий сон");
    drawSpinner(64, 26, 7);
    oled.drawRFrame(14, 39, 100, 7, 3);
    int w = (int)lroundf(96 * p);
    if (w > 0) oled.drawRBox(16, 41, w, 3, 1);
    oled.setFont(FONT_T);
    if (pwAct == 2) uTC(64, 54, "Увімкнути: утримай OK 2с");
    uTC(64, 62, "BACK - скасувати");
    return;
  }

  subHeader("Живлення", "");
  oled.setFont(FONT_T);
  for (int k = 0; k < 3; k++) {
    int cx = CX[k];
    if (k == 0)      oled.drawXBMP(cx - 8, 15, 17, 16, wx_moon);
    else if (k == 1) drawRestartIcon(cx, 24);
    else             oled.drawXBMP(cx - 7, 16, 15, 16, mi_power);
    uTC(cx, 43, L[k]);
  }
  float tgt = CX[pwSel] - 10;
  pwCurX = (pwCurX < 0) ? tgt : approach(pwCurX, tgt, 20);
  oled.drawXBMP((int)lroundf(pwCurX), 13, 20, 21, mi_frame);
  oled.drawHLine(0, 47, 128);
  uTC(64, 55, D1[pwSel]);
  uTC(64, 63, D2[pwSel]);
}

// =====================================================================
//  Сітка, відкриття підменю, керування
// =====================================================================

static void drawSub() {
  switch (menuSel) {
    case M_SET:   subHeader("Налаштування", ""); drawList(SET_N, rowSet); break;
    case M_INFO: {
      char r[12];
      sprintf(r, "%d/%d", subSel + 1, INFO_N);
      subHeader("Інформація", r);
      drawList(INFO_N, rowInfo);
      break;
    }
    case M_WIFI: {
      subHeader("Wi-Fi", "");
      int q = wifiQuality();                         // 4 стовпчики сигналу в заголовку
      for (int k = 0; k < 4; k++) {
        int hgt = 2 + k * 2, x = 110 + k * 4;
        if (q >= 0 && q >= k * 25 + 1) oled.drawBox(x, 9 - hgt, 3, hgt);
        else                            oled.drawFrame(x, 9 - hgt, 3, hgt);
      }
      drawList(wifiN(), rowWifi);
      break;
    }
    case M_WX:    drawWeather(); break;
    case M_LIGHT: subHeader("Світло", ""); drawList(LIGHT_N, rowLight); break;
    case M_POWER: drawPower(); break;
  }
}

static void drawGrid(int ox) {
  oled.setFontMode(1);
  oled.setBitmapMode(1);
  oled.setFont(u8g2_font_4x6_tr);
  for (int i = 0; i < 6; i++) {
    const MenuItem& m = MENU[i];
    oled.drawXBMP(ox + m.x, m.y, m.w, m.h, m.bm);
    oled.drawStr(ox + m.lx, m.ly, m.label);
  }
  // рамка: рівень 0 — краї екрана, рівень 1 — навколо пункту; між ними плавно стискається/розтягується
  float tg[4] = {0, 0, 128, 64};
  if (menuLevel >= 1) { int fx, fy; menuCell(menuSel, fx, fy); tg[0] = fx; tg[1] = fy; tg[2] = 20; tg[3] = 21; }
  for (int k = 0; k < 4; k++) menuR[k] = approach(menuR[k], tg[k], 18);
  int rx = ox + (int)lroundf(menuR[0]), ry = (int)lroundf(menuR[1]);
  if (menuLevel >= 1 && fabsf(menuR[2] - 20) < 0.6f && fabsf(menuR[3] - 21) < 0.6f)
    oled.drawXBMP(rx, ry, 20, 21, mi_frame);
  else if (menuLevel == 0 && menuR[2] > 127.4f)
    oled.drawFrame(ox, 0, 128, 64);
  else
    oled.drawRFrame(rx, ry, (int)lroundf(menuR[2]), (int)lroundf(menuR[3]), 3);
}

#define SUB_ANIM_MS 230

static void drawMenu(int ox) {
  if (menuLevel < 2) { drawGrid(ox); return; }
  if (subAnimDir == 0) { drawSub(); return; }
  float p = constrain((millis() - subAnimT0) / (float)SUB_ANIM_MS, 0.0f, 1.0f);
  float e = easeOut(p);
  if (subAnimDir < 0) e = 1.0f - e;
  int fx, fy;
  menuCell(menuSel, fx, fy);
  int x0 = (int)lroundf(fx * (1 - e)), y0 = (int)lroundf(fy * (1 - e));
  int x1 = (int)lroundf((fx + 19) * (1 - e) + 127 * e), y1 = (int)lroundf((fy + 20) * (1 - e) + 63 * e);
  drawGrid(0);
  oled.setDrawColor(0);
  oled.drawBox(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
  oled.setDrawColor(1);
  clipB[0] = x0; clipB[1] = y0; clipB[2] = x1 + 1; clipB[3] = y1 + 1;
  clipReset();
  drawSub();
  clipB[0] = 0; clipB[1] = 0; clipB[2] = 128; clipB[3] = 64;
  oled.setMaxClipWindow();
  oled.drawFrame(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
  if (p >= 1.0f) {
    if (subAnimDir < 0) menuLevel = 1;
    subAnimDir = 0;
  }
}

static bool menuAnimating() {
  return page == P_MENU && (subAnimDir != 0 || pwAct || toastMsg[0] ||
         (menuLevel == 2 && menuSel == M_WX && (wxBusy || (wxDetail && wxDetT < 1.0f))));
}

static void openSub() {
  menuLevel = 2;
  subSel = 0; subScroll = 0; subBarY = -1; subEdit = false;
  wxDetail = false; wxDetClose = false; wxDetT = 0; wxCard = 1; wxCardX = 0; wxSelF = 1;
  pwSel = 0; pwCurX = -1; pwAct = 0;
  subAnimDir = 1;
  subAnimT0 = millis();
}

static void closeSub() {
  subAnimDir = -1;
  subAnimT0 = millis();
}

static void subCommitEdit(bool quiet) {
  if (!subEdit) return;
  subEdit = false;
  ledPreview = -1;
  saveSettings();
  applyContrast();
  if (!quiet) toast("Збережено");
}

// Сон екрана: правки зберегти, меню закрити
static void menuReset() {
  subCommitEdit(true);
  pwAct = 0;
  wxDetail = false;
  wxDetClose = false;
  subAnimDir = 0;
  menuLevel = 0;
}

static void menuEncoder(int dir) {
  static uint32_t lastTick = 0;
  uint32_t n = millis();
  uint32_t gap = n - lastTick;
  lastTick = n;

  if (menuLevel == 1) { menuSel = (menuSel + dir + 6) % 6; return; }
  if (subAnimDir) return;
  switch (menuSel) {
    case M_WX:
      if (wxDetClose) return;
      if (wxDetail) wxHour = constrain((int)wxHour + dir, 0, 23);
      else {
        int idx[FC_DAYS];
        wxCard = constrain((int)wxCard + dir, 0, wxDays(idx));
      }
      return;
    case M_POWER:
      if (!pwAct) pwSel = constrain((int)pwSel + dir, 0, 2);
      return;
  }
  int n2 = subCount();
  if (subEdit) {
    if (menuSel == M_SET)   adjSet(subSel, dir, gap);
    if (menuSel == M_LIGHT) adjLight(subSel, dir, gap);
  } else {
    subSel = constrain((int)subSel + dir, 0, n2 - 1);
  }
}

static void menuOk() {
  if (menuLevel == 0) { menuLevel = 1; return; }
  if (menuLevel == 1) { openSub(); return; }
  if (subAnimDir) return;
  switch (menuSel) {
    case M_WX:
      if (wxDetail) return;
      if (wxCard == 0) {
        if (WiFi.status() != WL_CONNECTED) { toast("Нема Wi-Fi"); return; }
        wxForce = true;
        toast("Оновлюю прогноз");
      } else {
        int idx[FC_DAYS];
        if (wxDays(idx) == 0) return;
        struct tm t;
        wxHour = (wxCard == 1 && localNow(t)) ? t.tm_hour : 12;
        wxDetail = true;
        wxDetT = 0;
      }
      return;
    case M_POWER:
      if (pwAct) return;
      if (pwSel == 0) { setAllOff(true); return; }
      pwAct = pwSel;                                 // 1 = перезапуск, 2 = глибокий сон
      pwActT = millis();
      return;
  }
  uint8_t k = subKind(subSel);
  if (k == RK_EDIT) {
    if (subEdit) subCommitEdit(false);
    else {
      subEdit = true;
      if (menuSel == M_LIGHT && subSel >= 4 && subSel <= 7) ledPreview = subSel <= 5 ? 0 : 1;
    }
  } else if (k == RK_TOGGLE || k == RK_ACTION) {
    if (menuSel == M_SET)  actSet(subSel);
    if (menuSel == M_WIFI) actWifi(subSel);
  }
}

static void menuBack() {
  if (menuLevel == 1) { menuLevel = 0; return; }
  if (menuLevel == 0) { gotoPage(P_MAIN, -1); return; }
  if (subAnimDir) return;
  if (subEdit)  { subCommitEdit(false); return; }
  if (pwAct)    { pwAct = 0; toast("Скасовано"); return; }
  if (wxDetail) { wxDetClose = true; return; }
  closeSub();
}

// ---- Глибокий сон: усе вимкнено (реле, стрічка, екран, Wi-Fi). Увімкнути — утримати OK 2 с ----
static void enterDeep() {
  pwAct = 0;
  menuReset();
  relLevel = 0;
  editing = false;
  page = fromPage = P_MAIN;
  transT = 1.0f;
  deepOff = true;
  okHeld = false;                                   // OK натиснули для підтвердження — не рахуємо як утримання
  applyRelays();
  ledSync();
  if (oledOk) { oled.clearBuffer(); oled.sendBuffer(); oled.setPowerSave(1); }
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[PWR] глибокий сон");
}

// Вихід — чистий перезапуск: Wi-Fi, OTA, mDNS і веб гарантовано піднімаються заново
static void exitDeep() {
  Serial.println("[PWR] прокидаємось");
  if (oledOk) {
    oled.setPowerSave(0);
    oled.setContrast(255);
    oled.clearBuffer();
    txtC(FONT_M, 64, 36, "Увімкнення...");
    oled.sendBuffer();
  }
  delay(300);
  ESP.restart();
}

// Точка доступу потрібна, якщо її ввімкнули вручну або тимчасово (нема мережі)
static void applyApMode() {
  if (deepOff) return;
  bool want = apActive();
  bool has = WiFi.getMode() & WIFI_AP;
  if (want && !has) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
  } else if (!want && has) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }
}

static void setAp(bool on) {
  apOn = on;
  prefs.begin("ctl", false);
  prefs.putBool("ap", apOn);
  prefs.end();
  applyApMode();
}

static void netBegin() {
  if (netSsid[0]) WiFi.begin(netSsid, netPass);
}

// Раз на секунду: тимчасова точка доступу, коли мережа не задана або недоступна 2 хв.
// Знімається, коли Wi-Fi тримається 1 хв і до точки ніхто не підключений.
static void netWatch() {
  static uint32_t lostT = 0, connT = 0, t = 0;
  uint32_t now = millis();
  if (deepOff || now - t < 1000) return;
  t = now;
  bool c = WiFi.status() == WL_CONNECTED;
  if (c) { lostT = now; if (!connT) connT = now; } else connT = 0;
  if (!apTemp && (!netSsid[0] || now - lostT >= AP_TEMP_AFTER_MS)) {
    apTemp = true;
    applyApMode();
    Serial.println("[WiFi] тимчасова точка доступу");
  } else if (apTemp && c && now - connT >= 60000 && WiFi.softAPgetStationNum() == 0) {
    apTemp = false;
    applyApMode();
  }
  if (netApplyAt && (int32_t)(now - netApplyAt) >= 0) {   // нова мережа з веб-меню
    netApplyAt = 0;
    lostT = now;
    WiFi.disconnect(false);
    delay(50);
    netBegin();
    Serial.printf("[WiFi] нова мережа: %s\n", netSsid);
  }
}

// Викликається з loop(): відлік перезапуску / глибокого сну
static void menuTick() {
  if (!pwAct) return;
  uint32_t el = millis() - pwActT;
  if (pwAct == 1 && el >= PW_RESTART_MS) {
    saveSettings();
    if (oledOk) { oled.clearBuffer(); oled.sendBuffer(); }
    delay(50);
    ESP.restart();
  }
  if (pwAct == 2 && el >= PW_DEEP_MS) enterDeep();
}

// =====================================================================
//  9c. ВЕБ-МЕНЮ (http://esp32-test.local, IP плати, або 192.168.4.1 через точку доступу)
// =====================================================================

static WebServer web(80);
static uint32_t webRestartAt = 0, webDeepAt = 0;

static void jf(String& s, const char* fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  s += b;
}

static String jsEsc(const char* p) {
  String o;
  for (; *p; p++) {
    if (*p == '"' || *p == '\\') { o += '\\'; o += *p; }
    else if ((uint8_t)*p < 0x20) o += ' ';
    else o += *p;
  }
  return o;
}

static void webState() {
  String s;
  s.reserve(2600);
  struct tm t;
  bool ok = localNow(t);
  s += '{';
  if (ok) jf(s, "\"time\":\"%02d:%02d:%02d\",\"date\":\"%s, %02d.%02d.%04d\",\"daypart\":\"%s\",",
             t.tm_hour, t.tm_min, t.tm_sec, WD_FULL[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, saverDayPart(t));
  else    s += "\"time\":\"\",\"date\":\"\",\"daypart\":\"\",";
  bool isDay = true;
  if (ok) { int r, st; saverSun(t, r, st); int m = t.tm_hour * 60 + t.tm_min; isDay = m >= r && m < st; }
  jf(s, "\"isDay\":%s,", isDay ? "true" : "false");
  jf(s, "\"grid\":{\"on\":%s,\"v\":%.2f},", gridMv >= POWER_DETECT_MV ? "true" : "false", gridMv / 1000.0f);
  jf(s, "\"line\":%.2f,", (batOk && batV[2] >= 0) ? batV[2] : -1.0f);
  s += "\"bat\":[";
  for (int c = 0; c < 2; c++) {
    bool v = batOk && batV[c] >= BAT_MIN_V;
    jf(s, "%s{\"n\":\"%s\",\"v\":%.2f,\"p\":%d}", c ? "," : "", c ? BAT2_LABEL : BAT1_LABEL, v ? batV[c] : -1.0f, v ? batPercent(batV[c]) : 0);
  }
  s += "],";
  {
    float lo, hi;
    ldrScale(lo, hi);
    const char* w = saverLightWord(ok ? &t : nullptr);
    jf(s, "\"ldr\":{\"mv\":%d,\"lo\":%d,\"hi\":%d,\"pct\":%d,\"word\":\"%s\",\"absent\":%s},",
       (int)ldrShown, (int)lo, (int)hi, (int)lroundf(ldrFrac(ldrShown) * 100), w, ldrAbsent ? "true" : "false");
  }
  s += "\"relays\":[";
  for (int i = 0; i < NUM_RELAYS; i++) {
    jf(s, "%s{\"n\":\"%s\",\"name\":\"%s\",\"on\":%s,\"mode\":%d,\"thr\":%d,\"thrMv\":%d,\"dark\":%s,\"pend\":%d,\"night\":%s}",
       i ? "," : "", RELAY_SHORT[i], RELAY_NAME[i], relayOn[i] ? "true" : "false", relayMode[i], thrOff[i],
       (int)permToMv(thrOff[i]), relayDark[i] ? "true" : "false", pending[i] ? pendLeft[i] : 0, nightLatch[i] ? "true" : "false");
  }
  s += "],";
  {
    bool on; uint8_t pct, fx;
    ledTarget(on, pct, fx);
    jf(s, "\"led\":{\"mode\":%d,\"brN\":%d,\"fxN\":%d,\"brD\":%d,\"fxD\":%d,\"on\":%s,\"pct\":%d,\"fx\":%d},",
       ledCfg.mode, ledCfg.brN, ledCfg.fxN, ledCfg.brD, ledCfg.fxD, on ? "true" : "false", pct, fx);
  }
  s += "\"fx\":[";
  for (int i = 0; i < FX_COUNT; i++) jf(s, "%s\"%s\"", i ? "," : "", FX_NAMES[i]);
  s += "],";
  jf(s, "\"scr\":[%d,%d,%d],", scrBr[0], scrBr[1], scrBr[2]);
  jf(s, "\"set\":{\"sleep\":%d,\"dly\":%d,\"cal\":%d,\"sun\":%d,\"nof\":%d,\"noh\":%d},",
     sleepSec, switchDelaySec, calAuto, sunGate, nightOff, nightHour);
  if (sunDataValid()) jf(s, "\"sun\":{\"rise\":\"%02d:%02d\",\"set\":\"%02d:%02d\"},", sunRiseMin / 60, sunRiseMin % 60, sunSetMin / 60, sunSetMin % 60);
  else                s += "\"sun\":{\"rise\":\"\",\"set\":\"\"},";
  bool wv = wxValid();
  jf(s, "\"wx\":{\"ok\":%s,\"t\":%.1f,\"feel\":%.1f,\"hum\":%d,\"wind\":%.1f,\"code\":%d,\"cache\":%s,\"busy\":%s},",
     wv ? "true" : "false", (float)wx.temp, (float)wx.feel, (int)wx.hum, (float)wx.wind, (int)wx.code,
     wxFromCache ? "true" : "false", wxBusy ? "true" : "false");
  bool c = WiFi.status() == WL_CONNECTED;
  {
    wl_status_t st = WiFi.status();
    const char* ns = c ? "connected" : (!netSsid[0] ? "none" :
                     (st == WL_CONNECT_FAILED ? "failed" : (st == WL_NO_SSID_AVAIL ? "nossid" : "connecting")));
    s += "\"place\":\"" WEATHER_PLACE "\",\"net\":{\"saved\":\"";
    s += jsEsc(netSsid);
    jf(s, "\",\"st\":\"%s\",\"apTemp\":%s},", ns, apTemp ? "true" : "false");
  }
  s += "\"wifi\":{\"ssid\":\"";
  s += c ? jsEsc(WiFi.SSID().c_str()) : String("");
  jf(s, "\",\"rssi\":%d,\"ip\":\"%s\",\"host\":\"%s\",\"ap\":%s,\"apSsid\":\"%s\",\"apPass\":\"%s\",\"apIp\":\"%s\",\"apClients\":%d},",
     c ? (int)WiFi.RSSI() : 0, c ? WiFi.localIP().toString().c_str() : "", OTA_HOST, apOn ? "true" : "false",
     AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str(), apActive() ? (int)WiFi.softAPgetStationNum() : 0);
  jf(s, "\"sys\":{\"up\":%lu,\"heap\":%u,\"ppm\":%.1f,\"ntp\":%s,\"pro\":\"%s\",\"proOk\":%s,\"ina\":%s,\"allOff\":%s,\"fw\":\"%s %s\"}",
     (unsigned long)(millis() / 1000), (unsigned)(ESP.getFreeHeap() / 1024), (float)driftPpm, ntpMono ? "true" : "false",
     proVer, (millis() - lastRxMs < 15000) ? "true" : "false", inaAddr ? "true" : "false", allOff ? "true" : "false",
     __DATE__, __TIME__);
  s += '}';
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json; charset=utf-8", s);
}

static void webFc() {
  fcSnap();
  String s;
  s.reserve(5200);
  if (!fcUOk) { web.send(200, "application/json", "{}"); return; }
  jf(s, "{\"start\":%lu,\"fetched\":%lu,", (unsigned long)fcU.start, (unsigned long)fcU.fetched);
  s += "\"rise\":[";
  for (int d = 0; d < FC_DAYS; d++) jf(s, "%s%lu", d ? "," : "", (unsigned long)fcU.rise[d]);
  s += "],\"set\":[";
  for (int d = 0; d < FC_DAYS; d++) jf(s, "%s%lu", d ? "," : "", (unsigned long)fcU.set[d]);
  s += "]";
  auto arr = [&](const char* name, int kind) {
    s += ",\"";
    s += name;
    s += "\":[";
    for (int i = 0; i < FC_HOURS; i++) {
      if (i) s += ',';
      int v;
      bool nul;
      switch (kind) {
        case 0:  v = fcU.temp[i]; nul = v == -128; break;
        case 1:  v = fcU.code[i]; nul = v == 255;  break;
        case 2:  v = fcU.pop[i];  nul = v == 255;  break;
        case 3:  v = fcU.hum[i];  nul = v == 255;  break;
        default: v = fcU.wind[i]; nul = v == 255;  break;
      }
      if (nul) s += "null"; else s += v;
    }
    s += ']';
  };
  arr("temp", 0); arr("code", 1); arr("pop", 2); arr("hum", 3); arr("wind", 4);
  s += '}';
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json; charset=utf-8", s);
}

static void webSet() {
  String k = web.arg("k");
  int v = web.arg("v").toInt();
  if (k.length() == 2 && k[0] == 'm' && k[1] >= '0' && k[1] < '0' + NUM_RELAYS) {
    relayMode[k[1] - '0'] = (uint8_t)constrain(v, 0, MODE_COUNT - 1);
  } else if (k.length() == 2 && k[0] == 't' && k[1] >= '0' && k[1] < '0' + NUM_RELAYS) {
    thrOff[k[1] - '0'] = constrain(v, 10 + THR_HYST_PM, 990);
  } else if (k == "sleep") sleepSec = constrain(v, 10, 600);
  else if (k == "dly")   switchDelaySec = constrain(v, 1, 120);
  else if (k == "cal")   calAuto = v != 0;
  else if (k == "sun")   sunGate = v != 0;
  else if (k == "nof")   nightOff = v != 0;
  else if (k == "noh")   nightHour = constrain(v, 0, 23);
  else if (k == "lmode") ledCfg.mode = constrain(v, 0, MODE_COUNT - 1);
  else if (k == "lbrn")  ledCfg.brN = constrain(v, 0, 100);
  else if (k == "lfxn")  ledCfg.fxN = constrain(v, 0, FX_COUNT - 1);
  else if (k == "lbrd")  ledCfg.brD = constrain(v, 0, 100);
  else if (k == "lfxd")  ledCfg.fxD = constrain(v, 0, FX_COUNT - 1);
  else if (k == "scr0")  { scrBr[0] = constrain(v, 5, 100); applyContrast(); }
  else if (k == "scr1")  { scrBr[1] = constrain(v, 0, 100); applyContrast(); }
  else if (k == "scr2")  { scrBr[2] = constrain(v, 0, 100); applyContrast(); }
  else if (k == "ap")    { setAp(v != 0); }
  else { web.send(400, "application/json", "{\"err\":\"key\"}"); return; }
  saveSettings();
  dirty = true;
  webState();
}

static void webAct() {
  String a = web.arg("a");
  if      (a == "wx")       wxForce = true;
  else if (a == "calreset") { ldrLo = ldrHi = ldrMv; saveCalib(); }
  else if (a == "alloff" || a == "sleep") setAllOff(true);
  else if (a == "allon")    setAllOff(false);
  else if (a == "restart")  webRestartAt = millis() + 600;
  else if (a == "deep")     webDeepAt = millis() + 600;     // спершу відповідь, потім вимикаємо Wi-Fi
  else { web.send(400, "application/json", "{\"err\":\"act\"}"); return; }
  dirty = true;
  web.send(200, "application/json", "{\"ok\":true}");
}

// Прибрати зайві пробіли на краях (і нерозривні, і "невидимі" з клавіатури телефона)
static String trimAll(String v) {
  v.trim();
  for (bool ch = true; ch; ) {
    ch = false;
    if (v.startsWith("\xC2\xA0"))     { v.remove(0, 2); ch = true; }
    if (v.endsWith("\xC2\xA0"))       { v.remove(v.length() - 2); ch = true; }
    if (v.startsWith("\xE2\x80\x8B")) { v.remove(0, 3); ch = true; }
    if (v.endsWith("\xE2\x80\x8B"))   { v.remove(v.length() - 3); ch = true; }
    if (v.startsWith("\xEF\xBB\xBF")) { v.remove(0, 3); ch = true; }
    if (v.endsWith("\xEF\xBB\xBF"))   { v.remove(v.length() - 3); ch = true; }
    if (ch) v.trim();
  }
  return v;
}

// Пошук мереж: ?start=1 запускає (асинхронно), далі опитуємо, поки не буде списку
static void webScan() {
  if (web.hasArg("start")) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false);
    web.send(200, "application/json", "{\"busy\":true}");
    return;
  }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { web.send(200, "application/json", "{\"busy\":true}"); return; }
  if (n < 0) { web.send(200, "application/json", "{\"busy\":false,\"list\":[]}"); return; }
  String s = "{\"busy\":false,\"list\":[";
  String cur = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
  bool first = true;
  int used[40];
  int nu = 0;
  for (int k = 0; k < n && nu < 40; k++) {                    // найсильніша з однакових назв, за спаданням сигналу
    int best = -1;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i).length() == 0) continue;
      bool taken = false;
      for (int u = 0; u < nu && !taken; u++) taken = WiFi.SSID(used[u]) == WiFi.SSID(i);
      if (taken) continue;
      if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best < 0) break;
    used[nu++] = best;
    String id = WiFi.SSID(best);
    s += first ? "" : ",";
    first = false;
    s += "{\"ssid\":\"";
    s += jsEsc(id.c_str());
    jf(s, "\",\"rssi\":%d,\"ch\":%d,\"open\":%s,\"cur\":%s}", (int)WiFi.RSSI(best), (int)WiFi.channel(best),
       WiFi.encryptionType(best) == WIFI_AUTH_OPEN ? "true" : "false", id == cur ? "true" : "false");
  }
  s += "]}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json; charset=utf-8", s);
}

// Вибір мережі: POST ssid, pass (або forget=1 — повернути вбудовану з прошивки)
static void webWifi() {
  auto err = [](const char* m) {
    String o = "{\"ok\":false,\"err\":\"";
    o += m;
    o += "\"}";
    web.send(200, "application/json; charset=utf-8", o);
  };
  if (web.hasArg("forget")) {
    prefs.begin("ctl", false);
    prefs.remove("wssid");
    prefs.remove("wpass");
    prefs.end();
    snprintf(netSsid, sizeof(netSsid), "%s", WIFI_SSID);
    snprintf(netPass, sizeof(netPass), "%s", WIFI_PASS);
  } else {
    String ss = trimAll(web.arg("ssid"));
    String pw = trimAll(web.arg("pass"));
    if (ss.length() == 0)  return err("Вкажи назву мережі");
    if (ss.length() > 32)  return err("Назва задовга (макс. 32 байти)");
    if (pw.length() > 0 && pw.length() < 8) return err("Пароль закороткий: мінімум 8 символів");
    if (pw.length() > 63)  return err("Пароль задовгий (макс. 63 символи)");
    snprintf(netSsid, sizeof(netSsid), "%s", ss.c_str());
    snprintf(netPass, sizeof(netPass), "%s", pw.c_str());
    prefs.begin("ctl", false);
    prefs.putString("wssid", netSsid);
    prefs.putString("wpass", netPass);
    prefs.end();
  }
  netApplyAt = millis() + 800;                          // спершу відповідь, потім перепідключення
  String o = "{\"ok\":true,\"ssid\":\"";
  o += jsEsc(netSsid);
  o += "\"}";
  web.send(200, "application/json; charset=utf-8", o);
}

static void initWeb() {
  web.on("/", HTTP_GET, []() {
    web.sendHeader("Cache-Control", "no-cache");
    web.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
  });
  web.on("/api/state", HTTP_GET, webState);
  web.on("/api/fc", HTTP_GET, webFc);
  web.on("/api/set", HTTP_GET, webSet);
  web.on("/api/act", HTTP_GET, webAct);
  web.on("/api/scan", HTTP_GET, webScan);
  web.on("/api/wifi", HTTP_POST, webWifi);
  web.onNotFound([]() { web.sendHeader("Location", "/"); web.send(302, "text/plain", ""); });
  web.begin();
}

static void webTick() {
  web.handleClient();
  if (webRestartAt && (int32_t)(millis() - webRestartAt) >= 0) { saveSettings(); delay(50); ESP.restart(); }
  if (webDeepAt && (int32_t)(millis() - webDeepAt) >= 0) { webDeepAt = 0; enterDeep(); }
}

// =====================================================================
//  10. OTA
// =====================================================================

static void initOta() {
  ArduinoOTA.setHostname(OTA_HOST);
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
    otaBusy = true;
#if WDT_ENABLE
    esp_task_wdt_reset();
#endif
  });

  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
#if WDT_ENABLE
    esp_task_wdt_reset();       // під час OTA loop() не крутиться — годуємо сторожа тут
#endif
    if (!oledOk) return;
    static int last = -1;
    int pct = (int)((uint64_t)p * 100 / t);
    if (pct == last) return;
    last = pct;
    char b[8];
    snprintf(b, sizeof(b), "%d%%", pct);
    oled.clearBuffer();
    txtC(FONT_M, 64, 24, "OTA оновлення");
    oled.drawFrame(4, 34, 120, 12);
    oled.drawBox(6, 36, pct * 116 / 100, 8);
    txtC(FONT_M, 64, 60, b);
    oled.sendBuffer();
  });

  ArduinoOTA.onError([](ota_error_t) { otaBusy = false; dirty = true; });

  ArduinoOTA.begin();
}

// =====================================================================
//  10b. OLED: старт і самовідновлення
// =====================================================================

static bool oledInit() {
  Wire.beginTransmission(0x3C);
  if (Wire.endTransmission() != 0) return false;
  oled.begin();
  oled.setBusClock(400000);
  oled.setFontMode(1);          // прозорий фон тексту (потрібно для XOR-малювання)
  Wire.setTimeOut(20);
  oledOk = true;
  applyContrast();
  dirty = true;
  return true;
}

// Раз на 3 с перевіряємо, чи екран на шині: зник — не мучимо шину, з'явився — ініціалізуємо
static void oledWatch(uint32_t now) {
  static uint32_t t = 0;
  static uint8_t  miss = 0;
  if (now - t < 3000) return;
  t = now;
  Wire.beginTransmission(0x3C);
  bool present = (Wire.endTransmission() == 0);
  if (oledOk) {
    if (present) miss = 0;
    else if (++miss >= 2) { oledOk = false; miss = 0; }
  } else if (present) {
    oledInit();
    if (deepOff) oled.setPowerSave(1);
  }
}

// =====================================================================
//  11. SETUP / LOOP
// =====================================================================

void setup() {
  Serial.begin(115200);

  // Реле: спочатку безпечний рівень, потім режим виходу
  for (int i = 0; i < NUM_RELAYS; i++) {
    digitalWrite(RELAY_PIN[i], RELAY_OFF_LEVEL);
#if RELAY_OPEN_DRAIN
    pinMode(RELAY_PIN[i], OUTPUT_OPEN_DRAIN);
#else
    pinMode(RELAY_PIN[i], OUTPUT);
#endif
    digitalWrite(RELAY_PIN[i], RELAY_OFF_LEVEL);   // гарантовано відпустити пін після pinMode
  }

  loadPrefs();

  analogSetPinAttenuation(PIN_LDR, ADC_11db);
  analogSetPinAttenuation(PIN_POWER_DETECT, ADC_11db);

  Serial2.setRxBufferSize(512);
  Serial2.begin(LINK_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  proInit();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);
  Wire.setTimeOut(20);          // збій на шині не повинен підвішувати програму

  if (oledInit()) {
    oled.clearBuffer();
    txtC(FONT_M, 64, 36, "Запуск...");
    oled.sendBuffer();
  }

  inaDetect();

  apTemp = !netSsid[0];                      // мережа не задана — одразу точка доступу для налаштування
  WiFi.mode(apActive() ? WIFI_AP_STA : WIFI_STA);
  if (apActive()) WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setHostname(OTA_HOST);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);         // без енергозбереження Wi-Fi: стабільніші OTA і відгук
  netBegin();
  initWeb();                // веб-меню: працює і через роутер, і через власну точку доступу

  updateSensors();          // щоб реле одразу знало темно чи ні
  applyRelays();

  // Ядро 0 (де працює Wi-Fi): важке TLS-з'єднання погоди більше не гальмує loop() на ядрі 1
  fcLoad();                 // прогноз із флешу: погода є одразу, навіть без інтернету
  xTaskCreatePinnedToCore(weatherTask, "weather", 16384, nullptr, 1, nullptr, 0);

  lastActivity = millis();

#if WDT_ENABLE
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtCfg = { (uint32_t)WDT_TIMEOUT_S * 1000U, 0, true };
  esp_task_wdt_reconfigure(&wdtCfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);       // стежимо за loop()
#endif
}

void loop() {
#if WDT_ENABLE
  esp_task_wdt_reset();
#endif

  // Мережа: OTA і NTP стартують, коли з'явився Wi-Fi
  static bool netInit = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!netInit) {
      Serial.printf("[WiFi] підключено, IP %s\n", WiFi.localIP().toString().c_str());
      initOta();
      MDNS.addService("http", "tcp", 80);
      sntp_set_time_sync_notification_cb(onTimeSync);
      configTzTime(TZ_INFO, NTP_SERVER, "time.google.com", "time.cloudflare.com");
      netInit = true;
    }
    ArduinoOTA.handle();
  }

  // Повторне підключення до Wi-Fi
  // Wi-Fi: поки нема зв'язку — пробуємо знову кожні WIFI_RETRY_MS (роутер після відключення світла
  // піднімається довше за ESP32, одного повтору замало)
  static uint32_t tWifi = 0;
  if (deepOff) {
    tWifi = millis();
  } else if (WiFi.status() == WL_CONNECTED) {
    tWifi = millis();
  } else if (millis() - tWifi >= WIFI_RETRY_MS) {
    tWifi = millis();
    // поки хтось налаштовує через точку доступу — не перепідключаємось (пошук мережі збиває канал точки)
    if (netSsid[0] && !(apTemp && WiFi.softAPgetStationNum() > 0)) {
      WiFi.disconnect(false);
      netBegin();
    }
  }
  netWatch();

  readLink();
  if (!deepOff) webTick();
  menuTick();
  if (deepOff && okHeld && millis() - okDownT >= DEEP_WAKE_HOLD_MS) exitDeep();

  // ВАЖЛИВО: час беремо ПІСЛЯ readLink(). Раніше "now" був застарілим, а lastActivity
  // оновлювався вже після нього — різниця ставала від'ємною (≈4 млрд) і екран
  // засинав одразу після натискання кнопки.
  uint32_t now = millis();

  if (!otaBusy) oledWatch(now);

#if DEBUG_MODE
  static uint32_t tPing = 0;                  // перевірка зв'язку в обидва боки
  if (now - tPing >= 2000) { tPing = now; Serial2.println("PING"); Serial2.println("STATUS"); pingSent++; }
#endif

  static uint32_t tClock = 0;                 // годинник без інтернету: підправка на похибку кварцу
  if (now - tClock >= 60000) { tClock = now; clockKeep(); }

  static uint32_t tInit = 0;                  // раз на 10 с нагадуємо Pro Micro: події увімкнені
  if (now - tInit >= 10000) { tInit = now; proInit(); }

  // Датчики 10 разів на секунду
  static uint32_t tSens = 0;
  if (now - tSens >= 100) {
    tSens = now;
    updateSensors();
  }

  // Калібрування LDR раз на секунду, збереження раз на 30 хв (якщо змінилось)
  static uint32_t tCal = 0, tSave = 0;
  if (now - tCal >= 1000) {
    tCal = now;
    // без датчика не калібруємо — зіб'ється шкала. "Фікс." теж набирає діапазон, поки він порожній (після скидання)
    if (ldrReady && !ldrAbsent && (calAuto || ldrHi - ldrLo < LDR_MIN_RANGE_MV)) calibrateLdr();
  }
  if (now - tSave >= 30UL * 60UL * 1000UL) {
    tSave = now;
    if (fabsf(ldrLo - savedLo) > 25 || fabsf(ldrHi - savedHi) > 25) saveCalib();
  }

  applyRelays();
  ledSync();

  // Автоматичний сон екрана (знакове порівняння — безпечне при будь-якому порядку міток часу).
  // Під час правки сон не раніше ніж через 60 с.
  bool busyUi = editing || (page == P_MENU && menuLevel == 2);
  uint32_t limitSec = busyUi ? max<uint32_t>(sleepSec, 60) : sleepSec;
  if (!deepOff && !asleep && !otaBusy && !pwAct && (int32_t)(now - lastActivity) > (int32_t)(limitSec * 1000UL)) sleepNow();

  // Малювання: на межі FPS під час анімації, 50 мс на екрані сну, 60 мс на живих екранах
  static uint32_t tDraw = 0;
  if (oledOk && !otaBusy && !deepOff) {
    // швидко (16 мс) — лише поки щось рухається; у спокої рідше, щоб не завантажувати I2C і loop()
    bool anim = !asleep && (transT < 1.0f || menuAnimating() || animMoving || toastMsg[0]);
    uint32_t period = asleep ? SAVER_FRAME_MS : (anim ? 16 : (page == P_MAIN ? 50 : 100));
    if (dirty || now - tDraw >= period) {
      frameDt = constrain((now - tDraw) / 1000.0f, 0.001f, 0.1f);
      dirty = false;
      tDraw = now;
      stepAnim();
      animMoving = false;
      oled.clearBuffer();
      if (asleep) drawSaver(); else drawUi();
      oled.sendBuffer();
    }
  }

  delay(1);
}