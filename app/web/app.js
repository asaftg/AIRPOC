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
  /* MANUAL is the default: AUTO needs a ranking we have not agreed yet (it would pick row #1
   * of the merged list). Manual means the operator declares the target — nothing self-selects. */
  var trackMode = "man", engagedKey = null, sentEngage = null, sentTrkEngage = null, trkEngageSentAt = 0;
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
  $("nvid").addEventListener("error", function () { if (mp4State.srcSet) resetMp4State(); });   /* mp4 failed to load -> fall back to the MJPEG path */
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
    if (m === "auto") { engage(null); }
    updateTrackBanner();
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
  var EXP_DEFAULTS = { gaincap: 120, median: 0 };   /* console default AUTO state (median OFF by operator request) */
  document.querySelectorAll("#ae-btns [data-ae]").forEach(function (b) {
    b.onclick = function () {
      setSeg("ae-btns", b); var auto = b.dataset.ae === "1"; setExpMode(auto); ispTouch = Date.now();
      ctl("ae=" + b.dataset.ae);
      if (auto) {   /* returning to AUTO resets the sensor to defaults */
        ctl("gaincap=" + EXP_DEFAULTS.gaincap); $("s-gcap").value = EXP_DEFAULTS.gaincap; $("o-gcap").textContent = EXP_DEFAULTS.gaincap;
        ctl("median=" + EXP_DEFAULTS.median);   /* no MEDIAN control any more — just keep it off */
      }
    };
  });
  /* MEDIAN and DENOISE have no controls any more — the operator keeps both off, so the console
   * just pushes them off on load (see the defaults block at the bottom) and never shows a knob. */
  document.querySelectorAll("#disp-btns [data-disp]").forEach(function (b) {
    b.onclick = function () { setSeg("disp-btns", b); ispTouch = Date.now(); ctl("disp_fps=" + b.dataset.disp); };
  });
  function setSeg(id, on) { document.querySelectorAll("#" + id + " button").forEach(function (x) { x.classList.remove("on"); }); on.classList.add("on"); }
  /* Each knob is live only in the mode where it means something:
   *  - EXP ms / GAIN: MANUAL only — in AUTO the exposure loop owns them.
   *  - AUTO-CAP: AUTO only — it's the ceiling the loop may raise gain to; in MANUAL gain
   *    is set directly, so a cap does nothing. */
  function setExpMode(auto) {
    $("s-exp").disabled = auto; $("s-gain").disabled = auto;
    $("s-gcap").disabled = !auto;
  }
  /* moving EXP or GAIN drops the feed to MANUAL — reflect that optimistically */
  $("s-exp").oninput  = function () { $("o-exp").textContent = (+this.value).toFixed(2); manualAE(); ctl("expms=" + this.value); };
  $("s-gain").oninput = function () { $("o-gain").textContent = this.value; manualAE(); ctl("gain=" + this.value); };
  $("s-gcap").oninput = function () { $("o-gcap").textContent = this.value; ctl("gaincap=" + this.value); };
  function manualAE() { var m = document.querySelector('#ae-btns [data-ae="0"]'); if (m) setSeg("ae-btns", m); setExpMode(false); ispTouch = Date.now(); }
  var ispTouch = 0, fpsTouch = 0;

  /* stream bandwidth levers — res (display size) + fps cap, both live on the EO feed */
  /* QUALITY is always MANUAL — the operator's pick stands until they change it. (There used to
   * be a LINK MANUAL/AUTO toggle that stepped QUALITY down on a saturated link and probed back
   * up; removed at the operator's request — unused, and silently moving their setting is worse
   * than a visibly degraded picture.) */
  document.querySelectorAll("#res-btns [data-res]").forEach(function (b) {
    b.onclick = function () { setSeg("res-btns", b); ctl("res=" + b.dataset.res); };
  });
  $("s-fps").oninput = function () { $("o-fps").textContent = this.value; fpsTouch = Date.now(); ctl("fps=" + this.value); };

  /* ── radar controls — FOUR, on purpose (radar/docs/CONSOLE_CONTROLS.md). Sent namespaced as
   * radar_<key>= (the app strips the prefix → daemon /ctl); clamps are server-side. Init +
   * readback come from /rstats (the daemon's own /stats).
   *
   * The daemon still accepts all TEN knobs and `/ctl` still forwards all ten — the other six
   * (eps, minpts, doppler, confirm, coast, park) simply have no place in front of an operator:
   *   - they are tracker internals with no physical meaning ("merge gate 1.2 m/s" is not
   *     something anyone can reason about in a field), exposed originally for development;
   *   - and they are a VALIDATED SET. The defaults are the offline-validated operating point,
   *     pinned as a knob_vector in radar/tools/regression/tracker_baseline.json. Nudging one on
   *     the rig silently invalidates every corpus result with no record of what changed.
   * Bench tuning goes through radar/tools/track_replay.c against the corpus, not through here.
   *
   * The four that stay are the ones that answer physical questions: where to look (FOV / EL),
   * how sensitive to be (MIN SNR), and what counts as moving (MIN SPD). MIN SNR and MIN SPD
   * drive BOTH detectors — the per-frame clusterer and the slow chainer are one tracker to the
   * operator, so there is one pair of controls for both and deliberately no separate switch. ── */
  var RADARC = [
    { key: "fov",     stat: "fov_half_deg",     fmt: function (v) { return v.toFixed(0) + "°"; } },
    { key: "elmax",   stat: "el_max_deg",       fmt: function (v) { return v.toFixed(0) + "°"; } },
    { key: "snrmin",  stat: "snr_min_db",       fmt: function (v) { return v.toFixed(0) + " dB"; } },
    { key: "speed",   stat: "speed_min_mps",    fmt: function (v) { return v.toFixed(1) + " m/s"; } }
  ];
  var rcTouch = 0;
  RADARC.forEach(function (c) {
    $("rd-" + c.key).oninput = function () { $("rv-" + c.key).textContent = c.fmt(parseFloat(this.value)); rcTouch = Date.now(); ctl("radar_" + c.key + "=" + this.value); };
  });
  /* On load the console pushes the operator's chosen radar defaults (FOV ±60°, EL ±20°) — see the
   * load-defaults block below. Between reloads the sliders seed from the radar's live value via
   * pollRstats; the rcTouch guard keeps a readback from fighting an active drag or the load push. */
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
    /* max_dets has no control — fixed at MAX_DETS below, pushed on load. */
    { key: "mot_k",        fmt: function (v) { return v.toFixed(1); } },
    { key: "mot_persist",  fmt: function (v) { return String(v | 0); } },
    { key: "mot_window_s", fmt: function (v) { return v.toFixed(1) + " s"; } },
    /* temporal integration — the rows stay hidden until the detector reports these knobs */
    { key: "tbd_frames",   fmt: function (v) { return String(v | 0); } },
    { key: "tbd_lo",       fmt: function (v) { return v.toFixed(2); } }
  ];
  var dtTouch = 0;
  DETC.forEach(function (c) {
    $("dt-" + c.key).oninput = function () { $("dv-" + c.key).textContent = c.fmt(parseFloat(this.value)); dtTouch = Date.now(); ctl("det_" + c.key + "=" + this.value); };
  });
  document.querySelectorAll("#mot-btns [data-mot]").forEach(function (b) {
    b.onclick = function () { dtTouch = Date.now(); ctl("det_motion=" + b.dataset.mot); setSeg("mot-btns", b); };
  });
  document.querySelectorAll("#tmp-btns [data-tmp]").forEach(function (b) {
    b.onclick = function () { dtTouch = Date.now(); ctl("det_temporal=" + b.dataset.tmp); setSeg("tmp-btns", b); };
  });
  function pollDstats() {
    if (replaying) return;
    /* Polled even with DEV closed (it's ~1 small request/s): the OVERLAY needs to know whether
     * temporal integration is actually running, so it can decide whether the "t" provenance
     * marker means anything at all. */
    var devOpen = $("dev").classList.contains("open");
    fetch("/dstats").then(function (r) { return r.json(); }).then(function (d) {
      var k = d.knobs || {};
      if (typeof k.temporal === "number") detTemporalOn = (k.temporal === 1);
      if (!devOpen) return;                     /* everything below is DEV-panel UI */
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
      /* Temporal integration: no buttons for a control the running detector can't honour — the
       * rows appear by themselves once it reports the knobs (same gate as DISPLAY 30/60). */
      if (typeof k.temporal === "number") {
        document.querySelectorAll(".tbd-row").forEach(function (r) { r.hidden = false; });
        var tb = document.querySelector('#tmp-btns [data-tmp="' + k.temporal + '"]'); if (tb) setSeg("tmp-btns", tb);
      }
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

  /* ── ONE target list, TWO independent sensors ───────────────────────────────────────────
   * Radar targets and EO tracks are separate objects with separate id spaces — there is no
   * fusion yet, so nothing here dedups them (that is fusion's job, and guessing at it would
   * merge two different things). Each row is keyed "<src>:<tid>" so the ids can't collide,
   * and each sensor shows what it actually knows:
   *    EO  (tracker) — class, confidence, azimuth/elevation. No range: a camera has none.
   *    RDR (radar)   — range and speed. No class: the radar emits class-less boxes.
   *    FUS           — placeholder; fusion will emit range AND class on one row.
   * Persistence is each daemon's own: both run temporal trackers with stable ids, confirm
   * and coast, so rows come straight off the wire without a GUI-side hold (a hold here
   * would double-persist what the daemons already did). */
  function eoTracks() {
    if (!lastTrk || lastTrk.connected === false) return [];
    return (lastTrk.tracks || []).map(function (t) {
      var a = t.ang || [0, 0, 0, 0], D = 180 / Math.PI;
      return { src: "eo", tid: t.tid, key: "eo:" + t.tid, cls: t.cls || "unknown", conf: t.conf || 0,
               az: a[0] * D, el: a[1] * D, state: t.state, lock: t.lock, px: t.px, raw: t };
    });
  }
  function radarTargets() {
    return targets(lastRadar).map(function (t) {
      return { src: "rad", tid: t.tid, key: "rad:" + t.tid, rng: t.rng,
               spd: Math.hypot(t.vx, t.vy), az: t.az, el: t.el, conf: t.conf || 0, raw: t };
    });
  }
  /* ── FUSION (fusiond :8096) — when it is delivering frames it IS the target list ──────────
   * Fusion joins the two trackers into ONE picture: a fused row carries the radar's range and
   * the camera's angles + class together, under a global id (gid) that outlives per-sensor id
   * churn. Its wire guarantees a per-sensor track that appears as a constituent of a fused row
   * is never also published on its own, so we render targets[] VERBATIM — no client-side dedup
   * here, ever (guessing at a merge is exactly what fusion exists to stop).
   *
   * Rows are keyed by their CONSTITUENT ("eo:<eo_tid>" / "rad:<rad_tid>"), not by gid: the
   * video overlay and the PPI draw off the per-sensor wires at their own rate and only know
   * sensor ids, so a tap on a list row, a video box and a radar circle must all land on the
   * same key. gid still carries identity — it picks the row's colour and rides the FUS badge.
   *
   * Fusion angles are RIG FRAME and already include the radar<->EO mount trim (fusion owns that
   * trim now), so the console's own display trim is NOT added on top of them.
   *
   * Fusion is optional: down (or stale, or during replay, where it isn't recorded) the list
   * falls back to the two per-sensor lists exactly as before. */
  var lastFus = null, lastFusAt = 0, fusES = null, FUS_STALE_MS = 3000;
  function fusUp() {
    return !replaying && !!lastFus && lastFus.connected !== false && !!lastFus.targets
           && (Date.now() - lastFusAt) < FUS_STALE_MS;
  }
  function fusTargets() {
    var D = 180 / Math.PI;
    return (lastFus.targets || []).map(function (t) {
      var a = t.ang || [0, 0, 0, 0];
      var eo = (typeof t.eo_tid === "number") ? t.eo_tid : -1;
      var rd = (typeof t.rad_tid === "number") ? t.rad_tid : -1;
      return { src: t.src || "fus", gid: t.gid, eo_tid: eo, rad_tid: rd,
               key: eo >= 0 ? ("eo:" + eo) : ("rad:" + rd),
               cls: t.cls || "unknown", conf: t.conf || 0,
               az: a[0] * D, el: a[1] * D,
               rng: (typeof t.r_m === "number" && t.r_m >= 0) ? t.r_m : null,   /* -1 = no range */
               rdot: (typeof t.rdot_mps === "number") ? t.rdot_mps : null,
               rstale: t.r_stale === 1, raw: t };
    });
  }
  /* eo_tid / rad_tid -> the fused row it belongs to. The overlays keep drawing from the
   * per-sensor wires, so this is how a video box learns it is one half of a fused object:
   * it gets the fused mark (and the range fusion brought it), and the radar's own circle for
   * the other half is suppressed — one object, one mark on the video. */
  function fusedMap() {
    var m = { eo: {}, rad: {} };
    if (!fusUp()) return m;
    (lastFus.targets || []).forEach(function (t) {
      if (t.src !== "fus") return;
      if (typeof t.eo_tid === "number" && t.eo_tid >= 0) m.eo[t.eo_tid] = t;
      if (typeof t.rad_tid === "number" && t.rad_tid >= 0) m.rad[t.rad_tid] = t;
    });
    return m;
  }
  function allTargets() { return fusUp() ? fusTargets() : eoTracks().concat(radarTargets()); }

  /* Bearing reads as a SIDE, not a sign: "6R" / "7L" — an operator turning a head thinks
   * left/right, not positive/negative. Elevation keeps the sign, since up/down maps to it
   * naturally. Convention (both wires): azimuth + is right, elevation + is up. */
  function fmtAz(a) { var v = Math.round(Math.abs(a || 0)); return v + (a < 0 ? "L" : "R"); }
  function fmtEl(e) { var v = Math.round(Math.abs(e || 0)); return (e < 0 ? "-" : "+") + v + "°"; }

  /* ── RANKING ────────────────────────────────────────────────────────────────────────────────
   * One number per target, built from tiers that are far enough apart that a lower tier can
   * never outvote a higher one. Order of the tiers, most significant first:
   *
   *   1. SOURCE     fused > EO > radar. A fused row is two sensors agreeing; a radar row is an
   *                 unclassified blob. That is a real difference in how much the row is worth.
   *   2. MOVING     an EO/fused target whose bearing is actually changing outranks a static one,
   *                 whatever it is. A moving thing is the thing you care about.
   *   3. CLASS      human > vehicle > drone > unclassified.
   *   4. CONFIDENCE within a class, the better-supported target first.
   *   5. NEARNESS   pure tiebreak.
   *
   * The engaged target is not scored — it is pinned to the top separately, so the held target
   * can never scroll off no matter what else appears.
   *
   * Note this makes a moving VEHICLE outrank a stationary HUMAN. That follows "EO moving targets
   * are always stronger than static"; swap tiers 2 and 3 if class should win instead. */
  var CLS_RANK = { human: 300, vehicle: 200, drone: 100 };
  /* Bearing rate that counts as MOVING, in rad/s, with a Schmitt gap so a target sitting on the
   * threshold does not flip tiers every frame. 0.003 rad/s ~= 0.17 deg/s — above the tracker's
   * position jitter, below a walker at any range we care about. */
  var MOVE_ON = 0.003, MOVE_OFF = 0.0015;
  function isMoving(t, was) {
    var r = t.raw && t.raw.rate;
    if (!r || r.length < 2) return false;         /* radar rows carry no bearing rate */
    var m = Math.hypot(r[0] || 0, r[1] || 0);
    return was ? (m > MOVE_OFF) : (m > MOVE_ON);
  }
  function rankScore(t, moving) {
    var s = (t.src === "fus" ? 30000 : t.src === "eo" ? 20000 : 10000);
    if (moving) s += 3000;
    s += CLS_RANK[t.cls] || 0;
    s += (t.conf || 0) * 30;                      /* 0..30: a 0.1 confidence gap is worth 3 */
    s -= Math.min(t.rng || 0, 5000) / 2000;       /* 0..-2.5: nearer wins ties only */
    return s;
  }
  /* What a row SAYS is what that row's sensors actually know: a fused row is the only one that
   * can put class and range on the same line — that is the whole point of fusion. */
  function rowText(t) {
    if (t.src === "fus")
      return { mid: String(t.cls || "unknown").toUpperCase() + " " + Math.round((t.conf || 0) * 100) + "% · "
                    + fmtAz(t.az) + "/" + fmtEl(t.el),
               rgt: (t.rng != null ? t.rng.toFixed(0) + " m" : "—") };
    if (t.src === "eo")
      return { mid: String(t.cls || "unknown").toUpperCase() + " " + Math.round((t.conf || 0) * 100) + "%",
               rgt: fmtAz(t.az) + " / " + fmtEl(t.el) };
    /* radar: the live wire carries a velocity vector, a fusion passthrough row carries range-rate */
    var spd = (typeof t.spd === "number") ? t.spd : Math.abs(t.rdot || 0);
    return { mid: spd.toFixed(1) + " m/s · " + fmtAz(t.az),
             rgt: (t.rng != null ? t.rng.toFixed(0) + " m" : "—") };
  }
  /* ── THE LIST: STABLE ROWS, CALM ORDER ──────────────────────────────────────────────────────
   * Three separate things were making this unusable, and each needs its own fix.
   *
   * 1. The rows were rebuilt with innerHTML on every wire frame (up to 27/s). That destroys and
   *    recreates the <li> the operator is pressing, so the element under the finger at mousedown
   *    no longer exists at mouseup and NO CLICK EVENT IS EVER PRODUCED — the row taps that felt
   *    dead were dead. Now six <li> are built once and only their text/classes are updated.
   * 2. The order was a straight re-sort every frame, so two targets with nearly equal scores
   *    swapped places continuously. Now the order is a kept list that CONVERGES toward the ideal
   *    ranking: re-ranked twice a second, at most two adjacent swaps per tick, and a swap only
   *    happens when the challenger beats the incumbent by a real margin. Scores are smoothed
   *    first, so confidence jitter alone can never move a row.
   * 3. A target missing from a single frame collapsed its row and shifted everything below it.
   *    Now a row is HELD for a second after its target leaves the wire, dimmed. This is display
   *    smoothing only — it invents no target and no position, it just stops the list jumping
   *    when a tracker skips a beat.
   *
   * The engaged target is pinned to the top, outside all of the above. */
  var TGT_SLOTS = 6, TGT_HOLD_MS = 1000, RERANK_MS = 500, SWAP_MARGIN = 3, MAX_SWAPS = 2;
  var tgtSeen = {};      /* key -> { t, at, score, moving, live } — survives across frames */
  var tgtOrder = [];     /* keys in DISPLAY order; edited in place, never rebuilt */
  var lastRerank = 0, tgtSlots = null;

  function buildTgtSlots() {
    var ul = $("tgt-list");
    ul.innerHTML = "";
    tgtSlots = [];
    for (var i = 0; i < TGT_SLOTS; i++) {
      var li = document.createElement("li"); li.className = "tgt-row empty";
      var tid = document.createElement("span"); tid.className = "tid";
      var dash = document.createTextNode("—");
      var sw = document.createElement("span"); sw.className = "swatch"; sw.style.display = "none";
      tid.appendChild(dash); tid.appendChild(sw);
      var meta = document.createElement("span"); meta.className = "meta";
      var badge = document.createElement("b"); badge.className = "tsrc";
      var txt = document.createTextNode("");
      meta.appendChild(badge); meta.appendChild(txt);
      var rng = document.createElement("span"); rng.className = "rng";
      li.appendChild(tid); li.appendChild(meta); li.appendChild(rng);
      ul.appendChild(li);
      tgtSlots.push({ li: li, dash: dash, sw: sw, badge: badge, txt: txt, rng: rng, key: null, sig: "" });
    }
  }

  /* Re-score everything, then nudge the order a little way toward ideal. Called at 2 Hz. */
  function rerankTargets() {
    Object.keys(tgtSeen).forEach(function (k) {
      var e = tgtSeen[k];
      e.moving = isMoving(e.t, e.moving);
      var raw = rankScore(e.t, e.moving);
      e.score = (e.score == null) ? raw : e.score + (raw - e.score) * 0.4;   /* smooth the jitter */
    });
    for (var i = 0, swaps = 0; i < tgtOrder.length - 1 && swaps < MAX_SWAPS; i++) {
      var a = tgtSeen[tgtOrder[i]], b = tgtSeen[tgtOrder[i + 1]];
      if (a && b && b.score > a.score + SWAP_MARGIN) {
        var tmp = tgtOrder[i]; tgtOrder[i] = tgtOrder[i + 1]; tgtOrder[i + 1] = tmp;
        swaps++;
      }
    }
  }

  /* What AUTO should be holding: the best LIVE score, but it keeps what it already holds unless
   * something beats it by the same margin a list row needs to overtake another. Without that
   * margin AUTO flips between two near-equal targets several times a second. */
  function autoBest() {
    var best = null, bs = -1e9;
    Object.keys(tgtSeen).forEach(function (k) {
      var e = tgtSeen[k];
      if (!e.live || e.score == null) return;
      if (e.score > bs) { bs = e.score; best = k; }
    });
    if (!best) return null;
    var cur = engagedKey ? tgtSeen[engagedKey] : null;
    if (cur && cur.live && cur.score != null && bs <= cur.score + SWAP_MARGIN) return engagedKey;
    return best;
  }

  function renderTargetList() {
    if (!tgtSlots) buildTgtSlots();
    var now = Date.now(), live = allTargets(), seenNow = {};
    live.forEach(function (t) {
      seenNow[t.key] = 1;
      var e = tgtSeen[t.key] || (tgtSeen[t.key] = { score: null, moving: false });
      e.t = t; e.at = now; e.live = true;
    });
    Object.keys(tgtSeen).forEach(function (k) {
      if (!seenNow[k]) {
        tgtSeen[k].live = false;
        if (now - tgtSeen[k].at > TGT_HOLD_MS) delete tgtSeen[k];   /* held one second, then gone */
      }
    });
    tgtOrder = tgtOrder.filter(function (k) { return tgtSeen[k]; });
    /* new targets join at the end and climb — appearing straight at the top would shove the row
     * the operator is reading out from under them */
    Object.keys(tgtSeen).forEach(function (k) { if (tgtOrder.indexOf(k) < 0) tgtOrder.push(k); });
    if (now - lastRerank >= RERANK_MS) { lastRerank = now; rerankTargets(); }
    /* the held target is always row 1 — it must never scroll off or move */
    var ei = engagedKey ? tgtOrder.indexOf(engagedKey) : -1;
    if (ei > 0) { tgtOrder.splice(ei, 1); tgtOrder.unshift(engagedKey); }

    $("v-tgtcount").textContent = live.length;
    /* say WHERE the list came from — one fused picture, or the two per-sensor lists. The
     * operator reads range-next-to-class very differently depending on the answer. */
    $("v-fusbadge").hidden = !fusUp();

    for (var i = 0; i < TGT_SLOTS; i++) {
      var s = tgtSlots[i], k = tgtOrder[i], e = k ? tgtSeen[k] : null;
      if (!e) {
        if (s.key !== null) {
          s.key = null; s.sig = "";
          s.li.className = "tgt-row empty"; s.li.removeAttribute("data-key");
          s.li.style.borderLeftColor = ""; s.dash.nodeValue = "—"; s.sw.style.display = "none";
          s.badge.textContent = ""; s.badge.className = "tsrc"; s.txt.nodeValue = ""; s.rng.textContent = "";
        }
        continue;
      }
      /* colour follows the fusion gid when there is one — it survives the per-sensor id churn
       * underneath, so a target keeps its colour through a tid change */
      var t = e.t, col = tcolor(t.gid != null ? "g" + t.gid : t.key), tx = rowText(t);
      var cls = "tgt-row" + (t.key === engagedKey ? " eng" : "") + (e.live ? "" : " stale");
      var sig = cls + "|" + col + "|" + t.src + "|" + tx.mid + "|" + tx.rgt;
      if (s.key === k && s.sig === sig) continue;             /* nothing about this row changed */
      s.key = k; s.sig = sig;
      s.li.className = cls;
      s.li.dataset.key = t.key;
      s.li.style.borderLeftColor = col;
      s.dash.nodeValue = ""; s.sw.style.display = ""; s.sw.style.background = col;
      s.badge.className = "tsrc " + t.src;
      s.badge.textContent = t.src === "eo" ? "EO" : t.src === "fus" ? "FUS" : "RDR";
      s.txt.nodeValue = " " + tx.mid;
      s.rng.textContent = tx.rgt;
    }
  }
  /* No GUI-side persistence: the radar daemon is a temporal tracker now (stable tids,
   * M-of-N confirm, coasting through dropouts, park-hold). "In the frame" already means
   * "present" — adding a hold+fade here would double-persist. Draw the wire verbatim. */
  /* Selecting a target DECLARES the tracking state; each module then acts in its own domain
   * off that declaration (the tracker runs its lock loop; radar FOV / zoom / illuminator are
   * their owners' jobs, not ours). Two wires go out:
   *   trk_engage=<tid>  — only for an EO track, since that is the only thing the EO tracker
   *                       can lock. -1 clears it.
   *   engage=<tid>      — the console's own declared engagement, for any source, so a
   *                       radar-only pick is still published state rather than a dead click.
   * The tracker's wire (mode/engaged) is what we REFLECT — see onTrkFrame. */
  /* One place decides what the banner says, so the LOCKED state can never disagree with the
   * marks on the video: green LOCKED while holding a target, the manual prompt otherwise. */
  function updateTrackBanner() {
    var el = $("track-hint");
    if (engagedKey) { el.hidden = false; el.textContent = "LOCKED · tap elsewhere to release"; el.classList.add("locked"); }
    else { el.hidden = (trackMode !== "man"); el.textContent = "MANUAL · tap a target"; el.classList.remove("locked"); }
  }
  function engage(key) {
    engagedKey = key;
    updateTrackBanner();
    var eoTid = (key && key.indexOf("eo:") === 0) ? parseInt(key.slice(3), 10) : -1;
    if (eoTid !== sentTrkEngage) { sentTrkEngage = eoTid; trkEngageSentAt = Date.now(); ctl("trk_engage=" + eoTid); }
    var tid = key ? parseInt(key.split(":")[1], 10) : -1;
    if (tid !== sentEngage) { sentEngage = tid; ctl("engage=" + tid); }
  }

  /* ── radar→EO overlay settings — pure client-side render (not fusion). AZ/EL trim is
   * the radar↔camera mount alignment in degrees. No stored rig calibration exists yet (radar
   * README: calibration is the consumer's job), so the defaults are 0 — dial the trims until a
   * mark sits on its real object, then press SAVE.
   *
   * The trim is a property of the RIG, so it is stored ON THE JETSON (/uiprefs), not in the
   * browser. It used to be localStorage, which is per-origin: the same rig read back different
   * trims depending on whether you opened the console as 192.168.55.1, orin-nano.lan or
   * 10.42.0.1, and any new device started at zero. Now it's one value everywhere, and it
   * survives a reboot. OVERLAY on/off stays per-browser — that's a viewing preference. ── */
  var radarOv = { on: 1, az: 0, el: 0 };
  try { var rvs = JSON.parse(localStorage.getItem("radarOv") || "{}");
        if (typeof rvs.on === "number") radarOv.on = rvs.on; } catch (x) {}
  function saveRadarOv() { try { localStorage.setItem("radarOv", JSON.stringify({ on: radarOv.on })); } catch (x) {} }
  function showTrims() {
    $("rov-az").value = radarOv.az; $("rovv-az").textContent = radarOv.az.toFixed(1) + "°";
    $("rov-el").value = radarOv.el; $("rovv-el").textContent = radarOv.el.toFixed(1) + "°";
  }
  function initRadarOv() {
    var b = document.querySelector('#rov-btns [data-rov="' + radarOv.on + '"]'); if (b) setSeg("rov-btns", b);
    showTrims();
    /* pull the rig's saved trim off the Jetson */
    fetch("/uiprefs").then(function (r) { return r.json(); }).then(function (p) {
      if (p && typeof p.az === "number") radarOv.az = p.az;
      if (p && typeof p.el === "number") radarOv.el = p.el;
      showTrims(); drawEO();
    }).catch(function () {});
  }
  /* SAVE writes the current trim to the Jetson. Until it's pressed a change is session-only,
   * so nudging a slider mid-mission can't silently rewrite the rig's stored alignment. */
  $("rov-savebtn").onclick = function () {
    var btn = this;
    fetch("/uiprefs?set=" + encodeURIComponent(JSON.stringify({ az: radarOv.az, el: radarOv.el })))
      .then(function (r) { if (!r.ok) throw 0; return r.json(); })
      .then(function () {
        $("rovv-saved").textContent = "saved";
        btn.classList.add("on"); setTimeout(function () { btn.classList.remove("on"); }, 900);
        setTimeout(function () { $("rovv-saved").textContent = ""; }, 2500);
      })
      .catch(function () { $("rovv-saved").textContent = "save failed"; });
  };
  document.querySelectorAll("#rov-btns [data-rov]").forEach(function (b) {
    b.onclick = function () { radarOv.on = parseInt(b.dataset.rov, 10); setSeg("rov-btns", b); saveRadarOv(); drawEO(); };
  });

  /* detector mark style — BOX (full bounding box) or SEEKER (small centroid cross);
   * display-only, persisted per browser */
  var detStyle = "box";
  try { var dsv = localStorage.getItem("detStyle"); if (dsv === "seeker" || dsv === "box") detStyle = dsv; } catch (x) {}
  function initDetStyle() { var b = document.querySelector('#dst-btns [data-dst="' + detStyle + '"]'); if (b) setSeg("dst-btns", b); }
  document.querySelectorAll("#rawdet-btns [data-rawdet]").forEach(function (b) {
    b.onclick = function () { showRawDet = b.dataset.rawdet === "1"; setSeg("rawdet-btns", b); drawEO(); };
  });
  document.querySelectorAll("#dst-btns [data-dst]").forEach(function (b) {
    b.onclick = function () { detStyle = b.dataset.dst; setSeg("dst-btns", b); try { localStorage.setItem("detStyle", detStyle); } catch (x) {} drawEO(); };
  });
  /* Moving a trim applies immediately but is NOT stored until SAVE (see rov-savebtn). */
  $("rov-az").oninput = function () { radarOv.az = parseFloat(this.value); $("rovv-az").textContent = radarOv.az.toFixed(1) + "°"; $("rovv-saved").textContent = "unsaved"; drawEO(); };
  $("rov-el").oninput = function () { radarOv.el = parseFloat(this.value); $("rovv-el").textContent = radarOv.el.toFixed(1) + "°"; $("rovv-saved").textContent = "unsaved"; drawEO(); };

  /* ── canvas ── */
  /* Size the backing store from the element's LAYOUT box (offsetWidth), not getBoundingClientRect
   * — the latter includes CSS transforms, so once we CSS-scale the overlay for replay zoom its
   * measured size would feed back and double-scale. offsetWidth is transform-independent. */
  function fit(cv) { var dpr = window.devicePixelRatio || 1, w = cv.offsetWidth || 1, h = cv.offsetHeight || 1; cv.width = Math.max(1, w * dpr | 0); cv.height = Math.max(1, h * dpr | 0); return { ctx: cv.getContext("2d"), w: cv.width, h: cv.height, dpr: dpr }; }
  /* dark halo under on-video labels — thin text survives bright sky and dark bush alike
   * (the standard OSD treatment; the marks get the same via a two-pass stroke) */
  /* Where each EO track's box landed last draw, in 0..1 panel coords — so a tap hit-tests the
   * box the operator actually SAW rather than re-deriving the projection (which drifts out of
   * sync the moment zoom/letterbox handling changes on one side only). */
  var eoBoxes = [];
  var lockLive = false;   /* is the locked target actually on THIS frame? gates hide-the-rest */
  var showRawDet = false;          /* raw detector boxes: DEV overlay, off by default */
  var detTemporalOn = false;       /* is the detector actually running temporal integration? */
  /* how much of a box must be inside the zoom/ROI crop before we draw it as a target here */
  var BOX_MIN_VISIBLE = 0.5;
  function eoBoxHit(xf, yf) {
    var best = null, bestA = 1e9;
    eoBoxes.forEach(function (b) {
      if (xf < b.x0 || xf > b.x1 || yf < b.y0 || yf > b.y1) return;
      var a = (b.x1 - b.x0) * (b.y1 - b.y0);
      if (a < bestA) { bestA = a; best = b.key; }        /* smallest box containing the tap wins */
    });
    return best;
  }
  function haloText(ctx, txt, x, y, col, dpr) {
    ctx.save(); ctx.strokeStyle = "rgba(0,0,0,0.85)"; ctx.lineWidth = 3 * dpr; ctx.lineJoin = "round";
    ctx.strokeText(txt, x, y); ctx.fillStyle = col; ctx.fillText(txt, x, y); ctx.restore();
  }
  /* THE lock symbol — four corner brackets over a dark halo. One function so the EO box and the
   * radar mark draw the SAME shape: the operator must not have to learn two "locked" symbols
   * for what is, to them, the same act of holding a target. */
  function cornerBrackets(ctx, x0, y0, x1, y1, col, dpr) {
    var L = Math.max(5 * dpr, Math.min(18 * dpr, Math.min(x1 - x0, y1 - y0) * 0.3));
    [["rgba(0,0,0,0.85)", 4.5 * dpr], [col, 2.4 * dpr]].forEach(function (s) {
      ctx.strokeStyle = s[0]; ctx.lineWidth = s[1]; ctx.lineCap = "round";
      ctx.beginPath();
      ctx.moveTo(x0, y0 + L); ctx.lineTo(x0, y0); ctx.lineTo(x0 + L, y0);   /* top-left */
      ctx.moveTo(x1 - L, y0); ctx.lineTo(x1, y0); ctx.lineTo(x1, y0 + L);   /* top-right */
      ctx.moveTo(x0, y1 - L); ctx.lineTo(x0, y1); ctx.lineTo(x0 + L, y1);   /* bottom-left */
      ctx.moveTo(x1 - L, y1); ctx.lineTo(x1, y1); ctx.lineTo(x1, y1 - L);   /* bottom-right */
      ctx.stroke();
    });
    ctx.lineCap = "butt";
  }

  /* ── LOCKED-TARGET CLOSE-UP (PIP) ──────────────────────────────────────────────────────────
   * A magnified crop of the held target, pinned top-right, independent of what the main view is
   * doing. It samples the SAME video element the operator is watching, but crops in the source
   * image's own pixels — so panning/zooming the main view with ROI (a CSS transform) cannot move
   * it, and the target stays framed even when it is off the main view entirely.
   * Sizing is driven by the target's own box: a small far target gets magnified hard, a near one
   * barely at all, so the target fills a similar share of the PIP either way. */
  var PIP_FILL = 0.42;             /* target's long edge as a share of the PIP window */
  var pipSm = { key: null, ww: 0, cx: 0, cy: 0 };   /* smoothed PIP window (source px) */
  function drawPip() {
    var cv = $("pip");
    if (!engagedKey) { cv.hidden = true; return; }
    var es = lastStats.eo || {};
    var im = (lastTrk && lastTrk.img) || { w: 1440, h: 1088 };
    var z = (replaying && replayVideoSrc === "native") ? 1 : (es.zoom || 1);
    var cw = im.w / z, ch = im.h / z, ox = (im.w - cw) / 2, oy = (im.h - ch) / 2;

    /* where is the held target, in NATIVE sensor pixels? */
    var nx, ny, nw, nh, lab = "";
    var t = null;
    eoTracks().forEach(function (x) { if (x.key === engagedKey) t = x; });
    if (t && t.px && t.px.length >= 4) {
      nx = t.px[0]; ny = t.px[1]; nw = t.px[2]; nh = t.px[3];
      lab = String(t.cls || "?").toUpperCase();
    } else {
      /* radar lock: no box, so project its bearing into the frame and use a nominal window */
      var r = null;
      radarTargets().forEach(function (x) { if (x.key === engagedKey) r = x; });
      var hf = es.hfov || 0, vf = es.vfov || (hf * 0.75);
      if (!r || !hf) { cv.hidden = true; return; }
      nx = im.w / 2 + (r.az / (hf / 2)) * (cw / 2);
      ny = im.h / 2 - (r.el / (vf / 2)) * (ch / 2);
      nw = nh = Math.max(im.w, im.h) * 0.06;
      lab = "RDR " + r.rng.toFixed(0) + " m";
    }

    /* which element is actually showing video right now */
    var src = ($("nvid").style.display === "block") ? $("nvid") : $("video");
    var iw = src.naturalWidth || src.videoWidth || 0, ih = src.naturalHeight || src.videoHeight || 0;
    if (!iw || !ih) { cv.hidden = true; return; }

    /* native → source-image pixels (the stream is the zoom crop, scaled to its own size) */
    var sx = (nx - ox) / cw * iw, sy = (ny - oy) / ch * ih;
    var sw = nw / cw * iw, sh = nh / ch * ih;

    /* unhide BEFORE measuring: fit() sizes the backing store from the layout box, and a hidden
     * canvas measures 0, which would give a 1x1 buffer on the first frame after locking. */
    cv.hidden = false;
    var f = fit(cv), ctx = f.ctx, W = f.w, H = f.h;
    /* window sized so the target occupies PIP_FILL of it, clamped so we neither pixel-peep a
     * single blob nor zoom so far out that the point of the PIP is lost */
    var want = Math.max(sw, sh) / PIP_FILL;
    want = Math.min(iw, Math.max(want, iw * 0.06));
    /* SMOOTHING. The detector's box breathes a few percent every frame, and feeding that
     * straight into the window size made the magnification visibly pump. Size is heavily
     * damped (it should look constant unless the target really is closing); position is only
     * lightly damped, so the close-up still follows a moving target without lag. Snap on a new
     * lock, or on a big jump, so it never slews slowly across the frame. */
    if (pipSm.key !== engagedKey || !pipSm.ww ||
        Math.abs(want - pipSm.ww) > pipSm.ww * 0.6 ||
        Math.hypot(sx - pipSm.cx, sy - pipSm.cy) > Math.max(iw, ih) * 0.25) {
      pipSm.key = engagedKey; pipSm.ww = want; pipSm.cx = sx; pipSm.cy = sy;
    } else {
      pipSm.ww += (want - pipSm.ww) * 0.06;
      pipSm.cx += (sx - pipSm.cx) * 0.35;
      pipSm.cy += (sy - pipSm.cy) * 0.35;
    }
    var ww = pipSm.ww, wh = ww * (H / W);
    if (wh > ih) { wh = ih; ww = wh * (W / H); }
    var wx = Math.min(Math.max(pipSm.cx - ww / 2, 0), Math.max(0, iw - ww));
    var wy = Math.min(Math.max(pipSm.cy - wh / 2, 0), Math.max(0, ih - wh));

    ctx.clearRect(0, 0, W, H);
    try { ctx.drawImage(src, wx, wy, ww, wh, 0, 0, W, H); }
    catch (e) { cv.hidden = true; return; }        /* frame not decodable yet */
    /* ROI is a CSS crop, so the target is still in the stream and the PIP just works. The EO
     * feed's own ZOOM is a real sensor crop, though — if it cut the target out, there are no
     * pixels to show and the clamped window would quietly display the wrong patch. Say so. */
    if (sx < 0 || sx > iw || sy < 0 || sy > ih) {
      ctx.fillStyle = "rgba(0,0,0,0.55)"; ctx.fillRect(0, 0, W, H);
      ctx.font = (10 * f.dpr) + "px ui-monospace, monospace"; ctx.textAlign = "center";
      haloText(ctx, "TARGET OUTSIDE EO ZOOM", W / 2, H / 2, css("--err"), f.dpr);
      ctx.textAlign = "left";
      return;
    }
    /* mark the target inside the close-up, same bracket language as the main view */
    var dpr = f.dpr, bx = (sx - wx) / ww * W, by = (sy - wy) / wh * H;
    var bw = Math.max(10 * dpr, sw / ww * W), bh = Math.max(10 * dpr, sh / wh * H);
    cornerBrackets(ctx, bx - bw / 2, by - bh / 2, bx + bw / 2, by + bh / 2, css("--on"), dpr);
    ctx.font = (10 * dpr) + "px ui-monospace, monospace"; ctx.textAlign = "left";
    haloText(ctx, "LOCK " + lab, 5 * dpr, 13 * dpr, css("--on"), dpr);
    cv.hidden = false;
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
    /* The tracker is the ONLY source of EO boxes now, so if it's down say so rather than
     * showing an empty frame that looks like "nothing out there". Live only — a replay has
     * no tracker. */
    if (!replaying && !trkConnected()) {
      ctx.save();
      ctx.font = (11 * dpr) + "px ui-monospace, monospace"; ctx.textAlign = "center";
      haloText(ctx, "EO TRACK · NOT CONNECTED", cx, 22 * dpr, css("--err"), dpr);
      ctx.restore();
    } else if (!replaying && engagedKey && !lockLive) {
      /* The lock is still held — the tracker keeps it — but the target isn't on this frame.
       * Say so, and (see below) the rest of the scene stays visible so the operator isn't blind
       * while it's out. */
      ctx.save();
      ctx.font = (11 * dpr) + "px ui-monospace, monospace"; ctx.textAlign = "center";
      haloText(ctx, "LOCK HELD · TARGET NOT SEEN", cx, 22 * dpr, css("--amber"), dpr);
      ctx.restore();
    }
    var es = lastStats.eo || {};
    /* hfov/vfov describe the ZOOMED (cropped) frame that was streamed. A NATIVE replay shows the
     * FULL uncropped frame, which spans zoom x that angle — so scale the FOV up there, exactly as
     * the detector block below uses z=1 for native. Without this every mark projected far outside
     * the frame, hit the |fx|>1 cull, and the whole radar overlay silently vanished in native
     * replay while the detector boxes kept working. */
    /* which per-sensor tracks fusion has paired this frame (empty when fusion is down) */
    var fmap = fusedMap();
    var fovZ = (replaying && replayVideoSrc === "native") ? (es.zoom || 1) : 1;
    var eoHfov = (es.hfov || 0) * fovZ;
    var eoVfov = (es.vfov ? es.vfov : (es.hfov || 0) * 0.75) * fovZ;
    if (radarOv.on && eoHfov && lastRadar && lastRadar.connected) {
      var sw2 = es.dw || 4, sh2 = es.dh || 3, ar2 = sw2 / sh2, vw2, vh2, vx2, vy2;
      if (w / h > ar2) { vh2 = h; vw2 = h * ar2; vx2 = (w - vw2) / 2; vy2 = 0; }
      else { vw2 = w; vh2 = w / ar2; vx2 = 0; vy2 = (h - vh2) / 2; }
      ctx.font = (10 * dpr) + "px ui-monospace, monospace";
      targets(lastRadar).forEach(function (t) {
        /* locked → the video shows ONLY the held target, radar marks included */
        if (engagedKey && lockLive && ("rad:" + t.tid) !== engagedKey) return;
        /* FUSED → the camera side already marks this object. Two marks for one thing is the
         * double-display fusion exists to end, so the radar circle stands down and its range
         * rides on the EO box instead. */
        if (fmap.rad[t.tid]) return;
        var az = t.az + radarOv.az, el = t.el + radarOv.el;
        var fx = az / (eoHfov / 2), fy = -el / (eoVfov / 2);   /* -1..1 within frame; +el = up */
        if (Math.abs(fx) > 1 || Math.abs(fy) > 1) return;      /* off-frame → not drawn */
        var lx = vx2 + (fx + 1) / 2 * vw2, ly = vy2 + (fy + 1) / 2 * vh2;
        /* LOCKED radar target → the same corner-bracket symbol an EO lock gets. Holding a
         * target is one act to the operator, so it must look identical whichever sensor
         * found it; only the data behind it differs (radar brings range, EO brings class). */
        if (engagedKey === ("rad:" + t.tid)) {
          var rb = 16 * dpr;
          cornerBrackets(ctx, lx - rb, ly - rb, lx + rb, ly + rb, css("--on"), dpr);
          ctx.fillStyle = css("--on"); ctx.beginPath(); ctx.arc(lx, ly, 1.8 * dpr, 0, 2 * Math.PI); ctx.fill();
          haloText(ctx, "LOCK " + t.rng.toFixed(0) + " m", lx - rb + 2 * dpr, ly - rb - 4 * dpr, css("--on"), dpr);
          return;
        }
        /* BROKEN RING (fixed size) — four arcs with cardinal gaps over a dark halo, so
         * the mark reads over bright sky and dark bush alike. Fixed size on purpose:
         * the tracker's sx/sy estimates jitter, size-coding pulsed. */
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
    var trkTracks = eoTracks();
    if (trkTracks.length || (lastDet && showRawDet && (lastDet.dets || lastDet.movers))) {
      var im = (lastTrk && lastTrk.img) || (lastDet && lastDet.img) || { w: 1440, h: 1088 };
      var z = (replaying && replayVideoSrc === "native") ? 1 : (es.zoom || 1);
      var cw = im.w / z, ch = im.h / z, ox = (im.w - cw) / 2, oy = (im.h - ch) / 2;
      var sw = es.dw || cw, sh = es.dh || ch, ar = sw / sh;   /* streamed frame sets the letterbox */
      var vw, vh, vx, vy;
      if (w / h > ar) { vh = h; vw = h * ar; vx = (w - vw) / 2; vy = 0; }
      else { vw = w; vh = w / ar; vx = 0; vy = (h - vh) / 2; }
      ctx.save(); ctx.beginPath(); ctx.rect(vx, vy, vw, vh); ctx.clip();
      ctx.font = (10 * dpr) + "px ui-monospace, monospace";
      var drawDet = function (b, dashed, col, label, forceBox, tracked, tbd, fused) {
        if (!b.px || b.px.length < 4) return false;
        var bx = vx + (b.px[0] - ox) / cw * vw, by = vy + (b.px[1] - oy) / ch * vh;
        var bw2 = b.px[2] / cw * vw, bh2 = b.px[3] / ch * vh;
        /* VISIBILITY RULE. Drawing a box the moment ANY sliver of it falls inside the crop put
         * huge rectangles on screen whose target was almost entirely outside the zoomed view —
         * at 4x you'd see a corner bracket and a label for something you cannot actually see.
         * Require a real share of the box to be in view before claiming it as a target here.
         * The LOCKED target is exempt: if the operator is holding it, it stays marked even while
         * it leaves the crop — losing the mark on the one target you chose is worse. */
        var ix0 = Math.max(bx - bw2 / 2, vx), ix1 = Math.min(bx + bw2 / 2, vx + vw);
        var iy0 = Math.max(by - bh2 / 2, vy), iy1 = Math.min(by + bh2 / 2, vy + vh);
        var visible = Math.max(0, ix1 - ix0) * Math.max(0, iy1 - iy0);
        var area = Math.max(1, bw2 * bh2);
        if (!tracked && visible / area < BOX_MIN_VISIBLE) return false;
        if (visible <= 0) return false;
        if (tracked) {
          /* TRACKED — CORNER BRACKETS: the four corners of the box only, no connecting edges.
           * Shape (not colour) carries the tracking state, so class colour still answers "what
           * is it" while the bracket answers "we're holding this one". Overrides both the plain
           * rectangle and the seeker cross. Arms scale with the box but stay inside sane limits
           * so a tiny far box doesn't collapse into a blob or a near one grow a full frame. */
          var hw = bw2 / 2, hh = bh2 / 2;
          var x0 = bx - hw, x1 = bx + hw, y0 = by - hh, y1 = by + hh;
          cornerBrackets(ctx, x0, y0, x1, y1, col, dpr);   /* shared with the radar lock mark */
          haloText(ctx, label, x0 + 2 * dpr, y0 - 3 * dpr, col, dpr);
        } else if (detStyle === "seeker" && !forceBox) {
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
          /* FUSED in seeker mode → a ring around the cross. Both sensors hold this one, so it
           * gets a mark neither sensor alone can draw; the cross inside is still the class. */
          if (fused) {
            [["rgba(0,0,0,0.85)", 4.5 * dpr], [col, 2 * dpr]].forEach(function (s) {
              ctx.strokeStyle = s[0]; ctx.lineWidth = s[1];
              ctx.beginPath(); ctx.arc(bx, by, a + 3.5 * dpr, 0, 2 * Math.PI); ctx.stroke();
            });
          }
          ctx.setLineDash([]); ctx.lineCap = "butt";
          haloText(ctx, label, bx + a + 3 * dpr, by - 4 * dpr, col, dpr);
        } else {
          /* FUSED in box mode → the same box, drawn heavier. Shape stays the class's; weight
           * says "two sensors agree on this one". */
          ctx.strokeStyle = col; ctx.lineWidth = (fused ? 3 : 1.6) * dpr;
          ctx.setLineDash(dashed ? [5 * dpr, 4 * dpr] : []);
          ctx.strokeRect(bx - bw2 / 2, by - bh2 / 2, bw2, bh2);
          ctx.setLineDash([]);
          haloText(ctx, label, bx - bw2 / 2 + 2 * dpr, by - bh2 / 2 - 3 * dpr, col, dpr);
        }
        /* Temporal marker: a tiny "t" at the box's top-right corner (in the CLASS colour, so it
         * flags provenance without recolouring the box). Small on purpose — a quiet hint that this
         * one was promoted from collected evidence, not an alarm.
         * BOX SHAPES ONLY. The seeker cross draws no box, so a corner-anchored "t" floated in
         * empty space away from the mark and just read as a stray character. */
        if (tbd && detTemporalOn && !(detStyle === "seeker" && !forceBox && !tracked)) {
          ctx.save();
          ctx.font = "bold " + (9 * dpr) + "px ui-monospace, monospace"; ctx.textAlign = "left";
          var tx = bx + bw2 / 2 - 6 * dpr, ty = by - bh2 / 2 + 9 * dpr;
          haloText(ctx, "t", tx, ty, col, dpr);
          ctx.restore();
        }
        return true;   /* drawn -> caller may register it as tappable */
      };
      var seek = (detStyle === "seeker");
      /* RAW DETECTOR boxes — dev overlay only, OFF by default. These are the tracker's INPUT;
       * drawing them alongside its output is the double-display the tracker exists to remove.
       * Kept behind a toggle because seeing input vs output side by side is how you tell a
       * tracker fault from a detector fault. Deliberately dim so they read as background. */
      if (showRawDet && lastDet) {
        (lastDet.dets || []).forEach(function (d) {
          drawDet(d, true, "rgba(150,157,168,0.55)", "", true);
        });
        (lastDet.movers || []).forEach(function (mv) {
          drawDet(mv, true, "rgba(150,157,168,0.45)", "", true);
        });
      }
      /* EO TRACKS — the operator's boxes. Colour is the CLASS, always: never how the target
       * was arrived at (a far human promoted from faint evidence is still a human).
       * SHAPE marks exactly ONE thing: the LOCKED target. Every other track draws as the
       * ordinary rectangle (or seeker cross) it always did — corner brackets on every
       * confirmed track made the whole frame look "tracked" and hid which one is held.
       * When a target IS locked, only that target is drawn: the operator asked for the
       * picture to clear down to the thing being tracked. */
      eoBoxes = [];
      trkTracks.forEach(function (t) {
        var eng = (t.key === engagedKey);
        if (engagedKey && lockLive && !eng) return;      /* locked AND drawable → show only it */
        var col = t.cls === "human" ? "#40c4ff" : amber;
        var cl = String(t.cls || "?");                   /* coerce: a non-string cls would throw on [0]/.toUpperCase() and blank every overlay */
        var lab = seek ? cl[0].toUpperCase() + Math.round((t.conf || 0) * 100)
                       : cl.toUpperCase() + " " + Math.round((t.conf || 0) * 100) + "%";
        /* FUSED: the radar half of this object brings a RANGE the camera can never measure —
         * put it on the box. That number appearing next to a classified target is the whole
         * payoff of fusion, so it goes where the operator is already looking. */
        var fu = fmap.eo[t.tid];
        if (fu && typeof fu.r_m === "number" && fu.r_m >= 0) lab += " " + fu.r_m.toFixed(0) + "m";
        if (eng) lab = "LOCK " + lab;
        var drawn = drawDet(t, t.state === "coast", eng ? css("--on") : col, lab, false,
                eng,                                     /* corner brackets = LOCKED, nothing else */
                t.raw && (t.raw.tbd === 1 || t.raw.tbd === true),
                !!fu);                                   /* heavier box / ringed cross = FUSED */
        /* remember where it landed, in 0..1 panel coords, so a tap can hit-test the drawn
         * box instead of re-deriving the projection (which would drift out of sync).
         * Only boxes we actually DREW are tappable — a target culled for being mostly outside
         * the crop must not still swallow taps from an invisible rectangle. */
        if (drawn && t.px && t.px.length >= 4) {
          var ex = vx + (t.px[0] - ox) / cw * vw, ey = vy + (t.px[1] - oy) / ch * vh;
          var ew = t.px[2] / cw * vw, eh = t.px[3] / ch * vh;
          eoBoxes.push({ key: t.key, x0: (ex - ew / 2) / w, x1: (ex + ew / 2) / w,
                                     y0: (ey - eh / 2) / h, y1: (ey + eh / 2) / h });
        }
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
  /* ── scope palette ──────────────────────────────────────────────────────────────────────
   * The radar scope is a DRAWN display, not video, so it follows the theme: bright marks on a
   * black face at night, dark marks on a bright face in day. (The EO panel stays black in both
   * — that one shows the real sensor image, and inverting it would misrepresent the picture.)
   * Meanings are preserved across themes: red = closing, blue = opening, grey = static,
   * champagne = the fixed 100/250 m reference rings. */
  function isDay() { return theme === "day"; }
  function scopePal() {
    return isDay()
      ? { ring: "rgba(90,96,104,0.30)",  ringTxt: "rgba(64,70,80,0.80)",
          ref:  "rgba(154,123,69,0.85)", refTxt:  "rgba(116,91,46,0.95)",
          wedge:"rgba(70,80,95,0.07)",   edge:    "rgba(70,80,95,0.34)", bore: "rgba(60,70,85,0.55)" }
      : { ring: "rgba(150,157,168,0.16)", ringTxt: "rgba(170,175,185,0.55)",
          ref:  "rgba(193,161,115,0.65)", refTxt:  "rgba(216,189,144,0.95)",
          wedge:"rgba(150,157,168,0.06)", edge:    "rgba(150,157,168,0.24)", bore: "rgba(150,157,168,0.5)" };
  }
  /* Track colours are chosen bright for the black EO video; on the day scope's bright face they
   * would wash out. Darken them there — same hue, so a track still reads as the same colour
   * across the scope, the target list and the EO overlay. */
  function scopeCol(hex) {
    if (!isDay() || hex.charAt(0) !== "#") return hex;
    var n = parseInt(hex.slice(1), 16);
    return "rgb(" + (((n >> 16) & 255) * 0.5 | 0) + "," + (((n >> 8) & 255) * 0.5 | 0) + "," + ((n & 255) * 0.5 | 0) + ")";
  }
  function pointStyle(v, snr) {
    var s = (typeof snr === "number" && isFinite(snr)) ? Math.max(0.3, Math.min(1, (snr - 12) / 28)) : 0.7;
    if (isDay()) {
      if (Math.abs(v) < 0.2) return "rgba(72,80,92," + (s * 0.65) + ")";      /* static → graphite */
      return v > 0 ? "rgba(186,26,26," + s + ")" : "rgba(20,84,190," + s + ")";
    }
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
  /* ── SCENE LAYER — the static occupancy backdrop (radar/docs/SCENE_LAYER.md) ──────────────
   * Stationary radar returns (|doppler| < 0.1 m/s) accumulated into a polar grid, so over tens
   * of seconds the world draws itself: walls, parked cars, buildings. DISPLAY ONLY — it never
   * feeds tracking, guidance or fusion, and no target is ever derived from it. It is the fog-of-
   * war the targets move across, drawn UNDER the live points and boxes.
   *
   * Two channels, two questions, so they are bound to two visual properties:
   *   occ (0..255) = is something really there  -> OPACITY. Noise sits near 0, a wall reaches
   *                  255, ~4 orders of magnitude, so opacity alone separates world from noise
   *                  with no threshold to pick.
   *   snr (dB)     = can you believe WHERE it is -> COLOUR. Bearing accuracy is SNR-limited
   *                  (±2.6° at 60 dB, ±20° at 28 dB), so the faint smooth arcs in the far field
   *                  are real returns whose ANGLE is noise. Colouring by strength shows the
   *                  operator which parts of the picture are geometrically trustworthy instead
   *                  of hiding it.
   *
   * Rendered to an offscreen canvas and blitted: the layer updates a few times a second while the
   * scope redraws at the radar's ~27 Hz, so drawing a few thousand sectors per scope frame would
   * be pure waste. Re-rendered only when new data lands or the view geometry moves.
   *
   * RATE: the daemon publishes snapshots at its own configurable rate and reports it back as
   * `rate_hz` in every payload. The console FOLLOWS that number rather than assuming one, so the
   * fetch rate and the publish rate can never drift apart (and a rate set from anywhere else is
   * picked up on the next poll). Accumulation is always at the full frame rate regardless — the
   * knob only trades link bandwidth for how fresh the picture is. ── */
  var scene = { on: 0, data: null, frames: 0, poll: null, hz: 5, hzTouch: 0 };
  var sceneCv = null, sceneKey = "";
  try { scene.on = localStorage.getItem("sceneOn") === "1" ? 1 : 0; } catch (x) {}

  /* 16 dB (noise floor) -> 62 dB (hard structure), cold to hot. Bearing you can trust reads warm. */
  function snrColor(db) {
    var t = Math.max(0, Math.min(1, (db - 16) / (62 - 16)));
    var r, g, b;
    if (t < 0.5) { var u = t / 0.5; r = 30 + u * 20; g = 90 + u * 140; b = 200 - u * 60; }   /* blue -> green */
    else { var v = (t - 0.5) / 0.5; r = 50 + v * 205; g = 230 - v * 90; b = 140 - v * 130; } /* green -> amber */
    return "rgb(" + (r | 0) + "," + (g | 0) + "," + (b | 0) + ")";
  }

  /* Draw every lit cell as its true annular sector, in the CURRENT view geometry. Exact rather
   * than an axis-aligned approximation — at ±60° a rectangle per cell visibly shears the map. */
  function renderScene(w, h, cx, cy, scale) {
    var d = scene.data;
    var key = w + "x" + h + ":" + cx.toFixed(1) + "," + cy.toFixed(1) + "," + scale.toFixed(4) + ":" + scene.frames;
    if (sceneCv && sceneKey === key) return sceneCv;
    if (!sceneCv) sceneCv = document.createElement("canvas");
    if (sceneCv.width !== w || sceneCv.height !== h) { sceneCv.width = w; sceneCv.height = h; }
    var c = sceneCv.getContext("2d");
    c.clearRect(0, 0, w, h);
    sceneKey = key;
    if (!d || !d.cells || !d.cells.length) return sceneCv;
    var cells = d.cells, rs = d.r_step || 2.61, a0 = d.az0 || -60, as = d.az_step || 1;
    var D = Math.PI / 180, diag = Math.hypot(w, h);
    for (var i = 0; i + 3 < cells.length; i += 4) {
      var ri = cells[i], ai = cells[i + 1], occ = cells[i + 2], snr = cells[i + 3];
      var r0 = ri * rs * scale, r1 = (ri + 1) * rs * scale;
      if (r0 > diag) continue;                                  /* wholly off-canvas */
      var t0 = (a0 + ai * as) * D, t1 = (a0 + (ai + 1) * as) * D;
      var p0 = -Math.PI / 2 + t0, p1 = -Math.PI / 2 + t1;       /* world azimuth -> canvas angle */
      c.globalAlpha = Math.max(0.10, Math.min(0.92, occ / 255)); /* floor: faint cells stay visible */
      c.fillStyle = snrColor(snr);
      c.beginPath();
      c.arc(cx, cy, r1, p0, p1);
      c.arc(cx, cy, r0, p1, p0, true);
      c.closePath();
      c.fill();
    }
    c.globalAlpha = 1;
    return sceneCv;
  }

  /* Fetch one snapshot, and re-time the polling to whatever the daemon says it is publishing at.
   * Only runs while the layer is shown — an invisible backdrop costs nothing. */
  function pollScene() {
    fetch("/scene").then(function (r) { return r.json(); }).then(function (d) {
      if (!d || !d.cells) return;
      scene.data = d; scene.frames = d.frames || 0;
      var a = $("scn-age");
      if (a) a.textContent = scene.frames < 250 ? (scene.frames + " f · forming") : (scene.frames + " f");
      applySceneHz(d.rate_hz);
      draw(false, true);
    }).catch(function () {});
  }
  /* The daemon's applied rate is the authority — it clamps, so the value that comes back is not
   * always the value that was asked for. Re-time the poll to it and reflect it on the slider
   * (unless the operator is mid-drag). */
  function applySceneHz(hz) {
    if (typeof hz !== "number" || !isFinite(hz) || hz <= 0) return;
    var shown = Math.max(1, Math.min(26, Math.round(hz)));
    if ($("scn-rate") && Date.now() - scene.hzTouch > 1500 && document.activeElement !== $("scn-rate")) {
      $("scn-rate").value = shown; $("scn-ratev").textContent = shown + " Hz";
    }
    if (Math.abs(hz - scene.hz) < 0.01) return;
    scene.hz = hz;
    if (scene.on && scene.poll) {                       /* re-time the running poll to match */
      clearInterval(scene.poll);
      scene.poll = setInterval(pollScene, Math.max(40, Math.round(1000 / hz)));
    }
  }
  function setScene(on) {
    scene.on = on ? 1 : 0;
    try { localStorage.setItem("sceneOn", String(scene.on)); } catch (x) {}
    if (scene.poll) { clearInterval(scene.poll); scene.poll = null; }
    if (scene.on) { pollScene(); scene.poll = setInterval(pollScene, Math.max(40, Math.round(1000 / scene.hz))); }
    else { scene.data = null; sceneKey = ""; if ($("scn-age")) $("scn-age").textContent = "—"; }
    draw(false, true);
  }
  document.querySelectorAll("#scn-btns [data-scn]").forEach(function (b) {
    b.onclick = function () { setSeg("scn-btns", b); setScene(parseInt(b.dataset.scn, 10)); };
  });
  /* MAP RATE — how often the daemon republishes the snapshot, and so how often we fetch it. The
   * daemon clamps it (0.2..26 Hz) and returns the layer with the applied value, so the slider is
   * driven by what actually took effect, never by what was asked for. Accumulation is untouched:
   * the map still absorbs every frame, this is purely freshness against link bandwidth. */
  $("scn-rate").oninput = function () {
    var hz = parseInt(this.value, 10);
    scene.hzTouch = Date.now();
    $("scn-ratev").textContent = hz + " Hz";
    fetch("/scene?rate=" + hz).then(function (r) { return r.json(); }).then(function (d) {
      if (!d) return;
      if (d.cells) { scene.data = d; scene.frames = d.frames || 0; sceneKey = ""; }
      scene.hzTouch = 0;                      /* let the applied value win from here */
      applySceneHz(d.rate_hz);
      draw(false, true);
    }).catch(function () {});
  };
  /* CLEAR wipes the daemon's accumulation. The map is built in the SENSOR's frame, so slewing
   * the rig smears it — until the gimbal encoders let it accumulate in a world frame, this is
   * the operator's tool for "I moved, start over". */
  $("scn-clearbtn").onclick = function () {
    var btn = this;
    fetch("/scene?reset=1").then(function (r) { return r.json(); }).then(function (d) {
      scene.data = d && d.cells ? d : null; scene.frames = (d && d.frames) || 0; sceneKey = "";
      btn.classList.add("on"); setTimeout(function () { btn.classList.remove("on"); }, 700);
      draw(false, true);
    }).catch(function () {});
  };

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
    var dim = css("--dim"), P = scopePal();
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
      ctx.strokeStyle = P.ring; ctx.lineWidth = 1 * dpr;
      ctx.beginPath(); ctx.arc(cx, cy, gr, Math.PI, 2 * Math.PI); ctx.stroke();
      ctx.fillStyle = P.ringTxt; ctx.fillText(rm + " m", cx + 5 * dpr, cy - gr + 13 * dpr);
    }
    /* constant amber reference rings at 100 m + 250 m on every zoom (when in range) */
    [100, 250].forEach(function (m) {
      if (m > Reff * 1.02) return;
      var rr = m * scale;
      ctx.strokeStyle = P.ref; ctx.lineWidth = 1.4 * dpr;
      ctx.beginPath(); ctx.arc(cx, cy, rr, Math.PI, 2 * Math.PI); ctx.stroke();
      ctx.fillStyle = P.refTxt; ctx.textAlign = "right";
      ctx.fillText(m + " m", cx - 6 * dpr, cy - rr + 13 * dpr); ctx.textAlign = "left";
    });
    /* FOV wedge from the daemon's live fov_half_deg (tracks the FOV knob) + boresight */
    var fovDeg = (radar && typeof radar.fov_half_deg === "number") ? radar.fov_half_deg : 90;
    var fr = fovDeg * Math.PI / 180;
    var inFov = function (x, y) { return Math.abs(Math.atan2(x, y)) <= fr; };   /* azimuth within ±FOV */
    ctx.fillStyle = P.wedge; ctx.beginPath(); ctx.moveTo(cx, cy);
    ctx.arc(cx, cy, maxR, -Math.PI / 2 - fr, -Math.PI / 2 + fr, false); ctx.closePath(); ctx.fill();
    ctx.strokeStyle = P.edge; ctx.lineWidth = dpr; ctx.setLineDash([4 * dpr, 4 * dpr]);
    [-1, 1].forEach(function (s) { var a = -Math.PI / 2 + s * fr; ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx + Math.cos(a) * maxR, cy + Math.sin(a) * maxR); ctx.stroke(); });
    ctx.setLineDash([]);
    ctx.strokeStyle = P.bore; ctx.lineWidth = 1.5 * dpr;
    ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx, cy - maxR); ctx.stroke(); ctx.lineWidth = dpr;

    if (!radar || !radar.connected) { ctx.globalAlpha = 0.6; ctx.fillStyle = dim; ctx.textAlign = "center"; ctx.fillText((replaying && !replayHasRadar) ? "NO RADAR RECORDED" : "NOT CONNECTED", cx, cy - maxR * 0.45); ctx.textAlign = "left"; ctx.globalAlpha = 1; radarGeom = null; return; }
    radarGeom = { cx: cx, cy: cy, scale: scale, dpr: dpr };

    /* SCENE LAYER, under everything live — the world the targets move across. Blitted from its
     * own canvas (re-rendered only when the layer or the view actually changes). */
    if (scene.on && scene.data && !replaying) ctx.drawImage(renderScene(w, h, cx, cy, scale), 0, 0);

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
      var tc = W2C(t.x, t.y), col = scopeCol(tcolor(t.tid)), rr = 10 * dpr;
      var spd = Math.hypot(t.vx, t.vy);
      /* LOCKED → the same corner-bracket mark used on the video, so the held target reads
       * identically wherever the operator happens to be looking. Velocity vector stays: on the
       * scope that is the reason you locked it. */
      if (engagedKey === ("rad:" + t.tid)) {
        var lb = 13 * dpr, on = css("--on");
        cornerBrackets(ctx, tc[0] - lb, tc[1] - lb, tc[0] + lb, tc[1] + lb, on, dpr);
        ctx.strokeStyle = on; ctx.fillStyle = on; ctx.lineWidth = 1.5 * dpr;
        ctx.beginPath(); ctx.arc(tc[0], tc[1], 1.8 * dpr, 0, 2 * Math.PI); ctx.fill();
        var vl = W2C(t.x + t.vx, t.y + t.vy); ctx.beginPath(); ctx.moveTo(tc[0], tc[1]); ctx.lineTo(vl[0], vl[1]); ctx.stroke();
        ctx.fillText("LOCK  " + spd.toFixed(1) + " m/s · " + t.rng.toFixed(0) + " m", tc[0] + lb + 3 * dpr, tc[1] - lb - 3 * dpr);
        return;
      }
      ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = 1.5 * dpr;
      ctx.beginPath(); ctx.arc(tc[0], tc[1], rr, 0, 2 * Math.PI); ctx.stroke();
      ctx.beginPath(); ctx.arc(tc[0], tc[1], 1.6 * dpr, 0, 2 * Math.PI); ctx.fill();
      var vc = W2C(t.x + t.vx, t.y + t.vy); ctx.beginPath(); ctx.moveTo(tc[0], tc[1]); ctx.lineTo(vc[0], vc[1]); ctx.stroke();
      ctx.fillText("R#" + t.tid + "  " + spd.toFixed(1) + " m/s · " + t.rng.toFixed(0) + " m", tc[0] + rr + 3 * dpr, tc[1] - rr - 3 * dpr);
    });
  }

  function redrawAll() { drawEO(); drawPip(); drawRadar(lastRadar); }
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
      if (e) { drawEO(); drawPip(); }
    });
  }

  /* ── Selection: three surfaces, one result — declare the target and enter track mode.
   * Tapping an EO box, a radar circle, or a list row all do the same thing. Any of them
   * switches TRACK to MANUAL first, so a pick is never swallowed by AUTO. ── */
  /* Tapping the target that is already locked UNLOCKS it — the same press both ways, so the
   * operator never has to hunt for a separate "release". Tapping a different target moves the
   * lock straight there. */
  function pick(key) {
    if (!key) return;
    if (trackMode !== "man") setTrack("man");
    engage(key === engagedKey ? null : key);
  }
  /* EO panel: prefer the EO TRACK whose box was tapped (that is the thing the tracker can
   * actually lock); fall back to projecting the click azimuth onto a radar target. */
  $("eo").addEventListener("click", function (e) {
    if (roiArm || e.target.closest("#cluster") || e.target.closest("#zoombar")) return;
    var r = this.getBoundingClientRect();
    var cxp = (e.clientX - r.left) / r.width, cyp = (e.clientY - r.top) / r.height;   /* 0..1 in the panel */
    var hit = eoBoxHit(cxp, cyp);
    if (hit) { pick(hit); return; }
    /* While LOCKED, a tap anywhere off the held target RELEASES it — and does so immediately,
     * locally, rather than waiting for the tracker to acknowledge. It must not fall through to
     * "select the nearest radar target", which would swap one lock for another instead of
     * returning to stare. */
    if (engagedKey) { engage(null); draw(true, true); return; }
    var eoHfov = (lastStats.eo && lastStats.eo.hfov) || 0;
    var ts = radarTargets(); if (!ts.length || !eoHfov) return;
    var azClick = (cxp - 0.5) * eoHfov;
    pick(ts.slice().sort(function (a, b) { return Math.abs(a.az - azClick) - Math.abs(b.az - azClick); })[0].key);
  });
  $("radar-cv").addEventListener("click", function (e) {
    if (roiArm || !radarGeom) return;
    var r = this.getBoundingClientRect(), g = radarGeom;
    var px = (e.clientX - r.left) * (this.width / r.width), py = (e.clientY - r.top) * (this.height / r.height);
    var best = null, bd = 1e9;
    targets(lastRadar).forEach(function (t) { var dx = g.cx + t.x * g.scale - px, dy = g.cy - t.y * g.scale - py, d = Math.hypot(dx, dy); if (d < bd) { bd = d; best = t; } });
    if (best && bd < 40 * g.dpr) pick("rad:" + best.tid);
  });
  /* Tap a target-list row → declare it the tracked target, exactly like tapping its box on the
   * video or its circle on the scope. On POINTERDOWN, not click: a click only fires if the same
   * element is still there at release, and a touch adds its own delay on top. */
  $("tgt-list").addEventListener("pointerdown", function (e) {
    var li = e.target.closest("[data-key]"); if (!li) return;
    e.preventDefault();
    pick(li.dataset.key);
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
      /* SAT is reported, never acted on: QUALITY is the operator's call (see res-btns above). */
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
    if (!d.connected) { updateViewRange(null); renderTargetList(); draw(true, true); return; }
    updateViewRange(d);
    /* Drop the engagement only if the RADAR target we hold has gone. An EO engagement is the
     * tracker's to keep or drop (reflected in onTrkFrame) — a radar dropout must not clear it. */
    if (engagedKey && engagedKey.indexOf("rad:") === 0) {
      var present = {};
      targets(d).forEach(function (t) { present["rad:" + t.tid] = 1; });
      if (!present[engagedKey]) engage(null);
    }
    renderTargetList();
    /* AUTO runs off the SAME smoothed scores as the list, so what it holds is what the operator
     * would have picked. It reads the scores rather than row 1, because row 1 is pinned to the
     * engaged target — reading the display would make AUTO hold its own choice forever. */
    if (trackMode === "auto") engage(autoBest());
    draw(true, true);
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
  /* ---- Replay overlays over SSE (recorder pushes clock-paced at the full recorded rate) -------
   * The recorder streams replay radar + det on /rec/replay/{radar,det}/stream, paced to the
   * playback clock (pause/seek/rate). SSE = smooth ~26 Hz radar + on-time boxes (a poll was
   * ~8-14 Hz / laggy). Safe on the ~6-socket budget now that the old FIN-WAIT pileup causes are
   * gone: closeReplay closes these on EXIT (not next-open), the library list holds no streams,
   * and the recorder no longer transcodes on open. radarES/detES are the same handles the live
   * streams use — reused here, and closed by closeReplay / openReplay. */
  function stopReplayRadarPoll() {}   /* poll retired for SSE; kept as a safe no-op for existing callers */
  function stopReplayDetPoll() {}
  function openReplayRadarStream() {
    if (radarES) { radarES.close(); radarES = null; }
    radarES = new EventSource("/rec/replay/radar/stream");
    radarES.onmessage = function (e) { if (!replaying) return; var f = parseJSONsafe(e.data); if (f) onRadarFrame(f); };
  }
  function openReplayDetStream() {
    if (detES) { detES.close(); detES = null; } lastDet = null;
    detES = new EventSource("/rec/replay/det/stream");
    detES.onmessage = function (e) { if (!replaying) return; var m = parseJSONsafe(e.data); lastDet = (m && (m.dets || m.movers)) ? m : null; draw(true, false); };
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

  /* EO TRACKER — SSE push from /trk/stream. This is the SINGLE source of the EO boxes the
   * operator sees: the tracker turns the detector's per-frame boxes into targets with
   * identity (stable tid, confirm/coast, engaged lock). The raw detector boxes are a DEV
   * overlay only — drawing both was the double-display this module removes. A dead tracker
   * reads as NOT CONNECTED; it never silently falls back to raw detections. */
  var lastTrk = null, trkES = null;
  function openTrkStream() {
    if (trkES) { trkES.close(); trkES = null; }
    trkES = new EventSource("/trk/stream");
    trkES.onmessage = function (e) {
      if (replaying) return;
      var m = parseJSONsafe(e.data); if (m) lastTrk = (m.connected === false) ? null : m;
      onTrkFrame();
    };
    trkES.onerror = function () { if (!replaying) { lastTrk = null; onTrkFrame(); } };   /* auto-reconnects */
  }
  /* mode/engaged are reflected FROM THE WIRE, never from the button press — the tracker owns
   * the lock, so if it rejects or drops an engage the console must show that truth. */
  function onTrkFrame() {
    /* The operator's last action wins for a moment. The tracker takes a frame or two to adopt
     * an engage/release, and until it does its wire still reports the OLD value — mirroring that
     * blindly undid the press and made a release take a second or two to "stick". */
    var freshEngage = Date.now() - trkEngageSentAt < 1000;
    if (lastTrk && typeof lastTrk.engaged === "number") {
      var wire = lastTrk.engaged >= 0 ? ("eo:" + lastTrk.engaged) : null;
      if (wire && wire !== engagedKey && !freshEngage) { engagedKey = wire; updateTrackBanner(); }
      else if (!wire && engagedKey && engagedKey.indexOf("eo:") === 0 && !freshEngage) {
        engagedKey = null; updateTrackBanner();
      }
    }
    /* A LOCK IS THE OPERATOR'S, and only the operator (or the tracker itself) ends it. The
     * console does NOT drop it just because the track is missing from this frame: the tracker
     * keeps `engaged` pinned at the id even while the track is absent, so releasing on absence
     * made the two fight — we cleared, the next frame re-adopted from the wire, and the lock
     * flickered. Authority is the wire's `engaged`; we only mirror it.
     * We do track whether the locked target is actually ON this frame, because that decides
     * whether it is safe to hide everything else (see drawEO) — hiding the whole scene around a
     * lock we cannot draw would leave the operator blind. */
    lockLive = false;
    if (engagedKey) (lastTrk && lastTrk.tracks || []).forEach(function (t) { if (("eo:" + t.tid) === engagedKey) lockLive = true; });
    if (engagedKey && engagedKey.indexOf("rad:") === 0) lockLive = true;   /* radar locks are judged in drawEO */
    renderTargetList();
    draw(true, false);
  }
  function trkConnected() { return !!(lastTrk && lastTrk.connected !== false); }

  /* FUSION — SSE push from /fus/stream (~41/s with both trackers up, 1/s heartbeat otherwise).
   * It drives the target LIST and tags which video boxes are fused; the boxes themselves keep
   * coming from the tracker at its own rate, so a fusion stall never freezes the overlay.
   * Down or stale -> the list falls back to the two per-sensor lists. */
  function openFusStream() {
    if (fusES) { fusES.close(); fusES = null; }
    fusES = new EventSource("/fus/stream");
    fusES.onmessage = function (e) {
      if (replaying) return;
      var m = parseJSONsafe(e.data);
      if (m && m.connected !== false) { lastFus = m; lastFusAt = Date.now(); } else { lastFus = null; }
      renderTargetList(); draw(true, false);
    };
    fusES.onerror = function () {
      if (!replaying) { lastFus = null; renderTargetList(); draw(true, false); }   /* auto-reconnects */
    };
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
    /* median/denoise have no controls — the console forces both off on load, nothing to read back. */
    /* DISPLAY 30/60 is gated on the EO feed exposing disp_fps — the row stays hidden until then. */
    if (typeof eo.disp_fps === "number") {
      $("dispfps-row").style.display = "";
      if (settled) { var df = document.querySelector('#disp-btns [data-disp="' + eo.disp_fps + '"]'); if (df) setSeg("disp-btns", df); }
    }
  }

  /* ═══════════════════════ RECORDER / REPLAY ═══════════════════════ */
  var TAGVOCAB = ["filter", "night", "day", "human", "vehicle", "drone", "long-range", "short-range", "radar", "tracking", "fusion", "illum", "test", "bug", "demo", "calibration"];
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
  /* Default recording name: compact YYMMDD_HHMMSS, no "REC" prefix (every entry in the library is
   * a recording — the word cost ~4 characters of the visible name and told you nothing).
   * SECONDS matter: at minute resolution two recordings started in the same minute got identical
   * names, and the recorder names each folder inside an offload .tar after the name — so two
   * same-named recordings extracted into one directory would silently overwrite each other with
   * no error. Seconds makes the default unique by construction. */
  function localStamp() {
    var d = new Date(), p = function (n) { return ("0" + n).slice(-2); };
    return p(d.getFullYear() % 100) + p(d.getMonth() + 1) + p(d.getDate()) + "_"
         + p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds());
  }
  /* Older recordings were saved as "REC 2026-07-18 21:24 radar empty scene". Render those in the
   * new compact form so the actual description is readable, without rewriting stored names.
   * Anything that doesn't match is shown untouched — the name is operator-editable free text. */
  function libTitle(name) {
    if (!name) return "";
    /* seconds optional: old stored names are hh:mm, but a name may carry hh:mm:ss — without the
     * optional group the ":ss" fell through into the description as literal ":35 …". */
    var m = /^\s*(?:REC\s+)?(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2})(?::(\d{2}))?\s*(.*)$/.exec(name);
    if (!m) return name.replace(/^\s*REC\s+/, "");
    var rest = m[7].trim();
    return m[1].slice(2) + m[2] + m[3] + "_" + m[4] + m[5] + (m[6] || "") + (rest ? " " + rest : "");
  }
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
    $("dlg-name").value = localStamp();
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
    /* Pre-fill the SHOWN name too, so edit/copy/tile all agree. Saving then writes the compact
     * form back, quietly migrating old "REC yyyy-mm-dd hh:mm …" names as they're touched. */
    $("dlg-name").value = libTitle(s.name) || "";
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
    loadLibrary();   /* loadLibrary → renderLibrary rebuilds the tag-filter bar from live data */
  };
  $("lib-close").onclick = function () {   /* EXIT to live -> guarantee a clean live state */
    /* Cancel any in-flight replay open (its late callback would flip replaying=true over the live
     * view) and NEVER strand replaying=true in live — that dead-locks REC + LIGHT (both bail while
     * replaying) until a reload. Also stop any replay pollers and drop the replay chrome. */
    replayGen++; replaying = false;
    if (replayStatePoll) { clearInterval(replayStatePoll); replayStatePoll = null; }
    stopReplayRadarPoll(); stopReplayDetPoll();
    document.body.classList.remove("replay");
    $("library").hidden = true; libSel = {};
    API.stream = "/stream"; API.radar = "/radar"; API.stats = "/stats"; API.rstats = "/rstats";
    $("video").src = API.stream + "?t=" + Date.now();
    openRadarStream(); openDetStream();
  };
  $("lib-q").oninput = debounce(loadLibrary, 250);
  /* The filter bar = the common vocab PLUS every custom tag actually used across the library, so
   * operator-added tags become filterable (they weren't before — the bar was TAGVOCAB-only and
   * built once). Rebuilt on each library load; selection lives in libTagFilter so it survives the
   * rebuild, and a filter for a tag that no longer exists anywhere is dropped. */
  function renderTagFilter(sessions) {
    var used = {};
    (sessions || []).forEach(function (s) { (s.tags || []).forEach(function (t) { used[t] = 1; }); });
    var custom = Object.keys(used).filter(function (t) { return TAGVOCAB.indexOf(t) < 0; }).sort();
    var vocab = TAGVOCAB.concat(custom);
    Object.keys(libTagFilter).forEach(function (t) { if (vocab.indexOf(t) < 0) delete libTagFilter[t]; });
    var w = $("lib-tagfilter"); w.innerHTML = "";
    vocab.forEach(function (t) {
      var c = document.createElement("span");
      c.className = "tagchip" + (libTagFilter[t] ? " on" : "") + (custom.indexOf(t) >= 0 ? " custom" : "");
      c.textContent = t;
      c.onclick = function () { c.classList.toggle("on"); libTagFilter[t] = c.classList.contains("on"); loadLibrary(); };
      w.appendChild(c);
    });
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
    renderTagFilter(all);   /* refresh the filter bar from every tag in use, incl. custom ones */
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
  /* "Convert to native (HD)" per card. The recorder transcodes the recording to a 60fps H.264
   * mp4 in the background (GET /rec/replay/transcode?sid= -> {state:none|building|ready|failed, pct}),
   * one at a time, cached, survives navigation. We poll it for a progress %, then refresh the
   * library so the card flips to "HD ✓". hdBuild holds the one in-flight build so a re-render
   * (search/filter) keeps showing its progress. */
  var hdBuild = null, hdTimer = null, hdReady = {};   /* sids whose HD mp4 we KNOW is ready (converted this
                                                       * session). bytes.native is the RAW recording, NOT the
                                                       * HD mp4 — it does not mean HD is built. Until the
                                                       * recorder reports HD status per session in /rec/library,
                                                       * ✓ only shows for ones actually converted here. */
  function setHdBtn(btn, state, pct) {
    btn.classList.remove("ready", "building");
    if (state === "ready") { btn.textContent = "HD ✓"; btn.title = "native HD available"; btn.classList.add("ready"); btn.disabled = true; }
    else if (state === "building") { btn.textContent = "HD " + (pct || 0) + "%"; btn.title = "building native HD…"; btn.classList.add("building"); btn.disabled = true; }
    else { btn.textContent = "⬆ HD"; btn.title = "convert to native 60 fps HD (background, one-time)"; btn.disabled = false; }
  }
  function hdPoll(sid) {
    fetch("/rec/replay/transcode?sid=" + encodeURIComponent(sid)).then(function (r) { return r.json(); }).then(function (j) {
      var st = j && j.state, pct = (j && (j.pct != null ? j.pct : j.percent)) || 0;
      var btn = document.querySelector('.lib-card[data-sid="' + sid + '"] .lib-hd');
      if (st === "ready") { hdBuild = null; hdReady[sid] = 1; if (btn) setHdBtn(btn, "ready"); loadLibrary(); return; }
      if (st === "failed") { hdBuild = null; if (btn) setHdBtn(btn, "none"); toast("HD conversion failed.", "err", 3500); return; }
      hdBuild = { sid: sid, pct: pct };
      if (btn) setHdBtn(btn, "building", pct);
      hdTimer = setTimeout(function () { hdPoll(sid); }, 2000);
    }).catch(function () { hdTimer = setTimeout(function () { hdPoll(sid); }, 3000); });
  }
  function libCard(s) {
    var card = document.createElement("div"); card.className = "lib-card" + (libSel[s.sid] ? " sel" : ""); card.dataset.sid = s.sid;
    var hasThumbs = s.thumbs && s.thumbs > 0, poster;
    if (hasThumbs) {
      poster = document.createElement("img"); poster.className = "lib-poster"; poster.src = "/rec/thumbs/" + s.sid + "/2.jpg";
      var timer = null, i = 0;
      card.onmouseenter = function () { if (timer) return; timer = setInterval(function () { i = (i + 1) % 8; poster.src = "/rec/thumbs/" + s.sid + "/" + i + ".jpg"; }, 166); libTimers.push(timer); };
      card.onmouseleave = function () { if (timer) clearInterval(timer); timer = null; poster.src = "/rec/thumbs/" + s.sid + "/2.jpg"; };
    } else { poster = document.createElement("div"); poster.className = "lib-poster radaronly"; poster.textContent = "◟ RADAR ONLY ◞"; }
    /* Wrap the poster in an aspect-ratio box. Putting aspect-ratio on the <img> itself made the
     * CSS grid size the row to just the image and clip the card body (name/edit/copy) under
     * overflow:hidden until a reflow. A div wrapper resolves its height cleanly from its width. */
    var pwrap = document.createElement("div"); pwrap.className = "lib-postwrap"; pwrap.appendChild(poster);
    card.appendChild(pwrap);
    if (s.state !== "saved") { var rib = document.createElement("div"); rib.className = "lib-pending"; rib.textContent = "PENDING"; card.appendChild(rib); }
    var cb = document.createElement("input"); cb.type = "checkbox"; cb.className = "lib-cb"; cb.checked = !!libSel[s.sid];
    cb.onclick = function (e) { e.stopPropagation(); libSel[s.sid] = cb.checked; card.classList.toggle("sel", cb.checked); updateDelBtn(); };
    card.appendChild(cb);
    /* (No per-card "drop raw" control — removed at the operator's request. The recorder still
     * exposes /rec/ctl?purge_native=<sid> if reclaiming raw is ever needed.) */
    var body = document.createElement("div"); body.className = "lib-cardbody";
    /* Name on its own full-width line; the buttons sit on the row BELOW it. Sharing one line
     * meant the actions ate ~90px and every description was truncated to "…". */
    var nrow = document.createElement("div"); nrow.className = "lib-namerow";
    var nm = document.createElement("span"); nm.className = "lib-name";
    nm.textContent = libTitle(s.name) || s.sid; nm.title = s.name || s.sid;
    var eb = document.createElement("button"); eb.className = "lib-act"; eb.textContent = "✎"; eb.title = "edit name / tags / note";
    eb.onclick = function (e) { e.stopPropagation(); openEditDialog(s); };
    var cpb = document.createElement("button"); cpb.className = "lib-act"; cpb.textContent = "⧉"; cpb.title = "copy name";
    /* copy WHAT THE CARD SHOWS — copying the raw stored name handed back "REC 2026-07-09 11:58"
     * while the tile read "260709_1158", which is just a lie about what you clicked. */
    cpb.onclick = function (e) { e.stopPropagation(); copyText(libTitle(s.name) || s.sid); };
    var hdb = document.createElement("button"); hdb.className = "lib-act lib-hd";
    hdb.onclick = function (e) {
      e.stopPropagation();
      if (hdBuild && hdBuild.sid !== s.sid) { toast("Another HD conversion is running — one at a time.", "err", 3000); return; }
      if (hdTimer) clearTimeout(hdTimer);
      setHdBtn(hdb, "building", 0); hdBuild = { sid: s.sid, pct: 0 };
      fetch("/rec/replay/transcode?sid=" + encodeURIComponent(s.sid)).catch(function () {});
      hdPoll(s.sid);
    };
    setHdBtn(hdb, (hdBuild && hdBuild.sid === s.sid) ? "building" : (hdReady[s.sid] || s.hd === "ready" ? "ready" : "none"), hdBuild ? hdBuild.pct : 0);
    nrow.appendChild(nm); body.appendChild(nrow);
    var arow = document.createElement("div"); arow.className = "lib-actrow";
    arow.appendChild(eb); arow.appendChild(cpb); arow.appendChild(hdb); body.appendChild(arow);
    /* date · duration · size share ONE line (size pushed right) — they were three separate rows
     * that, with the tags underneath, overran the card and got clipped. */
    var rest = document.createElement("div");
    rest.innerHTML = '<div class="lib-meta"><span>' + libDate(s.t0) + "</span><span>" + fmtClock(s.dur_ms) + "</span>"
      + '<span class="lib-size">' + sizeBadge(s.bytes) + "</span></div>"
      + '<div class="lib-cardtags">' + (s.tags || []).map(function (t) { return '<span class="tagchip">' + esc(t) + "</span>"; }).join("") + "</div>"
      + (s.note ? '<div class="lib-note">' + esc(s.note) + "</div>" : "");
    body.appendChild(rest);
    card.appendChild(body);
    card.onclick = function () { openReplay(s); };
    return card;
  }
  function updateDelBtn() {
    var n = Object.keys(libSel).filter(function (k) { return libSel[k]; }).length;
    var d = $("lib-del"); d.hidden = n === 0; d.textContent = "DELETE (" + n + ")";
    var o = $("lib-offsel"); o.hidden = n === 0; o.textContent = "OFFLOAD (" + n + ")";
    /* flips to CLEAR once everything shown is ticked, so one button covers both directions */
    var sa = $("lib-selall");
    if (sa) sa.textContent = (libSessions.length && n >= libSessions.length) ? "CLEAR" : "SELECT ALL";
  }
  /* offload = download a .tar of the session(s) — display video + radar + data. Over plain
   * HTTP the browser can't pick a folder, so it lands in Downloads (enable the browser's
   * "ask where to save each file" to choose one). tier=display keeps it reasonable; raw
   * native is excluded — it is huge, and offload is for sharing/review. */
  /* Preferred download name: the RECORDING, not its session id. Sanitised for Windows/macOS/Linux
   * filenames (\ / : * ? " < > | are all illegal somewhere) and length-capped.
   *
   * CAVEAT — this is currently a FALLBACK ONLY. The recorder answers /rec/export with
   *   Content-Disposition: attachment; filename="airpoc-display-1session.tar"
   * and a server-supplied filename BEATS the <a download> attribute in Chrome, so that name wins
   * and this one is ignored. Naming has to be fixed on the recorder (it already derives the
   * recording name server-side for the folder inside the tar). This stays as the fallback for if
   * the header is ever dropped. */
  function offloadFilename(sids, n) {
    if (n === 1 && sids !== "all" && sids.indexOf(",") < 0) {
      var s = libSessions.filter(function (x) { return x.sid === sids; })[0];
      var nm = (s && libTitle(s.name)) || sids;
      nm = nm.replace(/[\\/:*?"<>|\u0000-\u001f]+/g, "-").replace(/\s+/g, " ").replace(/^[.\s]+|[.\s]+$/g, "").slice(0, 90);
      return (nm || sids) + ".tar";
    }
    return "airpoc-" + n + "-sessions.tar";
  }
  function offloadTar(sids, n) {
    if (!confirm("Offload " + n + " session(s) as a .tar (display video + radar + data)?\n\nIt saves to your Downloads. To CHOOSE A FOLDER each time, turn on Chrome ▸ Settings ▸ Downloads ▸ \"Ask where to save each file before downloading\" — then a Save-As window opens for every offload.\n\nA large session takes up to a minute to build before the download starts.")) return;
    var a = document.createElement("a");
    a.href = "/rec/export?sids=" + encodeURIComponent(sids) + "&tier=display";
    /* download attr => the browser SAVES A FILE (shows it in the download bar) instead of
     * NAVIGATING the tab to the URL — which is what dumped you on the ERR_EMPTY_RESPONSE page
     * when the build was slow. With "ask where to save" on, this same click opens the folder picker. */
    a.download = offloadFilename(sids, n);
    document.body.appendChild(a); a.click(); a.remove();
    toast("Offload started — building the .tar; it'll land in your downloads (large sessions take ~a minute).", "ok", 6000);
  }
  /* One selection model: SELECT ALL is just a fast way to tick every shown card, and the
   * OFFLOAD/DELETE buttons appear from the selection either way. (There used to be separate
   * OFFLOAD ALL / DELETE ALL buttons sitting there permanently — two routes to the same action,
   * with the destructive one always one click away even when nothing was selected.)
   * "Shown" means what the tag filter + search currently list, not the whole disk. */
  $("lib-selall").onclick = function () {
    if (!libSessions.length) { alert("Library is empty."); return; }
    var sel = Object.keys(libSel).filter(function (k) { return libSel[k]; }).length;
    var selectingAll = sel < libSessions.length;
    libSel = {};
    if (selectingAll) libSessions.forEach(function (s) { libSel[s.sid] = true; });
    document.querySelectorAll("#lib-grid .lib-card").forEach(function (c) {
      var on = !!libSel[c.dataset.sid];
      c.classList.toggle("sel", on);
      var cb = c.querySelector(".lib-cb"); if (cb) cb.checked = on;
    });
    updateDelBtn();
  };
  $("lib-offsel").onclick = function () { var s = Object.keys(libSel).filter(function (k) { return libSel[k]; }); if (!s.length) return; offloadTar(s.join(","), s.length); };
  $("lib-del").onclick = function () {
    var sids = Object.keys(libSel).filter(function (k) { return libSel[k]; });
    if (!sids.length) return;
    /* Selecting everything and deleting IS the old "delete all" — keep the second gate for it. */
    var wipingAll = libSessions.length && sids.length >= libSessions.length;
    if (wipingAll) {
      if (!confirm("Delete ALL " + sids.length + " shown session(s)? This permanently erases them — there is no undo.")) return;
      if (prompt("FINAL CHECK — type DELETE (capitals) to erase all " + sids.length + " recordings:") !== "DELETE") return;
    } else if (!confirm("Delete " + sids.length + " session(s)? Cannot be undone.")) return;
    fetch("/rec/ctl?delete=" + sids.map(encodeURIComponent).join(",")).then(function () { libSel = {}; loadLibrary(); }).catch(function () {});
  };

  /* ── REPLAY ── */
  /* NOTE: the live sensors are NO LONGER stopped while reviewing recordings. Stopping and
   * restarting eo_pipeline/radar/detector on every library visit thrashed the box — the camera
   * re-init + detector engine reload pegged the CPU and wedged the camera after a few enter/exit
   * cycles (black EO, unrecoverable without a reboot). The recorder pre-builds the replay mp4,
   * so keeping the live stack running during review costs nothing extra. The browser's live
   * media streams are still closed on library-open (connection budget) and reopened on exit. */
  var opening = false, replayGen = 0, autoNatived = false;
  function openReplay(s) {
    if (opening) return;                                 /* ignore double-taps while an open is in flight */
    if (!s || !s.sid) return;                            /* malformed session -> nothing to open */
    opening = true;
    var myGen = ++replayGen;                             /* if EXIT-to-live or another open happens while this
                                                          * async open is in flight, replayGen moves on and the
                                                          * late callback below aborts instead of flipping
                                                          * replaying=true over the live view (which dead-locked
                                                          * REC + LIGHT until a reload). */
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
        if (myGen !== replayGen) { fetch("/rec/replay/ctl?close=1").catch(function () {}); return; }   /* user left to live / opened another while this was in flight — abort, don't hijack the live view */
      replaySession = s; replaying = true;
      if (radarES) { radarES.close(); radarES = null; }   /* stop the live radar push while reviewing */
      if (detES) { detES.close(); detES = null; } lastDet = null;   /* live det push off; replay poller takes over */
      resetReplayZoom(); setZoomLabel(); radarROI = null; eoROI = false; roiArm = false; setRoiUI();
      /* "was this channel recorded?" from the actual captured bytes — NOT thumbs (a
       * session can have EO video with no thumbnails generated). */
      replayHasEO = !!(s.bytes && ((s.bytes.display > 0) || (s.bytes.native > 0)));
      replayHasRadar = !!(s.bytes && s.bytes.radar > 0);
      document.body.classList.add("replay"); $("library").hidden = true;
      /* Default to the INSTANT display (MJPEG) view so PLAY works immediately; native 60fps is one
       * tap on the DISPLAY/NATIVE toggle. NOTE: this does NOT avoid the transcode — the recorder
       * ffmpeg-transcodes the whole recording on OPEN regardless of mode, and does not kill it on
       * close. Stacking those encodes is what pegs the box; that is a recorder-side bug the console
       * cannot prevent (it must open the session to replay it). Flagged for the recorder module. */
      rctl("video=display");
      API.stream = "/rec/replay/stream"; API.radar = "/rec/replay/radar"; API.stats = "/rec/replay/stats"; API.rstats = "/rec/replay/rstats";
      openReplayRadarStream();                            /* replay radar — self-chaining poll (one socket max) */
      openReplayDetStream();                              /* replay det boxes — self-chaining poll (one socket max) */
      /* Show the recorded still at the open position — NOT the live stream. The replay
       * MJPEG only pushes while playing, so before Play we'd otherwise keep showing the
       * last live frame. pollReplayState swaps to the stream once playback starts. */
      replayPlaying = false; replayStillT = -1; resetMp4State(); autoNatived = false;   /* start on MJPEG; poll swaps to native mp4 if it's already built */
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
    replayGen++;                                         /* cancel any in-flight open so it can't re-enter replay */
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
  /* Native replay = the recorded H.264 mp4 (/rec/replay/native.mp4), full 60fps. The recorder
   * reports its status in /rec/replay/state as native_mp4 (none|building|ready|failed) + a pct,
   * so we READ it there — no endpoint probing. The <video> plays on its OWN clock and is nudged
   * to the transport clock only on a big drift; the scrub handler seeks it directly. Display
   * channel, radar-only, or while-building fall back to the paced MJPEG <img>. */
  var mp4State = { sid: null, srcSet: false };
  function resetMp4State() {
    mp4State = { sid: null, srcSet: false };
    var nv = $("nvid"); nv.pause(); nv.removeAttribute("src"); nv.load();
    nv.style.display = "none"; $("video").style.display = ""; $("nat-badge").hidden = true;
  }
  function updateReplayVideo(rs) {
    var vid = $("video"), nv = $("nvid"), badge = $("nat-badge");
    var sid = replaySession && replaySession.sid;
    var nat = rs.native_mp4;                                       /* none | building | ready | failed */
    if (rs.video_src === "native" && sid && nat === "ready") {
      if (mp4State.sid !== sid || !mp4State.srcSet) { mp4State.sid = sid; mp4State.srcSet = true; nv.src = "/rec/replay/native.mp4?sid=" + encodeURIComponent(sid); vid.src = BLANK; }  /* BLANK stops the hidden MJPEG stream */
      nv.style.display = "block"; vid.style.display = "none"; badge.hidden = true;   /* "" would revert to display:none -> black */
      replayPlaying = false; replayStillT = -1;                   /* re-arm MJPEG src if we switch back */
      var tv = rs.t_ms / 1000;
      nv.playbackRate = rs.rate || 1;
      if (rs.playing && rs.t_ms < rs.dur_ms) { if (nv.paused) nv.play().catch(function () {}); }
      else { if (!nv.paused) nv.pause(); }
      /* Let the <video> run on its OWN clock; correct to the transport clock ONLY on a big drift
       * (>0.3s: scrub, stall, throttled tab). Hard-setting currentTime every poll turned playback
       * and scrub into constant seeking that reset to frame 0. The scrub handler seeks directly
       * per input while scrubbing. Keyframe-per-second mp4 makes the rare correction cheap. */
      if (!scrubbing && nv.readyState >= 1 && isFinite(tv) && Math.abs(nv.currentTime - tv) > 0.3) nv.currentTime = tv;
      return;
    }
    if (rs.video_src === "native" && sid && nat === "building") {
      if (mp4State.srcSet) resetMp4State();                        /* was playing, now (re)building -> back to MJPEG under the badge */
      badge.hidden = false; badge.textContent = "PREPARING NATIVE 60 fps · " + (rs.native_mp4_pct || 0) + "%";
    } else {
      if (mp4State.sid !== null) resetMp4State();                  /* left native / none / failed -> tear the video down */
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
      /* If the HD mp4 is ALREADY built (cached), switch to it ONCE and stream it — no rebuild, no
       * tapping NATIVE. We open in display to avoid triggering a build on a not-yet-built movie;
       * this flips to the cache the moment the recorder reports it ready. A manual toggle wins after. */
      if (replayHasEO && !autoNatived && rs.native_mp4 === "ready" && rs.video_src !== "native") { autoNatived = true; rctl("video=native"); }
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
  $("tp-video").onclick = function () { autoNatived = true; rctl("video=" + ($("tp-video").textContent === "NATIVE" ? "display" : "native")); };   /* manual choice wins over auto-native */
  $("tp-step-b").onclick = function () { rctl("step=-1"); };
  $("tp-step-f").onclick = function () { rctl("step=1"); };
  $("tp-scrub").oninput = function () {
    scrubbing = true; var v = +this.value; $("tp-cur").textContent = fmtClockT(v);
    /* Native replay: seek the mp4 DIRECTLY on each scrub input so the picture tracks the drag,
     * like the DISPLAY still does. Without this the frame only moved on the 150 ms poll and the
     * mp4's own seeks lagged behind, so it looked frozen while scrubbing. */
    var nv = $("nvid");
    if (mp4State.srcSet && nv.readyState >= 1) nv.currentTime = v / 1000;   /* native loaded -> seek the mp4 directly to track the drag */
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

  setTrack("man");   /* MANUAL on boot - nothing self-selects a target */ setIllum("auto"); setExpMode(true); applyTheme();
  /* NO video-stream watchdog. It caused more harm than it fixed: its reconnect blanked the
   * <img> (src="") to BLACK, and at native res the reload can't complete before the next
   * check, so the video stays black while the overlays keep running — the exact bug seen.
   * At low res it showed as a periodic blink. A genuinely dead stream is rare; a manual
   * refresh recovers it. If auto-recovery is ever wanted, it must NOT blank the frame
   * (assign the new src straight onto a hidden shadow <img>, swap on its load). */

  setInterval(poll, 160); poll();
  openRadarStream();   /* live = SSE push; replay opens its own radar SSE (poll fallback) in openReplay */
  openDetStream();                                  /* raw detector boxes — DEV overlay only now */
  openTrkStream();                                  /* EO TRACKER — the operator's EO boxes + engaged lock */
  openFusStream();                                  /* FUSION — the one target list when it is up */
  initRadarOv();                                    /* overlay toggle (browser) + AZ/EL trim (from the Jetson) */
  initDetStyle();                                   /* detector mark style BOX/SEEKER (persisted) */
  /* scene layer: off unless this browser had it on. It is a viewing preference, and the daemon
   * keeps accumulating either way — showing it is free, so the choice is purely the operator's. */
  (function () {
    var b = document.querySelector('#scn-btns [data-scn="' + scene.on + '"]'); if (b) setSeg("scn-btns", b);
    if (scene.on) setScene(1);
  })();
  setInterval(pollRstats, 400); pollRstats();
  setInterval(pollDstats, 1000); pollDstats();
  setInterval(pollRec, 400); pollRec();
  /* Console-enforced defaults on load (operator request). These have no controls in DEV any more
   * — they're fixed values the operator never changes, so the console just asserts them and the
   * panel stays uncluttered: EO MEDIAN off + DENOISE off, detector MAX DETS 25, MOTION off,
   * TEMPORAL on, radar FOV ±60° + EL ±20°. Touch guards stop the /stats readback fighting the push. */
  var MAX_DETS = 25;
  setTimeout(function () {
    if (replaying) return;
    ispTouch = Date.now();
    ctl("median=0");
    ctl("denoise=0");
    dtTouch = Date.now();
    ctl("det_max_dets=" + MAX_DETS);
    /* MOTION defaults OFF (operator request). Its worker is frozen on the current build anyway,
     * but the intent is: the temporal-integration path is now the way faint movers get reported,
     * so the class-less motion safety net is off by default. */
    ctl("det_motion=0"); var mb = document.querySelector('#mot-btns [data-mot="0"]'); if (mb) setSeg("mot-btns", mb);
    /* TEMPORAL defaults ON (operator request) — finding far/weak targets is the point of the
     * seeker, and the cost is latency on those faint targets only. Harmless on a detector build
     * that doesn't know the knob yet: it just ignores it. */
    ctl("det_temporal=1"); var tb = document.querySelector('#tmp-btns [data-tmp="1"]'); if (tb) setSeg("tmp-btns", tb);
    rcTouch = Date.now();
    ctl("radar_fov=60");   $("rd-fov").value = 60;   $("rv-fov").textContent = "60°";
    ctl("radar_elmax=20"); $("rd-elmax").value = 20; $("rv-elmax").textContent = "20°";
  }, 1000);
})();
