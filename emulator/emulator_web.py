#!/usr/bin/env python3
"""HTML-емулятор контролера: справжня прошивка (build/libemu.so) + сторінка в браузері.

    python3 emulator/emulator_web.py          -> відкриє http://127.0.0.1:8765
Порт можна змінити: python3 emulator/emulator_web.py 9000
"""
import ctypes
import json
import os
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from emulator import build_if_needed, LIB   # та сама збірка, що й для віконного емулятора

build_if_needed()
lib = ctypes.CDLL(LIB)
lib.emu_set.argtypes = [ctypes.c_char_p, ctypes.c_double]
lib.emu_key.argtypes = [ctypes.c_char_p, ctypes.c_int]
lib.emu_start.argtypes = [ctypes.c_char_p]
lib.emu_state.argtypes = [ctypes.c_char_p, ctypes.c_int]
lib.emu_led.argtypes = [ctypes.POINTER(ctypes.c_int)]
lib.emu_start(os.path.join(HERE, "emu_prefs.txt").encode())
LOCK = threading.Lock()
PAGE = os.path.join(HERE, "web", "index.html")


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def out(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path in ("/", "/index.html"):
            with open(PAGE, "rb") as f:
                return self.out(200, f.read(), "text/html; charset=utf-8")
        if u.path == "/frame":                      # 1024 байти кадру + контраст + "екран вимкнено"
            buf = (ctypes.c_uint8 * 1024)()
            with LOCK:
                lib.emu_frame(buf)
                tail = bytes([lib.emu_contrast() & 255, 1 if lib.emu_power_save() else 0])
            return self.out(200, bytes(buf) + tail, "application/octet-stream")
        if u.path == "/state":
            s = ctypes.create_string_buffer(16384)
            rgb = (ctypes.c_int * 3)()
            with LOCK:
                lib.emu_state(s, 16384)
                on = lib.emu_led(rgb)
            try:
                d = json.loads(s.value.decode("utf-8") or "{}")
            except ValueError:
                d = {}
            d["led_rgb"] = [rgb[0], rgb[1], rgb[2], on]
            return self.out(200, json.dumps(d, ensure_ascii=False).encode(), "application/json; charset=utf-8")
        if u.path == "/key":                        # k=left|right|ok|back|conf, d=1 натиснуто / 0 відпущено
            with LOCK:
                lib.emu_key(q.get("k", "").encode(), int(q.get("d", "1")))
            return self.out(200, b"{}", "application/json")
        if u.path == "/set":                        # n=ldr|grid|wifi|bat1|bat2|line, v=число
            with LOCK:
                lib.emu_set(q.get("n", "").encode(), float(q.get("v", "0")))
            return self.out(200, b"{}", "application/json")
        self.out(404, b"404", "text/plain")


if __name__ == "__main__":
    nums = [a for a in sys.argv[1:] if a.isdigit()]
    port = int(nums[0]) if nums else 8765
    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    url = f"http://127.0.0.1:{port}"
    print(f"Емулятор: {url}  (Ctrl+C — зупинити)")
    if "--no-browser" not in sys.argv:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
