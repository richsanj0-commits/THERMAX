#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// =============================================================
// THERMAX - Cold Plate Cooling Optimization Prototype
// ESP32 web dashboard. Receives JSON telemetry from the Nano:
//   {"f1":..,"v1":..,"f2":..,"v2":..,"t1":..,"t2":..[,"p1":..,"p2":..]}
//   f = flow (L/min), v = volume (mL), t = coolant temp (C),
//   p = optional pressure (kPa, gauge) at plate inlet/outlet
// Index 1 = cold plate INLET, index 2 = cold plate OUTLET
// =============================================================

// WiFi credentials: WIFI_SSID, WIFI_PASS, AP_SSID, AP_PASS
// Copy include/secrets.example.h to include/secrets.h and fill in your own (secrets.h is not committed)
#include "secrets.h"

// Dashboard also reachable as http://thermax.local (on the router network)
const char* MDNS_NAME  = "thermax";

// Serial2 Pins: RX2 on GPIO 16 (receives safe 3.3V from Nano D8 via divider)
#define RXD2 16
#define TXD2 17

// Alarm thresholds (shared by serial log and web dashboard)
const float TEMP_CRIT_C        = 50.0;  // Either coolant temp above this -> CRITICAL
const float REVERSED_DT_C      = 1.0;   // Outlet colder than inlet by this -> CHECK SENSORS
const float FLOW_IMBALANCE_LPM = 1.0;   // Inlet/outlet flow difference -> FLOW IMBALANCE
const unsigned long LINK_TIMEOUT_MS = 3000;  // No Nano packet for this long -> link lost

// Coolant properties for heat-removal rate Q = m_dot * Cp * (Tout - Tin) (water)
const float COOLANT_DENSITY_KG_PER_L = 1.0;
const float COOLANT_CP_J_PER_KG_K    = 4186.0;

WebServer server(80);
Preferences prefs;

struct TelemetryData {
  float flow1 = 0.0;
  float vol1  = 0.0;
  float flow2 = 0.0;
  float vol2  = 0.0;
  float t1    = 0.0;      // Inlet coolant temp
  float t2    = 0.0;      // Outlet coolant temp (offset-corrected)
  float deltaT = 0.0;     // Tout - Tin (signed)
  float heatW  = 0.0;
  bool  heatValid = false;
  float p1 = 0.0;         // Inlet pressure (kPa)
  float p2 = 0.0;         // Outlet pressure (kPa)
  bool  pressureValid = false;
  float tOffset = 0.0;    // Subtracted from raw outlet temp (sensor matching)
  float rawDiffAvg = 0.0; // Running average of raw (t2 - t1), used for zeroing
  bool  rawDiffInit = false;
  const char* flowStatus    = "IDLE";
  const char* thermalStatus = "SENSOR OFFLINE";
  unsigned long lastPacketMs = 0;
  unsigned long packets      = 0;
  unsigned long badPackets   = 0;
} telemetry;

