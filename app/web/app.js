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
  var lastStats = {}, lastRadar = null;
  var rHz = 0, rLastFid = null, rLastTs = 0;   /* radar frame-rate (Hz) from frame_id/timestamp deltas */

  /* ── theme / dev / swap ── */
  var theme = localStorage.getItem("airpoc-theme") || "night";
  function applyTheme() { document.body.className = theme + (trackMode === "man" ? " manual" : ""); $("theme").textContent = theme.toUpperCase(); redrawAll(); }
  $("theme").onclick = function () { theme = theme === "day" ? "night" : "day"; localStorage.setItem("airpoc-theme", theme); applyTheme(); };
  /* DEV squeezes the layout (it's in-flow, not an overlay) — refit the canvases after the
   * width transition so the EO overlay + radar scope match their new sizes. */
  $("devbtn").onclick = function () { $("dev").classList.toggle("open"); setTimeout(redrawAll, 180); };
  $("devclose").onclick = function () { $("dev").classList.remove("open"); setTimeout(redrawAll, 180); };
  document.querySelectorAll("[data-exp]").forEach(function (b) { b.onclick = function (e) { e.stopPropagation(); $("stage").classList.toggle("rbig"); setTimeout(redrawAll, 20); }; });

  /* ── zoom ── the EO feed owns digital zoom; we forward zoom=N and drive the readout
   * optimistically. poll() reconciles from eo.zoom but is guarded for ~1.2s after a tap
   * so a slightly-stale /stats can't snap the label back mid-interaction. ── */
  var zoomTouch = 0;
  function setZoomLabel() { $("v-zval").textContent = zoom.toFixed(1) + "×"; }
  document.querySelectorAll("[data-zoom]").forEach(function (b) {
    b.onclick = function () {
      var i = ZOOMS.indexOf(zoom) + parseInt(b.dataset.zoom, 10);
      if (i < 0 || i >= ZOOMS.length) return;
      zoom = ZOOMS[i]; zoomTouch = Date.now(); ctl("zoom=" + zoom); setZoomLabel();
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
    /* On entering MANUAL, seed the sliders ONCE from the feed's current illuminator
     * state. After that the sliders are the source of truth — poll() must NOT write them
     * back, or adjusting one would snap the other to a (slightly stale) /stats value. */
    if (m === "man") {
      var eo = lastStats.eo || {};
      if (typeof eo.lpower === "number") { var pc = Math.round(eo.lpower * 100 / 255); $("s-pow").value = pc; $("o-pow").textContent = pc + "%"; }
      if (typeof eo.lfov === "number") { $("s-fov").value = Math.round(eo.lfov); $("o-fov").textContent = Math.round(eo.lfov) + "°"; }
    }
  }
  $("illum").onclick = function () { setIllum(illumMode === "auto" ? "man" : "auto"); };

  /* LIGHT: fire the 850 nm IR illuminator. No confirm dialog. Optimistic toggle off the
   * button's own shown state so it responds instantly and reverses reliably; poll()
   * reconciles from eo.laser but is guarded for ~1.2s so the round-trip can't fight it. */
  var lightTouch = 0;
  $("light").onclick = function () {
    var want = !$("light").classList.contains("firing");
    lightTouch = Date.now();
    ctl("laser=" + (want ? 1 : 0));
    $("light").classList.toggle("firing", want);
    $("light-s").textContent = want ? "ON" : "OFF";
  };
  $("s-pow").oninput = function () { $("o-pow").textContent = this.value + "%"; ctl("power=" + Math.round(this.value * 255 / 100)); };
  $("s-fov").oninput = function () { $("o-fov").textContent = this.value + "°"; ctl("fov=" + this.value); };

  /* ── EO ISP — the full preview control set, forwarded to the EO feed's /ctl:
   * ae (auto/manual exposure), expms, gain, gaincap, median. The feed owns the sensor;
   * these are the same knobs the eo_pipeline preview exposes. ── */
  var EXP_DEFAULTS = { gaincap: 120, median: 1 };   /* the sensor's default AUTO state */
  document.querySelectorAll("#ae-btns [data-ae]").forEach(function (b) {
    b.onclick = function () {
      setSeg("ae-btns", b); var auto = b.dataset.ae === "1"; setExpMode(auto); ispTouch = Date.now();
      ctl("ae=" + b.dataset.ae);
      if (auto) {   /* returning to AUTO resets the sensor to defaults */
        ctl("gaincap=" + EXP_DEFAULTS.gaincap); $("s-gcap").value = EXP_DEFAULTS.gaincap; $("o-gcap").textContent = EXP_DEFAULTS.gaincap;
        var md = document.querySelector('#md-btns [data-md="' + EXP_DEFAULTS.median + '"]'); if (md) setSeg("md-btns", md); ctl("median=" + EXP_DEFAULTS.median);
      }
    };
  });
  document.querySelectorAll("#md-btns [data-md]").forEach(function (b) {
    b.onclick = function () { setSeg("md-btns", b); ctl("median=" + b.dataset.md); };
  });
  function setSeg(id, on) { document.querySelectorAll("#" + id + " button").forEach(function (x) { x.classList.remove("on"); }); on.classList.add("on"); }
  /* AUTO exposure freezes EVERY sensor knob (EXP/GAIN/AUTO-CAP/MEDIAN) — nothing is
   * touchable in AUTO, like the illuminator's AUTO. MANUAL unfreezes them all. */
  function setExpMode(auto) {
    $("s-exp").disabled = auto; $("s-gain").disabled = auto; $("s-gcap").disabled = auto;
    document.querySelectorAll("#md-btns button").forEach(function (x) { x.disabled = auto; });
  }
  /* moving EXP or GAIN drops the feed to MANUAL — reflect that optimistically */
  $("s-exp").oninput  = function () { $("o-exp").textContent = (+this.value).toFixed(2); manualAE(); ctl("expms=" + this.value); };
  $("s-gain").oninput = function () { $("o-gain").textContent = this.value; manualAE(); ctl("gain=" + this.value); };
  $("s-gcap").oninput = function () { $("o-gcap").textContent = this.value; ctl("gaincap=" + this.value); };
  function manualAE() { var m = document.querySelector('#ae-btns [data-ae="0"]'); if (m) setSeg("ae-btns", m); setExpMode(false); ispTouch = Date.now(); }
  var ispTouch = 0, fpsTouch = 0;

  /* stream bandwidth levers — res (display size) + fps cap, both live on the EO feed */
  document.querySelectorAll("#res-btns [data-res]").forEach(function (b) {
    b.onclick = function () { setSeg("res-btns", b); ctl("res=" + b.dataset.res); };
  });
  $("s-fps").oninput = function () { $("o-fps").textContent = this.value; fpsTouch = Date.now(); ctl("fps=" + this.value); };

  /* ── radar controls — the daemon's six live knobs (radar/docs/INTEGRATION.md). Sent
   * namespaced as radar_<key>= (the app strips the prefix → daemon /ctl); clamps are
   * server-side. Init + readback come from /rstats (the daemon's own /stats). ── */
  var RADARC = [
    { key: "fov",     stat: "fov_half_deg",     fmt: function (v) { return v.toFixed(0) + "°"; } },
    { key: "snrmin",  stat: "snr_min_db",       fmt: function (v) { return v.toFixed(0) + " dB"; } },
    { key: "speed",   stat: "speed_min_mps",    fmt: function (v) { return v.toFixed(1) + " m/s"; } },
    { key: "doppler", stat: "doppler_gate_mps", fmt: function (v) { return v.toFixed(1) + " m/s"; } },
    { key: "eps",     stat: "cluster_eps_m",    fmt: function (v) { return v.toFixed(1) + " m"; } },
    { key: "minpts",  stat: "cluster_min_pts",  fmt: function (v) { return String(v | 0); } }
  ];
  var rcTouch = 0;
  RADARC.forEach(function (c) {
    $("rd-" + c.key).oninput = function () { $("rv-" + c.key).textContent = c.fmt(parseFloat(this.value)); rcTouch = Date.now(); ctl("radar_" + c.key + "=" + this.value); };
  });
  function pollRstats() {
    fetch("/rstats").then(function (r) { return r.json(); }).then(function (d) {
      if (Date.now() - rcTouch < 1500) return;   /* don't fight an active drag */
      RADARC.forEach(function (c) {
        var v = d[c.stat];
        if (typeof v === "number" && document.activeElement !== $("rd-" + c.key)) { $("rd-" + c.key).value = v; $("rv-" + c.key).textContent = c.fmt(v); }
      });
    }).catch(function () {});
  }

  /* ── reserved ── */
  $("rec").onclick = function () { $("rec").classList.toggle("active"); };
  $("restart").onclick = function () { if (confirm("Restart AIRPOC service?")) ctl("restart=1"); };
  $("logs").onclick = function () { var l = $("logs"); l.textContent = "reserved"; setTimeout(function () { l.textContent = "LOGS"; }, 900); };

  /* ── target model + selection (daemon schema: class-less objects) ── */
  function targets(radar) {
    if (!radar || !radar.targets) return [];
    return radar.targets.map(function (t) {
      return { tid: t.tid, x: t.x, y: t.y, z: t.z, vx: t.vx, vy: t.vy, sx: t.sx, sy: t.sy, conf: t.conf,
               rng: Math.hypot(t.x, t.y),
               az: Math.atan2(t.x, t.y) * 180 / Math.PI,                 /* azimuth from radar */
               el: Math.atan2(t.z || 0, Math.hypot(t.x, t.y)) * 180 / Math.PI };  /* elevation from radar z */
    });   /* no client filtering — the daemon gates points/clusters server-side now */
  }
  /* AUTO priority: fused (EO+radar) first [pending detector], then nearer, then conf. */
  function pickAuto(ts) { return ts.slice().sort(function (a, b) { return (a.rng - b.rng) || (b.conf - a.conf); })[0] || null; }

  /* Target list (top 5 by importance): fused > higher confidence > nearer. Fusion isn't
   * wired yet, so today this is radar-only (conf then range). Its own persistence
   * (~2 s) keeps rows from flickering when a target drops for a frame or two. */
  var listHold = {}, LIST_HOLD_MS = 2000;
  function importance(t) { return (t.fused ? 2e6 : 0) + (t.conf || 0) * 1e3 - t.rng; }
  function renderTargetList(radar) {
    var now = Date.now();
    targets(radar).forEach(function (t) { listHold[t.tid] = { t: t, ts: now }; });
    Object.keys(listHold).forEach(function (tid) { if (now - listHold[tid].ts > LIST_HOLD_MS) delete listHold[tid]; });
    var rows = Object.keys(listHold).map(function (tid) { return { t: listHold[tid].t, held: (now - listHold[tid].ts > 200) }; });
    rows.sort(function (a, b) { return importance(b.t) - importance(a.t); });
    rows = rows.slice(0, 5);
    $("v-tgtcount").textContent = rows.length;
    /* Always render 5 fixed slots so the panel never resizes as targets come and go. */
    var out = [];
    for (var i = 0; i < 5; i++) {
      if (i < rows.length) {
        var r = rows[i], t = r.t, col = tcolor(t.tid), spd = Math.hypot(t.vx, t.vy), az = t.az;
        var eng = (t.tid === engagedTid), cls = "tgt-row" + (eng ? " eng" : "") + (r.held ? " held" : "");
        out.push('<li class="' + cls + '" data-tid="' + t.tid + '" style="border-left-color:' + (eng ? "var(--on)" : col) + '">'
          + '<span class="tid" style="color:' + (eng ? "var(--on)" : col) + '">R#' + t.tid + '</span>'
          + '<span class="meta">' + spd.toFixed(1) + ' m/s · ' + (az >= 0 ? "+" : "") + az.toFixed(0) + '°</span>'
          + '<span class="rng">' + t.rng.toFixed(0) + ' m</span></li>');
      } else {
        out.push('<li class="tgt-row empty"><span class="tid">—</span><span class="meta"></span><span class="rng"></span></li>');
      }
    }
    $("tgt-list").innerHTML = out.join("");
  }
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
    ctx.globalAlpha = 1; ctx.beginPath(); ctx.arc(cx, cy, 2 * dpr, 0, 2 * Math.PI); ctx.fill();
    ctx.globalAlpha = 0.35; ctx.lineWidth = 1.2 * dpr; var m = 16 * dpr, L = 22 * dpr;
    [[m, m, 1, 1], [w - m, m, -1, 1], [m, h - m, 1, -1], [w - m, h - m, -1, -1]].forEach(function (c) { ctx.beginPath(); ctx.moveTo(c[0], c[1] + c[3] * L); ctx.lineTo(c[0], c[1]); ctx.lineTo(c[0] + c[2] * L, c[1]); ctx.stroke(); });
    ctx.globalAlpha = 1;

    /* engaged-target lock projected into the EO frame from the RADAR's azimuth (→ x, via
     * camera hfov) AND elevation (→ y, via camera vfov). The radar owns both angles; we
     * only map them to pixels. Off-frame → an edge arrow pointing the true 2-D direction. */
    var es = lastStats.eo || {};
    var eoHfov = es.hfov || 0, eoVfov = es.vfov || (eoHfov * 0.75);   /* 4:3 fallback */
    if (engagedTid !== null && eoHfov) {
      var t = targets(lastRadar).filter(function (x) { return x.tid === engagedTid; })[0];
      if (t) {
        var fx = t.az / (eoHfov / 2), fy = -t.el / (eoVfov / 2);   /* -1..1 within frame; +el = up */
        var col = css("--on");
        if (Math.abs(fx) <= 1 && Math.abs(fy) <= 1) {
          var lx = cx + fx * (w / 2), ly = cy + fy * (h / 2);
          var bw = 72 * dpr, bh = 92 * dpr;
          ctx.strokeStyle = col; ctx.lineWidth = 2 * dpr;
          var cc = 14 * dpr, x0 = lx - bw / 2, y0 = ly - bh / 2;
          [[x0, y0, 1, 1], [x0 + bw, y0, -1, 1], [x0, y0 + bh, 1, -1], [x0 + bw, y0 + bh, -1, -1]].forEach(function (c) { ctx.beginPath(); ctx.moveTo(c[0], c[1] + c[3] * cc); ctx.lineTo(c[0], c[1]); ctx.lineTo(c[0] + c[2] * cc, c[1]); ctx.stroke(); });
          ctx.beginPath(); ctx.moveTo(lx - 8 * dpr, ly); ctx.lineTo(lx + 8 * dpr, ly); ctx.moveTo(lx, ly - 8 * dpr); ctx.lineTo(lx, ly + 8 * dpr); ctx.stroke();
          ctx.fillStyle = col; ctx.font = (11 * dpr) + "px ui-monospace, monospace";
          ctx.fillText("LOCK #" + t.tid + "  " + t.rng.toFixed(0) + " m", x0, y0 - 4 * dpr);
        } else {
          var ex = cx + Math.max(-1, Math.min(1, fx)) * (w / 2 - 24 * dpr);
          var ey = cy + Math.max(-1, Math.min(1, fy)) * (h / 2 - 24 * dpr);
          ctx.fillStyle = col; ctx.globalAlpha = 0.9;
          ctx.save(); ctx.translate(ex, ey); ctx.rotate(Math.atan2(fy, fx));
          ctx.beginPath(); ctx.moveTo(15 * dpr, 0); ctx.lineTo(-11 * dpr, -10 * dpr); ctx.lineTo(-11 * dpr, 10 * dpr); ctx.closePath(); ctx.fill();
          ctx.restore(); ctx.globalAlpha = 1;
        }
      }
    }
  }

  /* Radar scope — matches the radar daemon's own PPI renderer (radar/web/radar_view.js):
   * same 8 target colours, 2 px dots, SNR-scaled alpha, half-circle rings, amber 100 m
   * reference. We add the GUI's jobs the daemon leaves to us: display persistence
   * (hold+fade a dropped box ~300 ms) and the engaged-target LOCK. */
  var TARGET_COLORS = ["#ff4d6d", "#40c4ff", "#ffd54f", "#81c784", "#ba68c8", "#ff8a65", "#4dd0e1", "#dce775"];
  function tcolor(tid) { return TARGET_COLORS[((tid % 8) + 8) % 8]; }
  function pointStyle(v, snr) {
    var s = (typeof snr === "number" && isFinite(snr)) ? Math.max(0.3, Math.min(1, (snr - 12) / 28)) : 0.7;
    if (Math.abs(v) < 0.2) return "rgba(150,157,168," + (s * 0.55) + ")";   /* static → titanium */
    return v > 0 ? "rgba(255,85,85," + s + ")" : "rgba(80,170,255," + s + ")";  /* doppler in/out (functional) */
  }

  /* View range: default 100 m; jump to 250 m once a target is beyond 100 m, and to
   * 500 m once one is beyond 250 m. Grow instantly, shrink only after a few quiet
   * frames so a one-frame drop doesn't make the scope pump. */
  var viewRangeM = 100, shrinkCtr = 0, SHRINK_FRAMES = 25;
  function updateViewRange(radar) {
    var maxTR = 0;
    (radar && radar.targets || []).forEach(function (t) { var r = Math.hypot(t.x || 0, t.y || 0); if (r > maxTR) maxTR = r; });
    var want = maxTR > 250 ? 500 : (maxTR > 100 ? 250 : 100);
    if (want > viewRangeM) { viewRangeM = want; shrinkCtr = 0; }
    else if (want < viewRangeM) { if (++shrinkCtr >= SHRINK_FRAMES) { viewRangeM = want; shrinkCtr = 0; } }
    else shrinkCtr = 0;
  }

  var radarGeom = null;
  function drawRadar(radar) {
    /* Force the scope panel to 2:1 (a forward half-circle wants width = 2 x height) so it
     * fills like the daemon's previewer instead of cramming into a tall box. When expanded
     * (rbig), clear the override and let it fill the stage. */
    var rw = $("radar-cv").parentElement;
    if ($("stage").classList.contains("rbig")) rw.style.height = "";
    else rw.style.height = Math.round(rw.clientWidth / 2) + "px";
    var f = fit($("radar-cv")), ctx = f.ctx, w = f.w, h = f.h, dpr = f.dpr;
    ctx.clearRect(0, 0, w, h);
    var cx = w / 2, cy = h - 10 * dpr, maxR = Math.max(20, Math.min(h - 16 * dpr, w / 2 - 6 * dpr));
    var dim = css("--dim");
    var scale = maxR / Math.max(viewRangeM, 1), W2C = function (x, y) { return [cx + x * scale, cy - y * scale]; };
    ctx.font = (11 * dpr) + "px ui-monospace, monospace"; ctx.textAlign = "left";

    /* rings + labels (100 m ring shown amber when it lands on a ring) */
    for (var i = 1; i <= 4; i++) {
      var ringM = viewRangeM * i / 4, ref = Math.abs(ringM - 100) < 0.5;
      ctx.strokeStyle = ref ? "rgba(193,161,115,0.6)" : "rgba(150,157,168,0.16)"; ctx.lineWidth = (ref ? 1.4 : 1) * dpr;
      ctx.beginPath(); ctx.arc(cx, cy, maxR * i / 4, Math.PI, 2 * Math.PI); ctx.stroke();
      ctx.fillStyle = ref ? "rgba(216,189,144,0.9)" : "rgba(170,175,185,0.55)";
      ctx.fillText(ringM.toFixed(0) + " m", cx + 5 * dpr, cy - maxR * i / 4 + 13 * dpr);
    }
    /* FOV wedge from the daemon's live fov_half_deg (tracks the FOV knob) + boresight */
    var fovDeg = (radar && typeof radar.fov_half_deg === "number") ? radar.fov_half_deg : 90;
    var fr = fovDeg * Math.PI / 180;
    ctx.fillStyle = "rgba(150,157,168,0.06)"; ctx.beginPath(); ctx.moveTo(cx, cy);
    ctx.arc(cx, cy, maxR, -Math.PI / 2 - fr, -Math.PI / 2 + fr, false); ctx.closePath(); ctx.fill();
    ctx.strokeStyle = "rgba(150,157,168,0.24)"; ctx.lineWidth = dpr; ctx.setLineDash([4 * dpr, 4 * dpr]);
    [-1, 1].forEach(function (s) { var a = -Math.PI / 2 + s * fr; ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx + Math.cos(a) * maxR, cy + Math.sin(a) * maxR); ctx.stroke(); });
    ctx.setLineDash([]);
    ctx.strokeStyle = "rgba(150,157,168,0.5)"; ctx.lineWidth = 1.5 * dpr;
    ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx, cy - maxR); ctx.stroke(); ctx.lineWidth = dpr;

    if (!radar || !radar.connected) { ctx.globalAlpha = 0.6; ctx.fillStyle = dim; ctx.textAlign = "center"; ctx.fillText("NOT CONNECTED", cx, cy - maxR * 0.45); ctx.textAlign = "left"; ctx.globalAlpha = 1; radarGeom = null; return; }
    radarGeom = { cx: cx, cy: cy, scale: scale, dpr: dpr };

    /* raw returns — already gated server-side by the daemon (snr/speed/fov). v = +approaching */
    (radar.points || []).forEach(function (p) {
      var pc = W2C(p.x, p.y);
      ctx.fillStyle = pointStyle(p.v, p.snr);
      ctx.beginPath(); ctx.arc(pc[0], pc[1], 2 * dpr, 0, 2 * Math.PI); ctx.fill();
    });

    /* target boxes — daemon style, plus hold+fade persistence and the engaged LOCK */
    var now = Date.now();
    ctx.font = (11 * dpr) + "px ui-monospace, monospace";
    Object.keys(held).forEach(function (tid) {
      var hh = held[tid], t = hh.t, fade = Math.max(0, 1 - (now - hh.ts) / HOLD_MS);
      if (fade <= 0.02) return;
      var tc = W2C(t.x, t.y), locked = (t.tid === engagedTid);
      var col = locked ? css("--on") : tcolor(t.tid);
      var wpx = Math.max(6 * dpr, 2 * t.sx * scale), hpx = Math.max(6 * dpr, 2 * t.sy * scale);
      ctx.globalAlpha = fade; ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = (locked ? 2.2 : 1.5) * dpr;
      if (locked) {
        var cc = 8 * dpr, x0 = tc[0] - wpx / 2, y0 = tc[1] - hpx / 2;
        [[x0, y0, 1, 1], [x0 + wpx, y0, -1, 1], [x0, y0 + hpx, 1, -1], [x0 + wpx, y0 + hpx, -1, -1]].forEach(function (c) { ctx.beginPath(); ctx.moveTo(c[0], c[1] + c[3] * cc); ctx.lineTo(c[0], c[1]); ctx.lineTo(c[0] + c[2] * cc, c[1]); ctx.stroke(); });
      } else ctx.strokeRect(tc[0] - wpx / 2, tc[1] - hpx / 2, wpx, hpx);
      var vc = W2C(t.x + t.vx, t.y + t.vy); ctx.beginPath(); ctx.moveTo(tc[0], tc[1]); ctx.lineTo(vc[0], vc[1]); ctx.stroke();
      var spd = Math.hypot(t.vx, t.vy);
      ctx.fillText((locked ? "LOCK #" : "R#") + t.tid + "  " + spd.toFixed(1) + " m/s · " + t.rng.toFixed(0) + " m", tc[0] - wpx / 2 + 2 * dpr, tc[1] - hpx / 2 - 3 * dpr);
      ctx.globalAlpha = 1;
    });
  }

  function redrawAll() { drawEO(); drawRadar(lastRadar); }
  window.addEventListener("resize", redrawAll);

  /* ── manual selection: tap the EO (project click az) or the radar target ── */
  $("eo").addEventListener("click", function (e) {
    if (trackMode !== "man" || e.target.closest("#cluster") || e.target.closest("#zoombar")) return;
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
  /* tap a target-list row → select it (switches TRACK to MANUAL) */
  $("tgt-list").addEventListener("click", function (e) {
    var li = e.target.closest("[data-tid]"); if (!li) return;
    if (trackMode !== "man") setTrack("man");
    engage(parseInt(li.dataset.tid, 10));
  });

  /* ── ZULU ── */
  function zulu() { var d = new Date(), p = function (n) { return (n < 10 ? "0" : "") + n; }; $("v-zulu").textContent = p(d.getUTCHours()) + ":" + p(d.getUTCMinutes()); }
  setInterval(zulu, 1000); zulu();

  /* ── polls ── */
  function num(v, dp, suf) { return (v === null || v === undefined) ? "—" : v.toFixed(dp) + (suf || ""); }
  function idle(el) { return document.activeElement !== el; }
  /* 4-bar signal icon from RSSI (dBm), phone-style: >-55 great, -65 good, -72 fair, -82 weak */
  function signalSVG(rssi) {
    if (rssi === null || rssi === undefined) return "";
    var bars = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -72 ? 2 : rssi >= -82 ? 1 : 0;
    var col = bars >= 3 ? css("--on") : bars === 2 ? css("--amber") : css("--err");
    var hs = [4, 7, 10, 13], s = "";
    for (var i = 0; i < 4; i++)
      s += '<rect x="' + (i * 4) + '" y="' + (13 - hs[i]) + '" width="3" height="' + hs[i] + '" rx="0.6" fill="' + (i < bars ? col : css("--bd3")) + '"/>';
    return '<svg width="15" height="13" viewBox="0 0 15 13" aria-label="signal">' + s + '</svg>';
  }

  function poll() {
    fetch("/stats").then(function (r) { return r.json(); }).then(function (d) {
      lastStats = d;
      var eo = d.eo || {};                              /* the EO feed's own /stats */
      var eoc = !!d.eo_connected;                        /* EO feed up + delivering? */
      var hfov = (typeof eo.hfov === "number") ? eo.hfov : null;
      $("eo-scrim").hidden = eoc; $("eo").classList.toggle("hide-video", !eoc);
      /* link chip: signal bars (wifi) · type · live Mb/s · delivered fps */
      $("v-ltype").textContent = d.link_type ? d.link_type.toUpperCase() : "LINK";
      $("v-link").textContent = num(d.mbps, 1) + " Mb/s";
      $("v-txfps").innerHTML = (eoc && d.tx_fps != null) ? "&nbsp;·&nbsp;" + Math.round(d.tx_fps) + " fps" : "";
      $("v-sig").innerHTML = signalSVG(d.rssi_dbm);
      $("p-link").classList.add("on");   /* steady green while connected — catch() clears it if the poll fails */
      $("v-batt").textContent = num(d.batt, 0, "%"); $("v-alt").textContent = num(d.alt, 0);
      /* live EO telemetry on the EO display: EFFECTIVE resolution (real sensor detail in
       * view) + zoom + FOV on line 1; sensor fps/exposure/gain on line 2. Prefer the feed's
       * own eff_w/eff_h (authoritative = min(stream size, native/zoom)); a "·lim" flag marks
       * when the sensor crop (not your quality pick) is the limit. */
      var z = eo.zoom || 1, resStr = "—", lim = "";
      if (typeof eo.eff_w === "number" && typeof eo.eff_h === "number") {
        resStr = eo.eff_w + "×" + eo.eff_h;
        if (eo.dw && eo.eff_w < eo.dw) lim = " ·lim";     /* crop-limited, not quality-limited */
      } else if (eo.dw && eo.dh) {
        resStr = Math.min(eo.dw, Math.round(1440 / z)) + "×" + Math.min(eo.dh, Math.round(1088 / z));
      }
      $("eo-tl").textContent = "EO · " + resStr + lim + " · " + z.toFixed(1) + "× · FOV " + num(hfov, 1, "°") + "\n"
        + num(eo.fps, 0, " fps") + " · exp " + num(eo.exp_ms, 1, " ms") + " · duty " + num(eo.duty_pct, 0, "%") + " · gain " + num(eo.gain, 0) + (eo.ae ? " · AUTO" : " · MAN");
      $("eo-tr").textContent = "BRG " + (d.brg === null ? "—" : num(d.brg, 0, "°")) + "  RNG " + (d.rng === null ? "—" : num(d.rng, 2, " km"));
      $("v-cpu").textContent = num(d.cpu_c, 0); $("v-cam").textContent = "—";
      if (typeof eo.zoom === "number" && ZOOMS.indexOf(eo.zoom) >= 0 && Date.now() - zoomTouch > 1200) { zoom = eo.zoom; setZoomLabel(); }
      updateISP(eo);
      var light = $("light");
      if (Date.now() - lightTouch > 1200) { light.classList.toggle("firing", !!eo.laser); $("light-s").textContent = eo.laser ? "ON" : "OFF"; }
      light.style.opacity = eo.lpresent ? "1" : ".5";
      if (illumMode === "man") {
        /* MANUAL: the sliders are the source of truth — never write them back from
         * /stats, or touching one would snap the other to a stale value. */
      } else {                                            /* AUTO: fit beam to camera FOV @ max */
        if (typeof eo.lfov === "number") $("o-fov").textContent = Math.round(eo.lfov) + "°";
        $("o-pow").textContent = "MAX";
        if (eo.lpresent && hfov && (Math.abs((eo.lfov || 0) - hfov) > 1 || (eo.lpower || 0) < 255)) { ctl("fov=" + hfov.toFixed(0)); ctl("power=255"); }
      }
    }).catch(function () { $("v-link").textContent = "—"; $("p-link").classList.remove("on"); });
  }

  function pollRadar() {
    fetch("/radar").then(function (r) { return r.json(); }).then(function (d) {
      lastRadar = d;
      /* radar Hz from the daemon's own frame_id + timestamp (accurate regardless of our poll rate) */
      if (d.connected && typeof d.frame_id === "number" && typeof d.timestamp === "number") {
        if (rLastFid !== null && d.frame_id > rLastFid && d.timestamp > rLastTs) {
          var inst = (d.frame_id - rLastFid) / (d.timestamp - rLastTs);
          rHz = rHz ? rHz * 0.7 + inst * 0.3 : inst;
        }
        rLastFid = d.frame_id; rLastTs = d.timestamp;
      }
      $("v-tracks").textContent = d.connected ? (Math.round(rHz) + " Hz · " + (d.num_targets || 0) + " TRK") : "no data";
      if (!d.connected) { held = {}; updateViewRange(null); engage(trackMode === "man" ? engagedTid : null); renderTargetList(null); drawRadar(d); drawEO(); return; }
      var cur = targets(d), now = Date.now(), present = {};
      cur.forEach(function (t) { held[t.tid] = { t: t, ts: now }; present[t.tid] = 1; });
      Object.keys(held).forEach(function (tid) { if (!present[tid] && now - held[tid].ts > HOLD_MS) delete held[tid]; });
      updateViewRange(d);
      if (trackMode === "auto") { var best = pickAuto(cur); engage(best ? best.tid : null); }
      else if (engagedTid !== null && !present[engagedTid]) engage(null);   /* engaged target gone */
      renderTargetList(d); drawRadar(d); drawEO();
    }).catch(function () {});
  }

  /* Reflect the EO feed's ISP state into the DEV panel (guarded so a poll never fights a
   * control the operator is actively touching). */
  function updateISP(eo) {
    var settled = Date.now() - ispTouch > 1500;
    if (typeof eo.res === "string") { var rb = document.querySelector('#res-btns [data-res="' + eo.res + '"]'); if (rb) setSeg("res-btns", rb); }
    if (typeof eo.fps_cap === "number" && idle($("s-fps")) && Date.now() - fpsTouch > 1500) { $("s-fps").value = eo.fps_cap; $("o-fps").textContent = eo.fps_cap; }
    if (typeof eo.ae === "number" && settled) {
      var b = document.querySelector('#ae-btns [data-ae="' + (eo.ae ? 1 : 0) + '"]'); if (b) setSeg("ae-btns", b);
      setExpMode(!!eo.ae);
    }
    if (typeof eo.exp_ms === "number" && idle($("s-exp")) && settled) { $("s-exp").value = eo.exp_ms; $("o-exp").textContent = eo.exp_ms.toFixed(2); }
    if (typeof eo.gain === "number" && idle($("s-gain")) && settled) { $("s-gain").value = eo.gain; $("o-gain").textContent = eo.gain; }
    if (typeof eo.gaincap === "number" && idle($("s-gcap"))) { $("s-gcap").value = eo.gaincap; $("o-gcap").textContent = eo.gaincap; }
    if (typeof eo.median === "number") { var m = document.querySelector('#md-btns [data-md="' + (eo.median ? 1 : 0) + '"]'); if (m) setSeg("md-btns", m); }
  }

  setTrack("auto"); setIllum("auto"); setExpMode(true); applyTheme();
  setInterval(poll, 160); poll();
  setInterval(pollRadar, 120); pollRadar();
  setInterval(pollRstats, 400); pollRstats();
})();
