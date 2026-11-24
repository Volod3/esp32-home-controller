// Скопіюй цей файл як include/secrets.h і впиши свої дані.
// secrets.h не потрапляє в git (див. .gitignore).
#pragma once

// Wi-Fi "з коробки". Можна лишити порожнім: тоді ESP32 підніме власну точку доступу,
// а мережу вибереш у веб-меню (http://192.168.4.1 -> Налаштування -> Мережа Wi-Fi).
#define WIFI_SSID      ""
#define WIFI_PASS      ""

// Пароль для прошивки по повітрю (має збігатися з upload_flags у secrets.ini)
#define OTA_PASS       "change-me"

// Власна точка доступу ESP32 (мінімум 8 символів)
#define AP_SSID        "ESP32-Control"
#define AP_PASS        "12345678"

// Координати для погоди Open-Meteo і підпис у веб-меню
#define WEATHER_LAT    49.8397f
#define WEATHER_LON    24.0297f
#define WEATHER_PLACE  "Львів"