// Web Dashboard UI
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta name="theme-color" content="#07090e">
  <title>THERMAX</title>
  <style>
    :root {
      --bg: #07090e; --card: rgba(18, 24, 38, 0.8); --border: rgba(255, 255, 255, 0.08);
      --cyan: #00f2fe; --blue: #4facfe; --amber: #ff9900; --red: #ff3366; --green: #00e676;
      --text: #f0f4f8; --dim: #8a99ad;
      --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; background: radial-gradient(circle at top right, #111a2e, #07090e 70%) fixed; background-color: var(--bg); color: var(--text); min-height: 100vh; padding: 20px 16px; }
    .wrap { max-width: 1200px; margin: 0 auto; }
    header { display: flex; justify-content: space-between; align-items: center; gap: 16px; flex-wrap: wrap; padding-bottom: 20px; border-bottom: 1px solid var(--border); margin-bottom: 20px; }
    .brand { display: flex; align-items: center; gap: 14px; }
    .logo { width: 46px; height: 46px; border-radius: 12px; background: linear-gradient(135deg, var(--cyan), var(--blue)); display: flex; align-items: center; justify-content: center; font: 900 20px var(--mono); color: #07090e; }
    h1 { font-size: 26px; font-weight: 900; letter-spacing: 4px; }
    .sub { font-size: 13px; color: var(--dim); }
    .pills { display: flex; gap: 10px; flex-wrap: wrap; }
    .pill { display: flex; align-items: center; gap: 8px; padding: 6px 14px; border-radius: 20px; background: var(--card); border: 1px solid var(--border); font: 13px var(--mono); }
    .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--dim); }
    .dot.live { background: var(--green); box-shadow: 0 0 10px var(--green); animation: pulse 2s infinite; }
    .dot.wait { background: var(--amber); box-shadow: 0 0 10px var(--amber); }
    .dot.off  { background: var(--red); box-shadow: 0 0 10px var(--red); }
    @keyframes pulse { 50% { opacity: 0.35; } }

    .alerts { display: flex; flex-direction: column; gap: 10px; margin-bottom: 20px; }
    .alerts:empty { display: none; }
    .alert { padding: 12px 16px; border-radius: 12px; font-size: 14px; font-weight: 600; }
    .alert.warn { background: rgba(255, 153, 0, 0.12); border: 1px solid rgba(255, 153, 0, 0.35); color: var(--amber); }
    .alert.crit { background: rgba(255, 51, 102, 0.12); border: 1px solid rgba(255, 51, 102, 0.4); color: var(--red); }

    .title { font-size: 13px; font-weight: 700; text-transform: uppercase; letter-spacing: 1.5px; color: var(--dim); margin: 8px 0 14px; display: flex; align-items: center; gap: 10px; }
    .title::after { content: ''; flex: 1; height: 1px; background: var(--border); }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(230px, 1fr)); gap: 16px; margin-bottom: 24px; }
    .grid.charts { grid-template-columns: repeat(auto-fit, minmax(min(100%, 420px), 1fr)); }
    .card { background: var(--card); border: 1px solid var(--border); border-radius: 18px; padding: 20px; position: relative; overflow: hidden; min-width: 0; }
    .card::before { content: ''; position: absolute; top: 0; left: 0; right: 0; height: 3px; background: linear-gradient(90deg, transparent, var(--accent, var(--cyan)), transparent); }
    .card.amber { --accent: var(--amber); }
    .card.red   { --accent: var(--red); }
    .card.green { --accent: var(--green); }
    .card.blue  { --accent: var(--blue); }
    .ch { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }
    .label { font-size: 12px; color: var(--dim); font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; }
    .val { font: 800 32px var(--mono); line-height: 1.1; margin-bottom: 6px; font-variant-numeric: tabular-nums; }
    .val small { font-size: 15px; font-weight: 400; color: var(--dim); }
    .val.word { font-size: 22px; padding-top: 6px; }
    .meta { font-size: 13px; color: var(--dim); }
    .badge { display: inline-block; padding: 4px 10px; border-radius: 6px; font-size: 11px; font-weight: 700; border: 1px solid; margin-top: 8px; }
    .b-ok   { background: rgba(0, 230, 118, 0.12); color: var(--green); border-color: rgba(0, 230, 118, 0.3); }
    .b-warn { background: rgba(255, 153, 0, 0.12); color: var(--amber); border-color: rgba(255, 153, 0, 0.3); }
    .b-crit { background: rgba(255, 51, 102, 0.12); color: var(--red); border-color: rgba(255, 51, 102, 0.3); }
    .b-off  { background: rgba(138, 153, 173, 0.12); color: var(--dim); border-color: rgba(138, 153, 173, 0.3); }
    .bar { width: 100%; height: 6px; background: rgba(255, 255, 255, 0.06); border-radius: 3px; margin-top: 14px; overflow: hidden; }
    .fill { height: 100%; width: 0%; border-radius: 3px; transition: width 0.5s ease; background: var(--cyan); }
    .fill.amber { background: var(--amber); }

    .chart-head { display: flex; justify-content: space-between; align-items: center; gap: 10px; flex-wrap: wrap; margin-bottom: 12px; }
    .legend { display: flex; gap: 14px; font-size: 12px; color: var(--dim); flex-wrap: wrap; }
    .legend i { display: inline-block; width: 12px; height: 3px; border-radius: 2px; margin-right: 6px; vertical-align: middle; }
    canvas { width: 100%; height: 200px; display: block; }

    .form { display: flex; gap: 10px; flex-wrap: wrap; align-items: flex-end; margin-bottom: 16px; }
    .field { display: flex; flex-direction: column; gap: 6px; }
    .field span { font-size: 11px; color: var(--dim); text-transform: uppercase; letter-spacing: 0.5px; }
    input { font: 14px var(--mono); color: var(--text); background: rgba(0, 0, 0, 0.3); border: 1px solid var(--border); border-radius: 10px; padding: 9px 12px; width: 170px; }
    input:focus { outline: none; border-color: var(--cyan); }
    button { font: 600 13px system-ui, sans-serif; color: var(--text); background: rgba(255, 255, 255, 0.06); border: 1px solid var(--border); border-radius: 10px; padding: 9px 16px; cursor: pointer; }
    button:hover { background: rgba(255, 255, 255, 0.12); }
    button.primary { background: linear-gradient(135deg, var(--cyan), var(--blue)); color: #07090e; border: none; }
    button:disabled { opacity: 0.4; cursor: not-allowed; }
    .table-wrap { overflow-x: auto; margin-bottom: 16px; }
    table { width: 100%; border-collapse: collapse; font: 13px var(--mono); white-space: nowrap; }
    th { text-align: right; font: 600 11px system-ui, sans-serif; color: var(--dim); text-transform: uppercase; padding: 8px 10px; border-bottom: 1px solid var(--border); }
    td { text-align: right; padding: 8px 10px; border-bottom: 1px solid rgba(255, 255, 255, 0.04); }
    th:nth-child(2), td:nth-child(2) { text-align: left; }
    .empty { text-align: center; color: var(--dim); padding: 18px; font-family: system-ui, sans-serif; }

    .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; }
    .stat .k { font-size: 11px; color: var(--dim); text-transform: uppercase; letter-spacing: 0.5px; }
    .stat .v { font: 600 15px var(--mono); margin-top: 4px; }
    .actions { display: flex; gap: 10px; flex-wrap: wrap; margin-top: 16px; }
    footer { text-align: center; font-size: 12px; color: var(--dim); margin-top: 8px; line-height: 1.6; }
  </style>
