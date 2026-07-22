// AIRPOC radar previewer — half-circle PPI over Server-Sent Events.
// Standalone port of the ground bench's gui/static/js/radar_view.js, minus
// the fusion/gimbal coupling: draws the AWR2944P point cloud + DBSCAN target
// boxes on a top-down polar canvas. Per-frame only — no trails/persistence
// (that's the operator GUI's job, not the radar's).
//
// Wire (SSE /stream):
//   { connected, frame_id, timestamp, profile, max_range_m, fov_half_deg,
//     num_points, num_targets,
//     points:  [{x,y,z,v,snr,r,az,el,tid}, ...],
//     targets: [{tid,x,y,z,vx,vy,vz,sx,sy,sz,conf,np,class}, ...] }
// Targets are per-frame detections only (no coasting) — display persistence,
// if wanted, is the operator GUI's job, not the radar's.
// Sensor frame: +x right, +y forward (boresight up on screen), +z up.

const TARGET_COLORS = ["#ff4d6d","#40c4ff","#ffd54f","#81c784","#ba68c8","#ff8a65","#4dd0e1","#dce775"];
const STATIC_DOPPLER_MPS = 0.2;
const NEAR_VIEW_M = 100, FAR_VIEW_M = 500, FAR_TRIGGER_M = 105, SHRINK_FRAMES = 30;

