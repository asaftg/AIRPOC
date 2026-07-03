/* AIRPOC operator console — polling + controls + canvas overlays. No websockets, no
 * deps. Video = MJPEG <img>; telemetry = /stats + /radar (poll); controls = /ctl.
 * Tracking: AUTO picks the most important target (fused > nearer > higher conf);
 * MANUAL selects the target you tap. Illuminator: AUTO fits the beam to the EO FOV at
 * max power; MANUAL uses the DEV sliders. Visual language = Seeker bench. */
(function () {
  "use strict";
  var $ = function (id) { return document.getElementById(id); };
  var ctl = function (qs) { fetch("/ctl?" + qs).catch(function () {}); };
  var ZOOMS = [1, 2, 4, 8];
  var css = function (n) { return getComputedStyle(document.body).getPropertyValue(n).trim(); };

  var zoom = 1;
  var trackMode = "auto", engagedTid = null, sentEngage = null;
  var illumMode = "auto";
  var rp = { snr: 16, speed: 0.4, rmin: 0, fov: 60 };   /* GUI display filters only */
  var lastStats = {}, lastRadar = null, trails = {};

  /* ── theme / dev / swap ── */
  var theme = localStorage.getItem("airpoc-theme") || "night";
  function applyTheme() { document.body.className = theme + (trackMode === "man" ? " manual" : ""); $("theme").textContent = theme.toUpperCase(); redrawAll(); }
  $("theme").onclick = function () { theme = theme === "day" ? "night" : "day"; localStorage.setItem("airpoc-theme", theme); applyTheme(); };
  $("devbtn").onclick = function () { $("dev").classList.toggle("open"); };
  $("devclose").onclick = function () { $("dev").classList.remove("open"); };
  document.querySelectorAll("[data-swap]").forEach(function (b) { b.onclick = function (e) { e.stopPropagation(); $("stage").classList.toggle("swap"); setTimeout(redrawAll, 20); }; });

  /* ── zoom ── */
  document.querySelectorAll("[data-zoom]").forEach(function (b) {
    b.onclick = function () {
      var i = ZOOMS.indexOf(zoom) + parseInt(b.dataset.zoom, 10);
      if (i < 0 || i >= ZOOMS.length) return;
      zoom = ZOOMS[i]; ctl("zoom=" + zoom); $("v-zval").textContent = zoom.toFixed(1) + "×";
    };
  });

  /* ── tracking mode ── */
  function setTrack(m) {
    trackMode = m;
    var b = $("track"); $("track-s").textContent = m.toUpperCase();
    b.classList.toggle("auto", m === "auto"); b.classList.toggle("man", m === "man");
    document.body.classList.toggle("manual", m === "man");
    $("stage").classList.toggle("manual", m === "man");
    if (m === "auto") { engagedTid = null; $("track-hint").hidden = true; }
    else { $("track-hint").hidden = (engagedTid !== null); }
    ctl("track=" + m);
  }
  $("track").onclick = function () { setTrack(trackMode === "auto" ? "man" : "auto"); };

  /* ── illuminator mode + fire ── */
  function setIllum(m) {
    illumMode = m;
    var b = $("illum"); $("illum-s").textContent = m.toUpperCase();
    b.classList.toggle("auto", m === "auto"); b.classList.toggle("man", m === "man");
    var tag = $("illum-tag"); tag.textContent = m.toUpperCase(); tag.classList.toggle("man", m === "man");
    $("s-pow").disabled = (m === "auto"); $("s-fov").disabled = (m === "auto");
    /* AUTO/MAN is a console concept; the EO feed owns the illuminator, so auto just
     * forwards fov=<camera FOV> + power=max to the feed (done in poll()). */
  }
  $("illum").onclick = function () { setIllum(illumMode === "auto" ? "man" : "auto"); };
  $("light").onclick = function () {
    var on = $("light").classList.contains("firing");
    if (!on && !confirm("Fire 850nm IR laser? (invisible, eye hazard)")) return;
    ctl("laser=" + (on ? 0 : 1));
  };
  $("s-pow").oninput = function () { $("o-pow").textContent = this.value + "%"; ctl("power=" + Math.round(this.value * 255 / 100)); };
  $("s-fov").oninput = function () { $("o-fov").textContent = this.value + "°"; ctl("fov=" + this.value); };

  /* ── stream: forwarded to the EO feed's /ctl. Two bandwidth levers the EO module owns:
   * res (low/med/high/native, all 4:3) + fps (12–60). Detector stays full-native. ── */
  $("s-fps").oninput = function () { $("o-fps").textContent = this.value; ctl("fps=" + this.value); };
  document.querySelectorAll("#res-btns [data-res]").forEach(function (b) {
    b.onclick = function () {
      document.querySelectorAll("#res-btns [data-res]").forEach(function (x) { x.classList.remove("on"); });
      b.classList.add("on"); ctl("res=" + b.dataset.res);
    };
  });

  /* ── radar display filters (client-side only — chip cfg is the radar module's job) ── */
  function bindR(id, key, fmt) {
    $(id).oninput = function () { rp[key] = parseFloat(this.value); $("ro-" + key).textContent = fmt(rp[key]); drawRadar(lastRadar); drawEO(); };
  }
  bindR("r-snr", "snr", function (v) { return v.toFixed(1) + " dB"; });
  bindR("r-fov", "fov", function (v) { return v.toFixed(0) + "°"; });
  bindR("r-speed", "speed", function (v) { return v.toFixed(1) + " m/s"; });
  bindR("r-rmin", "rmin", function (v) { return v.toFixed(0) + " m"; });

  /* cluster cfg — pushed to the radar module's DBSCAN (not a display filter) */
  $("r-eps").oninput = function () { $("ro-eps").textContent = parseFloat(this.value).toFixed(1) + " m"; ctl("radar_eps=" + this.value); };
  $("r-minpts").oninput = function () { $("ro-minpts").textContent = this.value; ctl("radar_minpts=" + this.value); };

  /* ── reserved ── */
  $("rec").onclick = function () { $("rec").classList.toggle("active"); };
  $("restart").onclick = function () { if (confirm("Restart AIRPOC service?")) ctl("restart=1"); };
  $("logs").onclick = function () { var l = $("logs"); l.textContent = "reserved"; setTimeout(function () { l.textContent = "LOGS"; }, 900); };

  /* ── target model + selection (daemon schema: class-less objects) ── */
  function targets(radar) {
    if (!radar || !radar.targets) return [];
    return radar.targets.map(function (t) {
      return { tid: t.tid, x: t.x, y: t.y, vx: t.vx, vy: t.vy, sx: t.sx, sy: t.sy, conf: t.conf,
               rng: Math.hypot(t.x, t.y), az: Math.atan2(t.x, t.y) * 180 / Math.PI };
    }).filter(function (t) { return t.rng >= rp.rmin && Math.abs(t.az) <= rp.fov; });
  }
  /* AUTO priority: fused (EO+radar) first [pending detector], then nearer, then conf. */
  function pickAuto(ts) { return ts.slice().sort(function (a, b) { return (a.rng - b.rng) || (b.conf - a.conf); })[0] || null; }
  /* Display persistence (GUI-owned): hold a dropped target's box at its last spot and
   * fade over HOLD_MS so a one-frame miss doesn't blink. Not motion prediction — that
   * (real coasting) belongs to the tracking module, not the display. */
  var held = {}, HOLD_MS = 300;
  function engage(tid) {
    engagedTid = tid;
    $("track-hint").hidden = (trackMode !== "man") || (tid !== null);
    if (tid !== sentEngage) { sentEngage = tid; ctl("engage=" + (tid === null ? -1 : tid)); }
  }

  /* ── canvas ── */
  function fit(cv) { var r = cv.getBoundingClientRect(), dpr = window.devicePixelRatio || 1; cv.width = Math.max(1, r.width * dpr | 0); cv.height = Math.max(1, r.height * dpr | 0); return { ctx: cv.getContext("2d"), w: cv.width, h: cv.height, dpr: dpr }; }

  function drawEO() {
    var f = fit($("eo-ovl")), ctx = f.ctx, w = f.w, h = f.h, dpr = f.dpr;
    ctx.clearRect(0, 0, w, h);
    var cx = w / 2, cy = h / 2, amber = css("--amber");
    /* reticle */
    ctx.strokeStyle = amber; ctx.fillStyle = amber; ctx.lineWidth = 1.4 * dpr; ctx.globalAlpha = 0.85;
    var gap = 10 * dpr, arm = 26 * dpr;
    [[0, -1], [0, 1], [-1, 0], [1, 0]].forEach(function (d) { ctx.beginPath(); ctx.moveTo(cx + d[0] * gap, cy + d[1] * gap); ctx.lineTo(cx + d[0] * (gap + arm), cy + d[1] * (gap + arm)); ctx.stroke(); });
    ctx.globalAlpha = 0.5; ctx.beginPath(); ctx.arc(cx, cy, 30 * dpr, 0, 2 * Math.PI); ctx.stroke();
    ctx.globalAlpha = 1; ctx.beginPath(); ctx.arc(cx, cy, 2 * dpr, 0, 2 * Math.PI); ctx.fill();
    ctx.globalAlpha = 0.35; ctx.lineWidth = 1.2 * dpr; var m = 16 * dpr, L = 22 * dpr;
    [[m, m, 1, 1], [w - m, m, -1, 1], [m, h - m, 1, -1], [w - m, h - m, -1, -1]].forEach(function (c) { ctx.beginPath(); ctx.moveTo(c[0], c[1] + c[3] * L); ctx.lineTo(c[0], c[1]); ctx.lineTo(c[0] + c[2] * L, c[1]); ctx.stroke(); });
    ctx.globalAlpha = 1;

    /* engaged-target lock projected into the EO frame via camera hfov */
    var eoHfov = (lastStats.eo && lastStats.eo.hfov) || 0;
    if (engagedTid !== null && eoHfov) {
      var t = targets(lastRadar).filter(function (x) { return x.tid === engagedTid; })[0];
      if (t) {
        var half = eoHfov / 2;
        if (Math.abs(t.az) <= half) {
          var lx = cx + (t.az / half) * (w / 2), ly = h * 0.5;
          var bw = 72 * dpr, bh = 92 * dpr, col = css("--on");
          ctx.strokeStyle = col; ctx.lineWidth = 2 * dpr;
          var cc = 14 * dpr, x0 = lx - bw / 2, y0 = ly - bh / 2;
          [[x0, y0, 1, 1], [x0 + bw, y0, -1, 1], [x0, y0 + bh, 1, -1], [x0 + bw, y0 + bh, -1, -1]].forEach(function (c) { ctx.beginPath(); ctx.moveTo(c[0], c[1] + c[3] * cc); ctx.lineTo(c[0], c[1]); ctx.lineTo(c[0] + c[2] * cc, c[1]); ctx.stroke(); });
          ctx.beginPath(); ctx.moveTo(lx - 8 * dpr, ly); ctx.lineTo(lx + 8 * dpr, ly); ctx.moveTo(lx, ly - 8 * dpr); ctx.lineTo(lx, ly + 8 * dpr); ctx.stroke();
          ctx.fillStyle = col; ctx.font = (11 * dpr) + "px ui-monospace, monospace";
          ctx.fillText("LOCK #" + t.tid + "  " + t.rng.toFixed(0) + " m", x0, y0 - 4 * dpr);
        } else {
          var dir = t.az > 0 ? 1 : -1, ex = cx + dir * (w / 2 - 24 * dpr);
          ctx.fillStyle = css("--on"); ctx.globalAlpha = 0.9; ctx.beginPath();
          ctx.moveTo(ex, cy); ctx.lineTo(ex - dir * 16 * dpr, cy - 10 * dpr); ctx.lineTo(ex - dir * 16 * dpr, cy + 10 * dpr); ctx.closePath(); ctx.fill(); ctx.globalAlpha = 1;
        }
      }
    }
  }

  var REF_M = 250;
  var TARGET_COLORS = ["#ff4d6d", "#40c4ff", "#ffd54f", "#81c784", "#ba68c8", "#ff8a65"];
  function drawRadar(radar) {
    var f = fit($("radar-cv")), ctx = f.ctx, w = f.w, h = f.h, dpr = f.dpr;
    ctx.clearRect(0, 0, w, h);
    var cx = w / 2, cy = h - 10 * dpr, maxR = Math.max(20, Math.min(h - 16 * dpr, w / 2 - 6 * dpr));
    var cyan = css("--cyan"), amber = css("--amber"), dim = css("--dim");
    var maxM = (radar && radar.max_range_m) || 500;
    var scale = maxR / maxM, W2C = function (x, y) { return [cx + x * scale, cy - y * scale]; };
    ctx.font = (10 * dpr) + "px ui-monospace, monospace";

    for (var i = 1; i <= 4; i++) {
      var ringM = maxM * i / 4, ref = Math.abs(ringM - REF_M) < 1;
      ctx.strokeStyle = ref ? amber : cyan; ctx.globalAlpha = ref ? 0.5 : 0.16; ctx.lineWidth = ref ? 1.4 * dpr : dpr;
      ctx.beginPath(); ctx.arc(cx, cy, maxR * i / 4, Math.PI, 2 * Math.PI); ctx.stroke();
      ctx.globalAlpha = ref ? 0.85 : 0.5; ctx.fillStyle = ref ? amber : dim; ctx.fillText(ringM.toFixed(0) + " m", cx + 4 * dpr, cy - maxR * i / 4 + 12 * dpr);
    }
    var fr = rp.fov * Math.PI / 180;
    ctx.globalAlpha = 0.07; ctx.fillStyle = cyan; ctx.beginPath(); ctx.moveTo(cx, cy);
    ctx.arc(cx, cy, maxR, -Math.PI / 2 - fr, -Math.PI / 2 + fr, false); ctx.closePath(); ctx.fill();
    ctx.globalAlpha = 0.28; ctx.strokeStyle = cyan; ctx.lineWidth = dpr; ctx.setLineDash([4 * dpr, 4 * dpr]);
    [-1, 1].forEach(function (s) { var a = -Math.PI / 2 + s * fr; ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx + Math.cos(a) * maxR, cy + Math.sin(a) * maxR); ctx.stroke(); });
    ctx.setLineDash([]);
    ctx.globalAlpha = 0.4; ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx, cy - maxR); ctx.stroke();
    ctx.globalAlpha = 0.6; ctx.fillStyle = cyan; ctx.textAlign = "center"; ctx.fillText("N", cx, cy - maxR + 12 * dpr); ctx.textAlign = "left";

    if (!radar || !radar.connected) { ctx.globalAlpha = 0.5; ctx.fillStyle = dim; ctx.textAlign = "center"; ctx.fillText("NOT CONNECTED", cx, cy - maxR * 0.45); ctx.textAlign = "left"; ctx.globalAlpha = 1; radarGeom = null; return; }
    radarGeom = { cx: cx, cy: cy, scale: scale, dpr: dpr };

    /* returns — gated by the display filters (fov/range/speed; snr only when the
     * firmware provides it — null on current fw). v = +approaching. */
    (radar.points || []).forEach(function (p) {
      var az = Math.abs(p.az != null ? p.az : Math.atan2(p.x, p.y) * 180 / Math.PI);
      var rng = (p.r != null) ? p.r : Math.hypot(p.x, p.y);
      if (rng < rp.rmin || az > rp.fov || Math.abs(p.v) < rp.speed) return;
      if (p.snr != null && p.snr < rp.snr) return;
      var pc = W2C(p.x, p.y);
      ctx.globalAlpha = (p.snr != null) ? Math.max(0.3, Math.min(1, (p.snr - 12) / 28)) : 0.8;
      ctx.fillStyle = Math.abs(p.v) < 0.3 ? cyan : (p.v > 0 ? "#ff5555" : "#50aaff");
      ctx.beginPath(); ctx.arc(pc[0], pc[1], 2 * dpr, 0, 2 * Math.PI); ctx.fill();
    });
    ctx.globalAlpha = 1;

    Object.keys(trails).forEach(function (tid) {
      var hist = trails[tid]; if (hist.length < 2) return;
      ctx.strokeStyle = TARGET_COLORS[tid % TARGET_COLORS.length]; ctx.lineWidth = 1.3 * dpr; ctx.beginPath();
      hist.forEach(function (p, k) { var c = W2C(p.x, p.y); if (k) ctx.lineTo(c[0], c[1]); else ctx.moveTo(c[0], c[1]); });
      ctx.globalAlpha = 0.5; ctx.stroke(); ctx.globalAlpha = 1;
    });

    var now = Date.now();
    Object.keys(held).forEach(function (tid) {
      var h = held[tid], t = h.t, fade = Math.max(0, 1 - (now - h.ts) / HOLD_MS);
      if (fade <= 0.02) return;
      var tc = W2C(t.x, t.y), locked = (t.tid === engagedTid);
      var col = locked ? css("--on") : TARGET_COLORS[t.tid % TARGET_COLORS.length];
      var wpx = Math.max(8 * dpr, 2 * t.sx * scale), hpx = Math.max(8 * dpr, 2 * t.sy * scale);
      ctx.globalAlpha = fade; ctx.strokeStyle = col; ctx.lineWidth = (locked ? 2.2 : 1.5) * dpr;
      if (locked) {
        var cc = 8 * dpr, x0 = tc[0] - wpx / 2, y0 = tc[1] - hpx / 2;
        [[x0, y0, 1, 1], [x0 + wpx, y0, -1, 1], [x0, y0 + hpx, 1, -1], [x0 + wpx, y0 + hpx, -1, -1]].forEach(function (c) { ctx.beginPath(); ctx.moveTo(c[0], c[1] + c[3] * cc); ctx.lineTo(c[0], c[1]); ctx.lineTo(c[0] + c[2] * cc, c[1]); ctx.stroke(); });
      } else ctx.strokeRect(tc[0] - wpx / 2, tc[1] - hpx / 2, wpx, hpx);
      var vc = W2C(t.x + t.vx, t.y + t.vy); ctx.beginPath(); ctx.moveTo(tc[0], tc[1]); ctx.lineTo(vc[0], vc[1]); ctx.stroke();
      var spd = Math.hypot(t.vx, t.vy);
      ctx.fillStyle = col; ctx.font = (10 * dpr) + "px ui-monospace, monospace";
      ctx.fillText((locked ? "LOCK " : "R#") + t.tid + " " + spd.toFixed(0) + "m/s " + t.rng.toFixed(0) + "m", tc[0] - wpx / 2, tc[1] - hpx / 2 - 3 * dpr);
      ctx.globalAlpha = 1;
    });
  }
  var radarGeom = null;

  function ingestTrails(radar) {
    var now = Date.now();
    targets(radar).forEach(function (t) { (trails[t.tid] = trails[t.tid] || []).push({ x: t.x, y: t.y, t: now }); if (trails[t.tid].length > 60) trails[t.tid].shift(); });
    Object.keys(trails).forEach(function (tid) { trails[tid] = trails[tid].filter(function (p) { return now - p.t < 6000; }); if (!trails[tid].length) delete trails[tid]; });
  }

  function redrawAll() { drawEO(); drawRadar(lastRadar); }
  window.addEventListener("resize", redrawAll);

  /* ── manual selection: tap the EO (project click az) or the radar target ── */
  $("eo").addEventListener("click", function (e) {
    if (trackMode !== "man" || e.target.closest(".exp")) return;
    var eoHfov = (lastStats.eo && lastStats.eo.hfov) || 0;
    var ts = targets(lastRadar); if (!ts.length || !eoHfov) return;
    var r = this.getBoundingClientRect(), frac = (e.clientX - r.left) / r.width - 0.5;
    var azClick = frac * eoHfov;
    engage(ts.slice().sort(function (a, b) { return Math.abs(a.az - azClick) - Math.abs(b.az - azClick); })[0].tid);
  });
  $("radar-cv").addEventListener("click", function (e) {
    if (trackMode !== "man" || !radarGeom) return;
    var r = this.getBoundingClientRect(), g = radarGeom;
    var px = (e.clientX - r.left) * (this.width / r.width), py = (e.clientY - r.top) * (this.height / r.height);
    var best = null, bd = 1e9;
    targets(lastRadar).forEach(function (t) { var dx = g.cx + t.x * g.scale - px, dy = g.cy - t.y * g.scale - py, d = Math.hypot(dx, dy); if (d < bd) { bd = d; best = t; } });
    if (best && bd < 40 * g.dpr) engage(best.tid);
  });

  /* ── ZULU ── */
  function zulu() { var d = new Date(), p = function (n) { return (n < 10 ? "0" : "") + n; }; $("v-zulu").textContent = p(d.getUTCHours()) + ":" + p(d.getUTCMinutes()); }
  setInterval(zulu, 1000); zulu();

  /* ── polls ── */
  function num(v, dp, suf) { return (v === null || v === undefined) ? "—" : v.toFixed(dp) + (suf || ""); }
  function idle(el) { return document.activeElement !== el; }

  function poll() {
    fetch("/stats").then(function (r) { return r.json(); }).then(function (d) {
      lastStats = d;
      var eo = d.eo || {};                              /* the EO feed's own /stats */
      var eoc = !!d.eo_connected;                        /* EO feed up + delivering? */
      var hfov = (typeof eo.hfov === "number") ? eo.hfov : null;
      $("eo-scrim").hidden = eoc; $("eo").classList.toggle("hide-video", !eoc);
      $("v-link").textContent = num(d.mbps, 1); $("p-link").classList.toggle("on", d.mbps > 0.05);
      $("v-batt").textContent = num(d.batt, 0, "%"); $("v-alt").textContent = num(d.alt, 0);
      $("eo-tl").textContent = "EO · FOV " + num(hfov, 1, "°") + " · " + (eo.zoom ? eo.zoom.toFixed(1) : "1.0") + "×";
      $("eo-tr").textContent = "BRG " + (d.brg === null ? "—" : num(d.brg, 0, "°")) + "  RNG " + (d.rng === null ? "—" : num(d.rng, 2, " km"));
      $("v-tracks").textContent = (d.tracks === null || d.tracks === undefined) ? "—" : d.tracks + " TRK";
      $("v-est").textContent = "EST " + num(d.mbps, 1) + " Mb/s · " + num(eo.fps, 0) + " fps";
      $("v-srcfps").textContent = num(eo.sfps, 0); $("v-cpu").textContent = num(d.cpu_c, 0); $("v-cam").textContent = "—";
      if (typeof eo.zoom === "number" && ZOOMS.indexOf(eo.zoom) >= 0) { zoom = eo.zoom; $("v-zval").textContent = zoom.toFixed(1) + "×"; }
      var light = $("light");
      light.classList.toggle("firing", !!eo.laser); $("light-s").textContent = eo.laser ? "ON" : "OFF"; light.style.opacity = eo.lpresent ? "1" : ".5";
      if (illumMode === "man") {
        if (typeof eo.lpower === "number" && idle($("s-pow"))) { var pc = Math.round(eo.lpower * 100 / 255); $("s-pow").value = pc; $("o-pow").textContent = pc + "%"; }
        if (typeof eo.lfov === "number" && idle($("s-fov"))) { $("s-fov").value = Math.round(eo.lfov); $("o-fov").textContent = Math.round(eo.lfov) + "°"; }
      } else {                                            /* AUTO: fit beam to camera FOV @ max */
        if (typeof eo.lfov === "number") $("o-fov").textContent = Math.round(eo.lfov) + "°";
        $("o-pow").textContent = "MAX";
        if (eo.lpresent && hfov && (Math.abs((eo.lfov || 0) - hfov) > 1 || (eo.lpower || 0) < 255)) { ctl("fov=" + hfov.toFixed(0)); ctl("power=255"); }
      }
      /* cluster cfg: reflect the daemon's APPLIED (clamped) value, not the request */
      if (typeof d.radar_eps === "number" && idle($("r-eps"))) { $("r-eps").value = d.radar_eps; $("ro-eps").textContent = d.radar_eps.toFixed(1) + " m"; }
      if (typeof d.radar_minpts === "number" && idle($("r-minpts"))) { $("r-minpts").value = d.radar_minpts; $("ro-minpts").textContent = d.radar_minpts; }
    }).catch(function () { $("v-link").textContent = "—"; $("p-link").classList.remove("on"); });
  }

  function pollRadar() {
    fetch("/radar").then(function (r) { return r.json(); }).then(function (d) {
      lastRadar = d;
      if (!d.connected) { held = {}; trails = {}; engage(trackMode === "man" ? engagedTid : null); drawRadar(d); drawEO(); return; }
      var cur = targets(d), now = Date.now(), present = {};
      cur.forEach(function (t) { held[t.tid] = { t: t, ts: now }; present[t.tid] = 1; });
      Object.keys(held).forEach(function (tid) { if (!present[tid] && now - held[tid].ts > HOLD_MS) delete held[tid]; });
      ingestTrails(d);
      if (trackMode === "auto") { var best = pickAuto(cur); engage(best ? best.tid : null); }
      else if (engagedTid !== null && !present[engagedTid]) engage(null);   /* engaged target gone */
      drawRadar(d); drawEO();
    }).catch(function () {});
  }

  setTrack("auto"); setIllum("auto"); applyTheme();
  setInterval(poll, 160); poll();
  setInterval(pollRadar, 120); pollRadar();
})();
