// Веб-меню контролера: одна сторінка, дані з /api/state і /api/fc, зміни через /api/set і /api/act
#pragma once
#include <pgmspace.h>

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="uk"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b0f17">
<title>Контролер</title>
<style>
:root{--bg:#0b0f17;--card:#131a26;--card2:#18202f;--line:#243044;--tx:#e8eef8;--mut:#8a97ad;--acc:#5b9cff;--acc2:#8b5bff;--ok:#36d399;--warn:#fbbd23;--bad:#f87272;--r:18px}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
body{padding:0 14px calc(90px + env(safe-area-inset-bottom));background:radial-gradient(1200px 600px at 80% -10%,#1b2b55 0,transparent 60%),radial-gradient(900px 500px at -10% 10%,#2a1b4d 0,transparent 55%),var(--bg);background-attachment:fixed;min-height:100vh}
.wrap{max-width:980px;margin:0 auto}
header{display:flex;align-items:center;justify-content:space-between;padding:18px 2px 10px;gap:12px}
.logo{display:flex;align-items:center;gap:10px;font-weight:700;font-size:18px}
.logo i{width:34px;height:34px;border-radius:11px;background:linear-gradient(135deg,var(--acc),var(--acc2));display:grid;place-items:center;font-style:normal;box-shadow:0 6px 20px #5b9cff55}
.clock{text-align:right}.clock b{font-size:22px;font-variant-numeric:tabular-nums;letter-spacing:.5px}.clock div{color:var(--mut);font-size:12px}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--mut);margin-right:6px;vertical-align:middle}
.dot.on{background:var(--ok);box-shadow:0 0 10px var(--ok)}.dot.off{background:var(--bad)}
nav{position:fixed;left:0;right:0;bottom:0;z-index:9;padding:8px 10px calc(8px + env(safe-area-inset-bottom));background:#0b0f17e8;backdrop-filter:blur(14px);border-top:1px solid var(--line)}
nav .in{max-width:980px;margin:0 auto;display:flex;gap:4px;overflow-x:auto;scrollbar-width:none}
nav button{flex:1;min-width:64px;border:0;background:none;color:var(--mut);padding:7px 4px;border-radius:12px;font:600 11px system-ui;display:flex;flex-direction:column;align-items:center;gap:3px;cursor:pointer;transition:.2s}
nav button span{font-size:19px;line-height:1}
nav button.act{color:var(--tx);background:linear-gradient(135deg,#5b9cff33,#8b5bff33)}
section{display:none;animation:fade .35s ease}section.act{display:block}
@keyframes fade{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}
h2{font-size:13px;text-transform:uppercase;letter-spacing:1.2px;color:var(--mut);margin:22px 4px 10px;font-weight:700}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(160px,1fr))}
.card{background:linear-gradient(180deg,var(--card2),var(--card));border:1px solid var(--line);border-radius:var(--r);padding:16px;position:relative;overflow:hidden}
.card h3{margin:0 0 10px;font-size:14px;color:var(--mut);font-weight:600;display:flex;justify-content:space-between;align-items:center;gap:8px}
.big{font-size:30px;font-weight:700;font-variant-numeric:tabular-nums}
.sub{color:var(--mut);font-size:13px}
.hero{display:grid;grid-template-columns:1fr auto;gap:10px;align-items:center}
.hero .ico{font-size:64px;line-height:1;filter:drop-shadow(0 6px 18px #0008)}
.hero .t{font-size:54px;font-weight:800;line-height:1;letter-spacing:-1px}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}
.chip{background:#ffffff0d;border:1px solid var(--line);padding:5px 10px;border-radius:999px;font-size:12.5px;color:var(--tx)}
.bar{height:10px;border-radius:99px;background:#ffffff12;overflow:hidden;position:relative;margin-top:10px}
.bar i{position:absolute;inset:0 auto 0 0;border-radius:99px;background:linear-gradient(90deg,var(--acc),var(--acc2));transition:width .6s ease}
.bar.ok i{background:linear-gradient(90deg,#1fb57f,var(--ok))}.bar.warn i{background:linear-gradient(90deg,#d98d06,var(--warn))}.bar.bad i{background:linear-gradient(90deg,#d64545,var(--bad))}
.pill{font-size:12px;font-weight:700;padding:4px 10px;border-radius:99px;background:#ffffff10;color:var(--mut)}
.pill.on{background:#36d39922;color:var(--ok)}.pill.off{background:#f8727222;color:var(--bad)}.pill.wait{background:#fbbd2322;color:var(--warn)}
.seg{display:flex;background:#0006;border:1px solid var(--line);border-radius:14px;padding:4px;gap:4px;margin-top:6px}
.seg button{flex:1;border:0;background:none;color:var(--mut);padding:9px 6px;border-radius:10px;font:600 13.5px system-ui;cursor:pointer;transition:.2s}
.seg button.act{background:linear-gradient(135deg,var(--acc),var(--acc2));color:#fff;box-shadow:0 4px 14px #5b9cff44}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 0;border-top:1px solid var(--line)}
.row:first-of-type{border-top:0}
.row label{font-weight:600}.row small{display:block;color:var(--mut);font-weight:400;font-size:12px}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:28px;background:none;margin:6px 0 0}
input[type=range]::-webkit-slider-runnable-track{height:8px;border-radius:99px;background:linear-gradient(90deg,var(--acc) var(--p,50%),#ffffff18 var(--p,50%))}
input[type=range]::-moz-range-track{height:8px;border-radius:99px;background:linear-gradient(90deg,var(--acc) var(--p,50%),#ffffff18 var(--p,50%))}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:#fff;margin-top:-8px;box-shadow:0 2px 10px #0008,0 0 0 4px #5b9cff55}
input[type=range]::-moz-range-thumb{width:22px;height:22px;border:0;border-radius:50%;background:#fff;box-shadow:0 0 0 4px #5b9cff55}
.sw{position:relative;width:50px;height:30px;flex:none}
.sw input{opacity:0;width:0;height:0}
.sw i{position:absolute;inset:0;border-radius:99px;background:#ffffff1c;transition:.25s;cursor:pointer}
.sw i:after{content:"";position:absolute;left:4px;top:4px;width:22px;height:22px;border-radius:50%;background:#fff;transition:.25s;box-shadow:0 2px 6px #0006}
.sw input:checked+i{background:linear-gradient(135deg,var(--acc),var(--acc2))}
.sw input:checked+i:after{transform:translateX(20px)}
select{background:#0006;color:var(--tx);border:1px solid var(--line);border-radius:12px;padding:9px 12px;font:600 14px system-ui}
.btn{border:0;border-radius:14px;padding:12px 16px;font:700 14px system-ui;color:#fff;background:linear-gradient(135deg,var(--acc),var(--acc2));cursor:pointer;box-shadow:0 6px 18px #5b9cff33;transition:transform .15s}
.btn:active{transform:scale(.97)}.btn.ghost{background:#ffffff10;box-shadow:none;border:1px solid var(--line)}.btn.red{background:linear-gradient(135deg,#ef4444,#f97316);box-shadow:0 6px 18px #ef444433}
.btn.w{width:100%}
.btns{display:grid;gap:10px;grid-template-columns:repeat(auto-fill,minmax(160px,1fr))}
.relhead{display:flex;align-items:center;gap:12px;margin-bottom:6px}
.lamp{width:42px;height:42px;border-radius:14px;display:grid;place-items:center;font-size:22px;background:#ffffff0c;transition:.3s}
.lamp.on{background:radial-gradient(circle,#ffd86b55,#ffb02022);box-shadow:0 0 24px #ffc94d55}
.relhead b{font-size:16px}.relhead .sub{font-size:12px}
.thr{position:relative;margin-top:12px}
.thr .ldr{position:absolute;top:2px;width:2px;height:24px;background:var(--warn);border-radius:2px;box-shadow:0 0 8px var(--warn);pointer-events:none;transition:left .5s}
.kv{display:grid;grid-template-columns:auto 1fr;gap:0}
.kv div{padding:9px 0;border-top:1px solid var(--line)}.kv div:nth-child(-n+2){border-top:0}
.kv div:nth-child(odd){color:var(--mut)}.kv div:nth-child(even){text-align:right;font-weight:600;font-variant-numeric:tabular-nums}
.days{display:flex;gap:10px;overflow-x:auto;padding:2px 2px 8px;scroll-snap-type:x mandatory}
.day{flex:0 0 108px;scroll-snap-align:start;text-align:center;padding:14px 8px;border-radius:16px;background:var(--card);border:1px solid var(--line);cursor:pointer;transition:.2s}
.day.act{border-color:transparent;background:linear-gradient(160deg,#5b9cff44,#8b5bff33);box-shadow:0 8px 24px #5b9cff22}
.day .e{font-size:34px;margin:6px 0}.day b{font-size:18px}.day .sub{font-size:12px}
.hours{display:flex;gap:6px;overflow-x:auto;padding-bottom:6px}
.hr{flex:0 0 52px;text-align:center;padding:8px 2px;border-radius:12px;background:#ffffff08;font-size:12px}
.hr .e{font-size:20px;margin:3px 0}.hr b{font-size:14px}
svg text{fill:var(--mut);font-size:11px}
.toast{position:fixed;left:50%;bottom:96px;transform:translateX(-50%) translateY(20px);opacity:0;background:#fff;color:#111;padding:10px 18px;border-radius:99px;font-weight:700;transition:.3s;z-index:20;pointer-events:none}
.toast.show{opacity:1;transform:translateX(-50%)}
.modal{position:fixed;inset:0;background:#000a;backdrop-filter:blur(6px);display:none;place-items:center;z-index:30;padding:20px}
.modal.show{display:grid}.modal .card{max-width:360px;width:100%;text-align:center}
.glow{position:absolute;right:-30px;top:-30px;width:120px;height:120px;border-radius:50%;filter:blur(30px);opacity:.35;pointer-events:none}
.net{display:flex;align-items:center;gap:12px;padding:12px 4px;border-top:1px solid var(--line);cursor:pointer;border-radius:12px;transition:background .2s}
.net:hover{background:#ffffff08}.net b{font-weight:600}
.sig{display:flex;align-items:flex-end;gap:2px;height:16px;flex:none}.sig i{width:4px;background:#ffffff22;border-radius:2px}.sig i.on{background:linear-gradient(var(--acc),var(--acc2))}
.inp{width:100%;background:#0006;border:1px solid var(--line);color:var(--tx);border-radius:12px;padding:12px 14px;font:15px system-ui;outline:none;transition:border .2s}
.inp:focus{border-color:var(--acc)}.field{text-align:left;margin-top:12px}.field label{display:block;font-size:12px;color:var(--mut);margin:0 0 6px 4px}
.pw{position:relative}.pw button{position:absolute;right:6px;top:6px;border:0;background:#ffffff10;color:var(--tx);border-radius:9px;padding:7px 10px;cursor:pointer}
.hint{color:var(--warn);font-size:12px;margin:6px 4px 0;min-height:14px}.err{color:var(--bad);font-weight:600;font-size:13px;min-height:18px;margin-top:10px}
.spin{display:inline-block;width:14px;height:14px;border:2px solid #fff6;border-top-color:#fff;border-radius:50%;animation:sp .8s linear infinite;vertical-align:-2px;margin-right:6px}
@keyframes sp{to{transform:rotate(360deg)}}
.oled{position:relative;background:#000;border-radius:14px;padding:10px;border:1px solid #1d2636;box-shadow:inset 0 0 30px #000,0 10px 30px #0006}
.oled canvas{display:block;width:100%;image-rendering:pixelated;image-rendering:crisp-edges;filter:drop-shadow(0 0 3px #7cc4ff88)}
.oled .live{position:absolute;top:16px;right:16px;font:700 10px system-ui;letter-spacing:1px;color:#ff6b6b;display:flex;align-items:center;gap:5px;opacity:.85}
.oled .live i{width:7px;height:7px;border-radius:50%;background:#ff4d4d;box-shadow:0 0 8px #ff4d4d;animation:blink 1.2s infinite}
@keyframes blink{50%{opacity:.25}}
.pad{display:grid;grid-template-columns:1fr 1.3fr 1fr;gap:8px;margin-top:12px}
.pad button{border:1px solid var(--line);background:#ffffff0c;color:var(--tx);border-radius:14px;padding:13px 6px;font:700 15px system-ui;cursor:pointer;transition:transform .1s,background .2s;touch-action:manipulation;user-select:none}
.pad button:active{transform:scale(.94);background:#5b9cff33}
.pad .ok{background:linear-gradient(135deg,var(--acc),var(--acc2));border:0;color:#fff;box-shadow:0 6px 18px #5b9cff44}
.pad2{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-top:8px}
.offbar{display:none;margin-top:12px;padding:12px 14px;border-radius:14px;background:#f8727222;color:#ffb4b4;font-weight:600}
.offbar.show{display:block}
</style></head><body><div class="wrap">
<header><div class="logo"><i>⚡</i><div>Контролер<div class="sub" id="hdrNet"><span class="dot" id="netDot"></span>…</div></div></div>
<div class="clock"><b id="hClock">--:--</b><div id="hDate">…</div></div></header>
<div class="offbar" id="offBar">Усе вимкнено (кнопка CONF або пульт). <button class="btn" style="margin-left:8px;padding:6px 12px" onclick="act('allon')">Увімкнути</button></div>

<section id="t-home" class="act">
 <div class="card" style="margin-top:12px"><h3>Дисплей <span class="sub" id="dispInfo"></span></h3>
  <div class="oled" id="oledBox"><canvas id="scr" width="128" height="64"></canvas><div class="live"><i></i>LIVE</div></div>
  <div class="pad"><button onclick="key('left')" title="←">⟲ Ліво</button><button class="ok" onclick="key('ok')" title="Enter">OK</button><button onclick="key('right')" title="→">Право ⟳</button></div>
  <div class="pad2"><button class="btn ghost" onclick="key('back')" title="Esc">BACK</button><button class="btn ghost" onclick="key('conf')">CONF</button><button class="btn ghost" id="wakeBtn" onclick="key(S&&S.disp.asleep?'wake':'sleep')">Заставка</button></div>
  <div class="row" style="display:block;margin-top:8px"><label>Яскравість <b id="s0hv"></b></label><input type="range" min="5" max="100" id="s0h" data-k="scr0"></div>
  <div class="sub">Крутіть коліщатком над екраном або стрілками ← →, Enter — OK, Esc — BACK.</div></div>
 <div class="card" style="margin-top:12px"><div class="glow" style="background:#5b9cff"></div>
  <div class="hero"><div><div class="sub" id="wxWhere">Погода</div><div class="t" id="wxT">--°</div><div id="wxD" style="font-weight:600;margin-top:6px">…</div></div><div class="ico" id="wxI">⛅</div></div>
  <div class="chips" id="wxChips"></div></div>
 <h2>Електрика</h2>
 <div class="grid">
  <div class="card"><h3>Світло в домі <span class="pill" id="gridP">…</span></h3><div class="big" id="gridV">--</div><div class="sub">датчик живлення</div></div>
  <div class="card"><h3>Лінія 5V</h3><div class="big" id="lineV">--</div><div class="bar" id="lineB"><i style="width:0"></i></div></div>
  <div class="card"><h3 id="b0n">АКБ</h3><div class="big" id="b0p">--</div><div class="sub" id="b0v">--</div><div class="bar" id="b0b"><i style="width:0"></i></div></div>
  <div class="card"><h3 id="b1n">АКБ</h3><div class="big" id="b1p">--</div><div class="sub" id="b1v">--</div><div class="bar" id="b1b"><i style="width:0"></i></div></div>
 </div>
 <h2>Освітленість</h2>
 <div class="card"><h3><span id="ldrW">…</span><span class="pill" id="ldrDp">…</span></h3>
  <div class="big" id="ldrMv">-- мВ</div><div class="bar"><i id="ldrB" style="width:0;background:linear-gradient(90deg,#334,#fbbd23,#fff6c4)"></i></div>
  <div class="chips" id="relChips"></div></div>
 <h2>Швидкі дії</h2>
 <div class="btns"><button class="btn red" id="allBtn" onclick="toggleAll()">Вимкнути все</button><button class="btn ghost" onclick="act('wx')">Оновити погоду</button></div>
</section>

<section id="t-rel"><h2>Реле</h2><div class="grid" id="relCards" style="grid-template-columns:repeat(auto-fill,minmax(280px,1fr))"></div>
 <p class="sub" style="margin:12px 4px">Риска на шкалі — освітленість зараз. Лівіше від повзунка = темно, у режимі «Авто» реле вмикається.</p></section>

<section id="t-light"><h2>LED-стрічка</h2>
 <div class="card"><h3>Режим <span class="pill" id="ledNow">…</span></h3><div class="seg" id="ledMode"></div>
  <div class="row" style="display:block"><label>☀️ Світло: яскравість <b id="brNv"></b></label><input type="range" min="0" max="100" id="brN" data-k="lbrn"></div>
  <div class="row"><label>☀️ Ефект</label><select id="fxN" data-k="lfxn"></select></div>
  <div class="row" style="display:block"><label>🌙 Темно: яскравість <b id="brDv"></b></label><input type="range" min="0" max="100" id="brD" data-k="lbrd"></div>
  <div class="row"><label>🌙 Ефект</label><select id="fxD" data-k="lfxd"></select></div></div>
 <h2>Яскравість екрана</h2>
 <div class="card">
  <div class="row" style="display:block"><label>Звичайна <b id="s0v"></b></label><input type="range" min="5" max="100" id="s0" data-k="scr0"></div>
  <div class="row" style="display:block"><label>Заставка <b id="s1v"></b></label><input type="range" min="0" max="100" id="s1" data-k="scr1"></div>
  <div class="row" style="display:block"><label>Заставка, коли все вимкнено <b id="s2v"></b></label><input type="range" min="0" max="100" id="s2" data-k="scr2"></div></div>
</section>

<section id="t-wx"><h2 style="display:flex;justify-content:space-between;align-items:center">Прогноз на 7 днів <button class="btn" style="padding:8px 14px" onclick="act('wx')">⟳ Оновити</button></h2>
 <div class="days" id="days"></div>
 <div class="card" style="margin-top:10px"><h3><span id="dTitle">…</span><span class="sub" id="dSun"></span></h3>
  <div class="chips" id="dChips" style="margin:0 0 10px"></div>
  <svg id="chart" viewBox="0 0 360 190" style="width:100%;height:auto"></svg>
  <div class="hours" id="hours"></div></div>
 <p class="sub" style="margin:10px 4px" id="fcInfo"></p></section>

<section id="t-set"><h2>Налаштування</h2>
 <div class="card">
  <div class="row" style="display:block"><label>Екран засинає через <b id="slv"></b></label><input type="range" min="10" max="600" step="5" id="sl" data-k="sleep"></div>
  <div class="row" style="display:block"><label>Затримка перемикання реле <b id="dlv"></b></label><input type="range" min="1" max="120" id="dl" data-k="dly"></div>
  <div class="row"><label>Автокалібрування LDR<small>шкала підлаштовується сама</small></label><label class="sw"><input type="checkbox" id="cal" data-k="cal"><i></i></label></div>
  <div class="row"><label>За сонцем<small>удень (схід–захід) «Авто» не вмикає реле</small></label><label class="sw"><input type="checkbox" id="sun" data-k="sun"><i></i></label></div>
  <div class="row"><label>Нічне вимкнення RGB<small>вимикати з вказаної години</small></label><label class="sw"><input type="checkbox" id="nof" data-k="nof"><i></i></label></div>
  <div class="row"><label>Година нічного вимкнення</label><select id="noh" data-k="noh"></select></div>
  <div class="row"><label>Калібрування LDR<small id="ldrRange"></small></label><button class="btn ghost" onclick="act('calreset')">Скинути</button></div></div>
 <h2>Слова освітленості</h2>
 <div class="card"><div class="row"><label>Ручні межі<small>інакше — автоматично від калібрування LDR</small></label><label class="sw"><input type="checkbox" id="lman" data-k="lman"><i></i></label></div>
  <div class="sub" style="margin:2px 0 6px">Зараз фоторезистор: <b id="ltNow">--</b></div>
  <div id="ltBox">
  <div class="row" style="display:block"><label>🌑 Темно — нижче <b id="lt1v"></b></label><input type="range" min="100" max="3250" step="10" id="lt1" data-k="lt1"></div>
  <div class="row" style="display:block"><label>🌆 Сутінки — нижче <b id="lt2v"></b></label><input type="range" min="100" max="3250" step="10" id="lt2" data-k="lt2"></div>
  <div class="row" style="display:block"><label>🌤 Світло — нижче <b id="lt3v"></b> <small>вище — Яскраво</small></label><input type="range" min="100" max="3250" step="10" id="lt3" data-k="lt3"></div></div></div>
 <h2>Мережа Wi-Fi</h2>
 <div class="card"><div class="row"><label>Підключення<small id="netSt">…</small></label><button class="btn" id="scanBtn" onclick="scan()">Знайти мережі</button></div>
  <div id="nets"></div>
  <div class="row"><label>Ввести вручну<small>прихована мережа</small></label><button class="btn ghost" onclick="pickNet('',false)">Ввести</button></div>
  <div class="row"><label>Скинути вибір<small>повернути мережу з прошивки</small></label><button class="btn ghost" onclick="confirmAct2('Скинути мережу','Повернути мережу, вписану в прошивку?',forgetNet)">Скинути</button></div></div>
 <h2>Точка доступу ESP32</h2>
 <div class="card"><div class="row"><label>Власний Wi-Fi<small>веб-меню без роутера</small></label><label class="sw"><input type="checkbox" id="ap" data-k="ap"><i></i></label></div>
  <div class="kv" id="apKv"></div></div></section>

<section id="t-sys"><h2>Система</h2>
 <div class="card"><div class="kv" id="sysKv"></div></div>
 <h2>Живлення</h2>
 <div class="btns"><button class="btn ghost" onclick="confirmAct('sleep','Сон','Вимкнути все й увімкнути заставку? Увімкнути — кнопка CONF або це меню.')">🌙 Сон</button>
 <button class="btn ghost" onclick="confirmAct('restart','Перезавантаження','Перезавантажити ESP32?')">⟳ Перезапуск</button>
 <button class="btn red" onclick="confirmAct('deep','Глибокий сон','Вимкнеться все: реле, стрічка, екран і Wi-Fi. Веб-меню стане недоступним. Увімкнути — утримати OK 2 с.')">⏻ Глибокий сон</button></div>
</section>
</div>
<nav><div class="in">
<button data-t="home" class="act"><span>🏠</span>Огляд</button><button data-t="rel"><span>💡</span>Реле</button><button data-t="light"><span>🌈</span>Світло</button><button data-t="wx"><span>⛅</span>Погода</button><button data-t="set"><span>⚙️</span>Налашт.</button><button data-t="sys"><span>ℹ️</span>Система</button>
</div></nav>
<div class="toast" id="toast"></div>
<div class="modal" id="netModal"><div class="card"><h3 style="justify-content:center;font-size:18px;color:var(--tx)" id="nTitle"></h3>
 <div class="field" id="nSsidRow"><label>Назва мережі (SSID)</label><input class="inp" id="nSsid" autocomplete="off" autocapitalize="off" spellcheck="false" maxlength="32"><div class="hint" id="nSsidH"></div></div>
 <div class="field" id="nPassRow"><label>Пароль</label><div class="pw"><input class="inp" id="nPass" type="password" autocomplete="off" autocapitalize="off" spellcheck="false" maxlength="63"><button type="button" onclick="const e=$('nPass');e.type=e.type=='password'?'text':'password'">👁</button></div><div class="hint" id="nPassH"></div></div>
 <div class="err" id="nErr"></div>
 <div class="btns" style="margin-top:12px"><button class="btn ghost" onclick="$('netModal').classList.remove('show')">Скасувати</button><button class="btn" id="nOk" onclick="joinNet()">Підключити</button></div></div></div>
<div class="modal" id="modal"><div class="card"><h3 style="justify-content:center;font-size:18px;color:var(--tx)" id="mT"></h3><p class="sub" id="mB"></p><div class="btns" style="margin-top:16px"><button class="btn ghost" onclick="closeM()">Скасувати</button><button class="btn red" id="mOk">Так</button></div></div></div>
<script>
const $=id=>document.getElementById(id);let S=null,FC=null,selDay=0,lock={};
const MODES=['Авто','Вимк','Увімк'];
function emo(c,day=true){if(c<=1)return day?'☀️':'🌙';if(c==2)return day?'⛅':'☁️';if(c==3)return'☁️';if(c<=48)return'🌫️';if(c<=57)return'🌦️';if(c<=65)return'🌧️';if(c<=67)return'🌨️';if(c<=77)return'❄️';if(c<=82)return'🌧️';if(c<=86)return'🌨️';return'⛈️'}
function wtxt(c){if(c<=1)return'Ясно';if(c==2)return'Мінлива хмарність';if(c==3)return'Хмарно';if(c<=48)return'Туман';if(c<=57)return'Мряка';if(c<=63)return'Дощ';if(c<=67)return'Крижаний дощ';if(c<=77)return'Сніг';if(c<=82)return'Злива';if(c<=86)return'Снігопад';return'Гроза'}
const sg=n=>{n=Math.round(n);return(n>0?'+':'')+n};
function toast(m){const t=$('toast');t.textContent=m;t.classList.add('show');clearTimeout(t._h);t._h=setTimeout(()=>t.classList.remove('show'),1600)}
async function api(u){try{const r=await fetch(u,{cache:'no-store'});if(!r.ok)throw 0;return await r.json()}catch(e){return null}}
async function set(k,v){lock[k]=Date.now();const j=await api('/api/set?k='+k+'&v='+v);if(j){S=j;render();toast('Збережено')}else toast('Нема зв\'язку')}
async function act(a){const j=await api('/api/act?a='+a);const M={wx:'Оновлюю погоду…',calreset:'LDR скинуто',restart:'Перезавантаження…',sleep:'Сон',deep:'Глибокий сон',allon:'Увімкнено',alloff:'Усе вимкнено'};toast(j?M[a]||'Готово':'Нема зв\'язку');if(a=='wx')setTimeout(loadFc,8000);setTimeout(poll,400)}
function confirmAct(a,t,b){$('mT').textContent=t;$('mB').textContent=b;$('mOk').onclick=()=>{closeM();act(a)};$('modal').classList.add('show')}
function confirmAct2(t,b,fn){$('mT').textContent=t;$('mB').textContent=b;$('mOk').onclick=()=>{closeM();fn()};$('modal').classList.add('show')}
const clean=v=>v.replace(/^[\s\u00a0\u200b\ufeff]+|[\s\u00a0\u200b\ufeff]+$/g,'');
const esc=v=>String(v).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const bytes=v=>new TextEncoder().encode(v).length;
function bars(r){const q=Math.max(1,Math.min(4,Math.round((r+95)/12)));return '<span class="sig">'+[1,2,3,4].map(k=>`<i class="${k<=q?'on':''}" style="height:${k*4}px"></i>`).join('')+'</span>'}
async function scan(){const b=$('scanBtn');b.disabled=true;b.innerHTML='<span class="spin"></span>Шукаю';$('nets').innerHTML='<div class="sub" style="padding:12px 4px">Сканую ефір…</div>';
 await api('/api/scan?start=1');let ok=false;for(let i=0;i<25;i++){await new Promise(r=>setTimeout(r,700));const j=await api('/api/scan');if(j&&!j.busy&&j.list){renderNets(j.list);ok=true;break}}
 if(!ok)$('nets').innerHTML='<div class="sub" style="padding:12px 4px">Не вдалося просканувати. Спробуй ще раз.</div>';b.disabled=false;b.textContent='Знайти мережі'}
function renderNets(l){$('nets').innerHTML=l.length?l.map((n,i)=>`<div class="net" data-i="${i}">${bars(n.rssi)}<div style="flex:1;min-width:0"><b style="display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${esc(n.ssid)}</b><span class="sub">${n.rssi} дБм · канал ${n.ch}${n.cur?' · <span style="color:var(--ok)">підключено</span>':''}</span></div>${n.open?'<span class="sub">відкрита</span>':'🔒'}</div>`).join(''):'<div class="sub" style="padding:12px 4px">Мереж не знайдено</div>';
 document.querySelectorAll('.net').forEach(e=>e.onclick=()=>{const n=l[e.dataset.i];pickNet(n.ssid,n.open)})}
function spaceHint(id,h){const v=$(id).value;$(h).textContent=v!==clean(v)?'Пробіли на початку/в кінці буде прибрано':''}
$('nSsid').oninput=()=>spaceHint('nSsid','nSsidH');$('nPass').oninput=()=>spaceHint('nPass','nPassH');
$('nSsid').onblur=()=>{$('nSsid').value=clean($('nSsid').value);spaceHint('nSsid','nSsidH')};$('nPass').onblur=()=>{$('nPass').value=clean($('nPass').value);spaceHint('nPass','nPassH')};
$('nPass').onkeydown=e=>{if(e.key=='Enter')joinNet()};
function pickNet(ssid,open){$('nSsid').value=ssid;$('nSsidRow').style.display=ssid?'none':'';$('nTitle').textContent=ssid||'Мережа вручну';$('nPass').value='';$('nPass').type='password';$('nPassRow').style.display=open?'none':'';$('nErr').textContent='';$('nSsidH').textContent='';$('nPassH').textContent='';$('nOk').disabled=false;$('nOk').textContent='Підключити';$('netModal').classList.add('show');setTimeout(()=>(ssid?(open?$('nOk'):$('nPass')):$('nSsid')).focus(),60)}
async function joinNet(){const s=clean($('nSsid').value),p=clean($('nPass').value);$('nSsid').value=s;$('nPass').value=p;const E=$('nErr');
 if(!s)return E.textContent='Вкажи назву мережі';if(bytes(s)>32)return E.textContent='Назва задовга (макс. 32 байти)';
 if(p&&p.length<8)return E.textContent='Пароль закороткий: мінімум 8 символів';if(p.length>63)return E.textContent='Пароль задовгий';
 const b=$('nOk');b.disabled=true;b.innerHTML='<span class="spin"></span>Зберігаю';
 let r=null;try{r=await(await fetch('/api/wifi',{method:'POST',body:new URLSearchParams({ssid:s,pass:p})})).json()}catch(e){}
 b.disabled=false;b.textContent='Підключити';if(!r||!r.ok){E.textContent=r&&r.err||'Нема зв\'язку з ESP32';return}
 $('netModal').classList.remove('show');toast('Підключаюсь до «'+s+'»…');netWait=Date.now()}
async function forgetNet(){let r=null;try{r=await(await fetch('/api/wifi',{method:'POST',body:new URLSearchParams({forget:1})})).json()}catch(e){}toast(r&&r.ok?'Повертаюсь до «'+r.ssid+'»':'Нема зв\'язку');netWait=Date.now()}
let netWait=0;
function netStatus(s){const n=s.net,M={connected:'підключено до «'+esc(s.wifi.ssid)+'»',connecting:'підключаюсь до «'+esc(n.saved)+'»…',failed:'не вдалося: перевір пароль до «'+esc(n.saved)+'»',nossid:'мережу «'+esc(n.saved)+'» не видно',none:'мережу не задано'};
 let t=M[n.st]||n.st;if(n.apTemp)t+=' · тимчасова точка '+esc(s.wifi.apSsid)+' увімкнена';$('netSt').innerHTML=t;$('netSt').style.color=n.st=='connected'?'var(--ok)':n.st=='connecting'?'var(--warn)':'var(--bad)';
 if(netWait&&n.st=='connected'&&Date.now()-netWait>1500){toast('Підключено до «'+s.wifi.ssid+'»');netWait=0}}
function closeM(){$('modal').classList.remove('show')}
function toggleAll(){if(!S)return;S.sys.allOff?act('allon'):confirmAct('alloff','Вимкнути все','Вимкнути реле, стрічку й екран (як кнопка CONF)?')}
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{document.querySelectorAll('nav button').forEach(x=>x.classList.toggle('act',x==b));document.querySelectorAll('section').forEach(s=>s.classList.toggle('act',s.id=='t-'+b.dataset.t));if(b.dataset.t=='wx')loadFc();scrollTo({top:0,behavior:'smooth'})});
function fill(r){r.style.setProperty('--p',((r.value-r.min)/(r.max-r.min)*100)+'%')}
function bindRange(id,fmt){const r=$(id),v=$(id+'v');r.oninput=()=>{fill(r);if(v)v.textContent=fmt(r.value);lock[r.dataset.k]=Date.now()};r.onchange=()=>set(r.dataset.k,r.value)}
function setRange(id,val,fmt){const r=$(id);if(lock[r.dataset.k]&&Date.now()-lock[r.dataset.k]<2500)return;r.value=val;fill(r);const v=$(id+'v');if(v)v.textContent=fmt(val)}
const pct=v=>v+'%',sec=v=>v>=60?Math.floor(v/60)+' хв '+(v%60?v%60+' с':''):v+' с';
bindRange('brN',pct);bindRange('brD',pct);bindRange('s0',pct);bindRange('s1',pct);bindRange('s2',pct);bindRange('sl',sec);bindRange('dl',v=>v+' с');const volt=v=>(v/1000).toFixed(2)+' В';bindRange('lt1',volt);bindRange('lt2',volt);bindRange('lt3',volt);
['cal','sun','nof','ap','lman'].forEach(id=>$(id).onchange=e=>set(e.target.dataset.k,e.target.checked?1:0));
['fxN','fxD','noh'].forEach(id=>$(id).onchange=e=>set(e.target.dataset.k,e.target.value));
for(let h=0;h<24;h++)$('noh').add(new Option(String(h).padStart(2,'0')+':00',h));
function seg(el,cur,key){if(!el.children.length){MODES.forEach((m,i)=>{const b=document.createElement('button');b.textContent=m;b.onclick=()=>{[...el.children].forEach(x=>x.classList.toggle('act',x==b));set(key,i)};el.appendChild(b)})}if(lock[key]&&Date.now()-lock[key]<2500)return;[...el.children].forEach((b,i)=>b.classList.toggle('act',i==cur))}
function batCls(p){return p>=50?'ok':p>=20?'warn':'bad'}
function kv(el,rows){el.innerHTML=rows.map(r=>'<div>'+r[0]+'</div><div>'+r[1]+'</div>').join('')}
function relCards(){const box=$('relCards');if(box.children.length!=S.relays.length){box.innerHTML='';S.relays.forEach((r,i)=>{const c=document.createElement('div');c.className='card';c.innerHTML=`<div class="relhead"><div class="lamp" id="lp${i}">💡</div><div style="flex:1"><b>${r.n}</b><div class="sub">${r.name}</div></div><span class="pill" id="rp${i}"></span></div><div class="seg" id="rs${i}"></div><div class="thr"><input type="range" min="30" max="990" step="2" id="rt${i}" data-k="t${i}"><div class="ldr" id="rl${i}"></div></div><div class="sub" id="ri${i}"></div>`;box.appendChild(c);const rg=$('rt'+i);rg.oninput=()=>{fill(rg);lock['t'+i]=Date.now();$('ri'+i).textContent='Поріг: '+mvOf(rg.value)+' мВ ('+(rg.value/10).toFixed(1)+'%)'};rg.onchange=()=>set('t'+i,rg.value)})}
 S.relays.forEach((r,i)=>{$('lp'+i).classList.toggle('on',r.on);const p=$('rp'+i);p.className='pill '+(r.pend?'wait':r.on?'on':'off');p.textContent=r.pend?'чекає '+r.pend+' с':r.on?'Увімкнено':'Вимкнено';seg($('rs'+i),r.mode,'m'+i);
 const rg=$('rt'+i);if(!(lock['t'+i]&&Date.now()-lock['t'+i]<2500)){rg.value=r.thr;fill(rg);$('ri'+i).textContent='Поріг: '+r.thrMv+' мВ ('+(r.thr/10).toFixed(1)+'%) · зараз '+(r.dark?'темно':'світло')+(r.night?' · нічне вимкнення':'')}
 const lp=Math.max(0,Math.min(100,(S.ldr.mv-S.ldr.lo)/(S.ldr.hi-S.ldr.lo)*100));const f=Math.max(0,Math.min(1,(lp*10-30)/960));$('rl'+i).style.left='calc(12px + (100% - 24px) * '+f.toFixed(3)+')';$('rl'+i).style.display=S.ldr.absent?'none':''})}
function mvOf(pm){return Math.round(S.ldr.lo+(S.ldr.hi-S.ldr.lo)*pm/1000)}
function render(){if(!S)return;const s=S;
 if(s.place)$('wxWhere').textContent=s.place;netStatus(s);
 $('hClock').textContent=s.time||'--:--';$('hDate').textContent=s.date||'час ще не відомий';
 $('netDot').className='dot '+(s.wifi.ip?'on':'off');$('hdrNet').lastChild.textContent=s.wifi.ip?s.wifi.ssid:'без Wi-Fi';
 $('offBar').classList.toggle('show',s.sys.allOff);$('allBtn').textContent=s.sys.allOff?'Увімкнути все':'Вимкнути все';$('allBtn').className='btn '+(s.sys.allOff?'':'red');
 const w=s.wx;if(w.ok){$('wxT').textContent=sg(w.t)+'°';$('wxD').textContent=wtxt(w.code)+(w.cache?' (з прогнозу)':'');$('wxI').textContent=emo(w.code,s.isDay);
  $('wxChips').innerHTML=(w.cache?'':`<span class="chip">🤚 відчувається ${sg(w.feel)}°</span><span class="chip">💧 ${w.hum}%</span><span class="chip">💨 ${Math.round(w.wind)} км/г</span>`)+(s.sun.rise?`<span class="chip">🌅 ${s.sun.rise}</span><span class="chip">🌇 ${s.sun.set}</span>`:'')}else{$('wxT').textContent='--°';$('wxD').textContent=w.busy?'Завантажую…':'Нема даних'}
 $('gridP').className='pill '+(s.grid.on?'on':'off');$('gridP').textContent=s.grid.on?'Є':'Нема';$('gridV').textContent=s.grid.on?'Світло є':'Без світла';
 $('lineV').textContent=s.line>=0?s.line.toFixed(2)+' В':'--';$('lineB').firstChild.style.width=(s.line>0?Math.min(100,s.line/5.2*100):0)+'%';
 s.bat.forEach((b,i)=>{$('b'+i+'n').textContent=b.n;$('b'+i+'p').textContent=b.v>0?b.p+'%':'--';$('b'+i+'v').textContent=b.v>0?b.v.toFixed(2)+' В':'не підключено';const B=$('b'+i+'b');B.className='bar '+batCls(b.p);B.firstChild.style.width=(b.v>0?b.p:0)+'%'});
 $('ldrW').textContent=s.ldr.absent?'Фоторезистор не підключено':s.ldr.word;$('ldrDp').textContent=s.daypart||'—';$('ldrMv').textContent=s.ldr.absent?'—':s.ldr.mv+' мВ · '+s.ldr.pct+'%';$('ldrB').style.width=s.ldr.pct+'%';
 $('relChips').innerHTML=s.relays.map(r=>`<span class="chip"><span class="dot ${r.on?'on':''}"></span>${r.n} · ${MODES[r.mode]}</span>`).join('');
 relCards();
 seg($('ledMode'),s.led.mode,'lmode');$('ledNow').textContent=s.led.on?s.fx[s.led.fx]+' '+s.led.pct+'%':'вимкнено';$('ledNow').className='pill '+(s.led.on?'on':'');
 ['fxN','fxD'].forEach(id=>{const e=$(id);if(!e.options.length)s.fx.forEach((f,i)=>e.add(new Option(f,i)));if(!(lock[e.dataset.k]&&Date.now()-lock[e.dataset.k]<2500))e.value=id=='fxN'?s.led.fxN:s.led.fxD});
 setRange('brN',s.led.brN,pct);setRange('brD',s.led.brD,pct);setRange('s0',s.scr[0],pct);setRange('s0h',s.scr[0],pct);$('wakeBtn').textContent=s.disp.asleep?'Розбудити':'Заставка';setRange('s1',s.scr[1],pct);setRange('s2',s.scr[2],pct);
 setRange('sl',s.set.sleep,sec);setRange('dl',s.set.dly,v=>v+' с');
 [['cal',s.set.cal],['sun',s.set.sun],['nof',s.set.nof],['ap',s.wifi.ap],['lman',s.set.lman]].forEach(([id,v])=>{const e=$(id);if(!(lock[e.dataset.k]&&Date.now()-lock[e.dataset.k]<2500))e.checked=!!v});
 if(!(lock.noh&&Date.now()-lock.noh<2500))$('noh').value=s.set.noh;
 [0,1,2].forEach(k=>setRange('lt'+(k+1),s.set.lt[k],volt));$('ltBox').style.opacity=s.set.lman?1:.4;$('ltNow').textContent=s.ldr.absent?'не підключено':(s.ldr.mv/1000).toFixed(2)+' В · '+s.ldr.word;
 $('ldrRange').textContent='шкала '+Math.round(s.ldr.lo)+'–'+Math.round(s.ldr.hi)+' мВ';
 kv($('apKv'),(s.wifi.ap||s.net.apTemp)?[['Назва',s.wifi.apSsid],['Пароль',s.wifi.apPass],['Адреса','http://'+s.wifi.apIp],['Підключено',s.wifi.apClients+' пристр.']]:[['Стан','вимкнено']]);
 const up=s.sys.up,d=Math.floor(up/86400),hh=Math.floor(up/3600)%24,mm=Math.floor(up/60)%60;
 kv($('sysKv'),[['Wi-Fi',s.wifi.ip?s.wifi.ssid:'нема'],['IP',s.wifi.ip||'—'],['Сигнал',s.wifi.ip?s.wifi.rssi+' дБм ('+Math.max(0,Math.min(100,2*(s.wifi.rssi+100)))+'%)':'—'],['Веб-меню','http://'+s.wifi.host+'.local'],['Годинник',s.sys.ntp?'NTP, '+(s.sys.ppm>0?'+':'')+s.sys.ppm.toFixed(1)+' ppm':'без NTP'],['Схід / захід',s.sun.rise?s.sun.rise+' / '+s.sun.set:'—'],['Pro Micro',s.sys.proOk?'зв\'язок є'+(s.sys.pro?' · v'+s.sys.pro:''):'нема зв\'язку'],['INA3221',s.sys.ina?'працює':'нема'],['Фоторезистор',s.ldr.absent?'нема':s.ldr.mv+' мВ'],['Вільна пам\'ять',s.sys.heap+' КБ'],['Працює',d+' д '+hh+' год '+mm+' хв'],['Прошивка',s.sys.fw]]);
}
async function poll(){const j=await api('/api/state');if(j){S=j;render()}else{$('netDot').className='dot off';$('hdrNet').lastChild.textContent='нема зв\'язку з ESP32'}}
async function loadFc(){const j=await api('/api/fc');if(!j||!j.start){$('fcInfo').textContent='Прогнозу ще нема — потрібен інтернет.';return}FC=j;renderFc()}
function dayData(d){const o={t:[],c:[],p:[],h:[],w:[]};for(let h=0;h<24;h++){const i=d*24+h;o.t.push(FC.temp[i]);o.c.push(FC.code[i]);o.p.push(FC.pop[i]);o.h.push(FC.hum[i]);o.w.push(FC.wind[i])}return o}
function dayCode(o){const cnt={};let wet=0;for(let h=8;h<=20;h++){const c=o.c[h];if(c==null)continue;cnt[c]=(cnt[c]||0)+1;if(c>=51)wet++}let best=null;for(const c in cnt){if(wet>=3&&c<51)continue;if(best==null||cnt[c]>cnt[best])best=c}return +best}
function renderFc(){const now=Date.now()/1000;let first=Math.max(0,Math.floor((now-FC.start)/86400));const box=$('days');box.innerHTML='';const WD=['Нд','Пн','Вт','Ср','Чт','Пт','Сб'];
 if(selDay<first)selDay=first;
 for(let d=first;d<7;d++){const o=dayData(d),v=o.t.filter(x=>x!=null);if(!v.length)continue;const dt=new Date((FC.start+d*86400+43200)*1000);const el=document.createElement('div');el.className='day'+(d==selDay?' act':'');
  el.innerHTML=`<div class="sub">${d==first?'Сьогодні':WD[dt.getDay()]+' '+dt.getDate()+'.'+String(dt.getMonth()+1).padStart(2,'0')}</div><div class="e">${emo(dayCode(o))}</div><b>${sg(Math.max(...v))}°</b> <span class="sub">${sg(Math.min(...v))}°</span><div class="sub">💧 ${Math.max(...o.p.map(x=>x||0))}%</div>`;el.onclick=()=>{selDay=d;renderFc()};box.appendChild(el)}
 const o=dayData(selDay),dt=new Date((FC.start+selDay*86400+43200)*1000);
 $('dTitle').textContent=dt.toLocaleDateString('uk-UA',{weekday:'long',day:'numeric',month:'long'});
 const tm=t=>new Date(t*1000).toLocaleTimeString('uk-UA',{hour:'2-digit',minute:'2-digit'});
 $('dSun').textContent=FC.rise[selDay]?'🌅 '+tm(FC.rise[selDay])+'  🌇 '+tm(FC.set[selDay]):'';
 const v=o.t.filter(x=>x!=null);$('dChips').innerHTML=`<span class="chip">${emo(dayCode(o))} ${wtxt(dayCode(o))}</span><span class="chip">🌡 ${sg(Math.min(...v))}…${sg(Math.max(...v))}°</span><span class="chip">💧 до ${Math.max(...o.p.map(x=>x||0))}%</span><span class="chip">💨 до ${Math.max(...o.w.map(x=>x||0))} км/г</span>`;
 chart(o);const isDay=h=>{const t=FC.start+selDay*86400+h*3600+1800;return FC.rise[selDay]?t>=FC.rise[selDay]&&t<FC.set[selDay]:h>=7&&h<18};
 $('hours').innerHTML=o.t.map((t,h)=>t==null?'':`<div class="hr"><div class="sub">${String(h).padStart(2,'0')}:00</div><div class="e">${emo(o.c[h],isDay(h))}</div><b>${sg(t)}°</b><div class="sub">💧${o.p[h]??'-'}%</div><div class="sub">💨${o.w[h]??'-'}</div></div>`).join('');
 const f=new Date(FC.fetched*1000);$('fcInfo').textContent=FC.fetched?'Прогноз Open-Meteo, оновлено '+f.toLocaleString('uk-UA'):'';}
function chart(o){const W=360,H=190,L=30,R=8,T=18,B=30;const v=o.t.filter(x=>x!=null);let lo=Math.min(...v),hi=Math.max(...v);if(hi-lo<4){const m=(hi+lo)/2;lo=m-2;hi=m+2}
 const X=h=>L+h*(W-L-R)/23,Y=t=>T+(hi-t)*(H-T-B)/(hi-lo);let pts=[];o.t.forEach((t,h)=>{if(t!=null)pts.push([X(h),Y(t)])});
 let path=pts.map((p,i)=>(i?'L':'M')+p[0].toFixed(1)+' '+p[1].toFixed(1)).join(' ');
 let area=path+` L${pts[pts.length-1][0]} ${H-B} L${pts[0][0]} ${H-B} Z`;
 let bars=o.p.map((p,h)=>p?`<rect x="${X(h)-4.5}" y="${H-B-(p/100)*(H-T-B)*.45}" width="9" height="${(p/100)*(H-T-B)*.45}" rx="3" fill="#5b9cff" opacity=".28"/>`:'').join('');
 let grid='';for(let i=0;i<=3;i++){const t=lo+(hi-lo)*i/3;grid+=`<line x1="${L}" x2="${W-R}" y1="${Y(t)}" y2="${Y(t)}" stroke="#ffffff12"/><text x="${L-6}" y="${Y(t)+3}" text-anchor="end">${sg(t)}°</text>`}
 let xs='';for(let h=0;h<24;h+=3)xs+=`<text x="${X(h)}" y="${H-B+16}" text-anchor="middle">${String(h).padStart(2,'0')}</text>`;
 let dots=pts.map((p,i)=>`<circle cx="${p[0]}" cy="${p[1]}" r="2.6" fill="#fff"/>`).join('');
 let mx=o.t.indexOf(Math.max(...v)),mn=o.t.indexOf(Math.min(...v));
 let lab=`<text x="${X(mx)}" y="${Y(o.t[mx])-8}" text-anchor="middle" style="fill:#fff;font-weight:700;font-size:12px">${sg(o.t[mx])}°</text><text x="${X(mn)}" y="${Y(o.t[mn])+16}" text-anchor="middle" style="fill:#9cc2ff;font-weight:700;font-size:12px">${sg(o.t[mn])}°</text>`;
 $('chart').innerHTML=`<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#8b5bff" stop-opacity=".55"/><stop offset="1" stop-color="#5b9cff" stop-opacity="0"/></linearGradient><linearGradient id="gl" x1="0" x2="1"><stop offset="0" stop-color="#5b9cff"/><stop offset="1" stop-color="#c084fc"/></linearGradient></defs>${grid}${bars}<path d="${area}" fill="url(#g)"/><path d="${path}" fill="none" stroke="url(#gl)" stroke-width="3" stroke-linejoin="round" stroke-linecap="round"/>${dots}${lab}${xs}`}
function openHash(){const b=document.querySelector('nav button[data-t="'+location.hash.slice(1)+'"]');if(b)b.click()}openHash();// ---- трансляція екрана ----
const cv=$('scr'),cx=cv.getContext('2d'),img=cx.createImageData(128,64);let scrBusy=false,frames=0,fT=Date.now();
function drawFrame(b,o){const d=img.data;
 for(let p=0;p<8;p++)for(let x=0;x<128;x++){const v=b[o+p*128+x];for(let k=0;k<8;k++){const i=((p*8+k)*128+x)*4,on=v>>k&1;d[i]=on?190:4;d[i+1]=on?228:8;d[i+2]=on?255:14;d[i+3]=255}}
 cx.putImageData(img,0,0);frames++}
const homeOn=()=>!document.hidden&&$('t-home').classList.contains('act');
const sleepMs=ms=>new Promise(r=>setTimeout(r,ms));
function fpsInfo(){if(Date.now()-fT>2000){$('dispInfo').textContent=(S?'екран '+Math.round(S.disp.fps)+' к/с · ':'')+'трансляція '+Math.round(frames*1000/(Date.now()-fT))+' к/с';frames=0;fT=Date.now()}}
// Потік: одне з'єднання, плата сама шле кадри по 1024 байти (~30 к/с). Якщо браузер не вміє потоки — опитування.
async function scrLoop(){
 for(;;){
  if(!homeOn()){await sleepMs(400);continue}
  const ac=new AbortController();
  try{const r=await fetch('/api/stream',{signal:ac.signal,cache:'no-store'});
   if(!r.body||!r.body.getReader){await pollFrames();continue}
   const rd=r.body.getReader();let buf=new Uint8Array(4096),len=0;
   for(;;){if(!homeOn()){ac.abort();break}
    const {value,done}=await rd.read();if(done)break;
    if(len+value.length>buf.length){const nb=new Uint8Array(len+value.length+2048);nb.set(buf.subarray(0,len));buf=nb}
    buf.set(value,len);len+=value.length;
    if(len>=1024){const last=Math.floor(len/1024)*1024-1024;drawFrame(buf,last);buf.copyWithin(0,last+1024,len);len-=last+1024}
    fpsInfo()}
  }catch(e){}
  await sleepMs(600)}}
async function pollFrames(){while(homeOn()){const t0=performance.now();
 try{const r=await fetch('/api/screen',{cache:'no-store'});const b=new Uint8Array(await r.arrayBuffer());if(b.length==1024)drawFrame(b,0)}catch(e){await sleepMs(800)}
 fpsInfo();await sleepMs(Math.max(0,33-(performance.now()-t0)))}}
async function key(k){await api('/api/key?k='+k)}
let wheelT=0;$('oledBox').addEventListener('wheel',e=>{e.preventDefault();const n=Date.now();if(n-wheelT<60)return;wheelT=n;key(e.deltaY>0?'right':'left')},{passive:false});
document.addEventListener('keydown',e=>{if(!$('t-home').classList.contains('act')||document.activeElement.tagName=='INPUT'||document.activeElement.tagName=='SELECT')return;
 const m={ArrowLeft:'left',ArrowRight:'right',Enter:'ok',Escape:'back',Backspace:'back'};if(m[e.key]){e.preventDefault();key(m[e.key])}});
bindRange('s0h',pct);
scrLoop();
poll();setInterval(poll,1500);loadFc();setInterval(loadFc,600000);
</script></body></html>)HTML";
