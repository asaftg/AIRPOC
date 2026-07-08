/* AIRPOC operator console — polling + controls + canvas overlays. No websockets, no
 * deps. Video = MJPEG <img>; telemetry = /stats + /radar (poll); controls = /ctl.
 * Tracking: AUTO picks the most important target (fused > nearer > higher conf);
 * MANUAL selects the target you tap. Illuminator: AUTO fits the beam to the EO FOV at
 * max power; MANUAL uses the DEV sliders. Visual language = Seeker bench. */
(function () {
  "use strict";
  var $ = function (id) { return document.getElementById(id); };
  var ctl = function (qs) { if (replaying) return; fetch("/ctl?" + qs).catch(function () {}); };  /* zero live /ctl in replay */
  var ZOOMS = [1, 2, 4, 8];
  var css = function (n) { return getComputedStyle(document.body).getPropertyValue(n).trim(); };

  var zoom = 1;
  var trackMode = "auto", engagedTid = null, sentEngage = null;
  var illumMode = "auto";
  var lastStats = {}, lastRadar = null;
  var rHz = 0, rLastFid = null, rLastTs = 0;   /* radar frame-rate (Hz) from frame_id/timestamp deltas */
  /* poll endpoints — swapped to /rec/replay/* in replay so the same renderers show recordings */
  var API = { stream: "/stream", radar: "/radar", stats: "/stats", rstats: "/rstats" };
  var replaying = false;

  /* ── theme / dev / swap ── */
  var theme = localStorage.getItem("airpoc-theme") || "night";
  function applyTheme() { document.body.className = theme + (trackMode === "man" ? " manual" : ""); $("theme").textContent = theme.toUpperCase(); redrawAll(); }
  $("theme").onclick = function () { theme = theme === "day" ? "night" : "day"; localStorage.setItem("airpoc-theme", theme); applyTheme(); };
  /* DEV squeezes the layout (it's in-flow, not an overlay) — refit the canvases after the
   * width transition so the EO overlay + radar scope match their new sizes. */
  function syncDev() { $("stage").classList.toggle("devopen", $("dev").classList.contains("open")); setTimeout(redrawAll, 180); }
  $("devbtn").onclick = function () { $("dev").classList.toggle("open"); syncDev(); };
  $("devclose").onclick = function () { $("dev").classList.remove("open"); syncDev(); };
  document.querySelectorAll("[data-exp]").forEach(function (b) { b.onclick = function (e) { e.stopPropagation(); $("stage").classList.toggle("rbig"); setTimeout(redrawAll, 20); }; });

  /* ── zoom ── the EO feed owns digital zoom; we forward zoom=N and drive the readout
   * optimistically. poll() reconciles from eo.zoom but is guarded for ~1.2s after a tap
   * so a slightly-stale /stats can't snap the label back mid-interaction. ── */
  var zoomTouch = 0, replayZoom = 1;
  function setZoomLabel() { $("v-zval").textContent = (replaying ? replayZoom : zoom).toFixed(1) + "×"; }
  /* replay has no live feed to crop, so zoom is a client-side digital zoom (CSS scale) on
   * the recorded frame — you can magnify even though it was recorded at 1x. */
  function applyReplayZoom() {
    var v = $("video");
    v.style.transform = replayZoom > 1 ? "scale(" + replayZoom + ")" : "";
    if (replayZoom <= 1) v.style.transformOrigin = "center";
    setZoomLabel();
  }
  function resetReplayZoom() { replayZoom = 1; var v = $("video"); v.style.transform = ""; v.style.transformOrigin = "center"; }
  document.querySelectorAll("[data-zoom]").forEach(function (b) {
    b.onclick = function () {
      if (replaying) {
        var ri = ZOOMS.indexOf(replayZoom) + parseInt(b.dataset.zoom, 10);
        if (ri < 0 || ri >= ZOOMS.length) return;
        replayZoom = ZOOMS[ri]; applyReplayZoom(); return;
      }
      var i = ZOOMS.indexOf(zoom) + parseInt(b.dataset.zoom, 10);
      if (i < 0 || i >= ZOOMS.length) return;
      zoom = ZOOMS[i]; zoomTouch = Date.now(); ctl("zoom=" + zoom); setZoomLabel();
    };
  });
  /* click the zoomed replay image to recenter the zoom on that point */
  $("video").addEventListener("click", function (e) {
    if (roiArm || !replaying || replayZoom <= 1) return;
    var r = this.getBoundingClientRect();
    this.style.transformOrigin = ((e.clientX - r.left) / r.width * 100).toFixed(1) + "% "
                               + ((e.clientY - r.top) / r.height * 100).toFixed(1) + "%";
  });

  /* ── ROI zoom — press ROI, drag a box on the EO or radar, it zooms there; press again to
   * reset. EO = CSS scale on the frame; radar = a pan+zoom world window (drawRadar). Works
   * live and in replay. ── */
  var roiArm = false, radarROI = null, eoROI = false, roiDrag = null;
  function setRoiUI() {
    $("roibtn").classList.toggle("on", roiArm || !!radarROI || eoROI);
    document.body.classList.toggle("roiarm", roiArm);
  }
  function clearROI() {
    radarROI = null; eoROI = false; roiArm = false;
    $("video").style.transform = ""; $("video").style.transformOrigin = "center";
    if (replaying) applyReplayZoom();               /* restore any replay zoom level */
    setRoiUI(); drawRadar(lastRadar);
  }
  $("roibtn").onclick = function () {
    if (roiArm) { roiArm = false; setRoiUI(); return; }       /* cancel arm */
    if (radarROI || eoROI) { clearROI(); return; }            /* reset to full view */
    roiArm = true; setRoiUI();                                /* arm: draw a box next */
  };
  function roiMove(e) {
    if (!roiDrag) return;
    var b = $("roibox"), x = Math.min(e.clientX, roiDrag.x0), y = Math.min(e.clientY, roiDrag.y0);
    b.style.left = x + "px"; b.style.top = y + "px";
    b.style.width = Math.abs(e.clientX - roiDrag.x0) + "px"; b.style.height = Math.abs(e.clientY - roiDrag.y0) + "px";
  }
  function roiStart(el, which, e) {
    if (!roiArm || e.button !== 0) return;
    roiDrag = { which: which, r: el.getBoundingClientRect(), x0: e.clientX, y0: e.clientY };
    $("roibox").style.display = "block"; roiMove(e); e.preventDefault();
  }
  function roiEnd(e) {
    if (!roiDrag) return;
    var d = roiDrag; roiDrag = null; $("roibox").style.display = "none";
    var bw = Math.abs(e.clientX - d.x0), bh = Math.abs(e.clientY - d.y0), r = d.r;
    roiArm = false;
    if (bw >= 14 && bh >= 14) {
      var bx0 = Math.min(e.clientX, d.x0) - r.left, by0 = Math.min(e.clientY, d.y0) - r.top;
      if (d.which === "eo") {
        var sc = Math.min(r.width / bw, r.height / bh), v = $("video");
        v.style.transformOrigin = ((bx0 + bw / 2) / r.width * 100).toFixed(1) + "% " + ((by0 + bh / 2) / r.height * 100).toFixed(1) + "%";
        v.style.transform = "scale(" + sc.toFixed(3) + ")"; eoROI = true;
      } else if (radarGeom) {
        var g = radarGeom, toW = function (sx, sy) { return [(sx - g.cx) / g.scale, (g.cy - sy) / g.scale]; };
        var c1 = toW(bx0 * g.dpr, by0 * g.dpr), c2 = toW((bx0 + bw) * g.dpr, (by0 + bh) * g.dpr);
        radarROI = { x0: Math.min(c1[0], c2[0]), x1: Math.max(c1[0], c2[0]), y0: Math.min(c1[1], c2[1]), y1: Math.max(c1[1], c2[1]) };
        drawRadar(lastRadar);
      }
    }
    setRoiUI();
  }
  $("eo").addEventListener("mousedown", function (e) { roiStart($("eo"), "eo", e); });
  $("radar-cv").parentElement.addEventListener("mousedown", function (e) { roiStart($("radar-cv").parentElement, "radar", e); });
  window.addEventListener("mousemove", roiMove);
  window.addEventListener("mouseup", roiEnd);

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
    var b = document.querySelector('#illum-btns [data-illum="' + m + '"]'); if (b) setSeg("illum-btns", b);
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
  document.querySelectorAll("#illum-btns [data-illum]").forEach(function (b) { b.onclick = function () { setIllum(b.dataset.illum); }; });

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
    { key: "minpts",  stat: "cluster_min_pts",  fmt: function (v) { return String(v | 0); } },
    /* temporal-tracker knobs (daemon 94af558): M-of-N confirm, dropout coast, park hold */
    { key: "confirm", stat: "confirm",          fmt: function (v) { return String(v | 0); } },
    { key: "coast",   stat: "coast_s",          fmt: function (v) { return v.toFixed(1) + " s"; } },
    { key: "park",    stat: "park_s",           fmt: function (v) { return v.toFixed(0) + " s"; } }
  ];
  var rcTouch = 0;
  RADARC.forEach(function (c) {
    $("rd-" + c.key).oninput = function () { $("rv-" + c.key).textContent = c.fmt(parseFloat(this.value)); rcTouch = Date.now(); ctl("radar_" + c.key + "=" + this.value); };
  });
  setTimeout(function () { if (!replaying) { rcTouch = Date.now(); ctl("radar_fov=60"); } }, 900);   /* default the radar FOV to ±60° on load */
  function pollRstats() {
    fetch(API.rstats).then(function (r) { return r.json(); }).then(function (d) {
      if (Date.now() - rcTouch < 1500) return;   /* don't fight an active drag */
      RADARC.forEach(function (c) {
        var v = d[c.stat];
        if (typeof v === "number" && document.activeElement !== $("rd-" + c.key)) { $("rd-" + c.key).value = v; $("rv-" + c.key).textContent = c.fmt(v); }
      });
    }).catch(function () {});
  }

  /* ── detector controls — the detection daemon's knobs (detection/docs/INTEGRATION.md).
   * Sent namespaced det_<key>= (the app strips the prefix → daemon :8094 /ctl); readback
   * comes from /dstats, where the applied values live under the nested "knobs" object. ── */
  var DETC = [
    { key: "conf",        fmt: function (v) { return v.toFixed(2); } },
    { key: "cadence",     fmt: function (v) { return String(v | 0); } },
    { key: "max_dets",    fmt: function (v) { return String(v | 0); } },
    { key: "mot_k",       fmt: function (v) { return v.toFixed(1); } },
    { key: "mot_persist", fmt: function (v) { return String(v | 0); } }
  ];
  var dtTouch = 0;
  DETC.forEach(function (c) {
    $("dt-" + c.key).oninput = function () { $("dv-" + c.key).textContent = c.fmt(parseFloat(this.value)); dtTouch = Date.now(); ctl("det_" + c.key + "=" + this.value); };
  });
  document.querySelectorAll("#mot-btns [data-mot]").forEach(function (b) {
    b.onclick = function () { dtTouch = Date.now(); ctl("det_motion=" + b.dataset.mot); setSeg("mot-btns", b); };
  });
  function pollDstats() {
    if (replaying) return;
    fetch("/dstats").then(function (r) { return r.json(); }).then(function (d) {
      var k = d.knobs || {};
      if (Date.now() - dtTouch < 1500) return;   /* don't fight an active drag */
      DETC.forEach(function (c) {
        var v = k[c.key];
        if (typeof v === "number" && document.activeElement !== $("dt-" + c.key)) { $("dt-" + c.key).value = v; $("dv-" + c.key).textContent = c.fmt(v); }
      });
      if (typeof k.motion === "number") { var mb = document.querySelector('#mot-btns [data-mot="' + k.motion + '"]'); if (mb) setSeg("mot-btns", mb); }
    }).catch(function () {});
  }

  /* ── reserved ── */
  $("to-control").onclick = function () { location.href = "http://" + location.hostname + ":8088/"; };  /* back to the START/STOP launcher */

  /* ── target model + selection (daemon schema: class-less objects) ── */
  function targets(radar) {
    if (!radar || !radar.targets) return [];
    return radar.targets.map(function (t) {
      return { tid: t.tid, x: t.x, y: t.y, z: t.z, vx: t.vx, vy: t.vy, sx: t.sx, sy: t.sy, sz: t.sz, conf: t.conf,
               rng: Math.hypot(t.x, t.y),
               az: Math.atan2(t.x, t.y) * 180 / Math.PI,                 /* azimuth from radar */
               el: Math.atan2(t.z || 0, Math.hypot(t.x, t.y)) * 180 / Math.PI };  /* elevation from radar z */
    });   /* no client filtering — the daemon gates points/clusters server-side now */
  }
  /* AUTO priority: fused (EO+radar) first [pending detector], then nearer, then conf. */
  function pickAuto(ts) { return ts.slice().sort(function (a, b) { return (a.rng - b.rng) || (b.conf - a.conf); })[0] || null; }

  /* Target list (top 5 by importance): fused > higher confidence > nearer. Fusion isn't
   * wired yet, so today this is radar-only (conf then range). No GUI-side row hold —
   * the daemon's tracker already confirms/coasts, so rows come straight from the frame. */
  function importance(t) { return (t.fused ? 2e6 : 0) + (t.conf || 0) * 1e3 - t.rng; }
  function renderTargetList(radar) {
    var rows = targets(radar).map(function (t) { return { t: t }; });
    rows.sort(function (a, b) { return importance(b.t) - importance(a.t); });
    rows = rows.slice(0, 5);
    $("v-tgtcount").textContent = rows.length;
    /* Always render 5 fixed slots so the panel never resizes as targets come and go. */
    var out = [];
    for (var i = 0; i < 5; i++) {
      if (i < rows.length) {
        var r = rows[i], t = r.t, col = tcolor(t.tid), spd = Math.hypot(t.vx, t.vy), az = t.az;
        var eng = (t.tid === engagedTid), cls = "tgt-row" + (eng ? " eng" : "");
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
  /* No GUI-side persistence: the radar daemon is a temporal tracker now (stable tids,
   * M-of-N confirm, coasting through dropouts, park-hold). "In the frame" already means
   * "present" — adding a hold+fade here would double-persist. Draw the wire verbatim. */
  function engage(tid) {
    engagedTid = tid;
    $("track-hint").hidden = (trackMode !== "man") || (tid !== null);
    if (tid !== sentEngage) { sentEngage = tid; ctl("engage=" + (tid === null ? -1 : tid)); }
  }

  /* ── radar→EO overlay settings — pure client-side render (not fusion). AZ/EL trim is
   * the radar↔camera mount alignment in degrees; persisted per browser. No stored rig
   * calibration exists yet (radar README: calibration is the consumer's job), so the
   * defaults are 0 — dial the trims until a mark sits on its real object. ── */
  var radarOv = { on: 1, az: 0, el: 0 };
  try { var rvs = JSON.parse(localStorage.getItem("radarOv") || "{}");
        if (typeof rvs.on === "number") radarOv.on = rvs.on;
        if (typeof rvs.az === "number") radarOv.az = rvs.az;
        if (typeof rvs.el === "number") radarOv.el = rvs.el; } catch (x) {}
  function saveRadarOv() { try { localStorage.setItem("radarOv", JSON.stringify(radarOv)); } catch (x) {} }
  function initRadarOv() {
    var b = document.querySelector('#rov-btns [data-rov="' + radarOv.on + '"]'); if (b) setSeg("rov-btns", b);
    $("rov-az").value = radarOv.az; $("rovv-az").textContent = radarOv.az.toFixed(1) + "°";
    $("rov-el").value = radarOv.el; $("rovv-el").textContent = radarOv.el.toFixed(1) + "°";
  }
  document.querySelectorAll("#rov-btns [data-rov]").forEach(function (b) {
    b.onclick = function () { radarOv.on = parseInt(b.dataset.rov, 10); setSeg("rov-btns", b); saveRadarOv(); drawEO(); };
  });
  $("rov-az").oninput = function () { radarOv.az = parseFloat(this.value); $("rovv-az").textContent = radarOv.az.toFixed(1) + "°"; saveRadarOv(); drawEO(); };
  $("rov-el").oninput = function () { radarOv.el = parseFloat(this.value); $("rovv-el").textContent = radarOv.el.toFixed(1) + "°"; saveRadarOv(); drawEO(); };

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

    /* Radar → EO overlay: EVERY radar target projected onto the video from its azimuth
     * (→ x, via camera hfov) and elevation (→ y, via camera vfov), plus the operator's
     * AZ/EL trim (radar↔camera mount alignment). Not fusion — a raw geometric overlay.
     * Bracket size = the target's real angular extent at its range. The engaged target
     * is green and keeps an off-frame edge arrow; the rest clip at the video edge.
     * Works in replay too — both video and radar come from the recording there. */
    var es = lastStats.eo || {};
    var eoHfov = es.hfov || 0, eoVfov = es.vfov || (eoHfov * 0.75);   /* 4:3 fallback */
    if (radarOv.on && eoHfov && lastRadar && lastRadar.connected) {
      var sw2 = es.dw || 4, sh2 = es.dh || 3, ar2 = sw2 / sh2, vw2, vh2, vx2, vy2;
      if (w / h > ar2) { vh2 = h; vw2 = h * ar2; vx2 = (w - vw2) / 2; vy2 = 0; }
      else { vw2 = w; vh2 = w / ar2; vx2 = 0; vy2 = (h - vh2) / 2; }
      ctx.font = (10 * dpr) + "px ui-monospace, monospace";
      targets(lastRadar).forEach(function (t) {
        var az = t.az + radarOv.az, el = t.el + radarOv.el;
        var fx = az / (eoHfov / 2), fy = -el / (eoVfov / 2);   /* -1..1 within frame; +el = up */
        var locked = (t.tid === engagedTid);
        var col = locked ? css("--on") : tcolor(t.tid);
        if (Math.abs(fx) <= 1 && Math.abs(fy) <= 1) {
          var lx = vx2 + (fx + 1) / 2 * vw2, ly = vy2 + (fy + 1) / 2 * vh2;
          /* fixed-size ring + centre dot — the tracker's size estimates (sx/sy) jitter
           * frame-to-frame, so size-coding made the marks pulse; position is stable. */
          var rr = (locked ? 18 : 14) * dpr;
          ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = (locked ? 2 : 1.4) * dpr;
          ctx.beginPath(); ctx.arc(lx, ly, rr, 0, 2 * Math.PI); ctx.stroke();
          if (locked) { ctx.beginPath(); ctx.arc(lx, ly, rr + 4 * dpr, 0, 2 * Math.PI); ctx.stroke(); }
          ctx.beginPath(); ctx.arc(lx, ly, 1.8 * dpr, 0, 2 * Math.PI); ctx.fill();
          ctx.fillText((locked ? "LOCK R#" : "R#") + t.tid + " " + t.rng.toFixed(0) + " m", lx + rr + 4 * dpr, ly - 4 * dpr);
        } else if (locked) {   /* engaged target off-frame → edge arrow pointing at it */
          var ex = cx + Math.max(-1, Math.min(1, fx)) * (w / 2 - 24 * dpr);
          var ey = cy + Math.max(-1, Math.min(1, fy)) * (h / 2 - 24 * dpr);
          ctx.fillStyle = col; ctx.globalAlpha = 0.9;
          ctx.save(); ctx.translate(ex, ey); ctx.rotate(Math.atan2(fy, fx));
          ctx.beginPath(); ctx.moveTo(15 * dpr, 0); ctx.lineTo(-11 * dpr, -10 * dpr); ctx.lineTo(-11 * dpr, 10 * dpr); ctx.closePath(); ctx.fill();
          ctx.restore(); ctx.globalAlpha = 1;
        }
      });
    }

    /* EO detector boxes — px = [cx,cy,w,h] in the NATIVE frame (msg.img, 1440x1088).
     * The display shows a centered 1/zoom crop of native, letterboxed into the panel
     * (object-fit: contain) — map native -> crop -> content rect. dets[] solid with
     * class+conf; movers[] dashed class-less. Live-only (guarded off in replay). */
    if (!replaying && lastDet && (lastDet.dets || lastDet.movers)) {
      var im = lastDet.img || { w: 1440, h: 1088 };
      var z = es.zoom || 1;
      var cw = im.w / z, ch = im.h / z, ox = (im.w - cw) / 2, oy = (im.h - ch) / 2;
      var sw = es.dw || cw, sh = es.dh || ch, ar = sw / sh;   /* streamed frame sets the letterbox */
      var vw, vh, vx, vy;
      if (w / h > ar) { vh = h; vw = h * ar; vx = (w - vw) / 2; vy = 0; }
      else { vw = w; vh = w / ar; vx = 0; vy = (h - vh) / 2; }
      ctx.save(); ctx.beginPath(); ctx.rect(vx, vy, vw, vh); ctx.clip();
      ctx.font = (10 * dpr) + "px ui-monospace, monospace";
      var drawDet = function (b, dashed, col, label) {
        if (!b.px || b.px.length < 4) return;
        var bx = vx + (b.px[0] - ox) / cw * vw, by = vy + (b.px[1] - oy) / ch * vh;
        var bw2 = b.px[2] / cw * vw, bh2 = b.px[3] / ch * vh;
        if (bx + bw2 / 2 < vx || bx - bw2 / 2 > vx + vw || by + bh2 / 2 < vy || by - bh2 / 2 > vy + vh) return;
        ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = 1.6 * dpr;
        ctx.setLineDash(dashed ? [5 * dpr, 4 * dpr] : []);
        ctx.strokeRect(bx - bw2 / 2, by - bh2 / 2, bw2, bh2);
        ctx.setLineDash([]);
        ctx.fillText(label, bx - bw2 / 2 + 2 * dpr, by - bh2 / 2 - 3 * dpr);
      };
      (lastDet.dets || []).forEach(function (d) {
        var col = d.cls === "human" ? "#40c4ff" : amber;
        drawDet(d, false, col, (d.cls || "?").toUpperCase() + " " + Math.round((d.conf || 0) * 100) + "%");
      });
      (lastDet.movers || []).forEach(function (mv) {
        drawDet(mv, true, "rgba(150,157,168,0.9)", "MOV·" + (mv.age || 0));
      });
      ctx.restore();
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
  var viewRangeM = 100, shrinkCtr = 0, SHRINK_FRAMES = 25, radarRangeMode = "auto";
  function updateViewRange(radar) {
    if (radarRangeMode !== "auto") { viewRangeM = radarRangeMode; return; }   /* fixed 50/100/250/500 m */
    var maxTR = 0;
    (radar && radar.targets || []).forEach(function (t) { var r = Math.hypot(t.x || 0, t.y || 0); if (r > maxTR) maxTR = r; });
    var want = maxTR > 250 ? 500 : (maxTR > 100 ? 250 : 100);
    if (want > viewRangeM) { viewRangeM = want; shrinkCtr = 0; }
    else if (want < viewRangeM) { if (++shrinkCtr >= SHRINK_FRAMES) { viewRangeM = want; shrinkCtr = 0; } }
    else shrinkCtr = 0;
  }
  document.querySelectorAll("#rrange [data-rng]").forEach(function (b) {
    b.onclick = function () {
      radarRangeMode = b.dataset.rng === "auto" ? "auto" : parseInt(b.dataset.rng, 10);
      document.querySelectorAll("#rrange [data-rng]").forEach(function (x) { x.classList.toggle("on", x === b); });
      updateViewRange(lastRadar); drawRadar(lastRadar);
    };
  });

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
    var maxR0 = Math.max(20, Math.min(h - 16 * dpr, w / 2 - 6 * dpr));
    var dim = css("--dim");
    /* view mapping. default = forward half-circle from bottom-centre. radarROI (a drawn
     * world rectangle) pans+zooms: cx,cy = screen pos of the radar origin (0,0). */
    var cx, cy, scale, Reff;
    if (radarROI) {
      var rww = Math.max(radarROI.x1 - radarROI.x0, 1), rhh = Math.max(radarROI.y1 - radarROI.y0, 1);
      scale = Math.min((w - 8 * dpr) / rww, (h - 8 * dpr) / rhh);
      var mx = (radarROI.x0 + radarROI.x1) / 2, my = (radarROI.y0 + radarROI.y1) / 2;
      cx = w / 2 - mx * scale; cy = h / 2 + my * scale;
      Reff = Math.max(Math.hypot(radarROI.x0, radarROI.y0), Math.hypot(radarROI.x1, radarROI.y1),
                      Math.hypot(radarROI.x0, radarROI.y1), Math.hypot(radarROI.x1, radarROI.y0));
    } else {
      cx = w / 2; cy = h - 10 * dpr; scale = maxR0 / Math.max(viewRangeM, 1); Reff = viewRangeM;
    }
    var maxR = radarROI ? Math.hypot(w, h) : maxR0;                 /* wedge/boresight ray length */
    var W2C = function (x, y) { return [cx + x * scale, cy - y * scale]; };
    ctx.font = (11 * dpr) + "px ui-monospace, monospace"; ctx.textAlign = "left";

    /* range grid — grey rings at a nice metric step, metre-labelled */
    var step = [10, 25, 50, 100, 250, 500, 1000].filter(function (s) { return s >= Reff / 4.5; })[0] || 1000;
    for (var rm = step; rm <= Reff * 1.02; rm += step) {
      var gr = rm * scale;
      ctx.strokeStyle = "rgba(150,157,168,0.16)"; ctx.lineWidth = 1 * dpr;
      ctx.beginPath(); ctx.arc(cx, cy, gr, Math.PI, 2 * Math.PI); ctx.stroke();
      ctx.fillStyle = "rgba(170,175,185,0.55)"; ctx.fillText(rm + " m", cx + 5 * dpr, cy - gr + 13 * dpr);
    }
    /* constant amber reference rings at 100 m + 250 m on every zoom (when in range) */
    [100, 250].forEach(function (m) {
      if (m > Reff * 1.02) return;
      var rr = m * scale;
      ctx.strokeStyle = "rgba(193,161,115,0.65)"; ctx.lineWidth = 1.4 * dpr;
      ctx.beginPath(); ctx.arc(cx, cy, rr, Math.PI, 2 * Math.PI); ctx.stroke();
      ctx.fillStyle = "rgba(216,189,144,0.95)"; ctx.textAlign = "right";
      ctx.fillText(m + " m", cx - 6 * dpr, cy - rr + 13 * dpr); ctx.textAlign = "left";
    });
    /* FOV wedge from the daemon's live fov_half_deg (tracks the FOV knob) + boresight */
    var fovDeg = (radar && typeof radar.fov_half_deg === "number") ? radar.fov_half_deg : 90;
    var fr = fovDeg * Math.PI / 180;
    var inFov = function (x, y) { return Math.abs(Math.atan2(x, y)) <= fr; };   /* azimuth within ±FOV */
    ctx.fillStyle = "rgba(150,157,168,0.06)"; ctx.beginPath(); ctx.moveTo(cx, cy);
    ctx.arc(cx, cy, maxR, -Math.PI / 2 - fr, -Math.PI / 2 + fr, false); ctx.closePath(); ctx.fill();
    ctx.strokeStyle = "rgba(150,157,168,0.24)"; ctx.lineWidth = dpr; ctx.setLineDash([4 * dpr, 4 * dpr]);
    [-1, 1].forEach(function (s) { var a = -Math.PI / 2 + s * fr; ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx + Math.cos(a) * maxR, cy + Math.sin(a) * maxR); ctx.stroke(); });
    ctx.setLineDash([]);
    ctx.strokeStyle = "rgba(150,157,168,0.5)"; ctx.lineWidth = 1.5 * dpr;
    ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx, cy - maxR); ctx.stroke(); ctx.lineWidth = dpr;

    if (!radar || !radar.connected) { ctx.globalAlpha = 0.6; ctx.fillStyle = dim; ctx.textAlign = "center"; ctx.fillText((replaying && !replayHasRadar) ? "NO RADAR RECORDED" : "NOT CONNECTED", cx, cy - maxR * 0.45); ctx.textAlign = "left"; ctx.globalAlpha = 1; radarGeom = null; return; }
    radarGeom = { cx: cx, cy: cy, scale: scale, dpr: dpr };

    /* raw returns — already gated server-side by the daemon (snr/speed/fov). v = +approaching */
    (radar.points || []).forEach(function (p) {
      if (!inFov(p.x, p.y)) return;                 /* don't draw returns outside the FOV */
      var pc = W2C(p.x, p.y);
      ctx.fillStyle = pointStyle(p.v, p.snr);
      ctx.beginPath(); ctx.arc(pc[0], pc[1], 2 * dpr, 0, 2 * Math.PI); ctx.fill();
    });

    /* target boxes — the daemon's tracker output drawn verbatim (it confirms/coasts/
     * dedups server-side; tids are stable) + the engaged LOCK */
    ctx.font = (11 * dpr) + "px ui-monospace, monospace";
    targets(radar).forEach(function (t) {
      if (!inFov(t.x, t.y)) return;                 /* hide targets outside the FOV */
      var tc = W2C(t.x, t.y), locked = (t.tid === engagedTid);
      var col = locked ? css("--on") : tcolor(t.tid);
      var wpx = Math.max(6 * dpr, 2 * t.sx * scale), hpx = Math.max(6 * dpr, 2 * t.sy * scale);
      ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = (locked ? 2.2 : 1.5) * dpr;
      if (locked) {
        var cc = 8 * dpr, x0 = tc[0] - wpx / 2, y0 = tc[1] - hpx / 2;
        [[x0, y0, 1, 1], [x0 + wpx, y0, -1, 1], [x0, y0 + hpx, 1, -1], [x0 + wpx, y0 + hpx, -1, -1]].forEach(function (c) { ctx.beginPath(); ctx.moveTo(c[0], c[1] + c[3] * cc); ctx.lineTo(c[0], c[1]); ctx.lineTo(c[0] + c[2] * cc, c[1]); ctx.stroke(); });
      } else ctx.strokeRect(tc[0] - wpx / 2, tc[1] - hpx / 2, wpx, hpx);
      var vc = W2C(t.x + t.vx, t.y + t.vy); ctx.beginPath(); ctx.moveTo(tc[0], tc[1]); ctx.lineTo(vc[0], vc[1]); ctx.stroke();
      var spd = Math.hypot(t.vx, t.vy);
      ctx.fillText((locked ? "LOCK #" : "R#") + t.tid + "  " + spd.toFixed(1) + " m/s · " + t.rng.toFixed(0) + " m", tc[0] - wpx / 2 + 2 * dpr, tc[1] - hpx / 2 - 3 * dpr);
    });
  }

  function redrawAll() { drawEO(); drawRadar(lastRadar); }
  window.addEventListener("resize", redrawAll);

  /* ── manual selection: tap the EO (project click az) or the radar target ── */
  $("eo").addEventListener("click", function (e) {
    if (roiArm || trackMode !== "man" || e.target.closest("#cluster") || e.target.closest("#zoombar")) return;
    var eoHfov = (lastStats.eo && lastStats.eo.hfov) || 0;
    var ts = targets(lastRadar); if (!ts.length || !eoHfov) return;
    var r = this.getBoundingClientRect(), frac = (e.clientX - r.left) / r.width - 0.5;
    var azClick = frac * eoHfov;
    engage(ts.slice().sort(function (a, b) { return Math.abs(a.az - azClick) - Math.abs(b.az - azClick); })[0].tid);
  });
  $("radar-cv").addEventListener("click", function (e) {
    if (roiArm || trackMode !== "man" || !radarGeom) return;
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
  function zulu() { if (replaying) return; var d = new Date(), p = function (n) { return (n < 10 ? "0" : "") + n; }; $("v-zulu").textContent = p(d.getUTCHours()) + ":" + p(d.getUTCMinutes()); }
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
    fetch(API.stats).then(function (r) { return r.json(); }).then(function (d) {
      lastStats = d;
      var eo = d.eo || {};                              /* the EO feed's own /stats */
      var eoc = !!d.eo_connected || !!(eo && eo.connected);   /* live top-level, or replay's nested eo.connected */
      if (replaying && !replayHasEO) eoc = false;             /* this session recorded no video */
      $("eo-scrim").textContent = (replaying && !replayHasEO) ? "EO · NO VIDEO RECORDED" : "EO · NOT CONNECTED";
      $("eo-tl").style.display = (replaying && !replayHasEO) ? "none" : "";   /* no video → hide the recorded EO status line */
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
      $("v-cpu").textContent = num(d.cpu_c, 0); $("v-cpupct").textContent = num(d.cpu_pct, 0); $("v-gpupct").textContent = num(d.gpu_pct, 0);
      var nc = d.ncpu || 6;
      $("v-cores").textContent = (typeof d.cpu_pct === "number") ? (d.cpu_pct / 100 * nc).toFixed(1) + "/" + nc : "";
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

  function onRadarFrame(d) {
    lastRadar = d;
    /* radar Hz from the daemon's own frame_id + timestamp (accurate regardless of rate) */
    if (d.connected && typeof d.frame_id === "number" && typeof d.timestamp === "number") {
      if (rLastFid !== null && d.frame_id > rLastFid && d.timestamp > rLastTs) {
        var inst = (d.frame_id - rLastFid) / (d.timestamp - rLastTs);
        rHz = rHz ? rHz * 0.7 + inst * 0.3 : inst;
      }
      rLastFid = d.frame_id; rLastTs = d.timestamp;
    }
    $("v-tracks").textContent = d.connected ? (Math.round(rHz) + " Hz · " + (d.num_targets || 0) + " TRK") : "no data";
    if (!d.connected) { updateViewRange(null); engage(trackMode === "man" ? engagedTid : null); renderTargetList(null); drawRadar(d); drawEO(); return; }
    var cur = targets(d), present = {};
    cur.forEach(function (t) { present[t.tid] = 1; });
    updateViewRange(d);
    if (trackMode === "auto") { var best = pickAuto(cur); engage(best ? best.tid : null); }
    else if (engagedTid !== null && !present[engagedTid]) engage(null);   /* engaged target gone */
    renderTargetList(d); drawRadar(d); drawEO();
  }
  /* Live radar is PUSHED over SSE (/radar/stream) at the sensor's native rate — no polling,
   * so the display matches the sensor instead of an 8 Hz sample. pollRadar is used ONLY in
   * replay, where frames come from the recorder over the replay endpoint. */
  var radarES = null;
  function openRadarStream() {
    if (radarES) { radarES.close(); radarES = null; }
    radarES = new EventSource("/radar/stream");
    radarES.onmessage = function (e) { if (replaying) return; try { onRadarFrame(JSON.parse(e.data)); } catch (x) {} };
    radarES.onerror = function () { if (!replaying) onRadarFrame({ connected: false }); };   /* auto-reconnects */
  }
  function pollRadar() {   /* replay only */
    if (!replaying) return;
    fetch(API.radar).then(function (r) { return r.json(); }).then(onRadarFrame).catch(function () {});
  }

  /* EO detector — SSE push from /det/stream (~15/s). Messages carry dets[] (classified
   * model boxes) + movers[] (motion-only); drawn over the video in drawEO. Live-only for
   * now (no replay channel yet). {"connected":false} heartbeat clears the boxes. */
  var lastDet = null, detES = null;
  function openDetStream() {
    if (detES) { detES.close(); detES = null; }
    detES = new EventSource("/det/stream");
    detES.onmessage = function (e) {
      if (replaying) return;
      try { var m = JSON.parse(e.data); lastDet = (m && m.connected === false) ? null : m; } catch (x) {}
      drawEO();
    };
    detES.onerror = function () { if (!replaying) { lastDet = null; drawEO(); } };   /* auto-reconnects */
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

  /* ═══════════════════════ RECORDER / REPLAY ═══════════════════════ */
  var TAGVOCAB = ["night", "day", "human", "vehicle", "drone", "long-range", "short-range", "radar", "tracking", "fusion", "illum", "test", "bug", "demo", "calibration"];
  var recState = null, pendingSid = null, libSel = {}, libTagFilter = {}, libSessions = [];
  var replaySession = null, replayStatePoll = null, scrubbing = false, scrubThrottle = 0;
  var replayHasEO = true, replayHasRadar = true;   /* per-session: was that channel recorded? */
  var replayPlaying = false, replayStillT = -1;    /* EO pane: MJPEG stream while playing, still frame while paused */
  var BLANK = "data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==";
  var RATES = [0.5, 1, 2, 4];

  function esc(s) { return String(s == null ? "" : s).replace(/[<>&"]/g, function (c) { return { "<": "&lt;", ">": "&gt;", "&": "&amp;", '"': "&quot;" }[c]; }); }
  function fmtClock(ms) { var s = Math.floor(Math.max(0, ms || 0) / 1000); return Math.floor(s / 60) + ":" + ("0" + (s % 60)).slice(-2); }
  function fmtClockT(ms) { var s = Math.max(0, ms || 0) / 1000; return Math.floor(s / 60) + ":" + ("0" + Math.floor(s % 60)).slice(-2) + "." + Math.floor((s * 10) % 10); }
  function localStamp() { var d = new Date(), p = function (n) { return ("0" + n).slice(-2); }; return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) + " " + p(d.getHours()) + ":" + p(d.getMinutes()); }
  function debounce(fn, ms) { var t; return function () { clearTimeout(t); t = setTimeout(fn, ms); }; }
  function rctl(q) { fetch("/rec/replay/ctl?" + q).catch(function () {}); }

  /* ── REC button + recorder health ── */
  function pollRec() {
    fetch("/rec/stats").then(function (r) { return r.json(); }).then(function (d) {
      recState = d;
      var rec = $("rec");
      if (!d || d.connected === false) { rec.classList.add("rec-off"); rec.classList.remove("rec-on"); rec.textContent = "REC"; rec.title = "RECORDER NOT CONNECTED"; return; }
      rec.classList.remove("rec-off"); rec.title = "";
      if (d.state === "recording") { rec.classList.add("rec-on"); rec.textContent = fmtClock(d.rec_elapsed_s * 1000); }
      else {
        rec.classList.remove("rec-on"); rec.textContent = "REC";
        if (d.pending_sid && !pendingSid && $("recdlg").hidden && !replaying) openSaveDialog(d.pending_sid);
      }
    }).catch(function () { var rec = $("rec"); rec.classList.add("rec-off"); rec.classList.remove("rec-on"); rec.textContent = "REC"; rec.title = "RECORDER NOT CONNECTED"; });
  }
  $("rec").onclick = function () {
    if (replaying || $("rec").classList.contains("rec-off")) return;
    if (recState && recState.state === "recording")
      fetch("/rec/ctl?rec=stop").then(function (r) { return r.json(); }).then(function (d) { if (d && d.sid) openSaveDialog(d.sid); }).catch(function () {});
    else fetch("/rec/ctl?rec=start").catch(function () {});
  };

  /* ── save dialog ── */
  function openSaveDialog(sid) {
    pendingSid = sid;
    $("dlg-name").value = "REC " + localStamp();
    $("dlg-note").value = "";
    var tw = $("dlg-tags"); tw.innerHTML = "";
    TAGVOCAB.forEach(function (t) { var c = document.createElement("span"); c.className = "tagchip"; c.textContent = t; c.onclick = function () { c.classList.toggle("on"); }; tw.appendChild(c); });
    $("recdlg").hidden = false; $("dlg-name").focus();
  }
  function closeSaveDialog() { $("recdlg").hidden = true; pendingSid = null; }
  $("dlg-x").onclick = closeSaveDialog;   /* dismiss leaves the session pending */
  /* type your own tag (not in the bank) → Enter adds it as a selected chip */
  $("dlg-tagadd").onkeydown = function (e) {
    if (e.key !== "Enter") return; e.preventDefault();
    var v = this.value.trim().toLowerCase().replace(/[\s,]+/g, "-").replace(/[^a-z0-9-]/g, "");
    this.value = ""; if (!v) return;
    var ex = [].slice.call(document.querySelectorAll("#dlg-tags .tagchip")).filter(function (c) { return c.textContent === v; })[0];
    if (ex) { ex.classList.add("on"); return; }
    var c = document.createElement("span"); c.className = "tagchip on"; c.textContent = v;
    c.onclick = function () { c.classList.toggle("on"); };
    $("dlg-tags").appendChild(c);
  };
  $("dlg-save").onclick = function () {
    if (!pendingSid) return;
    var tags = [].slice.call(document.querySelectorAll("#dlg-tags .tagchip.on")).map(function (c) { return encodeURIComponent(c.textContent); }).join(",");
    fetch("/rec/ctl?save=" + encodeURIComponent(pendingSid) + "&name=" + encodeURIComponent($("dlg-name").value) + "&tags=" + tags + "&note=" + encodeURIComponent($("dlg-note").value)).catch(function () {});
    closeSaveDialog(); if (!$("library").hidden) loadLibrary();
  };
  $("dlg-discard").onclick = function () {
    if (!pendingSid) return;
    if (!confirm("Discard this recording? It can't be recovered.")) return;
    fetch("/rec/ctl?discard=" + encodeURIComponent(pendingSid)).catch(function () {}); closeSaveDialog();
  };

  /* ── LIBRARY ── */
  $("libbtn").onclick = function () { $("library").hidden = false; buildTagFilter(); loadLibrary(); };
  $("lib-close").onclick = function () { $("library").hidden = true; libSel = {}; };
  $("lib-q").oninput = debounce(loadLibrary, 250);
  function buildTagFilter() {
    var w = $("lib-tagfilter"); if (w.childElementCount) return;
    TAGVOCAB.forEach(function (t) { var c = document.createElement("span"); c.className = "tagchip"; c.textContent = t; c.onclick = function () { c.classList.toggle("on"); libTagFilter[t] = c.classList.contains("on"); loadLibrary(); }; w.appendChild(c); });
  }
  function loadLibrary() {
    var tags = Object.keys(libTagFilter).filter(function (t) { return libTagFilter[t]; }).join(",");
    fetch("/rec/library?q=" + encodeURIComponent($("lib-q").value || "") + (tags ? "&tags=" + encodeURIComponent(tags) : ""))
      .then(function (r) { return r.json(); }).then(renderLibrary)
      .catch(function () { $("lib-grid").innerHTML = ""; $("lib-empty").hidden = false; $("lib-empty").textContent = "RECORDER NOT CONNECTED"; });
  }
  function renderLibrary(d) {
    if (d.disk_total_gb) {
      $("lib-diskbar").style.setProperty("--used", (100 * (1 - d.disk_free_gb / d.disk_total_gb)).toFixed(0) + "%");
      $("lib-disktext").textContent = Math.round(d.disk_free_gb) + " GB free" + (recState && recState.est_min_remaining ? " · ~" + recState.est_min_remaining + " min" : "");
    }
    var g = $("lib-grid"); g.innerHTML = "";
    var sessions = d.sessions || []; libSessions = sessions;
    $("lib-empty").hidden = sessions.length > 0; $("lib-empty").textContent = "No sessions match.";
    sessions.forEach(function (s) { g.appendChild(libCard(s)); });
    updateDelBtn();
  }
  function sizeBadge(b) {
    if (!b) return "";
    var disp = ((b.display || 0) + (b.meta || 0)) / 1e6;
    var s = "<b>" + (disp >= 1000 ? (disp / 1000).toFixed(1) + " GB" : disp.toFixed(0) + " MB") + "</b>";
    if (b.native > 0) s += ' <span class="raw">+ raw ' + (b.native / 1e9).toFixed(1) + " GB</span>";
    return s;
  }
  function libDate(t0) { try { var d = new Date(t0); return d.toLocaleDateString() + " " + ("0" + d.getHours()).slice(-2) + ":" + ("0" + d.getMinutes()).slice(-2); } catch (e) { return t0; } }
  function libCard(s) {
    var card = document.createElement("div"); card.className = "lib-card" + (libSel[s.sid] ? " sel" : ""); card.dataset.sid = s.sid;
    var hasThumbs = s.thumbs && s.thumbs > 0, poster;
    if (hasThumbs) {
      poster = document.createElement("img"); poster.className = "lib-poster"; poster.src = "/rec/thumbs/" + s.sid + "/2.jpg";
      var timer = null, i = 0;
      card.onmouseenter = function () { timer = setInterval(function () { i = (i + 1) % 8; poster.src = "/rec/thumbs/" + s.sid + "/" + i + ".jpg"; }, 166); };
      card.onmouseleave = function () { if (timer) clearInterval(timer); timer = null; poster.src = "/rec/thumbs/" + s.sid + "/2.jpg"; };
    } else { poster = document.createElement("div"); poster.className = "lib-poster radaronly"; poster.textContent = "◟ RADAR ONLY ◞"; }
    card.appendChild(poster);
    if (s.state !== "saved") { var rib = document.createElement("div"); rib.className = "lib-pending"; rib.textContent = "PENDING"; card.appendChild(rib); }
    var cb = document.createElement("input"); cb.type = "checkbox"; cb.className = "lib-cb"; cb.checked = !!libSel[s.sid];
    cb.onclick = function (e) { e.stopPropagation(); libSel[s.sid] = cb.checked; card.classList.toggle("sel", cb.checked); updateDelBtn(); };
    card.appendChild(cb);
    if (s.bytes && s.bytes.native > 0) {
      var free = document.createElement("button"); free.className = "lib-free"; free.textContent = "FREE — drop raw";
      free.onclick = function (e) { e.stopPropagation(); if (confirm("Drop the raw native channel? Display + radar are kept.")) fetch("/rec/ctl?purge_native=" + encodeURIComponent(s.sid)).then(loadLibrary); };
      card.appendChild(free);
    }
    var body = document.createElement("div"); body.className = "lib-cardbody";
    body.innerHTML = '<div class="lib-name">' + esc(s.name || s.sid) + "</div>"
      + '<div class="lib-meta"><span>' + libDate(s.t0) + "</span><span>" + fmtClock(s.dur_ms) + "</span></div>"
      + '<div class="lib-meta lib-size">' + sizeBadge(s.bytes) + "</div>"
      + '<div class="lib-cardtags">' + (s.tags || []).map(function (t) { return '<span class="tagchip">' + esc(t) + "</span>"; }).join("") + "</div>"
      + (s.note ? '<div class="lib-note">' + esc(s.note) + "</div>" : "");
    card.appendChild(body);
    card.onclick = function () { openReplay(s); };
    return card;
  }
  function updateDelBtn() {
    var n = Object.keys(libSel).filter(function (k) { return libSel[k]; }).length;
    var d = $("lib-del"); d.hidden = n === 0; d.textContent = "DELETE (" + n + ")";
    var o = $("lib-offsel"); o.hidden = n === 0; o.textContent = "OFFLOAD (" + n + ")";
  }
  /* offload = download a .tar of the session(s) — display video + radar + data. Over plain
   * HTTP the browser can't pick a folder, so it lands in Downloads (enable the browser's
   * "ask where to save each file" to choose one). tier=display keeps it reasonable; raw
   * native is excluded (regenerable). */
  function offloadTar(sids, n) {
    if (!confirm("Offload " + n + " session(s)?\n\nDownloads a .tar (display video + radar + data) to this device — it goes to your Downloads folder unless your browser is set to ask where to save.")) return;
    var a = document.createElement("a");
    a.href = "/rec/export?sids=" + encodeURIComponent(sids) + "&tier=display";
    document.body.appendChild(a); a.click(); a.remove();
  }
  $("lib-offall").onclick = function () { if (!libSessions.length) { alert("Library is empty."); return; } offloadTar("all", libSessions.length); };
  $("lib-offsel").onclick = function () { var s = Object.keys(libSel).filter(function (k) { return libSel[k]; }); if (!s.length) { alert("Select sessions first (the checkboxes)."); return; } offloadTar(s.join(","), s.length); };
  $("lib-del").onclick = function () {
    var sids = Object.keys(libSel).filter(function (k) { return libSel[k]; });
    if (!sids.length || !confirm("Delete " + sids.length + " session(s)? Cannot be undone.")) return;
    fetch("/rec/ctl?delete=" + sids.map(encodeURIComponent).join(",")).then(function () { libSel = {}; loadLibrary(); }).catch(function () {});
  };
  $("lib-delall").onclick = function () {           /* wipe everything — double verification */
    var n = libSessions.length;
    if (!n) { alert("Library is already empty."); return; }
    if (!confirm("Delete ALL " + n + " session(s)? This permanently erases every recording — there is no undo.")) return;
    if (prompt("FINAL CHECK — type DELETE (capitals) to erase all " + n + " recordings:") !== "DELETE") return;
    var sids = libSessions.map(function (s) { return s.sid; });
    fetch("/rec/ctl?delete=" + sids.map(encodeURIComponent).join(",")).then(function () { libSel = {}; loadLibrary(); }).catch(function () {});
  };

  /* ── REPLAY ── */
  function openReplay(s) {
    fetch("/rec/replay/ctl?open=" + encodeURIComponent(s.sid)).then(function () {
      replaySession = s; replaying = true;
      if (radarES) { radarES.close(); radarES = null; }   /* stop the live radar push while reviewing */
      if (detES) { detES.close(); detES = null; } lastDet = null;   /* no live det boxes over a recording */
      resetReplayZoom(); setZoomLabel(); radarROI = null; eoROI = false; roiArm = false; setRoiUI();
      /* "was this channel recorded?" from the actual captured bytes — NOT thumbs (a
       * session can have EO video with no thumbnails generated). */
      replayHasEO = !!(s.bytes && ((s.bytes.display > 0) || (s.bytes.native > 0)));
      replayHasRadar = !!(s.bytes && s.bytes.radar > 0);
      document.body.classList.add("replay"); $("library").hidden = true;
      API.stream = "/rec/replay/stream"; API.radar = "/rec/replay/radar"; API.stats = "/rec/replay/stats"; API.rstats = "/rec/replay/rstats";
      /* Show the recorded still at the open position — NOT the live stream. The replay
       * MJPEG only pushes while playing, so before Play we'd otherwise keep showing the
       * last live frame. pollReplayState swaps to the stream once playback starts. */
      replayPlaying = false; replayStillT = -1;
      $("video").src = replayHasEO ? ("/rec/replay/frame?t=0") : BLANK;
      $("rb-text").innerHTML = "REPLAY — " + esc(s.name || s.sid) + " — " + esc(s.t0)
        + (s.note ? ' <span class="rb-note">“' + esc(s.note) + '”</span>' : "");
      $("tp-dur").textContent = fmtClockT(s.dur_ms); $("tp-scrub").max = s.dur_ms; $("tp-scrub").value = 0;
      if (replayStatePoll) clearInterval(replayStatePoll);
      replayStatePoll = setInterval(pollReplayState, 150); pollReplayState();
    }).catch(function () {});
  }
  function closeReplay() {
    fetch("/rec/replay/ctl?close=1").catch(function () {});
    replaying = false; replaySession = null; document.body.classList.remove("replay");
    if (replayStatePoll) { clearInterval(replayStatePoll); replayStatePoll = null; }
    API.stream = "/stream"; API.radar = "/radar"; API.stats = "/stats"; API.rstats = "/rstats";
    $("video").src = API.stream + "?t=" + Date.now();
    openRadarStream();                                   /* resume the live radar push */
    openDetStream();                                     /* resume the live det boxes */
    resetReplayZoom(); setZoomLabel(); radarROI = null; eoROI = false; roiArm = false; setRoiUI();
    $("library").hidden = false; loadLibrary();
  }
  $("rb-close").onclick = closeReplay;
  function pollReplayState() {
    fetch("/rec/replay/state").then(function (r) { return r.json(); }).then(function (st) {
      var rs = st.replay_state || st.state || st; if (!rs) return;   /* /state nests as .state, /stats as .replay_state */
      /* EO pane source: live MJPEG replay stream while playing, recorded still while
       * paused/stepped/scrubbed — so it never falls back to the live view. */
      if (replayHasEO) {
        if (rs.playing && rs.t_ms < rs.dur_ms) {
          if (!replayPlaying) { replayPlaying = true; $("video").src = "/rec/replay/stream?t=" + Date.now(); }
        } else {
          replayPlaying = false;
          if (rs.t_ms !== replayStillT) { replayStillT = rs.t_ms; $("video").src = "/rec/replay/frame?t=" + rs.t_ms; }
        }
      }
      if (!scrubbing) { $("tp-scrub").value = rs.t_ms; $("tp-cur").textContent = fmtClockT(rs.t_ms); }
      $("tp-play").textContent = (rs.playing && rs.t_ms < rs.dur_ms) ? "⏸" : "⏵";
      $("tp-rate").textContent = rs.rate + "×";
      /* NATIVE/DISPLAY toggle — only when a native channel was recorded */
      if (rs.has_native) { $("tp-video").hidden = false; $("tp-video").textContent = (rs.video_src === "native") ? "NATIVE" : "DISPLAY"; }
      else $("tp-video").hidden = true;
      if (rs.t_wall_ms) { var d = new Date(rs.t_wall_ms); $("v-zulu").textContent = "REC " + ("0" + d.getUTCHours()).slice(-2) + ":" + ("0" + d.getUTCMinutes()).slice(-2); }
    }).catch(function () { $("eo-scrim").hidden = false; });
  }
  $("tp-play").onclick = function () { rctl($("tp-play").textContent === "⏸" ? "pause=1" : "play=1"); };
  $("tp-rate").onclick = function () { var i = (RATES.indexOf(parseFloat($("tp-rate").textContent)) + 1) % RATES.length; rctl("rate=" + RATES[i]); };
  $("tp-video").onclick = function () { rctl("video=" + ($("tp-video").textContent === "NATIVE" ? "display" : "native")); };
  $("tp-step-b").onclick = function () { rctl("step=-1"); };
  $("tp-step-f").onclick = function () { rctl("step=1"); };
  $("tp-scrub").oninput = function () {
    scrubbing = true; $("tp-cur").textContent = fmtClockT(+this.value);
    var now = Date.now(); if (now - scrubThrottle >= 80) { scrubThrottle = now; rctl("seek=" + Math.round(this.value)); }
  };
  $("tp-scrub").onchange = function () { rctl("seek=" + Math.round(this.value)); setTimeout(function () { scrubbing = false; }, 150); };
  $("tp-scrub").onmousemove = function (e) {
    if (!replaySession) return;
    var r = this.getBoundingClientRect(), t = Math.round((e.clientX - r.left) / r.width * replaySession.dur_ms);
    var h = $("tp-hover"); h.onerror = function () { h.style.display = "none"; }; h.src = "/rec/replay/frame?t=" + t;
    h.style.display = "block"; h.style.left = Math.max(4, e.clientX - r.left - 40) + "px";
  };
  $("tp-scrub").onmouseleave = function () { $("tp-hover").style.display = "none"; };
  document.addEventListener("keydown", function (e) {
    if (!replaying || e.target.tagName === "INPUT" || e.target.tagName === "TEXTAREA") return;
    var cur = parseFloat($("tp-rate").textContent), i;
    if (e.key === "ArrowRight") { rctl("step=1"); e.preventDefault(); }
    else if (e.key === "ArrowLeft") { rctl("step=-1"); e.preventDefault(); }
    else if (e.key === " ") { $("tp-play").onclick(); e.preventDefault(); }
    else if (e.key === "ArrowUp") { i = Math.min(RATES.length - 1, RATES.indexOf(cur) + 1); rctl("rate=" + RATES[i]); e.preventDefault(); }
    else if (e.key === "ArrowDown") { i = Math.max(0, RATES.indexOf(cur) - 1); rctl("rate=" + RATES[i]); e.preventDefault(); }
    else if (e.key === "Escape") { closeReplay(); }
  });

  setTrack("auto"); setIllum("auto"); setExpMode(true); applyTheme();
  setInterval(poll, 160); poll();
  setInterval(pollRadar, 120); openRadarStream();   /* live = SSE push; the 120ms poll only fires in replay */
  openDetStream();                                  /* EO detector boxes (SSE push, live-only) */
  initRadarOv();                                    /* radar→EO overlay toggle + trims (persisted) */
  setInterval(pollRstats, 400); pollRstats();
  setInterval(pollDstats, 1000); pollDstats();
  setInterval(pollRec, 400); pollRec();
})();
