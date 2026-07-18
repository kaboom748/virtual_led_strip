#include "virtual_led_strip.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/light/light_state.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef USE_HOST
#include <csignal>
#endif

#ifdef USE_ESP32
#include <lwip/tcp.h>
#endif
#ifndef TCP_NODELAY
#include <netinet/tcp.h>
#endif

namespace esphome {
namespace virtual_led_strip {

static const char *const TAG = "virtual_led_strip";

enum : uint8_t { ENC_RAW = 0, ENC_RLE = 1, ENC_DELTA = 2, MSG_HELLO = 0xFE, MSG_BEAT = 0xFF };

static const uint32_t HEARTBEAT_MS = 100;
static const size_t HTML_CHUNK = 256;
static const uint32_t REQUEST_TIMEOUT_MS = 2000;

#if defined(USE_ESP8266)
static const char PLATFORM_NAME[] = "ESP8266";
#elif defined(USE_ESP32)
static const char PLATFORM_NAME[] = "ESP32";
#else
static const char PLATFORM_NAME[] = "host";
#endif

static const char HTML_PAGE[] PROGMEM = R"HTMLDOC(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Virtual LED Strip</title>
<style>
* { box-sizing: border-box; }
body { margin: 0; padding: 18px; background: #0a0a0c; color: #ddd; font-family: -apple-system, system-ui, sans-serif; }
.wrap { max-width: 2000px; margin: 0 auto; }
.panel { background: #17171a; border: 1px solid #2c2c30; border-radius: 8px; padding: 14px 16px; font-family: monospace; font-size: 12px; }
.cols { display: flex; gap: 16px; align-items: flex-start; margin-top: 16px; flex-wrap: wrap; }
.cols > .panel { flex: 1 1 380px; min-width: 0; }
.stats { flex: 0 0 320px; }
h3 { margin: 0 0 10px 0; font-size: 11px; letter-spacing: 2px; color: #29E5FF; font-weight: normal; }
.meta { float: right; color: #55555a; letter-spacing: 0; font-size: 10px; }
#strip { width: 100%; display: block; }
.row { display: flex; justify-content: space-between; padding: 3px 0; }
.row span:first-child { color: #86868c; }
.row span:last-child { color: #eee; }
.sep { border-top: 1px solid #2c2c30; margin: 8px 0; }
.ctl { margin-bottom: 11px; }
.ctl label { display: flex; justify-content: space-between; color: #86868c; margin-bottom: 5px; }
.ctl label b { color: #29E5FF; font-weight: normal; }
input[type=range] { width: 100%; accent-color: #29E5FF; }
select { width: 100%; background: #202024; color: #ddd; border: 1px solid #3a3a40; border-radius: 4px; padding: 5px; font-family: monospace; font-size: 12px; }
.chk { display: flex; align-items: center; gap: 8px; color: #86868c; margin-bottom: 7px; }
.chk input { accent-color: #29E5FF; }
.hint { color: #55555a; font-size: 11px; line-height: 1.5; margin-top: 6px; }
.label { text-align: center; color: #4a4a4e; font-size: 11px; margin-top: 16px; letter-spacing: 1px; }
.g { color: #4ade80; } .y { color: #fbbf24; } .r { color: #ff6b6b; }
</style></head><body>
<div class="wrap">
 <div class="panel"><h3>STRIP<span class="meta" id="meta">--</span></h3>
  <canvas id="strip"></canvas>
  <div class="hint" id="geo">--</div>
 </div>
 <div class="cols">
  <div class="panel"><h3>LAYOUT</h3>
    <div class="ctl"><label>Physical LEDs <b id="physL">--</b></label>
    <input type="number" id="phys" min="1" max="2000" step="1" value="150"></div>
   <div class="ctl"><label>Rows <b id="rowsL">1</b></label>
    <input type="range" id="rows" min="1" max="8" step="1" value="4"></div>
   <div class="ctl"><label>Elbow LEDs <b id="elbowL">0</b></label>
    <input type="range" id="elbow" min="0" max="10" step="1" value="2"></div>
   <div class="chk"><input type="checkbox" id="serp" checked><span>Serpentine wiring</span></div>
   <div class="ctl"><label>Package</label>
    <select id="pkg"><option value="dome">12 mm diffused pixel</option><option value="smd">5050 strip LED</option></select></div>
   <div class="hint">Physical LEDs, rows, elbows, serpentine and package are assertions about YOUR wiring. The ESP knows num_leds -- how many it drives -- and nothing else. LEDs past num_leds are never addressed: they do not switch off, they HOLD, and at power-on their state is undefined.</div>
  </div>
  <div class="panel"><h3>VIEW</h3>
   <div class="ctl"><label>Monitor brightness <b id="briL">100%</b></label>
    <input type="range" id="bri" min="0.05" max="1" step="0.05" value="1"></div>
   <div class="ctl"><label>Ambient light <b id="ambL">34</b></label>
    <input type="range" id="amb" min="0" max="90" step="2" value="34"></div>
   <div class="ctl"><label>Play-out buffer <b id="poL">150 ms</b></label>
    <input type="range" id="po" min="0" max="400" step="10" value="150"></div>
   <div class="chk"><input type="checkbox" id="integ" checked><span>Integrate over display frame</span></div>
   <div class="chk"><input type="checkbox" id="glow" checked><span>Halo (an optical model, not data)</span></div>
   <div class="hint">Ambient is a claim about the light in YOUR room. A real strip at buffer 0 is invisible in the dark; here it stays legible.</div>
  </div>
  <div class="panel stats"><h3>NETWORK STATISTICS</h3>
   <div class="row"><span>Platform</span><span id="sPlat">--</span></div>
   <div class="row"><span>Link</span><span id="sLink" class="r">script not running</span></div>
   <div class="row"><span>LEDs</span><span id="sLeds">--</span></div>
   <div class="row"><span>Frames/s (ESP)</span><span id="sFps">--</span></div>
   <div class="row"><span>Display</span><span id="sDisp">--</span></div>
   <div class="row"><span>Render</span><span id="sRen">--</span></div>
   <div class="row"><span>Bytes/frame</span><span id="sBpf">--</span></div>
   <div class="row"><span>Encoding</span><span id="sEnc">--</span></div>
   <div class="row"><span>Throughput</span><span id="sThr">--</span></div>
   <div class="row"><span>Frames</span><span id="sFr">--</span></div>
   <div class="row"><span>Dropped frames</span><span id="sDrop">--</span></div>
   <div class="row"><span>Beat rate / target</span><span id="sBeat">--</span></div>
   <div class="row"><span>Link jitter</span><span id="sJit">--</span></div>
   <div class="row"><span>ESP loop worst 30s</span><span id="sLoop">--</span></div>
   <div class="row"><span>ESP loop worst max</span><span id="sLoopMax">--</span></div>
   <div class="row"><span>Reconnects</span><span id="sRec">--</span></div>
   <div class="sep"></div>
   <div class="row"><span>Margin</span><span id="sMar">--</span></div>
   <div class="row"><span>Margin min</span><span id="sMin">--</span></div>
   <div class="row"><span>Late frames</span><span id="sLate">--</span></div>
   <div class="row"><span>Re-anchors</span><span id="sAnc">--</span></div>
  </div>
 </div>
 <div class="label">Virtual LED Strip - virtual_led_strip</div>
</div>
<script>
var $ = function (i) { return document.getElementById(i); };
var set = function (i, v) { var e = $(i); if (e && e.textContent !== v) e.textContent = v; };
// Un panneau qui affiche un etat plausible alors que rien ne tourne est pire
// qu'un panneau vide. Les defauts HTML disent l'echec; le script les dement.
window.onerror = function (m, u, l) {
  var e = $('sLink'); if (e) { e.textContent = 'JS error line ' + l; e.className = 'r'; }
  return false;
};
$('sLink').textContent = 'connecting'; $('sLink').className = '';
['sFr', 'sDrop', 'sRec', 'sLate', 'sAnc'].forEach(function (i) { $(i).textContent = '0'; });

// Le tampon porte des rapports cycliques PWM: la lumiere emise est LINEAIRE en
// cette valeur. L'ecran attend du sRGB -- pas une puissance 2.2 mais une courbe
// par morceaux dont le segment lineaire couvre le bas de plage, precisement ou
// le gamma 2.8 d'ESPHome ecrase tout. Un rendu naif y perd un facteur 10 a 700.
var LUT = new Uint8Array(256), LIN = new Float32Array(256), i;
for (i = 0; i < 256; i++) {
  var L = i / 255;
  LIN[i] = L;
  LUT[i] = Math.round(255 * (L <= 0.0031308 ? 12.92 * L : 1.055 * Math.pow(L, 1 / 2.4) - 0.055));
}
function toSrgb(L) {
  if (L <= 0) return 0; if (L >= 1) return 255;
  return Math.round(255 * (L <= 0.0031308 ? 12.92 * L : 1.055 * Math.pow(L, 1 / 2.4) - 0.055));
}
function cl(v) { return v < 0 ? 0 : v > 255 ? 255 : Math.round(v); }

var N = 0, BPP = 3, strip = null, queue = [], pending = null;
var anchorEsp = null, anchorPerf = 0, playout = 0.15, resync = true;
var nFrames = 0, nDrop = 0, nLate = 0, nAnchor = 0, nRec = 0, bytes = 0;
var beats = [], arrivals = [], lastBeatTs = null, lastEspTs = null, lastArr = 0;
var gaps = [], espMaxAll = 0, marginMin = null, lastEnc = '--', lastLen = 0;
var t0 = 0, nDisp = 0, nEsp = 0, nRaf = 0;

function onHello(n, bpp) {
  N = n; BPP = bpp;
  strip = new Uint8Array(N * BPP);
  queue = []; anchorEsp = null; resync = true; pending = null;
  set('sLeds', String(N) + (BPP === 4 ? ' RGBW' : ''));
  set('meta', 'num_leds ' + N);
  // Plancher a 150: un ruban de 10 LED perdu dans un panneau vide n'a l'air de
  // rien, et la taille des domes doit rester la meme quel que soit num_leds.
  // Mais on ne pilote pas une LED qu'on n'a pas soudee: au-dela de 150, le
  // physique suit num_leds. Le '<' preserve un reglage manuel plus genereux.
  var want = Math.max(150, N);
  if (parseInt($('phys').value, 10) < want) { $('phys').value = want; sync(); }
}
function schedule(ts) {
  var now = performance.now() / 1000;
  if (anchorEsp === null || resync) {
    anchorEsp = ts; anchorPerf = now; resync = false; nAnchor++;
    set('sAnc', String(nAnchor));
  }
  return anchorPerf + (ts - anchorEsp) / 1000 + playout;
}
// L'ecart le plus long entre deux instants ESP, sur une fenetre glissante de
// 30 s. Un maximum depuis toujours colle un decrochage du demarrage au panneau
// pour l'eternite: impossible de distinguer un vieux gel d'un probleme en cours.
function noteGap(d) {
  var now = performance.now() / 1000;
  if (d > espMaxAll) { espMaxAll = d; set('sLoopMax', d + ' ms'); }
  gaps.push([now, d]);
  while (gaps.length && now - gaps[0][0] > 30) gaps.shift();
}
function decodeInto(dst, enc, p) {
  if (enc === 0) { dst.set(p); return; }
  if (enc === 1) {
    var o = 0;
    for (var k = 0; k + 1 + BPP <= p.length && o < dst.length; k += 1 + BPP) {
      for (var c = 0; c < p[k] && o < dst.length; c++) {
        for (var b = 0; b < BPP; b++) dst[o + b] = p[k + 1 + b];
        o += BPP;
      }
    }
    return;
  }
  var n = p[0] | (p[1] << 8);
  for (var j = 0; j < n; j++) {
    var off = 2 + j * (2 + BPP), idx = (p[off] | (p[off + 1] << 8)) * BPP;
    for (var b2 = 0; b2 < BPP; b2++) dst[idx + b2] = p[off + 2 + b2];
  }
}
function onFrame(enc, ts, seq, payload) {
  var arr = performance.now() / 1000;
  // La gigue reseau est l'ecart des ARRIVEES moins la periode que l'ESP a
  // reellement vecue. Sans cette soustraction on accuserait le reseau d'un gel
  // de l'ESP -- l'erreur que ce panneau existe pour empecher.
  if (lastEspTs !== null) {
    var d = ((ts - lastEspTs) >>> 0);
    noteGap(d);
    arrivals.push((arr - lastArr) * 1000 - d);
    if (arrivals.length > 40) arrivals.shift();
  }
  lastEspTs = ts; lastArr = arr;
  var t = schedule(ts);
  var margin = (t - performance.now() / 1000) * 1000;
  if (marginMin === null || margin < marginMin) marginMin = margin;
  if (margin < 0) { nLate++; set('sLate', String(nLate)); }
  set('sMar', margin.toFixed(1) + ' ms');
  set('sMin', marginMin.toFixed(1) + ' ms');
  var snap = new Uint8Array(strip.length);
  snap.set(strip);
  decodeInto(snap, enc, payload);
  strip = snap;
  queue.push({ t: t, f: snap });
  if (queue.length > 240) queue.shift();
  nFrames++; nEsp++;
  lastEnc = ['RAW', 'RLE', 'DELTA'][enc] || '?';
  lastLen = payload.length + 10;
  bytes += lastLen;
}
function onBeat(ts) {
  var now = performance.now() / 1000;
  if (lastBeatTs !== null) noteGap(((ts - lastBeatTs) >>> 0));
  lastBeatTs = ts;
  beats.push(now);
  while (beats.length && now - beats[0] > 2) beats.shift();
  if (anchorEsp === null) { anchorEsp = ts; anchorPerf = now; }
}
var buf = new Uint8Array(0);
function feed(chunk) {
  var n = new Uint8Array(buf.length + chunk.length);
  n.set(buf); n.set(chunk, buf.length); buf = n;
  for (;;) {
    if (buf.length < 2) return;
    if (buf[0] !== 0xAA) { buf = buf.subarray(1); continue; }
    var kind = buf[1];
    if (kind === 0xFE) {
      if (buf.length < 5) return;
      onHello(buf[2] | (buf[3] << 8), buf[4]);
      buf = buf.subarray(5); continue;
    }
    if (kind === 0xFF) {
      if (buf.length < 6) return;
      onBeat((buf[2] | (buf[3] << 8) | (buf[4] << 16) | (buf[5] << 24)) >>> 0);
      buf = buf.subarray(6); continue;
    }
    if (buf.length < 10) return;
    var ln = buf[8] | (buf[9] << 8);
    if (buf.length < 10 + ln) return;
    onFrame(kind, (buf[2] | (buf[3] << 8) | (buf[4] << 16) | (buf[5] << 24)) >>> 0,
            buf[6] | (buf[7] << 8), buf.subarray(10, 10 + ln));
    buf = buf.subarray(10 + ln);
  }
}
function stream() {
  set('sLink', 'connecting'); $('sLink').className = '';
  fetch('/events').then(function (r) {
    set('sPlat', r.headers.get('X-Platform') || '--');
    set('sLink', 'up'); $('sLink').className = 'g';
    var rd = r.body.getReader();
    function pump() {
      return rd.read().then(function (res) {
        if (res.done) throw new Error('closed');
        feed(res.value);
        return pump();
      });
    }
    return pump();
  }).catch(function () {
    set('sLink', 'down'); $('sLink').className = 'r';
    nRec++; set('sRec', String(nRec));
    resync = true; buf = new Uint8Array(0); lastEspTs = null; lastBeatTs = null;
    setTimeout(stream, 1000);
  });
}

// ---------- rendu ----------
// AMBIANT + EMISSION, aucune branche on/off. Une LED eteinte et une LED a zero
// sont le MEME objet: la lumiere de la piece sur le dome blanc ne disparait pas
// quand la puce s'allume, elle s'ajoute. Deux formules separees ne se
// rejoignaient pas a zero et la trainee d'une comete devenait plus noire que le
// ruban eteint. Verifie monotone sur les 256 niveaux.
// Un halo EST un flou: sa resolution ne veut rien dire, il n'y a rien a voir
// dedans. On le peint donc dans un tampon au quart et on l'etire -- seize fois
// moins de pixels, indiscernable. C'est ce que fait tout moteur de bloom.
// A 150 domes de 40 px, le plein resolution coutait 1,8 M px de degrade radial
// par image, soit 70 M px/s: le canvas saturait a 39 Hz. Et comme mon rendu
// tient le fil principal, fetch ne lisait pas pendant ce temps -- les trames
// arrivaient en rafale et le panneau appelait ca "Link jitter".
function halo(g, x, y, d, r, v, b) {
  var a = (r + v + b) / 3 / 255;
  var h = g.createRadialGradient(x, y, d * 0.4, x, y, d * 1.55);
  h.addColorStop(0, 'rgba(' + r + ',' + v + ',' + b + ',' + (a * 0.42).toFixed(3) + ')');
  h.addColorStop(0.4, 'rgba(' + r + ',' + v + ',' + b + ',' + (a * 0.11).toFixed(3) + ')');
  h.addColorStop(1, 'rgba(' + r + ',' + v + ',' + b + ',0)');
  g.fillStyle = h;
  g.beginPath(); g.arc(x, y, d * 1.55, 0, 6.2832); g.fill();
}
function dome(g, x, y, d, r, v, b, amb) {
  var lum = (r + v + b) / 3;
  var w = (lum / 255) * 0.72;
  var dg = g.createRadialGradient(x - d * 0.14, y - d * 0.17, d * 0.03, x, y, d * 0.54);
  dg.addColorStop(0, 'rgb(' + cl(amb * 1.5 + r + (255 - r) * w) + ',' + cl(amb * 1.5 + v + (255 - v) * w) + ',' + cl(amb * 1.6 + b + (255 - b) * w) + ')');
  dg.addColorStop(0.52, 'rgb(' + cl(amb * 1.15 + r * 0.9 + (255 - r) * w * 0.3) + ',' + cl(amb * 1.15 + v * 0.9 + (255 - v) * w * 0.3) + ',' + cl(amb * 1.25 + b * 0.9 + (255 - b) * w * 0.3) + ')');
  dg.addColorStop(1, 'rgb(' + cl(amb * 0.72 + r * 0.66) + ',' + cl(amb * 0.72 + v * 0.66) + ',' + cl(amb * 0.8 + b * 0.66) + ')');
  g.fillStyle = dg;
  g.beginPath(); g.arc(x, y, d / 2, 0, 6.2832); g.fill();
  if (d > 24) {
    g.fillStyle = 'rgba(255,255,255,0.15)';
    g.beginPath(); g.ellipse(x - d * 0.17, y - d * 0.21, d * 0.13, d * 0.09, -0.6, 0, 6.2832); g.fill();
  }
}
// Le 5050 nu: pas de diffuseur, donc les trois puces restent visibles a faible
// luminosite. Elles ne fusionnent qu'en montant. Meme regle: ambiant + emission.
function smd(g, x, y, s, r, v, b, amb, glow) {
  var lum = (r + v + b) / 3, h = s / 2;
  if (glow && lum > 2) { g.shadowColor = 'rgb(' + r + ',' + v + ',' + b + ')'; g.shadowBlur = 3 + lum / 255 * s * 0.85; }
  g.fillStyle = 'rgb(' + cl(amb + r * 0.74) + ',' + cl(amb + v * 0.74) + ',' + cl(amb * 1.04 + b * 0.74) + ')';
  var c = s * 0.24;
  g.beginPath();
  g.moveTo(x - h + c, y - h); g.lineTo(x + h - s * 0.1, y - h);
  g.quadraticCurveTo(x + h, y - h, x + h, y - h + s * 0.1);
  g.lineTo(x + h, y + h - s * 0.1);
  g.quadraticCurveTo(x + h, y + h, x + h - s * 0.1, y + h);
  g.lineTo(x - h + s * 0.1, y + h);
  g.quadraticCurveTo(x - h, y + h, x - h, y + h - s * 0.1);
  g.lineTo(x - h, y - h + c); g.closePath(); g.fill();
  g.shadowBlur = 0;
  var cw = s * 0.66;
  g.fillStyle = 'rgb(' + cl(amb * 0.42 + r * 0.9) + ',' + cl(amb * 0.42 + v * 0.9) + ',' + cl(amb * 0.46 + b * 0.9) + ')';
  g.beginPath();
  if (g.roundRect) g.roundRect(x - cw / 2, y - cw / 2, cw, cw, cw * 0.14); else g.rect(x - cw / 2, y - cw / 2, cw, cw);
  g.fill();
  var dz = Math.max(1.2, s * 0.17);
  [[-cw * 0.23, -cw * 0.2, r, 0, 0], [cw * 0.23, -cw * 0.2, 0, v, 0], [0, cw * 0.24, 0, 0, b]].forEach(function (q) {
    var l = Math.max(q[2], q[3], q[4]), k = l / 255 * 0.85;
    g.fillStyle = l < 3 ? 'rgb(' + cl(amb * 1.2) + ',' + cl(amb * 1.2) + ',' + cl(amb * 1.25) + ')'
      : 'rgb(' + cl(q[2] + (255 - q[2]) * k) + ',' + cl(q[3] + (255 - q[3]) * k) + ',' + cl(q[4] + (255 - q[4]) * k) + ')';
    g.beginPath(); g.arc(x + q[0], y + q[1], dz / 2, 0, 6.2832); g.fill();
  });
}

// La geometrie est une AFFIRMATION sur ton cablage: l'ESP ne connait que num_leds.
function phys() {
  var p = parseInt($('phys').value, 10);
  return (!p || p < 1) ? N : p;
}
// La geometrie se calcule sur le nombre PHYSIQUE, jamais sur num_leds. C'est ce
// qui fige la taille des domes: changer num_leds n'est pas redessiner le ruban,
// c'est en piloter une part differente. Un vrai ruban ne retrecit pas.
function geom() {
  var rows = parseInt($('rows').value, 10), elbow = parseInt($('elbow').value, 10);
  var P = phys();
  var body = P - elbow * (rows - 1);
  var perRow = rows > 0 ? Math.floor(body / rows) : N;
  var rest = body - perRow * rows;
  if (perRow < 1) { perRow = P; rows = 1; elbow = 0; rest = 0; }
  return { rows: rows, elbow: elbow, serp: $('serp').checked, perRow: perRow, rest: rest, phys: P };
}
function xy(k, g, p, x0, xN, ys) {
  var cycle = g.perRow + g.elbow, row = Math.floor(k / cycle), off = k % cycle;
  if (row > g.rows - 1) { row = g.rows - 1; off = g.perRow - 1; }
  var ltr = !g.serp || row % 2 === 0;
  if (off < g.perRow) return [ltr ? x0 + off * p : xN - off * p, ys[row]];
  return [ltr ? xN : x0, ys[row] + (off - g.perRow + 1) * p];
}
var cv = $('strip'), gx = null;
var haloCv = document.createElement('canvas'), haloGx = null;
var HSCALE = 4;
function draw(t) {
  requestAnimationFrame(draw);
  nRaf++;
  if (!t0) t0 = t;
  if (!N) return;
  if (!gx) gx = cv.getContext('2d');
  var g = geom(), W = cv.clientWidth;
  if (!W) return;
  // Le pas doit tenir dans les DEUX dimensions. Ne borner qu'en largeur donnait
  // 917 px par dome sur 2 LED par rangee, et un canvas de 4601 px de haut: deux
  // ballons de plage. Un pixel de 12 mm ne remplit pas un moniteur.
  var m = 8, MAXH = 420;
  var rowsPitch = (g.rows - 1) * (g.elbow + 1) + 1;
  var p = Math.min((W - 2 * m) / g.perRow, (MAXH - 2 * m) / rowsPitch);
  var d = p * 1.02;
  var H = Math.round(2 * m + (rowsPitch - 1) * p + p);
  var ox = Math.max(0, (W - (g.perRow * p + 2 * m)) / 2);  // centre si la hauteur borne
  if (cv.style.height !== H + 'px') cv.style.height = H + 'px';
  var dpr = window.devicePixelRatio || 1;
  if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) {
    cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr);
  }
  gx.setTransform(dpr, 0, 0, dpr, 0, 0);
  gx.clearRect(0, 0, W, H);

  var now = t / 1000, acc = null, wsum = 0, due = [];
  while (queue.length && queue[0].t <= now) due.push(queue.shift());
  if (due.length) {
    // L'oeil integre. Echantillonner ("la derniere trame dont t <= maintenant")
    // jette une image a chaque battement entre 16 ms et 16,7 ms: une saccade a
    // 2,5 Hz que NOUS ajoutons. Et il faut moyenner en LINEAIRE: moyenner des
    // octets sRGB assombrit un stroboscope 50/50 de 188 a 128.
    if ($('integ').checked && due.length > 1) {
      acc = new Float32Array(N * BPP);
      for (var q = 0; q < due.length; q++)
        for (var z = 0; z < N * BPP; z++) acc[z] += LIN[due[q].f[z]];
      wsum = due.length;
    }
    pending = due[due.length - 1].f;
    nDisp++;
  }
  if (!pending) return;

  var bri = parseFloat($('bri').value), amb = parseInt($('amb').value, 10);
  var glow = $('glow').checked, isDome = $('pkg').value === 'dome';
  var x0 = ox + m + p / 2, xN = ox + m + (g.perRow - 1) * p + p / 2, ys = [], k;
  for (k = 0; k < g.rows; k++) ys.push(m + k * (g.elbow + 1) * p + p / 2);
  var P = g.phys, pos = [];
  for (k = 0; k < P; k++) pos.push(xy(k, g, p, x0, xN, ys));
  if (isDome) {  // le cable: la chaine est physique, elle descend vraiment les coudes
    gx.strokeStyle = 'rgb(' + cl(amb * 0.5) + ',' + cl(amb * 0.5) + ',' + cl(amb * 0.55) + ')';
    gx.lineWidth = Math.max(1.5, d * 0.26); gx.lineCap = 'round'; gx.lineJoin = 'round';
    gx.beginPath(); gx.moveTo(pos[0][0], pos[0][1]);
    for (k = 1; k < P; k++) gx.lineTo(pos[k][0], pos[k][1]);
    gx.stroke();
  }
  var col = new Uint8Array(P * 3);  // au-dela de num_leds: reste a zero
  var lim = Math.min(N, P);
  for (k = 0; k < lim * BPP; k += BPP) {
    var o = (k / BPP) * 3;
    for (var c2 = 0; c2 < 3; c2++) {
      var lin = acc ? acc[k + c2] / wsum : LIN[pending[k + c2]];
      col[o + c2] = bri >= 0.999 && !acc ? LUT[pending[k + c2]] : toSrgb(lin * bri);
    }
  }
  // Deux passes. Les entrelacer faisait peindre le halo de la LED k par-dessus
  // le dome de la LED k-1: un halo ne traverse pas du plastique opaque.
  if (isDome && glow) {
    var hw = Math.max(1, Math.round(W * dpr / HSCALE)), hh = Math.max(1, Math.round(H * dpr / HSCALE));
    if (haloCv.width !== hw || haloCv.height !== hh) { haloCv.width = hw; haloCv.height = hh; haloGx = null; }
    if (!haloGx) haloGx = haloCv.getContext('2d');
    haloGx.setTransform(dpr / HSCALE, 0, 0, dpr / HSCALE, 0, 0);
    haloGx.clearRect(0, 0, W, H);
    for (k = 0; k < P; k++) {
      if (col[k * 3] + col[k * 3 + 1] + col[k * 3 + 2] > 6)
        halo(haloGx, pos[k][0], pos[k][1], d, col[k * 3], col[k * 3 + 1], col[k * 3 + 2]);
    }
    gx.drawImage(haloCv, 0, 0, W, H);
  }
  for (k = 0; k < P; k++) {
    if (isDome) dome(gx, pos[k][0], pos[k][1], d, col[k * 3], col[k * 3 + 1], col[k * 3 + 2], amb);
    else smd(gx, pos[k][0], pos[k][1], d - 2.4, col[k * 3], col[k * 3 + 1], col[k * 3 + 2], amb, glow);
  }
}

setInterval(function () {
  var el = (performance.now() - t0) / 1000;
  if (el <= 0) return;
  set('sFps', (nEsp / el).toFixed(1));
  // Display = les vrais requestAnimationFrame. Render = les trames nouvelles
  // reellement peintes. Les confondre faisait accuser l'ESP d'une saccade que
  // mon propre rendu aurait pu creer.
  set('sDisp', (nRaf / el).toFixed(1) + ' Hz');
  $('sDisp').className = nRaf / el > 50 ? 'g' : (nRaf / el > 30 ? 'y' : 'r');
  set('sRen', (nDisp / el).toFixed(1) + ' /s');
  set('sBpf', lastLen + ' B');
  set('sEnc', lastEnc); $('sEnc').className = 'g';
  set('sThr', (bytes / el / 1024).toFixed(1) + ' kB/s');
  set('sFr', String(nFrames));
  var br = beats.length / 2;
  set('sBeat', br.toFixed(1) + ' / 10.0 /s');
  $('sBeat').className = br > 9 ? 'g' : (br > 6 ? 'y' : 'r');
  if (arrivals.length > 4) {
    var s = arrivals.slice().sort(function (a, b) { return a - b; });
    var j = s[s.length - 1] - s[0];
    set('sJit', j.toFixed(1) + ' ms');
    $('sJit').className = j < 20 ? 'g' : (j < 60 ? 'y' : 'r');
  }
  var w30 = 0;
  for (var q = 0; q < gaps.length; q++) if (gaps[q][1] > w30) w30 = gaps[q][1];
  set('sLoop', w30 + ' ms');
  $('sLoop').className = w30 < 150 ? 'g' : (w30 < 300 ? 'y' : 'r');
  var g = geom();
  var un = g.phys - N;
  set('geo', g.rows + ' x ' + g.perRow + (g.elbow ? ' + ' + g.elbow + ' per elbow' : '') +
    (g.serp && g.rows > 1 ? ' - serpentine' : '') +
    '   ' + N + ' driven / ' + g.phys + ' physical' +
    (un > 0 ? '   [' + un + ' never addressed: they hold their last latch, undefined at power-on]' : '') +
    (un < 0 ? '   [num_leds ' + N + ' exceeds ' + g.phys + ' physical]' : '') +
    (g.rest ? '   [' + g.rest + ' LED left over: this geometry does not fit ' + g.phys + ']' : ''));
  $('geo').className = (g.rest || un < 0) ? 'hint r' : 'hint';
}, 250);

try {
  var saved = JSON.parse(localStorage.getItem('vls') || '{}');
  ['rows', 'elbow', 'bri', 'po', 'amb', 'pkg', 'phys'].forEach(function (i) { if (saved[i] !== undefined) $(i).value = saved[i]; });
  ['serp', 'integ', 'glow'].forEach(function (i) { if (saved[i] !== undefined) $(i).checked = saved[i]; });
} catch (e) { /* premiere visite */ }
function persist() {
  var o = {};
  ['rows', 'elbow', 'bri', 'po', 'amb', 'pkg', 'phys'].forEach(function (i) { o[i] = $(i).value; });
  ['serp', 'integ', 'glow'].forEach(function (i) { o[i] = $(i).checked; });
  try { localStorage.setItem('vls', JSON.stringify(o)); } catch (e) { /* mode prive */ }
}
function sync() {
  set('physL', $('phys').value || '--'); set('rowsL', $('rows').value); set('elbowL', $('elbow').value);
  set('briL', Math.round(parseFloat($('bri').value) * 100) + '%');
  set('ambL', $('amb').value); set('poL', $('po').value + ' ms');
  playout = parseInt($('po').value, 10) / 1000;
  persist();
}
['serp', 'integ', 'glow', 'pkg'].forEach(function (i) { $(i).onchange = sync; });
['rows', 'elbow', 'bri', 'po', 'amb', 'phys'].forEach(function (i) { $(i).oninput = sync; });
$('po').addEventListener('change', function () { resync = true; });
sync();
requestAnimationFrame(draw);
stream();
</script></body></html>
)HTMLDOC";

void VirtualLedStrip::setup() {
#ifdef USE_HOST
  // Le banc uniquement. Sous POSIX, ecrire sur un socket dont le pair est parti
  // leve SIGPIPE et tue le binaire. Sur lwIP il n'y a pas de signaux.
  std::signal(SIGPIPE, SIG_IGN);
#endif
  // A HARDWARE, on ne fait QUE la memoire. LightState s'initialise juste apres
  // (HARDWARE - 1.0f) et touche ce tampon en restaurant l'etat sauvegarde: il
  // doit exister maintenant, pas plus tard.
  this->bpp_ = this->is_rgbw_ ? 4 : 3;
  this->buf_.assign((size_t) this->num_leds_ * this->bpp_, 0);
  this->prev_.assign((size_t) this->num_leds_ * this->bpp_, 0xFF);
  this->effect_.assign(this->num_leds_, 0);
  this->out_.reserve((size_t) this->num_leds_ * this->bpp_ + 16);
  if (this->buf_.empty()) {
    ESP_LOGE(TAG, "out of memory for %u leds", this->num_leds_);
    this->mark_failed();
  }
}

// Le socket veut que le setup() du wifi soit passe -- l'api est a AFTER_WIFI
// pour exactement cette raison. Mais notre tampon doit exister a HARDWARE. On ne
// peut pas etre aux deux endroits: on ouvre le serveur a la premiere passe de
// loop(), qui suit tous les setup() quels qu'ils soient.
void VirtualLedStrip::start_server_() {
  this->server_started_ = true;
  this->server_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->server_ == nullptr) {
    ESP_LOGE(TAG, "socket failed");
    this->mark_failed();
    return;
  }
  // Not optional. A blocking accept() parks the whole ESPHome main loop.
  if (this->server_->setblocking(false) != 0) {
    ESP_LOGE(TAG, "setblocking failed");
    this->mark_failed();
    return;
  }
  int enable = 1;
  this->server_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
  struct sockaddr_storage sa {};
  socklen_t sl = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa), this->port_);
  if (this->server_->bind(reinterpret_cast<struct sockaddr *>(&sa), sl) != 0 || this->server_->listen(4) != 0) {
    ESP_LOGE(TAG, "bind/listen failed on port %u", this->port_);
    this->server_ = nullptr;
    this->mark_failed();
    return;
  }
  this->last_beat_ = millis();
  ESP_LOGI(TAG, "listening on port %u", this->port_);
}

void VirtualLedStrip::dump_config() {
  ESP_LOGCONFIG(TAG, "Virtual LED strip:\n  LEDs: %u\n  Port: %u\n  RGBW: %s", this->num_leds_, this->port_,
                YESNO(this->is_rgbw_));
  if (this->max_refresh_us_ != 0) {
    // Le seuil est quantifie par LightState::loop(): seuls les multiples de
    // l'intervalle de boucle tombent juste. 33 ms donne 20,9 img/s, pas 30.
    const uint32_t period = ((this->max_refresh_us_ + 15999) / 16000) * 16;
    ESP_LOGCONFIG(TAG, "  Max refresh rate: %u us -> ~%.1f fps (quantized to a %u ms loop)", this->max_refresh_us_,
                  1000.0f / (float) period, period);
  }
}

void VirtualLedStrip::clear_effect_data() { std::fill(this->effect_.begin(), this->effect_.end(), 0); }

light::ESPColorView VirtualLedStrip::get_view_internal(int32_t index) const {
  auto *b = const_cast<uint8_t *>(this->buf_.data()) + (size_t) index * this->bpp_;
  auto *e = const_cast<uint8_t *>(this->effect_.data()) + index;
  uint8_t *w = this->is_rgbw_ ? b + 3 : nullptr;
  return {b, b + 1, b + 2, w, e, &this->correction_};
}

void VirtualLedStrip::write_state(light::LightState *state) {
  const uint32_t us = micros();
  if (this->max_refresh_us_ != 0 && (us - this->last_refresh_) < this->max_refresh_us_) {
    this->schedule_show();  // on retente a la passe suivante, sans perdre le changement
    return;
  }
  this->last_refresh_ = us;
  this->mark_shown_();

  if (this->stream_client_ == nullptr)
    return;
  // Une trame ecrasee avant d'etre partie est une trame que le navigateur ne
  // verra jamais: elle compte comme perdue, pas comme lissee.
  if (this->dirty_)
    this->dropped_++;
  this->dirty_ = true;
  // Horodater ICI, avant que le reseau ne puisse ajouter sa gigue.
  this->dirty_ts_ = millis();
}

void VirtualLedStrip::encode_(uint32_t ts) {
  const size_t n = this->buf_.size();
  const uint8_t bpp = this->bpp_;
  const size_t px = this->num_leds_;

  size_t runs = 0;
  for (size_t i = 0; i < n;) {
    size_t j = i;
    while (j < n && std::memcmp(&this->buf_[j], &this->buf_[i], bpp) == 0 && (j - i) / bpp < 255)
      j += bpp;
    runs++;
    i = j;
  }
  size_t changed = 0;
  for (size_t i = 0; i < n; i += bpp)
    if (std::memcmp(&this->buf_[i], &this->prev_[i], bpp) != 0)
      changed++;

  const size_t raw_len = n;
  const size_t rle_len = runs * (1 + bpp);
  const size_t delta_len = 2 + changed * (2 + bpp);

  // Le plus court POUR CETTE TRAME. Mesure sur les vrais effets: sur un
  // arc-en-ciel RLE (1199 o) et delta (955 o) coutent tous deux plus que le brut
  // (900 o); sur un twinkle le delta tombe a 13 o. Aucun codage unique ne gagne.
  uint8_t enc = ENC_RAW;
  size_t len = raw_len;
  if (delta_len < len) { enc = ENC_DELTA; len = delta_len; }
  if (rle_len < len) { enc = ENC_RLE; len = rle_len; }

  this->out_.clear();
  this->out_.push_back(0xAA);
  this->out_.push_back(enc);
  this->out_.push_back((uint8_t) ts);
  this->out_.push_back((uint8_t) (ts >> 8));
  this->out_.push_back((uint8_t) (ts >> 16));
  this->out_.push_back((uint8_t) (ts >> 24));
  this->out_.push_back((uint8_t) this->seq_);
  this->out_.push_back((uint8_t) (this->seq_ >> 8));
  this->out_.push_back((uint8_t) len);
  this->out_.push_back((uint8_t) (len >> 8));
  this->seq_++;

  if (enc == ENC_RAW) {
    this->out_.insert(this->out_.end(), this->buf_.begin(), this->buf_.end());
  } else if (enc == ENC_DELTA) {
    this->out_.push_back((uint8_t) changed);
    this->out_.push_back((uint8_t) (changed >> 8));
    for (size_t i = 0; i < px; i++) {
      const size_t o = i * bpp;
      if (std::memcmp(&this->buf_[o], &this->prev_[o], bpp) == 0)
        continue;
      this->out_.push_back((uint8_t) i);
      this->out_.push_back((uint8_t) (i >> 8));
      for (uint8_t c = 0; c < bpp; c++)
        this->out_.push_back(this->buf_[o + c]);
    }
  } else {
    for (size_t i = 0; i < n;) {
      size_t j = i;
      while (j < n && std::memcmp(&this->buf_[j], &this->buf_[i], bpp) == 0 && (j - i) / bpp < 255)
        j += bpp;
      this->out_.push_back((uint8_t) ((j - i) / bpp));
      for (uint8_t c = 0; c < bpp; c++)
        this->out_.push_back(this->buf_[i + c]);
      i = j;
    }
  }
  this->out_pos_ = 0;
}

void VirtualLedStrip::promote_() {
  if (!this->dirty_ || this->out_pos_ < this->out_.size())
    return;
  this->encode_(this->dirty_ts_);
  // prev_ n'avance que sur une trame reellement engagee sur le fil: sinon le
  // client rebatirait ses deltas contre une trame qu'il n'a jamais vue.
  this->prev_ = this->buf_;
  this->dirty_ = false;
}

bool VirtualLedStrip::flush_() {
  while (this->out_pos_ < this->out_.size()) {
    const ssize_t w = this->stream_client_->write(this->out_.data() + this->out_pos_,
                                                 this->out_.size() - this->out_pos_);
    if (w > 0) {
      this->out_pos_ += (size_t) w;
      continue;
    }
    // Une ecriture courte n'est PAS une panne ici. A 564 octets toutes les
    // 32 ms le tampon d'emission d'un ESP8266 se remplit en regime normal.
    // virtual_output ferme dans ce cas -- correct a 100 o/s, desastreux ici.
    if (w == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
      return false;
    this->drop_stream_("write failed");
    return false;
  }
  return true;
}

void VirtualLedStrip::heartbeat_(uint32_t now) {
  if (now - this->last_beat_ < HEARTBEAT_MS)
    return;
  if (this->out_pos_ < this->out_.size())
    return;  // trame en cours: on retentera, la phase n'a pas bouge
  // Phase avancee. `= now` ajouterait le reste de la boucle a chaque periode:
  // un metronome qui ne peut pas atteindre sa propre cible ne mesure rien.
  this->last_beat_ += HEARTBEAT_MS;
  if (now - this->last_beat_ >= HEARTBEAT_MS)
    this->last_beat_ = now;
  this->out_.clear();
  this->out_.push_back(0xAA);
  this->out_.push_back(MSG_BEAT);
  this->out_.push_back((uint8_t) now);
  this->out_.push_back((uint8_t) (now >> 8));
  this->out_.push_back((uint8_t) (now >> 16));
  this->out_.push_back((uint8_t) (now >> 24));
  this->out_pos_ = 0;
  this->flush_();
}

void VirtualLedStrip::accept_client_() {
  if (this->server_ == nullptr || this->pending_client_ != nullptr)
    return;
  // accept_loop_monitored, PAS accept. Sur ESP32 (bsd_sockets) la boucle ESPHome
  // dort sur un select(); un client accepte doit y etre enregistre, sinon select
  // ne reveille jamais loop() quand ses octets arrivent. read() ne voit alors
  // jamais la requete GET, renvoie -1/EWOULDBLOCK en boucle, et le client est
  // largue comme "idle" au bout de 2 s: page blanche. L'api fait pareil (ligne
  // 238 de api_server.cpp). Sur ESP8266 (lwip_tcp) il n'y a pas de select, la
  // boucle tourne en continu, et accept() simple suffisait -- d'ou un bug
  // invisible partout sauf sur ESP32.
  auto client = this->server_->accept_loop_monitored(nullptr, nullptr);
  if (client == nullptr)
    return;
  client->setblocking(false);
  this->pending_client_ = std::move(client);
  this->request_len_ = 0;
  this->nl_ = 0;
  this->pending_since_ = millis();
  this->html_sending_ = false;
}

void VirtualLedStrip::read_request_() {
  if (this->pending_client_ == nullptr)
    return;
  // socket.h est explicite: une fois que des donnees sont la, lire jusqu'a ce
  // que ca bloque. DRAINER, pas lire une fois.
  //
  // Ce n'est pas une optimisation. Fermer un socket sur lequel il reste des
  // octets RECUS non lus fait emettre un RST au lieu d'un FIN -- et le RST jette
  // le tampon d'EMISSION. Lire seulement la premiere ligne et abandonner les
  // ~400 octets d'en-tetes du navigateur tronquait la page en plein <script>:
  // erreur de syntaxe, aucun code ne tourne, et le panneau affichait ses valeurs
  // HTML en dur comme si tout allait bien. En loopback rien ne se voit: tout est
  // deja livre avant le close(). Il faut un vrai reseau pour le reproduire.
  uint8_t b[64];
  for (;;) {
    const ssize_t len = this->pending_client_->read(b, sizeof(b));
    // 0 et -1 ne veulent PAS dire la meme chose, et les confondre gele le
    // serveur pour toujours. lwip_raw_tcp_impl.cpp est explicite:
    //   rx_closed_ && rx_buf_ == nullptr  -> return 0      (le pair est parti)
    //   rx_buf_ == nullptr                -> -1/EWOULDBLOCK (patiente)
    // Un socket speculatif de Chrome se connecte et repart sans rien envoyer.
    // Traite comme "bloquerait", il occupe pending_client_ a vie: accept() cesse
    // d'accepter, fetch('/events') n'arrive jamais, le ruban reste noir.
    if (len == 0) {
      this->pending_client_->close();
      this->pending_client_ = nullptr;
      return;
    }
    if (len < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN)
        return;  // la requete est incomplete, on reprendra
      this->pending_client_->close();
      this->pending_client_ = nullptr;
      return;
    }
    // Des octets arrivent: le client est vivant. Sans ce reveil, le delai de
    // garde couperait une requete qui s'etale sur plusieurs paquets.
    this->pending_since_ = millis();
    for (ssize_t i = 0; i < len; i++) {
      const char c = (char) b[i];
      if (this->request_len_ + 1 < sizeof(this->request_) && c != '\r')
        this->request_[this->request_len_++] = c;
      if (c == '\n') {
        if (++this->nl_ >= 2) {  // ligne vide: fin des en-tetes, tout est lu
          this->request_[this->request_len_] = '\0';
          this->handle_request_();
          return;
        }
      } else if (c != '\r') {
        this->nl_ = 0;
      }
    }
  }
}

void VirtualLedStrip::handle_request_() {
  if (strstr(this->request_, "GET /events") != nullptr) {
    this->start_stream_();
    return;
  }
  // Sans ceci, /favicon.ico -- que tout navigateur demande -- recevait la page
  // entiere, et le fetch('/events') attendait derriere 16 ko inutiles.
  if (strncmp(this->request_, "GET / ", 6) != 0 && strncmp(this->request_, "GET /?", 6) != 0) {
    static const char NF[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    this->pending_client_->write(NF, sizeof(NF) - 1);
    this->pending_client_->close();
    this->pending_client_ = nullptr;
    return;
  }
  // no-store, sinon Chrome te sert une page perimee apres un flash et tu
  // cherches un bug qui n'existe plus. Le flux l'avait, la page non.
  static const char HEADERS[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                                "Cache-Control: no-store, no-cache, must-revalidate\r\n"
                                "Connection: close\r\n\r\n";
  this->pending_client_->write(HEADERS, sizeof(HEADERS) - 1);
  this->html_sending_ = true;
  this->html_pos_ = 0;
}

void VirtualLedStrip::pump_html_() {
  if (this->pending_client_ == nullptr)
    return;
  const size_t total = sizeof(HTML_PAGE) - 1;
  while (this->html_pos_ < total) {
    const size_t len = std::min(HTML_CHUNK, total - this->html_pos_);
    char chunk[HTML_CHUNK];
    progmem_memcpy(chunk, HTML_PAGE + this->html_pos_, len);
    const ssize_t w = this->pending_client_->write(chunk, len);
    if (w > 0) {
      this->html_pos_ += (size_t) w;
      continue;
    }
    if (w == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
      return;  // tampon plein: on reprendra a la passe suivante
    this->pending_client_->close();
    this->pending_client_ = nullptr;
    this->html_sending_ = false;
    return;
  }
  this->pending_client_->close();
  this->pending_client_ = nullptr;
  this->html_sending_ = false;
}

void VirtualLedStrip::start_stream_() {
  if (this->stream_client_ != nullptr) {
    this->stream_client_->close();
    this->stream_client_ = nullptr;
  }
  int enable = 1;
  // Chaque trame est sous-MSS. Nagle retiendrait chacune jusqu'a l'ACK de la
  // precedente, ce qui deformerait exactement ce que ce module mesure.
  if (this->pending_client_->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(int)) != 0)
    ESP_LOGW(TAG, "TCP_NODELAY failed (errno=%d), expect jitter", errno);
  char headers[192];
  const int len = snprintf(headers, sizeof(headers),
                           "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                           "Cache-Control: no-store\r\nX-Platform: %s\r\nConnection: close\r\n\r\n",
                           PLATFORM_NAME);
  this->pending_client_->write(headers, len);
  this->stream_client_ = std::move(this->pending_client_);

  // Un client qui arrive en cours de route n'a jamais vu prev_: la premiere
  // trame doit etre complete, sinon il rebatit un delta sur du vide.
  std::fill(this->prev_.begin(), this->prev_.end(), 0xFF);
  this->out_.clear();
  this->out_.push_back(0xAA);
  this->out_.push_back(MSG_HELLO);
  this->out_.push_back((uint8_t) this->num_leds_);
  this->out_.push_back((uint8_t) (this->num_leds_ >> 8));
  this->out_.push_back(this->bpp_);
  this->out_pos_ = 0;
  this->dirty_ = true;
  this->dirty_ts_ = millis();
  ESP_LOGI(TAG, "viewer connected");
}

void VirtualLedStrip::drop_stream_(const char *why) {
  ESP_LOGW(TAG, "stream closed: %s", why);
  if (this->stream_client_ != nullptr) {
    this->stream_client_->close();
    this->stream_client_ = nullptr;
  }
  this->out_.clear();
  this->out_pos_ = 0;
  this->dirty_ = false;
}

void VirtualLedStrip::loop() {
  if (!this->server_started_)
    this->start_server_();
  if (this->server_ == nullptr)
    return;
  const uint32_t now = millis();
  this->accept_client_();
  if (this->html_sending_) {
    this->pump_html_();
  } else {
    this->read_request_();
    // Chrome ouvre des sockets speculatifs qui n'envoient jamais rien. Sans
    // delai de garde, le premier gele pending_client_ pour toujours: accept()
    // n'accepte plus rien, fetch('/events') n'arrive jamais, et le ruban reste
    // noir pendant que la page a l'air parfaitement chargee.
    // millis() FRAIS, pas le 'now' capture en tete de loop(). read_request_ vient
    // peut-etre de mettre pending_since_ a jour a une valeur PLUS RECENTE que ce
    // 'now' fige: la soustraction uint32_t deborde alors en un nombre enorme et
    // tue le client instantanement. Sur ESP8266 read() rend tout d'un coup et
    // handle_request_ part avant d'arriver ici; sur ESP32 read() plafonne a 64,
    // la requete s'etale sur plusieurs reads, on atteint ce test avec
    // pending_since_ > now -> underflow -> "idle client dropped" en pleine requete.
    if (this->pending_client_ != nullptr && millis() - this->pending_since_ > REQUEST_TIMEOUT_MS) {
      ESP_LOGD(TAG, "idle client dropped");
      this->pending_client_->close();
      this->pending_client_ = nullptr;
    }
  }

  if (this->stream_client_ == nullptr) {
    this->dirty_ = false;
    return;
  }
  if (!this->flush_())
    return;  // socket plein ou ferme: on retentera, sans rien jeter
  // Le battement AVANT la trame. Sinon promote_() remplit out_ a chaque passe et
  // le battement ne trouve jamais le tampon libre: la donnee affame l'instrument,
  // et Beat rate s'effondre precisement quand on lit le panneau pour comprendre
  // pourquoi. Le meme avertissement est dans virtual_output; je l'y ai ecrit.
  this->heartbeat_(now);
  if (!this->flush_())
    return;
  this->promote_();
  this->flush_();
}

}  // namespace virtual_led_strip
}  // namespace esphome
