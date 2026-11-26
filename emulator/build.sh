#!/bin/bash
# Збірка емулятора: справжня прошивка src/main.cpp + бібліотека U8g2 для ПК -> build/libemu.so
set -e
cd "$(dirname "$0")"
CLIB=$(ls -d ../.pio/libdeps/*/U8g2/src/clib 2>/dev/null | head -1)
if [ -z "$CLIB" ]; then
  echo "Не знайдено U8g2. Спершу зберіть прошивку один раз: pio run (бібліотека скачається в .pio/libdeps)"; exit 1
fi
mkdir -p build/u8g2
if [ ! -f build/libu8g2.a ]; then
  echo "Збираю U8g2..."
  for f in "$CLIB"/*.c; do gcc -O2 -fPIC -c -I"$CLIB" "$f" -o build/u8g2/$(basename "$f" .c).o; done
  ar rcs build/libu8g2.a build/u8g2/*.o
fi
INC=../include
echo "Збираю прошивку для ПК..."
g++ -std=gnu++17 -O2 -fPIC -shared -w -fpermissive -Ishim -I"$INC" -I"$CLIB" emu.cpp build/libu8g2.a -lpthread -o build/libemu.so
echo "Готово: build/libemu.so"