</head>
<body>
  <div class="wrap">
    <header>
      <div class="brand">
        <div class="logo">TX</div>
        <div>
          <h1>THERMAX</h1>
          <div class="sub">Cold Plate Cooling Optimization · Prototype Test Rig</div>
        </div>
      </div>
      <div class="pills">
        <div class="pill"><div class="dot" id="live-dot"></div><span id="conn-status">CONNECTING…</span></div>
        <div class="pill"><span id="ip-address">IP: --</span></div>
      </div>
    </header>

    <div class="alerts" id="alerts"></div>

    <div class="title">Coolant Flow (SEN-HZ21WA)</div>
    <div class="grid">
      <div class="card">
        <div class="ch"><span class="label">Inlet Flow Rate</span><span>💧</span></div>
        <div class="val" id="val-flow1">-- <small>L/min</small></div>
        <div class="meta" id="sub-vol1">Total volume: -- mL</div>
        <div class="bar"><div class="fill" id="bar-flow1"></div></div>
      </div>
      <div class="card">
        <div class="ch"><span class="label">Outlet Flow Rate</span><span>🔄</span></div>
        <div class="val" id="val-flow2">-- <small>L/min</small></div>
        <div class="meta" id="sub-vol2">Total volume: -- mL</div>
        <div class="bar"><div class="fill" id="bar-flow2"></div></div>
      </div>
      <div class="card">
        <div class="ch"><span class="label">Flow Balance</span><span>🛡️</span></div>
        <div class="val word" id="val-flowstatus">--</div>
        <div class="meta" id="sub-flowdiff">Inlet − outlet: -- L/min</div>
        <span class="badge b-off" id="badge-flow">NO DATA</span>
      </div>
    </div>

    <div class="title">Thermal Performance (MAX6675 K-Type)</div>
    <div class="grid">
      <div class="card amber">
        <div class="ch"><span class="label">Coolant Inlet T<sub>in</sub></span><span>🌡️</span></div>
        <div class="val" id="val-t1">-- <small>°C</small></div>
        <div class="meta" id="sub-t1">--</div>
        <div class="bar"><div class="fill amber" id="bar-t1"></div></div>
      </div>
      <div class="card amber">
        <div class="ch"><span class="label">Coolant Outlet T<sub>out</sub></span><span>🌡️</span></div>
        <div class="val" id="val-t2">-- <small>°C</small></div>
        <div class="meta" id="sub-t2">--</div>
        <div class="bar"><div class="fill amber" id="bar-t2"></div></div>
      </div>
      <div class="card red">
        <div class="ch"><span class="label">Coolant Temp Rise ΔT</span><span>📈</span></div>
        <div class="val" id="val-deltat">-- <small>°C</small></div>
        <div class="meta" id="sub-peak">Session peak: -- °C</div>
        <span class="badge b-off" id="badge-thermal">NO DATA</span>
      </div>
      <div class="card green">
        <div class="ch"><span class="label">Heat Removal Rate Q̇</span><span>🔥</span></div>
        <div class="val" id="val-heat">-- <small>W</small></div>
        <div class="meta">Q̇ = ṁ · C<sub>p</sub> · (T<sub>out</sub> − T<sub>in</sub>)</div>
        <div class="meta" id="sub-eff">Enter heater power below for efficiency</div>
      </div>
    </div>

    <div class="title">Hydraulics &amp; Test Condition</div>
    <div class="grid">
      <div class="card blue">
        <div class="ch"><span class="label">Pressure Drop ΔP</span><span>⏬</span></div>
        <div class="val" id="val-dp">-- <small>kPa</small></div>
        <div class="meta" id="sub-p">Pressure sensors not connected</div>
      </div>
      <div class="card green">
        <div class="ch"><span class="label">Steady State</span><span>⏱️</span></div>
        <div class="val word" id="val-steady">--</div>
        <div class="meta" id="sub-steady">Needs 30 s of data</div>
        <span class="badge b-off" id="badge-steady">WAITING</span>
      </div>
    </div>

    <div class="title">Live Trends (last 2 minutes)</div>
    <div class="grid charts">
      <div class="card amber">
        <div class="chart-head">
          <span class="label">Coolant Temperature (°C)</span>
          <div class="legend"><span><i style="background:#ff9900"></i>T<sub>in</sub></span><span><i style="background:#ff3366"></i>T<sub>out</sub></span></div>
        </div>
        <canvas id="chart-temp"></canvas>
      </div>
      <div class="card">
        <div class="chart-head">
          <span class="label">Flow (L/min)</span>
          <div class="legend"><span><i style="background:#00f2fe"></i>Inlet</span><span><i style="background:#4facfe"></i>Outlet</span></div>
        </div>
        <canvas id="chart-flow"></canvas>
      </div>
    </div>

    <div class="title">Test Runs</div>
    <div class="card" style="margin-bottom:24px;">
      <div class="form">
        <label class="field"><span>Run label</span><input id="in-label" placeholder="e.g. Valve 50%"></label>
        <label class="field"><span>Heater power (W)</span><input id="in-heater" type="number" min="0" step="1" placeholder="optional"></label>
        <button class="primary" id="btn-record">● Record point (30 s avg)</button>
      </div>
      <div class="table-wrap">
        <table>
          <thead><tr>
            <th>#</th><th>Label</th><th>Flow in</th><th>Flow out</th><th>T<sub>in</sub></th><th>T<sub>out</sub></th>
            <th>ΔT</th><th>Q̇ (W)</th><th>ΔP (kPa)</th><th>Heater (W)</th><th>Q̇/Heater</th><th>Steady</th>
          </tr></thead>
          <tbody id="runs-body"></tbody>
        </table>
      </div>
      <div class="chart-head">
        <span class="label">Performance vs Flow Rate</span>
        <div class="legend"><span><i style="background:#00e676"></i>Q̇ (W, left)</span><span><i style="background:#4facfe"></i>ΔP (kPa, right)</span></div>
      </div>
      <canvas id="chart-perf"></canvas>
      <div class="actions">
        <button id="btn-runs-csv">⬇ Export results CSV</button>
        <button id="btn-runs-undo">Delete last point</button>
        <button id="btn-runs-clear">Clear all points</button>
      </div>
    </div>

    <div class="title">System &amp; Calibration</div>
    <div class="card" style="margin-bottom:20px;">
      <div class="stats">
        <div class="stat"><div class="k">Nano link</div><div class="v" id="s-link">--</div></div>
        <div class="stat"><div class="k">Last packet</div><div class="v" id="s-age">--</div></div>
        <div class="stat"><div class="k">Packets / errors</div><div class="v" id="s-packets">--</div></div>
        <div class="stat"><div class="k">WiFi</div><div class="v" id="s-wifi">--</div></div>
        <div class="stat"><div class="k">ESP32 uptime</div><div class="v" id="s-uptime">--</div></div>
        <div class="stat"><div class="k">T<sub>out</sub> offset</div><div class="v" id="s-offset">--</div></div>
        <div class="stat"><div class="k">Logged samples</div><div class="v" id="s-samples">0</div></div>
      </div>
      <div class="actions">
        <button id="btn-zero">Zero ΔT (heater OFF)</button>
        <button id="btn-zero-clear">Clear ΔT offset</button>
        <button id="btn-csv">⬇ Download raw log CSV</button>
        <button id="btn-clear">Clear live history</button>
      </div>
    </div>

    <footer>
      Test section: aluminium plate 235 × 170 × 10 mm, Ø3 mm coolant passages · Coolant: water (C<sub>p</sub> 4186 J/kg·K)<br>
      Raw log stays in this browser tab; recorded test points are kept in this browser until cleared.
    </footer>
  </div>

  <script>
    const $ = id => document.getElementById(id);
    const VIEW_POINTS = 120;      // 2 minutes on screen at 1 sample/s
    const HISTORY_MAX = 7200;     // 2 hours kept for CSV export
    const AVG_WINDOW = 30;        // Samples averaged per recorded test point
    const STEADY_WINDOW = 60;     // Samples used for steady-state slope
    const STEADY_SLOPE = 0.15;    // °C/min below which temps are considered steady
    const RUNS_KEY = 'thermax_runs';
    const history = [];
    let runs = [];
    let latest = null, lastPacketCount = -1, failCount = 0, peakTemp = null, steadyNow = false;

    try { runs = JSON.parse(localStorage.getItem(RUNS_KEY) || '[]'); } catch (e) { runs = []; }
    function saveRuns() { try { localStorage.setItem(RUNS_KEY, JSON.stringify(runs)); } catch (e) {} }

    const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    const fx = (v, n) => v === null || v === undefined || !isFinite(v) ? '--' : v.toFixed(n);
    const csvVal = v => v === null || v === undefined ? '' : v;
    function mean(arr) { const a = arr.filter(v => v !== null && v !== undefined); return a.length ? a.reduce((s, v) => s + v, 0) / a.length : null; }
    function slopePerMin(samples, key) {
      const p = samples.filter(s => s[key] !== null);
      if (p.length < 10) return null;
      const t0 = p[0].ts, xs = p.map(s => (s.ts - t0) / 60000), ys = p.map(s => s[key]);
      const mx = mean(xs), my = mean(ys);
      let num = 0, den = 0;
      xs.forEach((x, i) => { num += (x - mx) * (ys[i] - my); den += (x - mx) * (x - mx); });
      return den ? num / den : null;
    }
    function setBadge(el, cls, text) { el.className = 'badge ' + cls; el.textContent = text; }
    function setConn(state, text) { $('live-dot').className = 'dot ' + state; $('conn-status').textContent = text; }
    function fmtUptime(s) {
      const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), x = s % 60;
      return (h ? h + 'h ' : '') + (h || m ? m + 'm ' : '') + x + 's';
    }
    function setTemp(key, t) {
      if (t > 0) {
        $('val-' + key).innerHTML = t.toFixed(1) + ' <small>°C</small>';
        $('sub-' + key).textContent = (t * 9 / 5 + 32).toFixed(1) + ' °F';
        $('bar-' + key).style.width = Math.min(t / 60 * 100, 100) + '%';
      } else {
        $('val-' + key).innerHTML = '-- <small>°C</small>';
        $('sub-' + key).textContent = 'Sensor offline';
        $('bar-' + key).style.width = '0%';
      }
    }

    // ---------- Charts ----------
    function prepCanvas(id) {
      const c = $(id), dpr = window.devicePixelRatio || 1, w = c.clientWidth, h = c.clientHeight;
      if (c.width !== w * dpr || c.height !== h * dpr) { c.width = w * dpr; c.height = h * dpr; }
      const g = c.getContext('2d');
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, w, h);
      g.font = '11px ui-monospace, Consolas, monospace';
      g.lineWidth = 1;
      return { g, w, h };
    }
    function range(values, minSpan, floorZero) {
      let lo = Math.min(...values), hi = Math.max(...values);
      if (!values.length) { lo = 0; hi = minSpan; }
      if (hi - lo < minSpan) { const mid = (hi + lo) / 2; lo = mid - minSpan / 2; hi = mid + minSpan / 2; }
      const pad = (hi - lo) * 0.1; lo -= pad; hi += pad;
      if (floorZero && lo < 0) { hi -= lo; lo = 0; }
      return [lo, hi];
    }
    function yAxis(g, x, align, T, ph, lo, hi, color, grid, w, R) {
      g.fillStyle = color; g.textAlign = align; g.textBaseline = 'middle'; g.strokeStyle = 'rgba(255,255,255,0.06)';
      for (let i = 0; i <= 4; i++) {
        const y = T + ph * i / 4;
        if (grid) { g.beginPath(); g.moveTo(44, y); g.lineTo(w - R, y); g.stroke(); }
        g.fillText((hi - (hi - lo) * i / 4).toFixed(1), x, y);
      }
    }

    function drawLineChart(id, series, minSpan, floorZero) {
      const { g, w, h } = prepCanvas(id);
      const pts = history.slice(-VIEW_POINTS);
      const vals = [];
      series.forEach(s => pts.forEach(p => { if (p[s.key] !== null) vals.push(p[s.key]); }));
      const [lo, hi] = range(vals, minSpan, floorZero);
      const L = 44, R = 8, T = 8, B = 8, pw = w - L - R, ph = h - T - B;
      yAxis(g, L - 6, 'right', T, ph, lo, hi, '#8a99ad', true, w, R);
      if (!pts.length) { g.textAlign = 'center'; g.fillText('Waiting for data…', L + pw / 2, T + ph / 2); return; }
      series.forEach(s => {
        g.strokeStyle = s.color; g.lineWidth = 2; g.lineJoin = 'round'; g.beginPath();
        let drawing = false;
        pts.forEach((p, i) => {
          const v = p[s.key];
          if (v === null) { drawing = false; return; }
          const x = L + pw * (i + VIEW_POINTS - pts.length) / (VIEW_POINTS - 1);
          const y = T + ph * (hi - v) / (hi - lo);
          if (drawing) g.lineTo(x, y); else { g.moveTo(x, y); drawing = true; }
        });
        g.stroke();
      });
    }

    function drawPerfChart() {
      const { g, w, h } = prepCanvas('chart-perf');
      const L = 44, R = 44, T = 8, B = 24, pw = w - L - R, ph = h - T - B;
      const pts = runs.map(r => ({ x: mean([r.f1, r.f2]), q: r.q, dp: r.dp })).filter(p => p.x !== null).sort((a, b) => a.x - b.x);
      const [xlo, xhi] = range(pts.map(p => p.x), 0.5, true);
      const [qlo, qhi] = range(pts.filter(p => p.q !== null).map(p => p.q), 10, false);
      const dps = pts.filter(p => p.dp !== null);
      const [plo, phi] = range(dps.map(p => p.dp), 1, true);
      yAxis(g, L - 6, 'right', T, ph, qlo, qhi, '#00e676', true, w, R);
      if (dps.length) yAxis(g, w - R + 6, 'left', T, ph, plo, phi, '#4facfe', false, w, R);
      g.fillStyle = '#8a99ad'; g.textAlign = 'center'; g.textBaseline = 'top';
      for (let i = 0; i <= 4; i++) g.fillText((xlo + (xhi - xlo) * i / 4).toFixed(2), L + pw * i / 4, T + ph + 6);
      if (!pts.length) { g.textBaseline = 'middle'; g.fillText('Record test points at different flow rates to build this curve', L + pw / 2, T + ph / 2); return; }
      const X = x => L + pw * (x - xlo) / (xhi - xlo);
      [['q', '#00e676', qlo, qhi], ['dp', '#4facfe', plo, phi]].forEach(([key, color, lo, hi]) => {
        const s = pts.filter(p => p[key] !== null);
        if (!s.length) return;
        const Y = v => T + ph * (hi - v) / (hi - lo);
        g.strokeStyle = color; g.fillStyle = color; g.lineWidth = 1.5; g.beginPath();
        s.forEach((p, i) => i ? g.lineTo(X(p.x), Y(p[key])) : g.moveTo(X(p.x), Y(p[key])));
        g.stroke();
        s.forEach(p => { g.beginPath(); g.arc(X(p.x), Y(p[key]), 4, 0, Math.PI * 2); g.fill(); });
      });
    }

    function drawCharts() {
      drawLineChart('chart-temp', [{ key: 't1', color: '#ff9900' }, { key: 't2', color: '#ff3366' }], 2, false);
      drawLineChart('chart-flow', [{ key: 'f1', color: '#00f2fe' }, { key: 'f2', color: '#4facfe' }], 1, true);
    }

    // ---------- Test runs ----------
    function heaterPower() { const v = parseFloat($('in-heater').value); return isFinite(v) && v > 0 ? v : null; }

    function renderRuns() {
      $('runs-body').innerHTML = runs.length ? runs.map((r, i) => '<tr>' +
        '<td>' + (i + 1) + '</td><td>' + esc(r.label) + '</td>' +
        '<td>' + fx(r.f1, 2) + '</td><td>' + fx(r.f2, 2) + '</td>' +
        '<td>' + fx(r.tin, 1) + '</td><td>' + fx(r.tout, 1) + '</td><td>' + fx(r.dt, 2) + '</td>' +
        '<td>' + fx(r.q, 0) + '</td><td>' + fx(r.dp, 1) + '</td><td>' + fx(r.heater, 0) + '</td>' +
        '<td>' + (r.q !== null && r.heater ? (r.q / r.heater * 100).toFixed(0) + '%' : '--') + '</td>' +
        '<td>' + (r.steady ? '✔' : '✖') + '</td></tr>').join('')
        : '<tr><td colspan="12" class="empty">No test points yet. Set the flow, wait for STEADY, then record.</td></tr>';
      drawPerfChart();
    }

    $('btn-record').onclick = () => {
      const win = history.slice(-AVG_WINDOW);
      if (win.length < 5) { alert('Not enough live data yet. Wait for the Nano to send readings.'); return; }
      if (!steadyNow && !confirm('Temperatures are still changing. Record this point anyway?')) return;
      const tin = mean(win.map(s => s.t1)), tout = mean(win.map(s => s.t2));
      runs.push({
        ts: Date.now(),
        label: $('in-label').value.trim() || 'Run ' + (runs.length + 1),
        heater: heaterPower(),
        f1: mean(win.map(s => s.f1)), f2: mean(win.map(s => s.f2)),
        tin, tout, dt: tin !== null && tout !== null ? tout - tin : null,
        q: mean(win.map(s => s.heat)), dp: mean(win.map(s => s.dp)),
        samples: win.length, steady: steadyNow
      });
      saveRuns();
      renderRuns();
      $('in-label').value = '';
    };
    $('btn-runs-undo').onclick = () => { if (runs.length && confirm('Delete the last test point?')) { runs.pop(); saveRuns(); renderRuns(); } };
    $('btn-runs-clear').onclick = () => { if (runs.length && confirm('Delete ALL recorded test points?')) { runs = []; saveRuns(); renderRuns(); } };

    function download(name, rows) {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([rows.join('\n')], { type: 'text/csv' }));
      a.download = name + '_' + new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-') + '.csv';
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    }
    $('btn-runs-csv').onclick = () => {
      const rows = ['run,label,time,flow_in_lpm,flow_out_lpm,t_in_c,t_out_c,delta_t_c,heat_removal_w,pressure_drop_kpa,heater_w,q_over_heater_pct,steady,samples'];
      runs.forEach((r, i) => rows.push([i + 1, '"' + r.label.replace(/"/g, '""') + '"', new Date(r.ts).toISOString(),
        csvVal(r.f1), csvVal(r.f2), csvVal(r.tin), csvVal(r.tout), csvVal(r.dt), csvVal(r.q), csvVal(r.dp), csvVal(r.heater),
        r.q !== null && r.heater ? (r.q / r.heater * 100).toFixed(1) : '', r.steady ? 1 : 0, r.samples].join(',')));
      download('thermax_results', rows);
    };
    $('btn-csv').onclick = () => {
      const rows = ['time,flow_in_lpm,flow_out_lpm,vol_in_ml,vol_out_ml,t_in_c,t_out_c,delta_t_c,heat_removal_w,p_in_kpa,p_out_kpa,pressure_drop_kpa'];
      history.forEach(p => rows.push([new Date(p.ts).toISOString(), p.f1, p.f2, p.v1, p.v2,
        csvVal(p.t1), csvVal(p.t2), csvVal(p.dt), csvVal(p.heat), csvVal(p.p1), csvVal(p.p2), csvVal(p.dp)].join(',')));
      download('thermax_raw_log', rows);
    };
    $('btn-clear').onclick = () => { history.length = 0; peakTemp = null; $('s-samples').textContent = 0; drawCharts(); };

    async function zero(clear) {
      if (!clear && !confirm('Zero the inlet/outlet sensor difference?\n\nOnly do this with the HEATER OFF and coolant flowing, so both sensors read the same water temperature.')) return;
      try {
        const res = await fetch('/api/zero' + (clear ? '?clear=1' : ''), { method: 'POST' });
        const msg = await res.text();
        if (!res.ok) alert(msg);
      } catch (e) { alert('Could not reach the ESP32.'); }
    }
    $('btn-zero').onclick = () => zero(false);
    $('btn-zero-clear').onclick = () => zero(true);

    // ---------- Live data ----------
    function render(d) {
      latest = d;
      const linkOk = d.link_ok;
      if (linkOk) setConn('live', 'LIVE TELEMETRY');
      else setConn('wait', d.packets === 0 ? 'WAITING FOR NANO' : 'NANO LINK LOST');
      $('ip-address').textContent = d.wifi ? 'IP: ' + d.sta_ip : 'Hotspot: ' + d.ap_ip;

      // Flow
      $('val-flow1').innerHTML = d.f1.toFixed(2) + ' <small>L/min</small>';
      $('val-flow2').innerHTML = d.f2.toFixed(2) + ' <small>L/min</small>';
      $('sub-vol1').textContent = 'Total volume: ' + d.v1.toFixed(0) + ' mL';
      $('sub-vol2').textContent = 'Total volume: ' + d.v2.toFixed(0) + ' mL';
      $('bar-flow1').style.width = Math.min(d.f1 / 10 * 100, 100) + '%';
      $('bar-flow2').style.width = Math.min(d.f2 / 10 * 100, 100) + '%';
      $('val-flowstatus').textContent = d.flow_status;
      $('sub-flowdiff').textContent = 'Inlet − outlet: ' + (d.f1 - d.f2).toFixed(2) + ' L/min';
      const bFlow = $('badge-flow');
      if (!linkOk) setBadge(bFlow, 'b-off', 'NO DATA');
      else if (d.flow_status === 'FLOW IMBALANCE') setBadge(bFlow, 'b-warn', 'CHECK FOR LEAK / BLOCKAGE');
      else if (d.flow_status === 'ACTIVE') setBadge(bFlow, 'b-ok', 'CONSISTENT');
      else setBadge(bFlow, 'b-off', 'PUMP OFF');

      // Thermal
      setTemp('t1', d.t1);
      setTemp('t2', d.t2);
      const both = d.t1 > 0 && d.t2 > 0;
      $('val-deltat').innerHTML = (both ? d.delta_t.toFixed(2) : '--') + ' <small>°C</small>';
      [d.t1, d.t2].forEach(t => { if (t > 0 && (peakTemp === null || t > peakTemp)) peakTemp = t; });
      $('sub-peak').textContent = 'Session peak: ' + fx(peakTemp, 1) + ' °C';
      const bTherm = $('badge-thermal');
      if (d.thermal_status === 'CRITICAL') setBadge(bTherm, 'b-crit', 'HIGH TEMP ALERT');
      else if (d.thermal_status === 'CHECK SENSORS') setBadge(bTherm, 'b-warn', 'T_out < T_in: CHECK SENSORS');
      else if (d.thermal_status === 'NORMAL') setBadge(bTherm, 'b-ok', 'NORMAL');
      else setBadge(bTherm, 'b-off', 'SENSOR OFFLINE');
      $('val-heat').innerHTML = fx(d.heat_w, 0) + ' <small>W</small>';
      const hp = heaterPower();
      $('sub-eff').textContent = hp && d.heat_w !== null
        ? 'Captured: ' + (d.heat_w / hp * 100).toFixed(0) + '% of ' + hp + ' W heater'
        : 'Enter heater power below for efficiency';

      // Pressure
      if (d.dp !== null) {
        $('val-dp').innerHTML = d.dp.toFixed(1) + ' <small>kPa</small>';
        $('sub-p').textContent = 'P_in ' + d.p1.toFixed(1) + ' · P_out ' + d.p2.toFixed(1) + ' kPa';
      } else {
        $('val-dp').innerHTML = '-- <small>kPa</small>';
        $('sub-p').textContent = 'Pressure sensors not connected';
      }

      // System
      $('s-link').textContent = linkOk ? 'OK' : (d.packets === 0 ? 'Waiting' : 'Lost');
      $('s-age').textContent = d.packets === 0 ? '--' : (d.age_ms / 1000).toFixed(1) + ' s ago';
      $('s-packets').textContent = d.packets + ' / ' + d.errors;
      $('s-wifi').textContent = d.wifi ? d.rssi + ' dBm' : 'Hotspot only';
      $('s-uptime').textContent = fmtUptime(d.uptime);
      $('s-offset').textContent = (d.t_offset >= 0 ? '+' : '') + d.t_offset.toFixed(2) + ' °C';

      // History: one sample per new Nano packet
      if (linkOk && d.packets !== lastPacketCount) {
        history.push({
          ts: Date.now(), f1: d.f1, f2: d.f2, v1: d.v1, v2: d.v2,
          t1: d.t1 > 0 ? d.t1 : null, t2: d.t2 > 0 ? d.t2 : null,
          dt: both ? d.delta_t : null, heat: d.heat_w, p1: d.p1, p2: d.p2, dp: d.dp
        });
        if (history.length > HISTORY_MAX) history.shift();
      }
      lastPacketCount = d.packets;
      $('s-samples').textContent = history.length;

      // Steady state: both coolant temps changing slower than STEADY_SLOPE
      const win = history.slice(-STEADY_WINDOW);
      const sIn = slopePerMin(win, 't1'), sOut = slopePerMin(win, 't2');
      const bSteady = $('badge-steady');
      if (!linkOk || win.length < AVG_WINDOW || sIn === null || sOut === null) {
        steadyNow = false;
        $('val-steady').textContent = '--';
        $('sub-steady').textContent = 'Needs ' + AVG_WINDOW + ' s of temperature data';
        setBadge(bSteady, 'b-off', 'WAITING');
      } else {
        const worst = Math.max(Math.abs(sIn), Math.abs(sOut));
        steadyNow = worst < STEADY_SLOPE;
        $('val-steady').textContent = steadyNow ? 'STEADY' : 'SETTLING';
        $('sub-steady').textContent = 'T drift: ' + worst.toFixed(2) + ' °C/min (limit ' + STEADY_SLOPE + ')';
        setBadge(bSteady, steadyNow ? 'b-ok' : 'b-warn', steadyNow ? 'READY TO RECORD' : 'WAIT');
      }

      // Alerts
      const alerts = [];
      if (!linkOk) alerts.push(['warn', d.packets === 0
        ? 'No data from the Nano yet. Check the Nano D8 → divider → ESP32 GPIO16 wire and the shared GND.'
        : 'Nano stopped sending ' + Math.round(d.age_ms / 1000) + ' s ago. Showing the last values received.']);
      if (d.thermal_status === 'CRITICAL') alerts.push(['crit', 'Coolant temperature above ' + d.temp_crit + ' °C. Reduce heater power.']);
      if (linkOk && d.flow_status === 'FLOW IMBALANCE') alerts.push(['warn', 'Inlet and outlet flow differ by more than ' + d.flow_imbalance + ' L/min. Possible leak in the plate passages or a blockage.']);
      if (linkOk && d.thermal_status === 'CHECK SENSORS') alerts.push(['warn', 'Outlet is colder than inlet. Thermocouples may be swapped, or zero ΔT with the heater off.']);
      $('alerts').innerHTML = alerts.map(a => '<div class="alert ' + a[0] + '">' + a[1] + '</div>').join('');

      drawCharts();
    }

    async function update() {
      const ctl = new AbortController();
      const timer = setTimeout(() => ctl.abort(), 2500);
      try {
        const res = await fetch('/api/data', { signal: ctl.signal, cache: 'no-store' });
        render(await res.json());
        failCount = 0;
      } catch (e) {
        if (++failCount >= 2) setConn('off', 'ESP32 OFFLINE');
      } finally {
        clearTimeout(timer);
      }
    }

    window.addEventListener('resize', () => { drawCharts(); drawPerfChart(); });
    renderRuns();
    drawCharts();
    update();
    setInterval(update, 1000);
  </script>
</body>
</html>
)rawliteral";