class RadarView {
  constructor(canvasId) {
    this.canvas = document.getElementById(canvasId);
    this.ctx = this.canvas.getContext("2d");
    this.last = null;
    this.viewRangeM = NEAR_VIEW_M;
    this.shrinkCounter = 0;
    this.fovHalfDeg = 60;   // pre-frame default (useful AoA); replaced by wire fov_half_deg
    window.addEventListener("resize", () => this.fit());
    this.fit();
  }
  fit() {
    const r = this.canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    this.canvas.width = Math.max(1, Math.floor(r.width * dpr));
    this.canvas.height = Math.max(1, Math.floor(r.height * dpr));
    this.redraw();
  }
  geom() {
    const w = this.canvas.width, h = this.canvas.height;
    return { w, h, cx: w / 2, cy: h - 8, maxR: Math.max(20, h - 14) };
  }
  toCanvas(xm, ym) {
    const { cx, cy, maxR } = this.geom();
    const pxPerM = maxR / Math.max(this.viewRangeM, 1);
    return { px: cx + xm * pxPerM, py: cy - ym * pxPerM, pxPerM };
  }
  updateViewRange(r) {
    let far = false;
    for (const t of (r.targets || [])) {
      if (Math.hypot(t.x || 0, t.y || 0) > FAR_TRIGGER_M) { far = true; break; }
    }
    if (far) { this.viewRangeM = FAR_VIEW_M; this.shrinkCounter = 0; }
    else if (this.viewRangeM > NEAR_VIEW_M) {
      if (++this.shrinkCounter >= SHRINK_FRAMES) { this.viewRangeM = NEAR_VIEW_M; this.shrinkCounter = 0; }
    } else this.shrinkCounter = 0;
  }
  backdrop() {
    const ctx = this.ctx, { w, h, cx, cy, maxR } = this.geom();
    const dpr = window.devicePixelRatio || 1;
    ctx.clearRect(0, 0, w, h);
    for (let i = 1; i <= 4; i++) {
      const ringM = (this.viewRangeM * i) / 4;
      const isRef = Math.abs(ringM - NEAR_VIEW_M) < 0.5;
      ctx.strokeStyle = isRef ? "rgba(255,170,60,0.55)" : "rgba(0,212,255,0.15)";
      ctx.lineWidth = isRef ? 1.4 : 1;
      ctx.beginPath(); ctx.arc(cx, cy, maxR * (i / 4), Math.PI, 2 * Math.PI); ctx.stroke();
    }
    // FOV wedge (no gimbal rotation in the standalone previewer: pan = 0).
    const fovRad = this.fovHalfDeg * Math.PI / 180;
    ctx.strokeStyle = "rgba(0,212,255,0.22)"; ctx.setLineDash([4, 4]);
    for (const sign of [-1, 1]) {
      const a = sign * fovRad;
      ctx.beginPath(); ctx.moveTo(cx, cy);
      ctx.lineTo(cx + Math.sin(a) * maxR, cy - Math.cos(a) * maxR); ctx.stroke();
    }
    ctx.setLineDash([]);
    ctx.fillStyle = "rgba(0,212,255,0.07)";
    ctx.beginPath(); ctx.moveTo(cx, cy);
    ctx.arc(cx, cy, maxR, -Math.PI / 2 - fovRad, -Math.PI / 2 + fovRad, false);
    ctx.closePath(); ctx.fill();
    // Boresight tick.
    ctx.strokeStyle = "rgba(0,212,255,0.45)"; ctx.lineWidth = 1.5;
    ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx, cy - maxR); ctx.stroke();
    ctx.lineWidth = 1;
    // Range labels.
    ctx.font = `${Math.round(11 * dpr)}px monospace`; ctx.textAlign = "left";
    for (let i = 1; i <= 4; i++) {
      const rM = (this.viewRangeM * i) / 4;
      ctx.fillStyle = (Math.abs(rM - NEAR_VIEW_M) < 0.5) ? "rgba(255,190,90,0.9)" : "rgba(180,220,240,0.55)";
      ctx.fillText(`${rM.toFixed(0)} m`, cx + 4, cy - maxR * (i / 4));
    }
  }
  pointStyle(v, snr) {
    const s = (typeof snr === "number" && isFinite(snr))
      ? Math.max(0.3, Math.min(1.0, (snr - 12) / 28)) : 0.7;
    if (Math.abs(v) < STATIC_DOPPLER_MPS) return `rgba(0,212,255,${s * 0.55})`;
    return v > 0 ? `rgba(255,85,85,${s})` : `rgba(80,170,255,${s})`;
  }
  /* Scene layer: colour = echo strength (can I trust the bearing),
     opacity = occupancy (is it really there). Rendered once per /scene
     update into an offscreen canvas, then blitted every frame. */
  sceneColour(snr, a) {
    const t = Math.max(0, Math.min(1, (snr - 16) / 46));
    let r, g, b;
    if (t < 0.5) { const u = t * 2; r = 30 + u * -30; g = 80 + u * 140; b = 255 + u * -115; }
    else         { const u = (t - 0.5) * 2; r = 0 + u * 255; g = 220 + u * -150; b = 140 + u * -100; }
    return `rgba(${r | 0},${g | 0},${b | 0},${a})`;
  }
  buildSceneLayer() {
    const s = this.scene;
    this._sceneCanvas = null;
    if (!s || !s.cells || !s.cells.length) return;
    const { w, h } = this.geom();
    const off = document.createElement("canvas");
    off.width = w; off.height = h;
    const c = off.getContext("2d"), cells = s.cells, D2R = Math.PI / 180;
    const dpr = window.devicePixelRatio || 1;
    for (let i = 0; i < cells.length; i += 4) {
      const occ = cells[i + 2];
      const r = (cells[i] + 0.5) * s.r_step;
      const az = (s.az0 + (cells[i + 1] + 0.5) * s.az_step) * D2R;
      const p = this.toCanvas(r * Math.sin(az), r * Math.cos(az));
      if (p.px < -50 || p.px > w + 50 || p.py < -50 || p.py > h + 50) continue;
      c.fillStyle = this.sceneColour(cells[i + 3], Math.max(0.06, occ / 255));
      const sz = Math.max(1.5 * dpr, s.r_step * p.pxPerM);
      c.fillRect(p.px - sz / 2, p.py - sz / 2, sz, sz);
    }
    this._sceneCanvas = off;
  }
  drawScene() {
    if (!this.showScene || !this._sceneCanvas) return;
    this.ctx.drawImage(this._sceneCanvas, 0, 0);
  }
  setScene(s) { this.scene = s; this.buildSceneLayer(); this.redraw(); }
  drawPoints(points) {
    const ctx = this.ctx, dpr = window.devicePixelRatio || 1, rad = 2 * dpr;
    for (const p of (points || [])) {
      const { px, py } = this.toCanvas(p.x, p.y);
      ctx.fillStyle = this.pointStyle(p.v, p.snr);
      ctx.beginPath(); ctx.arc(px, py, rad, 0, 2 * Math.PI); ctx.fill();
    }
  }
  drawTargets(targets) {
    const ctx = this.ctx, dpr = window.devicePixelRatio || 1;
    ctx.lineWidth = 1.5 * dpr; ctx.font = `${Math.round(11 * dpr)}px monospace`; ctx.textAlign = "left";
    for (const t of (targets || [])) {
      const colour = TARGET_COLORS[((t.tid % 8) + 8) % 8];
      const { px, py, pxPerM } = this.toCanvas(t.x, t.y);
      const wpx = Math.max(6, 2 * t.sx * pxPerM), hpx = Math.max(6, 2 * t.sy * pxPerM);
      ctx.strokeStyle = colour; ctx.strokeRect(px - wpx / 2, py - hpx / 2, wpx, hpx);
      const tip = this.toCanvas(t.x + t.vx, t.y + t.vy);
      ctx.beginPath(); ctx.moveTo(px, py); ctx.lineTo(tip.px, tip.py); ctx.stroke();
      const speed = Math.hypot(t.vx, t.vy), range = Math.hypot(t.x, t.y);
      ctx.fillStyle = colour;
      ctx.fillText(`R#${t.tid}  ${speed.toFixed(1)} m/s · ${range.toFixed(0)} m`,
                   px - wpx / 2 + 2 * dpr, py - hpx / 2 - 2 * dpr);
    }
  }
  redraw() {
    this.backdrop();
    const r = this.last;
    if (!r || !r.connected) return;
    this.drawScene();
    this.drawPoints(r.points); this.drawTargets(r.targets);
  }
  update(r) {
    if (r && typeof r.fov_half_deg === "number" && r.fov_half_deg > 0) this.fovHalfDeg = r.fov_half_deg;
    this.last = r;
    if (!r || !r.connected) { this.viewRangeM = NEAR_VIEW_M; this.backdrop(); return; }
    this.updateViewRange(r); this.redraw();
  }
  setDisconnected() { this.last = null; this.viewRangeM = NEAR_VIEW_M; this.backdrop(); }
}

