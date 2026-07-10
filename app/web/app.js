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
  /* Step to the next zoom rung in a direction. Works when the current zoom is NOT a rung — an
   * ROI/box zoom leaves replayZoom an arbitrary float (e.g. 3.7), for which ZOOMS.indexOf() is
   * -1 and made "+" jump to 1x and "-" go dead. dir>0 = first rung above, dir<0 = first below. */
  function stepZoom(cur, dir) {
    var i;
    if (dir > 0) { for (i = 0; i < ZOOMS.length; i++) if (ZOOMS[i] > cur + 1e-6) return i; return -1; }
    for (i = ZOOMS.length - 1; i >= 0; i--) if (ZOOMS[i] < cur - 1e-6) return i;
    return -1;
  }
  var css = function (n) { return getComputedStyle(document.body).getPropertyValue(n).trim(); };

  var zoom = 1;
  var trackMode = "auto", engagedTid = null, sentEngage = null;
  var illumMode = "auto";
  var lastStats = {}, lastRadar = null;
  var eoDownN = 0, nextLiveHeal = 0, lastEoc = true;   /* live-view EO self-heal (see poll) */
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
  $("devbtn").onclick = function () { var open = $("dev").classList.toggle("open"); syncDev(); if (open) { pollRstats(); pollDstats(); } };   /* seed the DEV sliders on open (they aren't polled while closed) */
  $("devclose").onclick = function () { $("dev").classList.remove("open"); syncDev(); };
  document.querySelectorAll("[data-exp]").forEach(function (b) { b.onclick = function (e) { e.stopPropagation(); $("stage").classList.toggle("rbig"); setTimeout(redrawAll, 20); }; });

  /* ── zoom ── the EO feed owns digital zoom; we forward zoom=N and drive the readout
   * optimistically. poll() reconciles from eo.zoom but is guarded for ~1.2s after a tap
   * so a slightly-stale /stats can't snap the label back mid-interaction. ── */
  var zoomTouch = 0, replayZoom = 1;
  /* Zoom readout. Live = the feed's zoom. Replay DISPLAY = the zoom baked into the recorded
   * frame (from the recorded stats) × any client digital zoom on top — so a clip captured at
   * 2× reads "2.0×", not "1×". Replay NATIVE = full frame (capture zoom 1) × client digital
   * zoom, i.e. pure digital zoom. */
  function setZoomLabel() {
    var v = zoom;
    if (replaying) {
      var cap = (replayVideoSrc === "native") ? 1 : ((lastStats.eo && lastStats.eo.zoom) || 1);
      v = cap * replayZoom;
    }
    $("v-zval").textContent = v.toFixed(1) + "×";
  }
  /* replay has no live feed to crop, so zoom is a client-side digital zoom (CSS scale) on
   * the recorded frame — you can magnify even though it was recorded at 1x. */
  /* The digital zoom must scale the image AND the detection/radar overlay together, or the
   * boxes stay put while the picture magnifies. Apply the same transform to the MJPEG <img>,
   * the native <video>, and the overlay canvas (fit() is transform-independent, see above). */
  function zoomEls() { return [$("video"), $("nvid"), $("eo-ovl")]; }
  function applyReplayZoom() {
    var t = replayZoom > 1 ? "scale(" + replayZoom + ")" : "";
    zoomEls().forEach(function (el) { el.style.transform = t; if (replayZoom <= 1) el.style.transformOrigin = "center"; });
    setZoomLabel();
  }
  function resetReplayZoom() { replayZoom = 1; zoomEls().forEach(function (el) { el.style.transform = ""; el.style.transformOrigin = "center"; }); }
  document.querySelectorAll("[data-zoom]").forEach(function (b) {
    b.onclick = function () {
      var dir = parseInt(b.dataset.zoom, 10);
      if (replaying) {
        var ri = stepZoom(replayZoom, dir);   /* replayZoom may be a float from an ROI box zoom */
        if (ri < 0) return;
        replayZoom = ZOOMS[ri]; applyReplayZoom(); return;
      }
      var i = stepZoom(zoom, dir);
      if (i < 0) return;
      zoom = ZOOMS[i]; zoomTouch = Date.now(); ctl("zoom=" + zoom); setZoomLabel();
    };
  });
  /* click the zoomed replay picture to recenter the zoom on that point (whichever display
   * element is showing — MJPEG img or native video); the origin applies to the overlay too */
  function recenterZoom(e) {
    if (roiArm || !replaying || replayZoom <= 1) return;
    var r = this.getBoundingClientRect();
    var org = ((e.clientX - r.left) / r.width * 100).toFixed(1) + "% "
            + ((e.clientY - r.top) / r.height * 100).toFixed(1) + "%";
    zoomEls().forEach(function (el) { el.style.transformOrigin = org; });
  }
  $("video").addEventListener("click", recenterZoom);
  $("nvid").addEventListener("click", recenterZoom);
  /* if the mp4 can't decode/load, never sit on a black <video> — mark it failed so
   * updateReplayVideo falls back to the MJPEG. Guarded so resetMp4State's load() (no src)
   * doesn't trip it. */
  $("nvid").addEventListener("error", function () { if (mp4State.srcSet) { mp4State.ready = false; mp4State.pct = -1; } });
  /* Recorded still frame not ready yet (recorder just opened the session) -> the <img> errors,
   * and because the still src only re-sets when the timeline moves it would stay blank ("EO not
   * there"). Retry the current still a few times so it recovers on its own. */
  var replayFrameRetries = 0;
  $("video").addEventListener("load", function () { if (replaying && !replayPlaying) replayFrameRetries = 0; });
  $("video").addEventListener("error", function () {
    if (!replaying || replayPlaying || replayStillT < 0 || replayFrameRetries >= 6) return;
    replayFrameRetries++;
    setTimeout(function () { if (replaying && !replayPlaying) $("video").src = "/rec/replay/frame?t=" + replayStillT + "&r=" + Date.now(); }, 450);
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
    if (replaying) { resetReplayZoom(); setZoomLabel(); }   /* back to the natural view (capture zoom for display, 1x for native) */
    else { [$("video"), $("eo-ovl")].forEach(function (el) { el.style.transform = ""; el.style.transformOrigin = "center"; }); }
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
        var sc = Math.min(r.width / bw, r.height / bh);
        var org = ((bx0 + bw / 2) / r.width * 100).toFixed(1) + "% " + ((by0 + bh / 2) / r.height * 100).toFixed(1) + "%";
        if (replaying) {                                  /* ROI box -> replay digital zoom; scales the shown video (display or native) AND the overlay together */
          replayZoom = sc;
          zoomEls().forEach(function (el) { el.style.transform = "scale(" + sc.toFixed(3) + ")"; el.style.transformOrigin = org; });
          setZoomLabel();
        } else {   /* live: scale the video AND the overlay canvas together so the detector/radar marks track the zoom (they were left at the un-zoomed position) */
          [$("video"), $("eo-ovl")].forEach(function (el) { el.style.transformOrigin = org; el.style.transform = "scale(" + sc.toFixed(3) + ")"; });
        }
        eoROI = true;
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
  /* Each knob is live only in the mode where it means something:
   *  - EXP ms / GAIN: MANUAL only — in AUTO the exposure loop owns them.
   *  - AUTO-CAP: AUTO only — it's the ceiling the loop may raise gain to; in MANUAL gain
   *    is set directly, so a cap does nothing.
   *  - MEDIAN: a denoise, independent of exposure — always live. */
  function setExpMode(auto) {
    $("s-exp").disabled = auto; $("s-gain").disabled = auto;
    $("s-gcap").disabled = !auto;
    document.querySelectorAll("#md-btns button").forEach(function (x) { x.disabled = false; });
  }
  /* moving EXP or GAIN drops the feed to MANUAL — reflect that optimistically */
  $("s-exp").oninput  = function () { $("o-exp").textContent = (+this.value).toFixed(2); manualAE(); ctl("expms=" + this.value); };
  $("s-gain").oninput = function () { $("o-gain").textContent = this.value; manualAE(); ctl("gain=" + this.value); };
  $("s-gcap").oninput = function () { $("o-gcap").textContent = this.value; ctl("gaincap=" + this.value); };
  function manualAE() { var m = document.querySelector('#ae-btns [data-ae="0"]'); if (m) setSeg("ae-btns", m); setExpMode(false); ispTouch = Date.now(); }
  var ispTouch = 0, fpsTouch = 0;

  /* stream bandwidth levers — res (display size) + fps cap, both live on the EO feed */
  /* link auto-quality — OPT-IN: when the link is saturated (SAT), step QUALITY down one
   * rung until the video flows again, and probe back up when the link stays clean; the
   * operator's chosen QUALITY is the ceiling AUTO never exceeds. MANUAL (default) never
   * touches settings. Persisted per browser. */
  var linkAuto = 0, linkCeil = "high", lkSatN = 0, lkCleanN = 0, lkStepT = 0, lkUpHold = 0;
  try { var lks = JSON.parse(localStorage.getItem("linkAuto") || "{}");
        if (lks.a === 0 || lks.a === 1) linkAuto = lks.a;
        if (typeof lks.c === "string") linkCeil = lks.c; } catch (x) {}
  function saveLka() { try { localStorage.setItem("linkAuto", JSON.stringify({ a: linkAuto, c: linkCeil })); } catch (x) {} }
  function initLka() { var b = document.querySelector('#lka-btns [data-lka="' + linkAuto + '"]'); if (b) setSeg("lka-btns", b); }
  document.querySelectorAll("#lka-btns [data-lka]").forEach(function (b) {
    b.onclick = function () { linkAuto = parseInt(b.dataset.lka, 10); setSeg("lka-btns", b); saveLka(); };
  });

  document.querySelectorAll("#res-btns [data-res]").forEach(function (b) {
    b.onclick = function () { setSeg("res-btns", b); linkCeil = b.dataset.res; saveLka(); ctl("res=" + b.dataset.res); };
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
    if (!$("dev").classList.contains("open")) return;   /* only the DEV panel shows these — don't poll when it's closed */
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
    { key: "nms",         fmt: function (v) { return v.toFixed(2); } },
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
    if (!$("dev").classList.contains("open")) return;   /* only the DEV panel shows these — don't poll when it's closed */
    fetch("/dstats").then(function (r) { return r.json(); }).then(function (d) {
      var k = d.knobs || {};
      /* measured model rate beside CADENCE (not 60/N — shows the truth when the GPU
       * saturates at low cadence). Updated regardless of the drag guard below. */
      var df = (d.det && typeof d.det.fps === "number") ? d.det.fps : null;
      $("dv-cadfps").textContent = (df !== null) ? "· " + (Math.round(df * 10) / 10) + "/s" : "";
      if (Date.now() - dtTouch < 1500) return;   /* don't fight an active drag */
      DETC.forEach(function (c) {
        var v = k[c.key];
        if (typeof v === "number" && document.activeElement !== $("dt-" + c.key)) { $("dt-" + c.key).value = v; $("dv-" + c.key).textContent = c.fmt(v); }
      });
      if (typeof k.motion === "number") { var mb = document.querySelector('#mot-btns [data-mot="' + k.motion + '"]'); if (mb) setSeg("mot-btns", mb); }
    }).catch(function () {});
  }

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
  var lastTgtHtml = "";
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
        out.push('<li class="tgt-row' + (t.tid === engagedTid ? ' eng' : '') + '" data-tid="' + t.tid + '" style="border-left-color:' + col + '">'
          + '<span class="tid"><span class="swatch" style="background:' + col + '"></span></span>'
          + '<span class="meta">' + spd.toFixed(1) + ' m/s · ' + (az >= 0 ? "+" : "") + az.toFixed(0) + '°</span>'
          + '<span class="rng">' + t.rng.toFixed(0) + ' m</span></li>');
      } else {
        out.push('<li class="tgt-row empty"><span class="tid">—</span><span class="meta"></span><span class="rng"></span></li>');
      }
    }
    var html = out.join("");
    if (html !== lastTgtHtml) { $("tgt-list").innerHTML = html; lastTgtHtml = html; }   /* skip the HTML re-parse when nothing changed (was rebuilding ~27x/s) */
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

  /* detector mark style — BOX (full bounding box) or SEEKER (small centroid cross);
   * display-only, persisted per browser */
  var detStyle = "box";
  try { var dsv = localStorage.getItem("detStyle"); if (dsv === "seeker" || dsv === "box") detStyle = dsv; } catch (x) {}
  function initDetStyle() { var b = document.querySelector('#dst-btns [data-dst="' + detStyle + '"]'); if (b) setSeg("dst-btns", b); }
  document.querySelectorAll("#dst-btns [data-dst]").forEach(function (b) {
    b.onclick = function () { detStyle = b.dataset.dst; setSeg("dst-btns", b); try { localStorage.setItem("detStyle", detStyle); } catch (x) {} drawEO(); };
  });
  $("rov-az").oninput = function () { radarOv.az = parseFloat(this.value); $("rovv-az").textContent = radarOv.az.toFixed(1) + "°"; saveRadarOv(); drawEO(); };
  $("rov-el").oninput = function () { radarOv.el = parseFloat(this.value); $("rovv-el").textContent = radarOv.el.toFixed(1) + "°"; saveRadarOv(); drawEO(); };

  /* ── canvas ── */
  /* Size the backing store from the element's LAYOUT box (offsetWidth), not getBoundingClientRect
   * — the latter includes CSS transforms, so once we CSS-scale the overlay for replay zoom its
   * measured size would feed back and double-scale. offsetWidth is transform-independent. */
  function fit(cv) { var dpr = window.devicePixelRatio || 1, w = cv.offsetWidth || 1, h = cv.offsetHeight || 1; cv.width = Math.max(1, w * dpr | 0); cv.height = Math.max(1, h * dpr | 0); return { ctx: cv.getContext("2d"), w: cv.width, h: cv.height, dpr: dpr }; }
  /* dark halo under on-video labels — thin text survives bright sky and dark bush alike
   * (the standard OSD treatment; the marks get the same via a two-pass stroke) */
  function haloText(ctx, txt, x, y, col, dpr) {
    ctx.save(); ctx.strokeStyle = "rgba(0,0,0,0.85)"; ctx.lineWidth = 3 * dpr; ctx.lineJoin = "round";
    ctx.strokeText(txt, x, y); ctx.fillStyle = col; ctx.fillText(txt, x, y); ctx.restore();
  }

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
        if (Math.abs(fx) > 1 || Math.abs(fy) > 1) return;      /* off-frame → not drawn */
        var lx = vx2 + (fx + 1) / 2 * vw2, ly = vy2 + (fy + 1) / 2 * vh2;
        /* BROKEN RING (fixed size) — four arcs with cardinal gaps over a dark halo, so
         * the mark reads over bright sky and dark bush alike. Fixed size on purpose:
         * the tracker's sx/sy estimates jitter, size-coding pulsed. No engaged/LOCK
         * styling: tracking isn't a phase yet — all marks are equal. */
        var col = tcolor(t.tid), rr = 14 * dpr, gp = 0.42;
        [["rgba(0,0,0,0.85)", 5 * dpr], [col, 2.6 * dpr]].forEach(function (s) {
          ctx.strokeStyle = s[0]; ctx.lineWidth = s[1]; ctx.lineCap = "round";
          for (var k = 0; k < 4; k++) {
            var a0 = k * Math.PI / 2 + gp, a1 = (k + 1) * Math.PI / 2 - gp;
            ctx.beginPath(); ctx.arc(lx, ly, rr, a0, a1); ctx.stroke();
          }
        });
        ctx.lineCap = "butt";
        ctx.fillStyle = "rgba(0,0,0,0.85)"; ctx.beginPath(); ctx.arc(lx, ly, 3 * dpr, 0, 2 * Math.PI); ctx.fill();
        ctx.fillStyle = col; ctx.beginPath(); ctx.arc(lx, ly, 1.8 * dpr, 0, 2 * Math.PI); ctx.fill();
        /* range only — the ring COLOUR is the track identity (matches the target list);
         * the tid churns between frames and just added flicker */
        haloText(ctx, t.rng.toFixed(0) + " m", lx + rr + 5 * dpr, ly - 5 * dpr, col, dpr);
      });
    }

    /* EO detector boxes — px = [cx,cy,w,h] in the NATIVE frame (msg.img, 1440x1088).
     * The display shows a centered 1/zoom crop of native, letterboxed into the panel
     * (object-fit: contain) — map native -> crop -> content rect. dets[] solid with
     * class+conf; movers[] dashed class-less. Works in replay too: lastDet is fed by
     * the replay det poller there (recorded boxes over recorded video); a NATIVE replay
     * shows the full uncropped frame, so the zoom crop doesn't apply to it. */
    if (lastDet && (lastDet.dets || lastDet.movers)) {
      var im = lastDet.img || { w: 1440, h: 1088 };
      var z = (replaying && replayVideoSrc === "native") ? 1 : (es.zoom || 1);
      var cw = im.w / z, ch = im.h / z, ox = (im.w - cw) / 2, oy = (im.h - ch) / 2;
      var sw = es.dw || cw, sh = es.dh || ch, ar = sw / sh;   /* streamed frame sets the letterbox */
      var vw, vh, vx, vy;
      if (w / h > ar) { vh = h; vw = h * ar; vx = (w - vw) / 2; vy = 0; }
      else { vw = w; vh = w / ar; vx = 0; vy = (h - vh) / 2; }
      ctx.save(); ctx.beginPath(); ctx.rect(vx, vy, vw, vh); ctx.clip();
      ctx.font = (10 * dpr) + "px ui-monospace, monospace";
      var drawDet = function (b, dashed, col, label, forceBox) {
        if (!b.px || b.px.length < 4) return;
        var bx = vx + (b.px[0] - ox) / cw * vw, by = vy + (b.px[1] - oy) / ch * vh;
        var bw2 = b.px[2] / cw * vw, bh2 = b.px[3] / ch * vh;
        if (bx + bw2 / 2 < vx || bx - bw2 / 2 > vx + vw || by + bh2 / 2 < vy || by - bh2 / 2 > vy + vh) return;
        if (detStyle === "seeker" && !forceBox) {
          /* HEAVY HALO CROSS — short thick arms with a centre gap over a dark halo,
           * readable on bright sky and dark bush (field pick, option E) */
          var a = 11 * dpr, g = 3.5 * dpr;
          [["rgba(0,0,0,0.85)", 6 * dpr], [col, 3 * dpr]].forEach(function (s) {
            ctx.strokeStyle = s[0]; ctx.lineWidth = s[1]; ctx.lineCap = "round";
            ctx.setLineDash(dashed ? [5 * dpr, 4 * dpr] : []);
            ctx.beginPath();
            ctx.moveTo(bx - a, by); ctx.lineTo(bx - g, by); ctx.moveTo(bx + g, by); ctx.lineTo(bx + a, by);
            ctx.moveTo(bx, by - a); ctx.lineTo(bx, by - g); ctx.moveTo(bx, by + g); ctx.lineTo(bx, by + a);
            ctx.stroke();
          });
          ctx.setLineDash([]); ctx.lineCap = "butt";
          haloText(ctx, label, bx + a + 3 * dpr, by - 4 * dpr, col, dpr);
        } else {
          ctx.strokeStyle = col; ctx.lineWidth = 1.6 * dpr;
          ctx.setLineDash(dashed ? [5 * dpr, 4 * dpr] : []);
          ctx.strokeRect(bx - bw2 / 2, by - bh2 / 2, bw2, bh2);
          ctx.setLineDash([]);
          haloText(ctx, label, bx - bw2 / 2 + 2 * dpr, by - bh2 / 2 - 3 * dpr, col, dpr);
        }
      };
      var seek = (detStyle === "seeker");
      (lastDet.dets || []).forEach(function (d) {
        var col = d.cls === "human" ? "#40c4ff" : amber;
        var cl = String(d.cls || "?");                   /* coerce: a non-string cls (e.g. a numeric id) would throw on [0]/.toUpperCase() and blank every overlay */
        var lab = seek ? cl[0].toUpperCase() + Math.round((d.conf || 0) * 100)
                       : cl.toUpperCase() + " " + Math.round((d.conf || 0) * 100) + "%";
        drawDet(d, false, col, lab);
      });
      /* motion-head marks: ALWAYS a gentle thin dashed box (never the heavy cross), so a
       * mover reads as a soft hint distinct from the model's class marks */
      (lastDet.movers || []).forEach(function (mv) {
        drawDet(mv, true, "rgba(150,157,168,0.9)", "MOT ·" + (mv.age || 0), true);
      });
      ctx.restore();
    }
  }

  /* Radar scope — matches the radar daemon's own PPI renderer (radar/web/radar_view.js):
   * same 8 target colours, 2 px dots, SNR-scaled alpha, half-circle rings, amber 100 m
   * reference. We add the GUI's jobs the daemon leaves to us: display persistence
   * (hold+fade a dropped box ~300 ms) and the engaged-target LOCK. */
  /* 20 maximally-distinct colours so a target can be identified by COLOUR alone (matched
   * between the EO marks, the scope, and the list). Collisions past 20 targets are fine. */
  var TARGET_COLORS = ["#ff4d4d", "#ff8c1a", "#ffd11a", "#c2e01a", "#66cc33", "#1ac26a",
    "#1ad1b0", "#1ab2e0", "#4d94ff", "#6d5cff", "#a64dff", "#e04dff", "#ff4dab", "#ff6f7d",
    "#d98cff", "#8cff66", "#66ffd9", "#ffb84d", "#4dd2ff", "#ff99cc"];
  /* accepts a number (radar tid) or a string key (e.g. "E3") — hashed so radar and EO
   * ids don't systematically collide */
  function tcolor(key) {
    var s = String(key), h = 0;
    for (var i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0;
    return TARGET_COLORS[((h % 20) + 20) % 20];
  }
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

    /* target marks — the daemon's tracker output drawn verbatim (it confirms/coasts/
     * dedups server-side; tids are stable). Fixed ring + centre dot like the EO overlay
     * (the sx/sy extent estimates jitter, so size-coding pulsed); velocity stays as a
     * vector line. One colour per track across scope, list and EO overlay. */
    ctx.font = (11 * dpr) + "px ui-monospace, monospace";
    targets(radar).forEach(function (t) {
      if (!inFov(t.x, t.y)) return;                 /* hide targets outside the FOV */
      var tc = W2C(t.x, t.y), col = tcolor(t.tid), rr = 10 * dpr;
      ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = 1.5 * dpr;
      ctx.beginPath(); ctx.arc(tc[0], tc[1], rr, 0, 2 * Math.PI); ctx.stroke();
      ctx.beginPath(); ctx.arc(tc[0], tc[1], 1.6 * dpr, 0, 2 * Math.PI); ctx.fill();
      var vc = W2C(t.x + t.vx, t.y + t.vy); ctx.beginPath(); ctx.moveTo(tc[0], tc[1]); ctx.lineTo(vc[0], vc[1]); ctx.stroke();
      var spd = Math.hypot(t.vx, t.vy);
      ctx.fillText("R#" + t.tid + "  " + spd.toFixed(1) + " m/s · " + t.rng.toFixed(0) + " m", tc[0] + rr + 3 * dpr, tc[1] - rr - 3 * dpr);
    });
  }

  function redrawAll() { drawEO(); drawRadar(lastRadar); }
  window.addEventListener("resize", redrawAll);

  /* Coalesce overlay redraws: radar (~27/s) + detector (~15/s) events would otherwise call
   * drawRadar/drawEO ~40x/s on the main thread and steal paint time from the MJPEG <img> —
   * the video stutters (worst while moving). Mark dirty + draw at most once per animation
   * frame, and skip drawing entirely when the tab is hidden. */
  var _dEO = false, _dRadar = false, _rafOn = false;
  function draw(eo, radar) {
    if (eo) _dEO = true; if (radar) _dRadar = true;
    if (_rafOn) return; _rafOn = true;
    requestAnimationFrame(function () {
      _rafOn = false;
      if (document.hidden) { _dEO = _dRadar = false; return; }
      var r = _dRadar, e = _dEO; _dRadar = _dEO = false;
      if (r) drawRadar(lastRadar);
      if (e) drawEO();
    });
  }

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
      /* Live-view crash backstop: in the real-time view (not replay, library closed) the
       * producers should be up. If EO is genuinely down for ~2.5 s (a crashed producer), ask the
       * launcher to bring the stack back, rate-limited to once / 30 s so it can't thrash. When it
       * recovers, force a FRESH /stream connection since a stalled MJPEG <img> won't reconnect. */
      if (!replaying && $("library").hidden) {
        if (!eoc) {
          if (++eoDownN >= 16 && Date.now() > nextLiveHeal) {
            nextLiveHeal = Date.now() + 30000;
            fetch(location.protocol + "//" + location.hostname + ":8088/resume", { mode: "no-cors" }).catch(function () {});
          }
        } else {
          if (!lastEoc) $("video").src = "/stream?t=" + Date.now();   /* just recovered -> reconnect the live MJPEG */
          eoDownN = 0;
        }
        lastEoc = eoc;
      }
      /* link chip: signal bars (wifi) · type · live Mb/s · delivered fps */
      $("v-ltype").textContent = d.link_type ? d.link_type.toUpperCase() : "LINK";
      $("v-link").textContent = num(d.mbps, 1) + " Mb/s";
      $("v-txfps").innerHTML = (eoc && d.tx_fps != null) ? "&nbsp;·&nbsp;" + Math.round(d.tx_fps) + " fps" : "";
      /* total link traffic — shown only when it clearly exceeds the video (something else
       * is sharing the pipe: an offload, a labeling pull, another viewer) */
      var totm = d.link_total_mbps;
      $("v-tot").innerHTML = (typeof totm === "number" && totm > d.mbps * 1.25 + 2) ? "&nbsp;·&nbsp;tot " + Math.round(totm) : "";
      $("v-sig").innerHTML = signalSVG(d.rssi_dbm);
      $("p-link").classList.add("on");   /* steady green while connected — catch() clears it if the poll fails */
      /* SAT = the link delivers less than half the video being produced (live only) —
       * the chip goes amber so a starving link reads as a LINK problem, not a frozen EO */
      var prodFps = eo.sfps || eo.fps || 0;
      var sat = !replaying && eoc && typeof d.tx_fps === "number" && prodFps > 0 && d.tx_fps < prodFps * 0.5;
      $("p-link").classList.toggle("sat", !!sat);
      $("v-sat").textContent = sat ? "SAT" : "";
      /* LINK AUTO (opt-in): ~2s of SAT → step QUALITY down a rung; ~20s clean → probe one
       * rung back up (never above the operator's chosen ceiling; 60s hold after a step
       * down so probing can't flap) */
      if (linkAuto && !replaying && eoc) {
        if (sat) { lkSatN++; lkCleanN = 0; } else { lkCleanN++; lkSatN = 0; }
        var ORD = ["low", "med", "high", "native"], cur = ORD.indexOf(eo.res), ceil = ORD.indexOf(linkCeil), nowL = Date.now();
        if (ceil < 0) ceil = 2;
        if (lkSatN >= 12 && cur > 0 && nowL - lkStepT > 3000) {
          lkStepT = nowL; lkUpHold = nowL + 60000; lkSatN = 0; ctl("res=" + ORD[cur - 1]);
        } else if (lkCleanN >= 125 && cur >= 0 && cur < ceil && nowL > lkUpHold && nowL - lkStepT > 3000) {
          lkStepT = nowL; lkCleanN = 0; ctl("res=" + ORD[cur + 1]);
        }
      } else { lkSatN = 0; lkCleanN = 0; }
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
      if (!replaying && typeof eo.zoom === "number" && ZOOMS.indexOf(eo.zoom) >= 0 && Date.now() - zoomTouch > 1200) { zoom = eo.zoom; setZoomLabel(); }
      if (replaying) setZoomLabel();                 /* track the recorded capture zoom over the timeline */
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
    if (!d.connected) { updateViewRange(null); engage(trackMode === "man" ? engagedTid : null); renderTargetList(null); draw(true, true); return; }
    var cur = targets(d), present = {};
    cur.forEach(function (t) { present[t.tid] = 1; });
    updateViewRange(d);
    if (trackMode === "auto") { var best = pickAuto(cur); engage(best ? best.tid : null); }
    else if (engagedTid !== null && !present[engagedTid]) engage(null);   /* engaged target gone */
    renderTargetList(d); draw(true, true);
  }
  /* NaN-safe JSON. The feeds/recorder occasionally emit bare NaN/Infinity (invalid JSON), which
   * makes JSON.parse throw and silently freezes an overlay on its last frame — the scope then
   * quietly lies. Parse normally; on failure coerce NaN/Infinity to null and retry so one bad
   * field can't stall the view. Returns null only if truly unparseable. */
  function parseJSONsafe(t) {
    try { return JSON.parse(t); }
    catch (e) { try { return JSON.parse(String(t).replace(/-?\bInfinity\b/g, "null").replace(/\bNaN\b/g, "null")); } catch (e2) { return null; } }
  }
  /* Live radar is PUSHED over SSE (/radar/stream) at the sensor's native rate — no polling,
   * so the display matches the sensor instead of an 8 Hz sample. Replay radar is POLLED
   * (self-chaining) — see the replay-overlays block below. */
  var radarES = null;
  function openRadarStream() {
    if (radarES) { radarES.close(); radarES = null; }
    radarES = new EventSource("/radar/stream");
    radarES.onmessage = function (e) { if (replaying) return; var f = parseJSONsafe(e.data); if (f) onRadarFrame(f); };
    radarES.onerror = function () { if (!replaying) onRadarFrame({ connected: false }); };   /* auto-reconnects */
  }
  /* ---- Replay overlays are POLLED, never SSE --------------------------------------------------
   * Rock-solid connection budget: a replay holds exactly ONE long-lived socket — the mp4 <video>.
   * Radar + det overlays ride SELF-CHAINING polls: each fetch schedules the next only AFTER it
   * settles (+ a floor gap), so at most one request per stream is ever in flight. That cannot pile
   * up sockets on a lossy link the way a fixed-interval poll (stacks when a fetch outlives its
   * period) or a long-lived SSE (slow to tear down -> FIN-WAIT pileup, the "can't enter a movie
   * after 3-4" bug) does. Fast link -> self-paces to ~14 Hz; bad link -> slows, never leaks. */
  var REPLAY_GAP = 70;                                    /* ms floor between overlay polls */
  var radarPollT = null, radarPolling = false;
  function stopReplayRadarPoll() { radarPolling = false; if (radarPollT) { clearTimeout(radarPollT); radarPollT = null; } }
  function openReplayRadarStream() {                      /* name kept; now a self-chaining poll */
    if (radarES) { radarES.close(); radarES = null; }     /* ensure no live radar SSE lingers */
    stopReplayRadarPoll(); radarPolling = true; pumpReplayRadar();
  }
  function pumpReplayRadar() {
    if (!replaying || !radarPolling) return;
    fetch(API.radar).then(function (r) { if (!r.ok) throw 0; return r.text(); })
      .then(function (t) { if (replaying) { var f = parseJSONsafe(t); if (f) onRadarFrame(f); } })
      .catch(function () {})
      .then(function () { if (replaying && radarPolling) radarPollT = setTimeout(pumpReplayRadar, REPLAY_GAP); });
  }
  var detPollT = null, detPolling = false, detGap = REPLAY_GAP;
  function stopReplayDetPoll() { detPolling = false; if (detPollT) { clearTimeout(detPollT); detPollT = null; } }
  function openReplayDetStream() {                         /* name kept; now a self-chaining poll */
    if (detES) { detES.close(); detES = null; } lastDet = null;
    stopReplayDetPoll(); detGap = REPLAY_GAP; detPolling = true; pumpReplayDet();
  }
  function pumpReplayDet() {
    if (!replaying || !detPolling) return;
    fetch("/rec/replay/det").then(function (r) {
        if (r.status === 404) { detGap = 5000; return null; }   /* channel not shipped -> slow probe, not 14/s */
        detGap = REPLAY_GAP; if (!r.ok) throw 0; return r.text();
      })
      .then(function (t) { if (replaying && t !== null) { var m = parseJSONsafe(t); lastDet = (m && (m.dets || m.movers)) ? m : null; draw(true, false); } })
      .catch(function () { if (replaying && lastDet) { lastDet = null; draw(true, false); } })
      .then(function () { if (replaying && detPolling) detPollT = setTimeout(pumpReplayDet, detGap); });
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
      var m = parseJSONsafe(e.data); if (m) lastDet = (m.connected === false) ? null : m;
      draw(true, false);
    };
    detES.onerror = function () { if (!replaying) { lastDet = null; draw(true, false); } };   /* auto-reconnects */
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
  var recState = null, pendingSid = null, libSel = {}, libTagFilter = {}, libSessions = [], handledSids = {};
  var healing = false, toastTimer = null;

  /* transient status banner (top-center). kind = "" | "warn" | "ok" | "err"; ms auto-hide. */
  function toast(msg, kind, ms) {
    var t = $("toast");
    t.textContent = msg; t.className = kind || ""; t.hidden = false;
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.hidden = true; }, ms || 4000);
  }

  /* Which feeds are LIVE but whose recorder tap is DOWN — REC on these records 0 bytes.
   * A tap is "down" when its channel is present in /rec/stats but not connected, AND the
   * corresponding feed is actually up (so it's a detached tap, not a dead sensor). */
  function recTapsDown() {
    if (!recState || !recState.channels) return [];
    var eoUp = !!lastStats.eo_connected, radarUp = !!(lastRadar && lastRadar.connected), down = [];
    recState.channels.forEach(function (c) {
      var conn = c.connected === true || c.connected === 1;
      if (conn) return;
      if ((c.name === "eo_y10" || c.name === "eo_jpeg") && eoUp) { if (down.indexOf("EO") < 0) down.push("EO"); }
      else if (c.name === "radar_raw" && radarUp) { if (down.indexOf("RADAR") < 0) down.push("RADAR"); }
    });
    return down;
  }
  var replaySession = null, replayStatePoll = null, scrubbing = false, scrubThrottle = 0;
  var replayHasEO = true, replayHasRadar = true;   /* per-session: was that channel recorded? */
  var replayPlaying = false, replayStillT = -1;    /* EO pane: MJPEG stream while playing, still frame while paused */
  var replayVideoSrc = "display";                  /* which recorded video channel replay shows (native = full frame) */
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
    if (healing) return;                 /* the heal flow owns recState + the button while re-attaching */
    fetch("/rec/stats").then(function (r) { return r.json(); }).then(function (d) {
      recState = d;
      var rec = $("rec");
      if (!d || d.connected === false) { rec.classList.remove("rec-warn"); rec.classList.add("rec-off"); rec.classList.remove("rec-on"); rec.textContent = "REC"; rec.title = "RECORDER NOT CONNECTED"; return; }
      rec.classList.remove("rec-off");
      if (d.state === "recording") { rec.classList.remove("rec-warn"); rec.title = ""; rec.classList.add("rec-on"); rec.textContent = fmtClock(d.rec_elapsed_s * 1000); }
      else {
        rec.classList.remove("rec-on"); rec.textContent = "REC";
        var down = recTapsDown();        /* feed live but recorder detached → warn on the button itself */
        if (down.length) { rec.classList.add("rec-warn"); rec.title = "Recorder tap DOWN (" + down.join("+") + ") — press REC to re-attach, then record"; }
        else { rec.classList.remove("rec-warn"); rec.title = ""; }
        if (d.pending_sid && !pendingSid && !handledSids[d.pending_sid] && $("recdlg").hidden && !replaying) openSaveDialog(d.pending_sid);
      }
    }).catch(function () { var rec = $("rec"); rec.classList.remove("rec-warn"); rec.classList.add("rec-off"); rec.classList.remove("rec-on"); rec.textContent = "REC"; rec.title = "RECORDER NOT CONNECTED"; });
  }
  $("rec").onclick = function () {
    if (replaying || $("rec").classList.contains("rec-off") || healing) return;
    if (recState && recState.state === "recording") {
      fetch("/rec/ctl?rec=stop").then(function (r) { return r.json(); }).then(function (d) { if (d && d.sid) openSaveDialog(d.sid); }).catch(function () {});
      return;
    }
    var down = recTapsDown();
    if (down.length) startRecWithHeal(down);     /* tap detached → re-attach the systems first, THEN record */
    else fetch("/rec/ctl?rec=start").catch(function () {});
  };

  /* The operator hit REC while a recorder tap was DOWN. Don't silently record nothing:
   * (1) show WHAT is down, (2) ask the launcher (:8088) to bounce the recorder so its shm
   * taps re-bind to the live feeds, (3) poll until the tap is back, then start recording.
   * This is the "hit record → it shows me + turns the systems on for recording" behaviour. */
  function startRecWithHeal(down) {
    if (healing) return;
    healing = true;
    var rec = $("rec");
    rec.classList.add("rec-warn"); rec.classList.remove("rec-on"); rec.textContent = "ATTACH…";
    toast("Recorder tap was DOWN (" + down.join("+") + ") — re-attaching before recording…", "warn", 8000);
    fetch(location.protocol + "//" + location.hostname + ":8088/reattach", { mode: "no-cors" }).catch(function () {});
    var tries = 0;
    (function waitAttach() {
      tries++;
      fetch("/rec/stats").then(function (r) { return r.json(); }).then(function (d) {
        recState = d;
        if (d && d.connected !== false && !recTapsDown().length) {
          healing = false;                                   /* taps back — record */
          fetch("/rec/ctl?rec=start").catch(function () {});
          toast("Recorder re-attached — recording.", "ok", 3500);
        } else if (tries < 12) { setTimeout(waitAttach, 1000); }   /* recorder is restarting */
        else {
          healing = false; rec.textContent = "REC";
          toast("Recorder still not attached (" + (recTapsDown().join("+") || "?") + "). Press START on the control page to restart the feeds.", "err", 9000);
        }
      }).catch(function () {                                  /* /rec/stats blips while the recorder restarts */
        if (tries < 12) setTimeout(waitAttach, 1000);
        else { healing = false; rec.textContent = "REC"; toast("Recorder didn't come back. Check the launcher.", "err", 9000); }
      });
    })();
  }

  /* ── save dialog ── */
  function openSaveDialog(sid) {
    pendingSid = sid;
    $("dlg-name").value = "REC " + localStamp();
    $("dlg-note").value = "";
    var tw = $("dlg-tags"); tw.innerHTML = "";
    TAGVOCAB.forEach(function (t) { var c = document.createElement("span"); c.className = "tagchip"; c.textContent = t; c.onclick = function () { c.classList.toggle("on"); }; tw.appendChild(c); });
    $("dlg-discard").style.display = "";                 /* fresh recording -> discard allowed */
    $("recdlg").hidden = false; $("dlg-name").focus();
  }
  /* Edit an already-saved recording: same dialog, pre-filled, DISCARD hidden (it would delete);
   * SAVE just updates name/tags/note. Reuses the pendingSid + dlg-save flow. */
  function openEditDialog(s) {
    pendingSid = s.sid;
    $("dlg-name").value = s.name || "";
    $("dlg-note").value = s.note || "";
    var tw = $("dlg-tags"); tw.innerHTML = "";
    var cur = s.tags || [];
    var mk = function (t, on) { var c = document.createElement("span"); c.className = "tagchip" + (on ? " on" : ""); c.textContent = t; c.onclick = function () { c.classList.toggle("on"); }; tw.appendChild(c); };
    TAGVOCAB.forEach(function (t) { mk(t, cur.indexOf(t) >= 0); });
    cur.filter(function (t) { return TAGVOCAB.indexOf(t) < 0; }).forEach(function (t) { mk(t, true); });   /* keep custom tags */
    $("dlg-discard").style.display = "none";             /* editing a saved clip -> no delete here */
    $("recdlg").hidden = false; $("dlg-name").focus();
  }
  /* copy to clipboard — works over plain http (the async clipboard API needs a secure context,
   * which we don't have on the LAN/AP, so fall back to a hidden textarea + execCommand). */
  function copyText(t) {
    var ok = function () { toast("Copied: " + t, "ok", 1800); };
    if (navigator.clipboard && navigator.clipboard.writeText && window.isSecureContext) {
      navigator.clipboard.writeText(t).then(ok).catch(function () { copyFallback(t, ok); });
    } else copyFallback(t, ok);
  }
  function copyFallback(t, ok) {
    var ta = document.createElement("textarea"); ta.value = t;
    ta.style.position = "fixed"; ta.style.top = "-1000px"; ta.setAttribute("readonly", "");
    document.body.appendChild(ta); ta.select(); try { ta.setSelectionRange(0, t.length); } catch (e) {}
    var done = false; try { done = document.execCommand("copy"); } catch (e) {}
    document.body.removeChild(ta);
    done ? ok() : toast("Couldn't copy — select the name manually", "err", 3000);
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
    var sid = pendingSid;
    handledSids[sid] = 1;    /* mark BEFORE closing so the ~400ms poll can't re-offer this sid */
    var tags = [].slice.call(document.querySelectorAll("#dlg-tags .tagchip.on")).map(function (c) { return encodeURIComponent(c.textContent); }).join(",");
    fetch("/rec/ctl?save=" + encodeURIComponent(sid) + "&name=" + encodeURIComponent($("dlg-name").value) + "&tags=" + tags + "&note=" + encodeURIComponent($("dlg-note").value))
      .then(function (r) { if (!r.ok) throw 0; if (!$("library").hidden) loadLibrary(); })
      .catch(function () { delete handledSids[sid]; alert("Save failed — the recorder didn't confirm. Try again."); });
    closeSaveDialog();
  };
  $("dlg-discard").onclick = function () {
    if (!pendingSid) return;
    if (!confirm("Discard this recording? It can't be recovered.")) return;
    var sid = pendingSid;
    handledSids[sid] = 1;
    fetch("/rec/ctl?discard=" + encodeURIComponent(sid)).catch(function () { delete handledSids[sid]; });
    closeSaveDialog();
  };

  /* ── LIBRARY ── */
  $("libbtn").onclick = function () {
    /* If we're inside a replay, LIBRARY means EXIT-to-list — run the FULL replay teardown
     * (closes the state poll + overlay polls + sends close=1 to the recorder), otherwise the
     * poll and the recorder session leak. closeReplay already returns to the library list. */
    if (replaying) { closeReplay(); return; }
    $("library").hidden = false;
    /* The library list needs NONE of the live media streams, and a browser only allows ~6
     * sockets per host. Holding video+radar+det open here leaves too few slots for the
     * replay-open request, which then queues/times out ("tap a movie, nothing happens").
     * Drop every long-lived stream so browsing (and opening) always has free sockets. */
    if (radarES) { radarES.close(); radarES = null; }
    if (detES)   { detES.close();   detES = null; }
    $("video").src = BLANK;
    buildTagFilter(); loadLibrary();
  };
  $("lib-close").onclick = function () {   /* EXIT to live -> reconnect the live streams */
    $("library").hidden = true; libSel = {};
    API.stream = "/stream"; API.radar = "/radar"; API.stats = "/stats"; API.rstats = "/rstats";
    $("video").src = API.stream + "?t=" + Date.now();
    openRadarStream(); openDetStream();
  };
  $("lib-q").oninput = debounce(loadLibrary, 250);
  function buildTagFilter() {
    var w = $("lib-tagfilter"); if (w.childElementCount) return;
    TAGVOCAB.forEach(function (t) { var c = document.createElement("span"); c.className = "tagchip"; c.textContent = t; c.onclick = function () { c.classList.toggle("on"); libTagFilter[t] = c.classList.contains("on"); loadLibrary(); }; w.appendChild(c); });
  }
  function loadLibrary() {
    /* Tag filtering is done CLIENT-SIDE in renderLibrary (every session already carries its
     * tags), so it can't be broken by query-encoding/proxy quirks — which was why filtering
     * "lost" all annotations. Only the free-text search still goes to the recorder. */
    fetch("/rec/library?q=" + encodeURIComponent($("lib-q").value || ""))
      .then(function (r) { return r.json(); }).then(renderLibrary)
      .catch(function () { $("lib-grid").innerHTML = ""; $("lib-empty").hidden = false; $("lib-empty").textContent = "RECORDER NOT CONNECTED"; });
  }
  var libTimers = [];   /* hover-cycler intervals — cleared on every re-render so a card removed
                         * mid-hover (its onmouseleave never fires) can't leak its interval. */
  function renderLibrary(d) {
    libTimers.forEach(function (t) { clearInterval(t); }); libTimers = [];
    if (d.disk_total_gb) {
      $("lib-diskbar").style.setProperty("--used", (100 * (1 - d.disk_free_gb / d.disk_total_gb)).toFixed(0) + "%");
      $("lib-disktext").textContent = Math.round(d.disk_free_gb) + " GB free" + (recState && recState.est_min_remaining ? " · ~" + recState.est_min_remaining + " min" : "");
    }
    var g = $("lib-grid"); g.innerHTML = "";
    var all = d.sessions || [];
    var active = Object.keys(libTagFilter).filter(function (t) { return libTagFilter[t]; });   /* AND-match the selected tags */
    var sessions = active.length ? all.filter(function (s) { var st = s.tags || []; return active.every(function (t) { return st.indexOf(t) >= 0; }); }) : all;
    libSessions = sessions;   /* the shown set — delete/offload-all act on what's filtered */
    $("lib-empty").hidden = sessions.length > 0; $("lib-empty").textContent = active.length ? "No sessions match those tags." : "No sessions.";
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
      card.onmouseenter = function () { if (timer) return; timer = setInterval(function () { i = (i + 1) % 8; poster.src = "/rec/thumbs/" + s.sid + "/" + i + ".jpg"; }, 166); libTimers.push(timer); };
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
    var nrow = document.createElement("div"); nrow.className = "lib-namerow";
    var nm = document.createElement("span"); nm.className = "lib-name"; nm.textContent = s.name || s.sid; nm.title = s.name || s.sid;
    var eb = document.createElement("button"); eb.className = "lib-act"; eb.textContent = "✎"; eb.title = "edit name / tags / note";
    eb.onclick = function (e) { e.stopPropagation(); openEditDialog(s); };
    var cpb = document.createElement("button"); cpb.className = "lib-act"; cpb.textContent = "⧉"; cpb.title = "copy name";
    cpb.onclick = function (e) { e.stopPropagation(); copyText(s.name || s.sid); };
    nrow.appendChild(nm); nrow.appendChild(eb); nrow.appendChild(cpb); body.appendChild(nrow);
    var rest = document.createElement("div");
    rest.innerHTML = '<div class="lib-meta"><span>' + libDate(s.t0) + "</span><span>" + fmtClock(s.dur_ms) + "</span></div>"
      + '<div class="lib-meta lib-size">' + sizeBadge(s.bytes) + "</div>"
      + '<div class="lib-cardtags">' + (s.tags || []).map(function (t) { return '<span class="tagchip">' + esc(t) + "</span>"; }).join("") + "</div>"
      + (s.note ? '<div class="lib-note">' + esc(s.note) + "</div>" : "");
    body.appendChild(rest); card.appendChild(body);
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
  /* NOTE: the live sensors are NO LONGER stopped while reviewing recordings. Stopping and
   * restarting eo_pipeline/radar/detector on every library visit thrashed the box — the camera
   * re-init + detector engine reload pegged the CPU and wedged the camera after a few enter/exit
   * cycles (black EO, unrecoverable without a reboot). The recorder pre-builds the replay mp4,
   * so keeping the live stack running during review costs nothing extra. The browser's live
   * media streams are still closed on library-open (connection budget) and reopened on exit. */
  var opening = false;
  function openReplay(s) {
    if (opening) return;                                 /* ignore double-taps while an open is in flight */
    if (!s || !s.sid) return;                            /* malformed session -> nothing to open */
    opening = true;
    var done = false;
    var fin = function () { if (done) return false; done = true; clearTimeout(guard); opening = false; return true; };
    /* Arm the timeout FIRST so `opening` is ALWAYS released — even if the synchronous setup below
     * throws (a null DOM node, a bad session). Otherwise a throw before the fetch left `opening`
     * stuck true and the open-recording button dead until a page reload. */
    var guard = setTimeout(function () {                 /* recorder hung/slow -> free the button + tell the user */
      if (fin()) toast("Recorder didn't respond — tap the recording again.", "err", 4500);
    }, 9000);
    try {
    /* Free browser connection slots BEFORE the open fetch. HTTP/1.1 caps ~6 sockets per host and
     * the console holds long-lived streams (MJPEG video + radar/det) — so the open request would
     * otherwise queue behind them and hit the 9 s timeout. Drop them now; replay reopens its own
     * (poll-based) overlays on success, or closeReplay reopens live streams on exit. */
    if (radarES) { radarES.close(); radarES = null; }
    if (detES) { detES.close(); detES = null; }
    stopReplayRadarPoll(); stopReplayDetPoll();
    $("video").src = BLANK;
    /* Close any prior/stale replay session FIRST, then open. A leftover open on the recorder
     * (tab closed mid-replay, or a fast close->reopen) is exactly what makes a tap "do nothing"
     * until you leave the library/app — serialising close->open clears it. Silent failure
     * (the old empty .catch) is why it looked like a dead button; now it reports + you retry. */
    fetch("/rec/replay/ctl?close=1").catch(function () {})
      .then(function () { return fetch("/rec/replay/ctl?open=" + encodeURIComponent(s.sid)); })
      .then(function (r) {
        if (done) return;                                /* already timed out */
        if (!r || !r.ok) throw 0;
        fin();
      replaySession = s; replaying = true;
      if (radarES) { radarES.close(); radarES = null; }   /* stop the live radar push while reviewing */
      if (detES) { detES.close(); detES = null; } lastDet = null;   /* live det push off; replay poller takes over */
      resetReplayZoom(); setZoomLabel(); radarROI = null; eoROI = false; roiArm = false; setRoiUI();
      /* "was this channel recorded?" from the actual captured bytes — NOT thumbs (a
       * session can have EO video with no thumbnails generated). */
      replayHasEO = !!(s.bytes && ((s.bytes.display > 0) || (s.bytes.native > 0)));
      replayHasRadar = !!(s.bytes && s.bytes.radar > 0);
      document.body.classList.add("replay"); $("library").hidden = true;
      /* Default to the INSTANT display (MJPEG) view. Native 60fps is opt-in via the DISPLAY/NATIVE
       * toggle: opening native makes the recorder ffmpeg-transcode the WHOLE recording (~2 cores)
       * and it is NOT killed on close, so browsing recordings in native stacked encodes and pegged
       * the box. Display needs no transcode, so browsing is now free. */
      rctl("video=display");
      API.stream = "/rec/replay/stream"; API.radar = "/rec/replay/radar"; API.stats = "/rec/replay/stats"; API.rstats = "/rec/replay/rstats";
      openReplayRadarStream();                            /* replay radar — self-chaining poll (one socket max) */
      openReplayDetStream();                              /* replay det boxes — self-chaining poll (one socket max) */
      /* Show the recorded still at the open position — NOT the live stream. The replay
       * MJPEG only pushes while playing, so before Play we'd otherwise keep showing the
       * last live frame. pollReplayState swaps to the stream once playback starts. */
      replayPlaying = false; replayStillT = -1; resetMp4State();   /* start on MJPEG; poll swaps to mp4 when ready */
      sawOpen = false; replayBad = 0; replayFrameRetries = 0;      /* fresh open: grace the self-heal + still-frame retry */
      $("video").src = replayHasEO ? ("/rec/replay/frame?t=0") : BLANK;
      $("rb-text").innerHTML = "REPLAY — " + esc(s.name || s.sid) + " — " + esc(s.t0)
        + (s.note ? ' <span class="rb-note">“' + esc(s.note) + '”</span>' : "");
      $("tp-dur").textContent = fmtClockT(s.dur_ms); $("tp-scrub").max = s.dur_ms; $("tp-scrub").value = 0;
      if (replayStatePoll) clearInterval(replayStatePoll);
      replayStatePoll = setInterval(pollReplayState, 150); pollReplayState();
      })
      .catch(function () { if (fin()) toast("Couldn't open that recording — tap it again.", "err", 4500); });
    } catch (e) { if (fin()) toast("Couldn't open that recording — tap it again.", "err", 4500); }
  }
  function closeReplay() {
    fetch("/rec/replay/ctl?close=1").catch(function () {});
    replaying = false; replaySession = null; document.body.classList.remove("replay");
    if (replayStatePoll) { clearInterval(replayStatePoll); replayStatePoll = null; }
    /* CLOSE the replay SSE EventSources NOW — this was the leak. They were only closed on the
     * NEXT open, so every exit left radar+det SSE draining; a browser allows ~6 sockets/host and
     * SSE teardown over WiFi is slow, so 3-4 quick open/exit cycles stacked 6 half-closed sockets
     * (the 6× FIN-WAIT-1 we saw) and the next open had nowhere to connect. Closing on exit gives
     * them the whole library-browsing time to drain before the next open. */
    if (radarES) { radarES.close(); radarES = null; }
    if (detES)   { detES.close();   detES = null; } lastDet = null;
    stopReplayRadarPoll(); stopReplayDetPoll();          /* drop any replay fallback polls */
    resetMp4State();                                     /* stop + release the native mp4 <video> */
    API.stream = "/stream"; API.radar = "/radar"; API.stats = "/stats"; API.rstats = "/rstats";
    /* Returning to the library LIST — do NOT reopen the live streams here; the list holds no
     * long-lived sockets so the next replay-open always has room. lib-close reopens live. */
    $("video").src = BLANK;
    resetReplayZoom(); setZoomLabel(); radarROI = null; eoROI = false; roiArm = false; setRoiUI();
    $("library").hidden = false; loadLibrary();
  }
  $("rb-close").onclick = closeReplay;
  /* Native replay prefers the recorded H.264 mp4 (/rec/replay/native.mp4) — full 60 fps and
   * small over WiFi — over the MJPEG stream the recorder caps at 20 fps. The mp4 is transcoded
   * on demand; readiness is probed from the endpoint itself (1-byte range: 200/206 = ready,
   * 202 = building{pct}) so it works whatever recorder build is running (the deployed one
   * doesn't report mp4 status in /state). The <video> is SLAVED to the transport clock so the
   * det/radar overlays — paced by the recorder clock — stay in sync. Display channel, radar-
   * only, or while-building all fall back to the paced MJPEG <img>. */
  var mp4State = { sid: null, ready: false, pct: 0, nextProbe: 0, srcSet: false };
  function resetMp4State() {
    mp4State = { sid: null, ready: false, pct: 0, nextProbe: 0, srcSet: false };
    var nv = $("nvid"); nv.pause(); nv.removeAttribute("src"); nv.load();
    nv.style.display = "none"; $("video").style.display = ""; $("nat-badge").hidden = true;
  }
  function probeMp4(sid) {
    mp4State.nextProbe = Date.now() + 1500;                        /* throttle re-probes */
    fetch("/rec/replay/native.mp4?sid=" + encodeURIComponent(sid), { headers: { Range: "bytes=0-0" } })
      .then(function (r) {
        if (mp4State.sid !== sid) return;
        if (r.status === 200 || r.status === 206) { mp4State.ready = true; mp4State.pct = 100; }
        else if (r.status === 202) { r.json().then(function (j) { if (mp4State.sid === sid) mp4State.pct = (j && (j.pct != null ? j.pct : j.percent)) || 0; }).catch(function () {}); }
        else mp4State.pct = -1;                                    /* unavailable -> stay on MJPEG */
      }).catch(function () {});
  }
  function updateReplayVideo(rs) {
    var vid = $("video"), nv = $("nvid"), badge = $("nat-badge");
    var sid = replaySession && replaySession.sid;
    if (rs.video_src === "native" && sid) {
      if (mp4State.sid !== sid) { resetMp4State(); mp4State.sid = sid; }
      if (!mp4State.ready && mp4State.pct !== -1 && Date.now() >= mp4State.nextProbe) probeMp4(sid);
      if (mp4State.ready) {                                        /* play the mp4, loosely following the clock */
        if (!mp4State.srcSet) { mp4State.srcSet = true; nv.src = "/rec/replay/native.mp4?sid=" + encodeURIComponent(sid); vid.src = BLANK; }  /* BLANK stops the hidden MJPEG stream */
        nv.style.display = "block"; vid.style.display = "none"; badge.hidden = true;   /* "" would revert to the stylesheet's display:none -> black */
        replayPlaying = false; replayStillT = -1;                 /* re-arm MJPEG src if we switch back */
        var tv = rs.t_ms / 1000;
        nv.playbackRate = rs.rate || 1;
        if (rs.playing && rs.t_ms < rs.dur_ms) {
          if (nv.paused) nv.play().catch(function () {});
          /* Let the <video> FREE-RUN on its own smooth decode clock; only re-sync to the
           * transport clock on a BIG drift (a scrub, a decode stall, a throttled background
           * tab). Nudging currentTime every poll turned smooth playback into constant seeking
           * — that was the passive-watch stutter. Keyframe-per-second mp4 makes the rare
           * correction cheap. */
          if (!scrubbing && nv.readyState >= 1 && isFinite(tv) && Math.abs(nv.currentTime - tv) > 0.3) nv.currentTime = tv;
        } else {
          if (!nv.paused) nv.pause();
          /* paused: pin the exact frame to the transport position. While actively SCRUBBING the
           * scrub handler seeks the video directly (immediate, per-input) — don't fight it here
           * with the laggier poll-clock value, or the frame appears to freeze during the drag. */
          if (!scrubbing && nv.readyState >= 1 && isFinite(tv) && Math.abs(nv.currentTime - tv) > 0.05) nv.currentTime = tv;
        }
        return;
      }
      badge.hidden = mp4State.pct === -1;                          /* building -> show progress */
      if (!badge.hidden) badge.textContent = "PREPARING NATIVE 60 fps · " + mp4State.pct + "%";
    } else {
      if (mp4State.sid !== null) resetMp4State();                  /* left native -> tear the video down */
      badge.hidden = true;
    }
    /* MJPEG path: stream while playing, recorded still while paused/scrubbed */
    nv.style.display = "none"; vid.style.display = "";
    if (rs.playing && rs.t_ms < rs.dur_ms) {
      if (!replayPlaying) { replayPlaying = true; vid.src = "/rec/replay/stream?t=" + Date.now(); }
    } else {
      replayPlaying = false;
      if (rs.t_ms !== replayStillT) { replayStillT = rs.t_ms; vid.src = "/rec/replay/frame?t=" + rs.t_ms; }
    }
  }
  var replayBad = 0, sawOpen = false;
  function pollReplayState() {
    fetch("/rec/replay/state").then(function (r) { if (!r.ok) throw 0; return r.json(); }).then(function (st) {
      /* Self-heal a STUCK replay state, but only once the session was actually seen open and then
       * lost (recorder restart/reboot) — otherwise every poll 404-floods and card taps do nothing.
       * On the INITIAL open the recorder can take a moment to register the session; healing then is
       * what caused "tap a movie -> nothing" (it kicked you back out mid-open). So: not-yet-open ->
       * long grace (~6 s) before giving up; already-seen-open -> fast heal (~0.5 s) if it vanishes. */
      if (st && st.open === false) {
        var lim = sawOpen ? 3 : 40;
        if (replaying && ++replayBad >= lim) { replayBad = 0; sawOpen = false; closeReplay(); }
        return;
      }
      sawOpen = true; replayBad = 0;
      var rs = st.replay_state || st.state || st; if (!rs) return;   /* /state nests as .state, /stats as .replay_state */
      if (replayHasEO) updateReplayVideo(rs);   /* native mp4 (60 fps) or paced MJPEG, + overlay sync */
      if (!scrubbing) { $("tp-scrub").value = rs.t_ms; $("tp-cur").textContent = fmtClockT(rs.t_ms); }
      $("tp-play").textContent = (rs.playing && rs.t_ms < rs.dur_ms) ? "⏸" : "⏵";
      $("tp-rate").textContent = rs.rate + "×";
      /* NATIVE/DISPLAY toggle — only when a native channel was recorded */
      replayVideoSrc = (rs.video_src === "native") ? "native" : "display";
      if (rs.has_native) { $("tp-video").hidden = false; $("tp-video").textContent = (rs.video_src === "native") ? "NATIVE" : "DISPLAY"; }
      else $("tp-video").hidden = true;
      if (rs.t_wall_ms) { var d = new Date(rs.t_wall_ms); $("v-zulu").textContent = "REC " + ("0" + d.getUTCHours()).slice(-2) + ":" + ("0" + d.getUTCMinutes()).slice(-2); }
    }).catch(function () { $("eo-scrim").hidden = false; if (replaying && sawOpen && ++replayBad >= 3) { replayBad = 0; sawOpen = false; closeReplay(); } });
  }
  $("tp-play").onclick = function () { rctl($("tp-play").textContent === "⏸" ? "pause=1" : "play=1"); };
  $("tp-rate").onclick = function () { var i = (RATES.indexOf(parseFloat($("tp-rate").textContent)) + 1) % RATES.length; rctl("rate=" + RATES[i]); };
  $("tp-video").onclick = function () { rctl("video=" + ($("tp-video").textContent === "NATIVE" ? "display" : "native")); };
  $("tp-step-b").onclick = function () { rctl("step=-1"); };
  $("tp-step-f").onclick = function () { rctl("step=1"); };
  $("tp-scrub").oninput = function () {
    scrubbing = true; var v = +this.value; $("tp-cur").textContent = fmtClockT(v);
    /* Native replay: seek the mp4 DIRECTLY on each scrub input so the picture tracks the drag,
     * like the DISPLAY still does. Without this the frame only moved on the 150 ms poll and the
     * mp4's own seeks lagged behind, so it looked frozen while scrubbing. */
    var nv = $("nvid");
    if (mp4State.ready && mp4State.srcSet && nv.readyState >= 1) nv.currentTime = v / 1000;
    var now = Date.now(); if (now - scrubThrottle >= 80) { scrubThrottle = now; rctl("seek=" + Math.round(v)); }
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
  /* NO video-stream watchdog. It caused more harm than it fixed: its reconnect blanked the
   * <img> (src="") to BLACK, and at native res the reload can't complete before the next
   * check, so the video stays black while the overlays keep running — the exact bug seen.
   * At low res it showed as a periodic blink. A genuinely dead stream is rare; a manual
   * refresh recovers it. If auto-recovery is ever wanted, it must NOT blank the frame
   * (assign the new src straight onto a hidden shadow <img>, swap on its load). */

  setInterval(poll, 160); poll();
  openRadarStream();   /* live = SSE push; replay opens its own radar SSE (poll fallback) in openReplay */
  openDetStream();                                  /* EO detector boxes (SSE push, live); replay opens its own det SSE (poll fallback) in openReplay */
  initRadarOv();                                    /* radar→EO overlay toggle + trims (persisted) */
  initDetStyle();                                   /* detector mark style BOX/SEEKER (persisted) */
  initLka();                                        /* link MANUAL/AUTO quality mode (persisted) */
  setInterval(pollRstats, 400); pollRstats();
  setInterval(pollDstats, 1000); pollDstats();
  setInterval(pollRec, 400); pollRec();
})();
