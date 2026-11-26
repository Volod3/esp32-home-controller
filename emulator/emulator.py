#!/usr/bin/env python3
"""Емулятор контролера: справжня прошивка (src/main.cpp), зібрана для ПК, у вікні.

Керування мишею над екраном:
  коліщатко  — крутилка (енкодер)
  ліва       — OK      (утримання 2 с — вихід із глибокого сну)
  права      — BACK
  середня    — CONF    (усе вимкнути / увімкнути)
Клавіатура: ← → (або ↑ ↓) — крутилка, Enter — OK, Esc / Backspace — BACK, пробіл — CONF.
Праворуч — "датчики" (фоторезистор, світло в домі, акумулятори, Wi-Fi) і стан реле та стрічки.
"""
import ctypes
import json
import os
import subprocess
import sys
import tkinter as tk
from tkinter import ttk

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "build", "libemu.so")
SRC = [os.path.join(HERE, "..", "src", "main.cpp"), os.path.join(HERE, "..", "src", "web_page.h"),
       os.path.join(HERE, "emu.cpp"), os.path.join(HERE, "shim", "emu_hal.h")]

ZOOM = 4
try:
    from PIL import Image          # згладжене масштабування (без нього — звичайне збільшення)
except ImportError:
    Image = None
BG, PANEL, FG, MUTED, ACC = "#0b0f17", "#131a26", "#e8eef8", "#8a97ad", "#5b9cff"


def build_if_needed():
    stale = not os.path.exists(LIB) or any(os.path.getmtime(s) > os.path.getmtime(LIB) for s in SRC if os.path.exists(s))
    if stale:
        print("Збираю емулятор (прошивка змінилась)...")
        r = subprocess.run(["bash", os.path.join(HERE, "build.sh")])
        if r.returncode:
            sys.exit("Збірка не вдалась")


