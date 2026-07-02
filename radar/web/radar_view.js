// AIRPOC radar previewer — half-circle PPI over Server-Sent Events.
// Standalone port of the ground bench's gui/static/js/radar_view.js, minus
// the fusion/gimbal coupling: draws the AWR2944P point cloud + DBSCAN target
// boxes + per-track trails on a top-down polar canvas.
//
// Wire (SSE /stream):
//   { connected, frame_id, timestamp, profile, max_range_m, fov_half_deg,
//     num_points, num_targets,
//     points:  [{x,y,z,v,snr,r,az,el,tid}, ...],
//     targets: [{tid,x,y,z,vx,vy,vz,sx,sy,sz,conf,np,coasting,class}, ...] }
// Sensor frame: +x right, +y forward (boresight up on screen), +z up.

const TARGET_COLORS = ["#ff4d6d","#40c4ff","#ffd54f","#81c784","#ba68c8","#ff8a65","#4dd0e1","#dce775"];
const STATIC_DOPPLER_MPS = 0.2;
const NEAR_VIEW_M = 100, FAR_VIEW_M = 500, FAR_TRIGGER_M = 105, SHRINK_FRAMES = 30;
const TRAIL_MAX_AGE_MS = 10000, TRAIL_MAX_POINTS = 200, TRAIL_EMA_ALPHA = 0.35;

class RadarView {
  constructor(canvasId) {
    this.canvas = document.getElementById(canvasId);
    this.ctx = this.canvas.getContext("2d");
    this.last = null;
    this.viewRangeM = NEAR_VIEW_M;
    this.shrinkCounter = 0;
    this.fovHalfDeg = 60;   // pre-frame default (useful AoA); replaced by wire fov_half_deg
    this.trails = new Map();
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
      if (t.coasting) continue;
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
  drawPoints(points) {
    const ctx = this.ctx, dpr = window.devicePixelRatio || 1, rad = 2 * dpr;
    for (const p of (points || [])) {
      const { px, py } = this.toCanvas(p.x, p.y);
      ctx.fillStyle = this.pointStyle(p.v, p.snr);
      ctx.beginPath(); ctx.arc(px, py, rad, 0, 2 * Math.PI); ctx.fill();
    }
  }
  ingestTrails(targets) {
    const nowMs = performance.now();
    for (const t of (targets || [])) {
      if (t.tid == null) continue;
      let hist = this.trails.get(t.tid);
      if (!hist) { hist = []; this.trails.set(t.tid, hist); }
      let sx = t.x, sy = t.y;
      if (hist.length) { const p = hist[hist.length - 1]; sx = TRAIL_EMA_ALPHA * t.x + (1 - TRAIL_EMA_ALPHA) * p.x; sy = TRAIL_EMA_ALPHA * t.y + (1 - TRAIL_EMA_ALPHA) * p.y; }
      hist.push({ x: sx, y: sy, t: nowMs });
      if (hist.length > TRAIL_MAX_POINTS) hist.splice(0, hist.length - TRAIL_MAX_POINTS);
    }
    for (const [tid, hist] of this.trails) {
      while (hist.length && nowMs - hist[0].t > TRAIL_MAX_AGE_MS) hist.shift();
      if (!hist.length) this.trails.delete(tid);
    }
  }
  drawTrails() {
    const ctx = this.ctx, nowMs = performance.now(), dpr = window.devicePixelRatio || 1;
    ctx.lineWidth = 1.4 * dpr; ctx.lineCap = "round";
    for (const [tid, hist] of this.trails) {
      if (hist.length < 2) continue;
      const colour = TARGET_COLORS[((tid % 8) + 8) % 8];
      const pts = hist.map(h => { const p = this.toCanvas(h.x, h.y); return { px: p.px, py: p.py, t: h.t }; });
      for (let i = 1; i < pts.length; i++) {
        const alpha = Math.max(0, 1 - (nowMs - pts[i].t) / TRAIL_MAX_AGE_MS) * 0.8;
        if (alpha <= 0.02) continue;
        ctx.globalAlpha = alpha; ctx.strokeStyle = colour;
        ctx.beginPath(); ctx.moveTo(pts[i - 1].px, pts[i - 1].py); ctx.lineTo(pts[i].px, pts[i].py); ctx.stroke();
      }
    }
    ctx.globalAlpha = 1; ctx.lineCap = "butt";
  }
  drawTargets(targets) {
    const ctx = this.ctx, dpr = window.devicePixelRatio || 1;
    ctx.lineWidth = 1.5 * dpr; ctx.font = `${Math.round(11 * dpr)}px monospace`; ctx.textAlign = "left";
    for (const t of (targets || [])) {
      const colour = TARGET_COLORS[((t.tid % 8) + 8) % 8];
      const { px, py, pxPerM } = this.toCanvas(t.x, t.y);
      const wpx = Math.max(6, 2 * t.sx * pxPerM), hpx = Math.max(6, 2 * t.sy * pxPerM);
      if (t.coasting) { ctx.setLineDash([5 * dpr, 4 * dpr]); ctx.globalAlpha = 0.55; }
      else { ctx.setLineDash([]); ctx.globalAlpha = 1; }
      ctx.strokeStyle = colour; ctx.strokeRect(px - wpx / 2, py - hpx / 2, wpx, hpx); ctx.setLineDash([]);
      const tip = this.toCanvas(t.x + t.vx, t.y + t.vy);
      ctx.beginPath(); ctx.moveTo(px, py); ctx.lineTo(tip.px, tip.py); ctx.stroke();
      const speed = Math.hypot(t.vx, t.vy), range = Math.hypot(t.x, t.y);
      ctx.fillStyle = colour;
      ctx.fillText(`R#${t.tid}  ${speed.toFixed(1)} m/s · ${range.toFixed(0)} m${t.coasting ? " · coast" : ""}`,
                   px - wpx / 2 + 2 * dpr, py - hpx / 2 - 2 * dpr);
      ctx.globalAlpha = 1;
    }
  }
  redraw() {
    this.backdrop();
    const r = this.last;
    if (!r || !r.connected) return;
    this.drawPoints(r.points); this.drawTrails(); this.drawTargets(r.targets);
  }
  update(r) {
    if (r && typeof r.fov_half_deg === "number" && r.fov_half_deg > 0) this.fovHalfDeg = r.fov_half_deg;
    this.last = r;
    if (!r || !r.connected) { this.trails.clear(); this.viewRangeM = NEAR_VIEW_M; this.backdrop(); return; }
    this.ingestTrails(r.targets); this.updateViewRange(r); this.redraw();
  }
  setDisconnected() { this.last = null; this.trails.clear(); this.viewRangeM = NEAR_VIEW_M; this.backdrop(); }
}

// ── wiring: SSE stream + stats poll ──
const view = new RadarView("ppi");
function connect() {
  const es = new EventSource("/stream");
  es.onmessage = (e) => { try { view.update(JSON.parse(e.data)); } catch (_) {} };
  es.onerror = () => { view.setDisconnected(); es.close(); setTimeout(connect, 1000); };
}
connect();

async function poll() {
  try {
    const d = await (await fetch("/stats")).json();
    document.getElementById("hud").textContent =
      `${d.profile}  ${d.fps.toFixed(1)} Hz  pts ${d.num_points}  tgts ${d.num_targets}  ` +
      `drops ${d.drops}  Rmax ${d.max_range_m.toFixed(0)} m  ${d.connected ? "" : "— DISCONNECTED"}`;
  } catch (_) {}
}
setInterval(poll, 250); poll();