void handleApiData() {
  unsigned long now = millis();
  unsigned long ageMs = telemetry.packets ? now - telemetry.lastPacketMs : 0;
  bool linkOk = telemetry.packets && ageMs < LINK_TIMEOUT_MS;
  bool wifiOk = WiFi.status() == WL_CONNECTED;

  char heatStr[16], p1Str[16], p2Str[16], dpStr[16];
  if (telemetry.heatValid) snprintf(heatStr, sizeof(heatStr), "%.1f", telemetry.heatW);
  else strcpy(heatStr, "null");
  if (telemetry.pressureValid) {
    snprintf(p1Str, sizeof(p1Str), "%.2f", telemetry.p1);
    snprintf(p2Str, sizeof(p2Str), "%.2f", telemetry.p2);
    snprintf(dpStr, sizeof(dpStr), "%.2f", telemetry.p1 - telemetry.p2);
  } else {
    strcpy(p1Str, "null"); strcpy(p2Str, "null"); strcpy(dpStr, "null");
  }

  char json[768];
  snprintf(json, sizeof(json),
    "{\"sta_ip\":\"%s\",\"ap_ip\":\"%s\",\"wifi\":%s,\"rssi\":%d,\"uptime\":%lu,"
    "\"link_ok\":%s,\"age_ms\":%lu,\"packets\":%lu,\"errors\":%lu,"
    "\"f1\":%.2f,\"v1\":%.0f,\"f2\":%.2f,\"v2\":%.0f,\"flow_status\":\"%s\","
    "\"t1\":%.2f,\"t2\":%.2f,\"delta_t\":%.2f,\"t_offset\":%.2f,\"thermal_status\":\"%s\",\"heat_w\":%s,"
    "\"p1\":%s,\"p2\":%s,\"dp\":%s,"
    "\"temp_crit\":%.0f,\"flow_imbalance\":%.1f}",
    WiFi.localIP().toString().c_str(), WiFi.softAPIP().toString().c_str(),
    wifiOk ? "true" : "false", wifiOk ? WiFi.RSSI() : 0, now / 1000,
    linkOk ? "true" : "false", ageMs, telemetry.packets, telemetry.badPackets,
    telemetry.flow1, telemetry.vol1, telemetry.flow2, telemetry.vol2, telemetry.flowStatus,
    telemetry.t1, telemetry.t2, telemetry.deltaT, telemetry.tOffset, telemetry.thermalStatus, heatStr,
    p1Str, p2Str, dpStr,
    TEMP_CRIT_C, FLOW_IMBALANCE_LPM);

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// POST /api/zero        -> match outlet sensor to inlet (heater off, coolant flowing)
// POST /api/zero?clear=1 -> remove the offset
void handleZero() {
  if (server.hasArg("clear")) {
    telemetry.tOffset = 0.0;
  } else {
    bool linkOk = telemetry.packets && millis() - telemetry.lastPacketMs < LINK_TIMEOUT_MS;
    if (!linkOk || !telemetry.rawDiffInit || telemetry.t1 <= 0.0 || telemetry.t2 <= 0.0) {
      server.send(409, "text/plain", "Both thermocouples must be reading live data before zeroing.");
      return;
    }
    telemetry.tOffset = telemetry.rawDiffAvg;
  }
  prefs.putFloat("tOffset", telemetry.tOffset);
  Serial.printf("[CAL] Outlet temperature offset set to %.2f C\n", telemetry.tOffset);
  server.send(200, "text/plain", "OK");
}

// Reads the number after "key": up to the next ',' or '}'
bool extractField(const String& packet, const char* key, float& out) {
  String pattern = String("\"") + key + "\":";
  int start = packet.indexOf(pattern);
  if (start == -1) return false;
  start += pattern.length();
  int end = packet.indexOf(',', start);
  if (end == -1) end = packet.indexOf('}', start);
  if (end == -1) return false;
  out = packet.substring(start, end).toFloat();
  return true;
}

void parseNanoPacket(String packet) {
  packet.trim();
  if (packet.length() == 0) return;
  if (!packet.startsWith("{") || !packet.endsWith("}")) {
    telemetry.badPackets++;
    return;
  }

  float f1, v1, f2, v2, t1, t2Raw, p1, p2;
  if (!(extractField(packet, "f1", f1) && extractField(packet, "v1", v1) &&
        extractField(packet, "f2", f2) && extractField(packet, "v2", v2) &&
        extractField(packet, "t1", t1) && extractField(packet, "t2", t2Raw))) {
    telemetry.badPackets++;
    return;
  }

  telemetry.flow1 = f1; telemetry.vol1 = v1;
  telemetry.flow2 = f2; telemetry.vol2 = v2;
  telemetry.t1 = t1;
  telemetry.lastPacketMs = millis();
  telemetry.packets++;

  // Optional pressure sensors (not every build has them)
  telemetry.pressureValid = extractField(packet, "p1", p1) && extractField(packet, "p2", p2);
  if (telemetry.pressureValid) { telemetry.p1 = p1; telemetry.p2 = p2; }

  // Nano sends 0.0 for a disconnected thermocouple
  bool tempsValid = t1 > 0.0 && t2Raw > 0.0;
  if (tempsValid) {
    float rawDiff = t2Raw - t1;
    telemetry.rawDiffAvg = telemetry.rawDiffInit ? telemetry.rawDiffAvg * 0.9 + rawDiff * 0.1 : rawDiff;
    telemetry.rawDiffInit = true;
    telemetry.t2 = t2Raw - telemetry.tOffset;
    telemetry.deltaT = telemetry.t2 - telemetry.t1;
    if (telemetry.t1 > TEMP_CRIT_C || telemetry.t2 > TEMP_CRIT_C) telemetry.thermalStatus = "CRITICAL";
    else if (telemetry.deltaT < -REVERSED_DT_C) telemetry.thermalStatus = "CHECK SENSORS";
    else telemetry.thermalStatus = "NORMAL";
  } else {
    telemetry.t2 = t2Raw;
    telemetry.deltaT = 0.0;
    bool anyHot = t1 > TEMP_CRIT_C || t2Raw > TEMP_CRIT_C;
    telemetry.thermalStatus = anyHot ? "CRITICAL" : "SENSOR OFFLINE";
  }

  if (telemetry.flow1 > 0.0 || telemetry.flow2 > 0.0) {
    telemetry.flowStatus = (fabs(telemetry.flow1 - telemetry.flow2) > FLOW_IMBALANCE_LPM) ? "FLOW IMBALANCE" : "ACTIVE";
  } else {
    telemetry.flowStatus = "IDLE";
  }

  // Heat removal rate: Q = m_dot * Cp * (Tout - Tin), m_dot from mean of inlet/outlet flow
  float avgFlowLpm = (telemetry.flow1 + telemetry.flow2) / 2.0;
  telemetry.heatValid = tempsValid && avgFlowLpm > 0.0;
  telemetry.heatW = telemetry.heatValid
    ? (avgFlowLpm / 60.0) * COOLANT_DENSITY_KG_PER_L * COOLANT_CP_J_PER_KG_K * telemetry.deltaT
    : 0.0;
}

// Non-blocking line reader for Serial2 (readStringUntil would stall the web server)
void pollNanoSerial() {
  static char lineBuf[192];
  static size_t lineLen = 0;
  static bool overflow = false;

  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      if (overflow) telemetry.badPackets++;
      else { lineBuf[lineLen] = '\0'; parseNanoPacket(String(lineBuf)); }
      lineLen = 0;
      overflow = false;
    } else if (c != '\r') {
      if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
      else overflow = true;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== THERMAX - Cold Plate Cooling Optimization ===");

  prefs.begin("thermax", false);
  telemetry.tOffset = prefs.isKey("tOffset") ? prefs.getFloat("tOffset", 0.0) : 0.0;
  Serial.printf("[CAL] Outlet temperature offset: %.2f C\n", telemetry.tOffset);

  // Serial2 for listening to Arduino Nano: RX2 on GPIO 16, TX2 on GPIO 17
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] Connecting to '%s'", WIFI_SSID);

  unsigned long tStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - tStart < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected to %s\n", WIFI_SSID);
    Serial.printf("[WiFi] Web Dashboard: http://%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Router connection timed out. Use the hotspot below.");
  }

  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[SoftAP] Hotspot: '%s' (password: %s)\n", AP_SSID, AP_PASS);
  Serial.printf("[SoftAP] Dashboard: http://%s\n", WiFi.softAPIP().toString().c_str());

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] Also at: http://%s.local\n", MDNS_NAME);
  }

  server.on("/", []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/data", handleApiData);
  server.on("/api/zero", HTTP_POST, handleZero);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println("[HTTP] Server listening on port 80.");
}

void loop() {
  server.handleClient();
  pollNanoSerial();
}