// ── wiring: SSE stream + stats poll ──
const view = new RadarView("ppi");
function connect() {
  view.showScene = true;
  const sceneBox = document.getElementById("scene_on");
  if (sceneBox) sceneBox.onchange = () => { view.showScene = sceneBox.checked; view.redraw(); };
  const sceneClr = document.getElementById("scene_clear");
  if (sceneClr) sceneClr.onclick = () => fetch("/scene?reset=1").catch(() => {});
  const pollScene = () => fetch("/scene")
      .then(r => r.json()).then(s => view.setScene(s)).catch(() => {});
  pollScene(); setInterval(pollScene, 1000);

  const es = new EventSource("/stream");
  es.onmessage = (e) => { try { view.update(JSON.parse(e.data)); } catch (_) {} };
  es.onerror = () => { view.setDisconnected(); es.close(); setTimeout(connect, 1000); };
}
connect();

// ── tuning sliders → /ctl (live), synced back from /stats ──
const CTL = [
  { id: "eps",     key: "cluster_eps_m",   fmt: (v) => v.toFixed(1) },
  { id: "minpts",  key: "cluster_min_pts", fmt: (v) => String(v) },
  { id: "speed",   key: "speed_min_mps",   fmt: (v) => v.toFixed(1) },
  { id: "snrmin",  key: "snr_min_db",      fmt: (v) => v.toFixed(0) },
  { id: "fov",     key: "fov_half_deg",    fmt: (v) => v.toFixed(0) },
  { id: "doppler", key: "doppler_gate_mps", fmt: (v) => v.toFixed(1) },
];
let lastTouch = 0, sendTimer = null;
function pushCtl() {
  const q = `eps=${el("eps").value}&minpts=${el("minpts").value}` +
            `&speed=${el("speed").value}&snrmin=${el("snrmin").value}` +
            `&fov=${el("fov").value}&doppler=${el("doppler").value}`;
  fetch("/ctl?" + q).catch(() => {});
}
function el(id) { return document.getElementById(id); }
for (const c of CTL) {
  const s = el(c.id);
  s.addEventListener("input", () => {
    lastTouch = performance.now();
    el(c.id + "_v").textContent = c.fmt(parseFloat(s.value));
    clearTimeout(sendTimer);
    sendTimer = setTimeout(pushCtl, 80);   // debounce drags
  });
}

async function poll() {
  try {
    const d = await (await fetch("/stats")).json();
    document.getElementById("hud").textContent =
      `${d.profile}  ${d.fps.toFixed(1)} Hz  pts ${d.num_points}  tgts ${d.num_targets}  ` +
      `drops ${d.drops}  Rmax ${d.max_range_m.toFixed(0)} m  ${d.connected ? "" : "— DISCONNECTED"}`;
    // Sync sliders from the daemon (post-clamp) unless the user is mid-drag.
    if (performance.now() - lastTouch > 1500) {
      for (const c of CTL) {
        if (d[c.key] == null) continue;
        el(c.id).value = d[c.key];
        el(c.id + "_v").textContent = c.fmt(d[c.key]);
      }
    }
  } catch (_) {}
}
setInterval(poll, 250); poll();