class Emulator:
    def __init__(self, root):
        self.root = root
        self.lib = ctypes.CDLL(LIB)
        self.lib.emu_set.argtypes = [ctypes.c_char_p, ctypes.c_double]
        self.lib.emu_key.argtypes = [ctypes.c_char_p, ctypes.c_int]
        self.lib.emu_start.argtypes = [ctypes.c_char_p]
        self.lib.emu_state.argtypes = [ctypes.c_char_p, ctypes.c_int]
        self.lib.emu_led.argtypes = [ctypes.POINTER(ctypes.c_int)]
        self.lib.emu_start(os.path.join(HERE, "emu_prefs.txt").encode())

        self.buf = (ctypes.c_uint8 * 1024)()
        self.last = None
        self.lastLook = None
        self.held = set()
        # таблиці: байт стовпця -> 0/1 для кожного з 8 рядків смуги
        self.bit = [bytes((v >> k) & 1 for v in range(256)) for k in range(8)]

        root.title("Емулятор контролера ESP32")
        root.configure(bg=BG)
        root.minsize(900, 420)
        style = ttk.Style()
        style.theme_use("clam")
        style.configure(".", background=PANEL, foreground=FG, fieldbackground=PANEL)
        style.configure("TLabel", background=PANEL, foreground=FG, font=("Sans", 10))
        style.configure("Mut.TLabel", foreground=MUTED, font=("Sans", 9))
        style.configure("H.TLabel", foreground=FG, font=("Sans", 11, "bold"))
        style.configure("TCheckbutton", background=PANEL, foreground=FG)
        style.map("TCheckbutton", background=[("active", PANEL)])
        style.configure("Horizontal.TScale", background=PANEL, troughcolor="#243044")
        style.configure("TButton", background="#1d2636", foreground=FG, borderwidth=0, padding=8, font=("Sans", 10, "bold"))
        style.map("TButton", background=[("active", "#2a3650"), ("pressed", ACC)])

        left = tk.Frame(root, bg=BG)
        left.pack(side="left", fill="both", expand=True, padx=18, pady=18)
        tk.Label(left, text="OLED 128×64 — справжня прошивка", bg=BG, fg=MUTED, font=("Sans", 10)).pack(anchor="w")
        bezel = tk.Frame(left, bg="#05070b", padx=14, pady=14, highlightthickness=1, highlightbackground="#243044")
        bezel.pack(pady=8)
        W, H = 128 * ZOOM, 64 * ZOOM
        self.cv = tk.Canvas(bezel, width=W, height=H, bg="#000", highlightthickness=0, cursor="hand2")
        self.cv.pack()
        self.small = tk.PhotoImage(width=128, height=64)
        self.big = tk.PhotoImage(width=W, height=H)
        self.cv.create_image(0, 0, image=self.big, anchor="nw")
        tk.Label(left, text="Коліщатко — крутилка · ЛКМ — OK (утримати 2 с — розбудити з глиб. сну) · ПКМ — BACK · середня — CONF\n"
                            "Клавіші: ← → ↑ ↓ — крутилка, Enter — OK, Esc — BACK, пробіл — CONF",
                 bg=BG, fg=MUTED, font=("Sans", 9), justify="left").pack(anchor="w")

        pad = tk.Frame(left, bg=BG)
        pad.pack(pady=10)
        self.mkbtn(pad, "⟲  Ліво", "left").grid(row=0, column=0, padx=4)
        self.mkbtn(pad, "OK", "ok").grid(row=0, column=1, padx=4)
        self.mkbtn(pad, "Право  ⟳", "right").grid(row=0, column=2, padx=4)
        self.mkbtn(pad, "BACK", "back").grid(row=0, column=3, padx=(18, 4))
        self.mkbtn(pad, "CONF", "conf").grid(row=0, column=4, padx=4)

        right = tk.Frame(root, bg=PANEL, padx=16, pady=14)
        right.pack(side="right", fill="y", padx=(0, 18), pady=18)
        ttk.Label(right, text="Датчики", style="H.TLabel").pack(anchor="w", pady=(0, 6))
        self.vars = {}
        self.slider(right, "ldr", "Фоторезистор", 0, 3300, 1800, "{:.0f} мВ")
        self.check(right, "grid", "Світло в домі (є живлення)", True)
        self.slider(right, "bat1", "РЕЗ.АКУМ", 2.8, 4.3, 4.10, "{:.2f} В")
        self.slider(right, "bat2", "ОСН.АКУМ", 2.8, 4.3, 3.95, "{:.2f} В")
        self.slider(right, "line", "Лінія 5V", 0, 5.5, 5.05, "{:.2f} В")
        self.check(right, "wifi", "Wi-Fi підключено", True)

        ttk.Label(right, text="Стан", style="H.TLabel").pack(anchor="w", pady=(14, 6))
        st = tk.Frame(right, bg=PANEL)
        st.pack(fill="x")
        self.relay = []
        for i, n in enumerate(["RGB", "LED", "GRL"]):
            c = tk.Canvas(st, width=16, height=16, bg=PANEL, highlightthickness=0)
            c.grid(row=i, column=0, pady=3)
            dot = c.create_oval(2, 2, 14, 14, fill="#333", outline="")
            lab = ttk.Label(st, text=n)
            lab.grid(row=i, column=1, sticky="w", padx=8)
            self.relay.append((c, dot, lab))
        lf = tk.Frame(right, bg=PANEL)
        lf.pack(fill="x", pady=(8, 0))
        ttk.Label(lf, text="LED-стрічка").pack(side="left")
        self.ledc = tk.Canvas(lf, width=90, height=18, bg=PANEL, highlightthickness=0)
        self.ledc.pack(side="right")
        self.ledbar = self.ledc.create_rectangle(0, 2, 90, 16, fill="#222", outline="#243044")
        self.info = ttk.Label(right, text="", style="Mut.TLabel", justify="left")
        self.info.pack(anchor="w", pady=(12, 0))

        # миша й клавіатура
        for w in (self.cv,):
            w.bind("<ButtonPress-1>", lambda e: self.press("ok"))
            w.bind("<ButtonRelease-1>", lambda e: self.release("ok"))
            w.bind("<ButtonPress-3>", lambda e: self.press("back"))
            w.bind("<ButtonRelease-3>", lambda e: self.release("back"))
            w.bind("<ButtonPress-2>", lambda e: self.press("conf"))
            w.bind("<ButtonRelease-2>", lambda e: self.release("conf"))
            w.bind("<Button-4>", lambda e: self.step("left"))       # Linux X11
            w.bind("<Button-5>", lambda e: self.step("right"))
            w.bind("<MouseWheel>", lambda e: self.step("left" if e.delta > 0 else "right"))   # Windows / macOS
        root.bind("<Left>", lambda e: self.step("left"))
        root.bind("<Up>", lambda e: self.step("left"))
        root.bind("<Right>", lambda e: self.step("right"))
        root.bind("<Down>", lambda e: self.step("right"))
        for key, name in (("<Return>", "ok"), ("<Escape>", "back"), ("<BackSpace>", "back"), ("<space>", "conf")):
            root.bind("<KeyPress-%s>" % key[1:-1], lambda e, n=name: self.press(n))
            root.bind("<KeyRelease-%s>" % key[1:-1], lambda e, n=name: self.release(n, delayed=True))

        self.tick()
        self.panel()

    # ---------- елементи панелі ----------
    def mkbtn(self, parent, text, key):
        b = ttk.Button(parent, text=text, width=9)
        if key in ("left", "right"):
            b.configure(command=lambda: self.step(key))
        else:
            b.bind("<ButtonPress-1>", lambda e: self.press(key))
            b.bind("<ButtonRelease-1>", lambda e: self.release(key))
        return b

    def slider(self, parent, name, label, lo, hi, val, fmt):
        f = tk.Frame(parent, bg=PANEL)
        f.pack(fill="x", pady=4)
        top = tk.Frame(f, bg=PANEL)
        top.pack(fill="x")
        ttk.Label(top, text=label).pack(side="left")
        vl = ttk.Label(top, text=fmt.format(val), style="Mut.TLabel")
        vl.pack(side="right")
        v = tk.DoubleVar(value=val)

        def changed(*_):
            vl.configure(text=fmt.format(v.get()))
            self.lib.emu_set(name.encode(), v.get())
        ttk.Scale(f, from_=lo, to=hi, variable=v, command=changed, length=260).pack(fill="x")
        self.vars[name] = v
        self.lib.emu_set(name.encode(), val)

    def check(self, parent, name, label, val):
        v = tk.IntVar(value=1 if val else 0)
        ttk.Checkbutton(parent, text=label, variable=v,
                        command=lambda: self.lib.emu_set(name.encode(), v.get())).pack(anchor="w", pady=4)
        self.lib.emu_set(name.encode(), 1 if val else 0)

    # ---------- введення ----------
    def step(self, d):
        self.lib.emu_key(d.encode(), 1)

    def press(self, k):
        if k in self.held:                       # автоповтор клавіші — це не нове натискання
            if getattr(self, "_rel_" + k, None):
                self.root.after_cancel(getattr(self, "_rel_" + k))
                setattr(self, "_rel_" + k, None)
            return
        self.held.add(k)
        self.lib.emu_key(k.encode(), 1)

    def release(self, k, delayed=False):
        if delayed:                              # X11 шле release+press при автоповторі — чекаємо, чи не натиснуто знову
            setattr(self, "_rel_" + k, self.root.after(40, lambda: self.release(k)))
            return
        setattr(self, "_rel_" + k, None)
        if k in self.held:
            self.held.discard(k)
            self.lib.emu_key(k.encode(), 0)

    # ---------- кадр екрана ----------
    def tick(self):
        self.lib.emu_frame(self.buf)
        b = bytes(self.buf)
        look = (self.lib.emu_contrast(), self.lib.emu_power_save())
        if b != self.last or look != self.lastLook:
            self.last, self.lastLook = b, look
            on = 0 if look[1] else 1
            k = 0.35 + 0.65 * look[0] / 255.0
            rows = []
            for y in range(64):
                p = (y >> 3) * 128
                rows.append(b[p:p + 128].translate(self.bit[y & 7]))
            bits = b"".join(rows)
            if not on:
                bits = bytes(8192)
            px = bytearray(8192 * 3)
            px[0::3] = bits.translate(bytes([6, int(175 * k)] + [0] * 254))
            px[1::3] = bits.translate(bytes([8, int(220 * k)] + [0] * 254))
            px[2::3] = bits.translate(bytes([12, int(255 * k)] + [0] * 254))
            if Image:                                   # згладжені пікселі
                W, H = 128 * ZOOM, 64 * ZOOM
                im = Image.frombytes("RGB", (128, 64), bytes(px)).resize((W, H), Image.NEAREST)   # чіткі пікселі
                self.big.configure(data=b"P6 %d %d 255\n" % (W, H) + im.tobytes(), format="PPM")
            else:
                self.small.configure(data=b"P6 128 64 255\n" + bytes(px), format="PPM")
                self.big.tk.call(self.big.name, "copy", self.small.name, "-zoom", ZOOM, ZOOM)
        self.root.after(12, self.tick)

    def panel(self):
        s = ctypes.create_string_buffer(16384)
        self.lib.emu_state(s, 16384)
        try:
            d = json.loads(s.value.decode("utf-8"))
            modes = ["Авто", "Вимк", "Увімк"]
            for (c, dot, lab), r in zip(self.relay, d["relays"]):
                c.itemconfigure(dot, fill="#ffc94d" if r["on"] else "#333")
                extra = f" · чекає {r['pend']} с" if r["pend"] else ""
                lab.configure(text=f"{r['n']} — {'увімк' if r['on'] else 'вимк'} · {modes[r['mode']]} · поріг {r['thrMv']} мВ{extra}")
            disp = d.get("disp", {})
            self.info.configure(text=(
                f"Час {d.get('time') or '--'}  ·  {d.get('daypart') or ''}\n"
                f"Освітленість: {d['ldr']['word']} ({d['ldr']['pct']}%)\n"
                f"Екран: {round(disp.get('fps', 0))} к/с{' · заставка' if disp.get('asleep') else ''}"
                f"{' · УСЕ ВИМКНЕНО' if d['sys']['allOff'] else ''}\n"
                f"Погода: тестова (в емуляторі нема інтернету)\n"
                f"Налаштування зберігаються в emulator/emu_prefs.txt"))
        except Exception:
            pass
        rgb = (ctypes.c_int * 3)()
        on = self.lib.emu_led(rgb)
        m = max(rgb[0], rgb[1], rgb[2], 1)        # колір без урахування яскравості — щоб було видно відтінок
        self.ledc.itemconfigure(self.ledbar, fill="#%02x%02x%02x" % tuple(int(v * 255 / m) for v in rgb) if on else "#222")
        self.root.after(200, self.panel)


if __name__ == "__main__":
    build_if_needed()
    root = tk.Tk()
    Emulator(root)
    root.mainloop()
