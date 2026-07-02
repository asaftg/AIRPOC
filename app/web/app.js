/* AIRPOC operator console — polling + controls + canvas overlays (EO reticle, radar
 * polar scope). No websockets, no dependencies. Video is the MJPEG <img>; telemetry
 * is /stats (poll); controls are /ctl (one-shot). Visual language = Seeker bench. */
(function () {
  "use strict";
  var $ = function (id) { return document.getElementById(id); };
  var ctl = function (qs) { fetch("/ctl?" + qs).catch(function () {}); };
  var ZOOMS = [1, 2, 4, 8];
  var css = function (name) { return getComputedStyle(document.body).getPropertyValue(name).trim(); };

  /* ── theme ── */
  var theme = localStorage.getItem("airpoc-theme") || "night";
  function applyTheme() { document.body.className = theme; $("theme").textContent = theme.toUpperCase(); redrawAll(); }
  $("theme").onclick = function () { theme = theme === "day" ? "night" : "day"; localStorage.setItem("airpoc-theme", theme); applyTheme(); };

  /* ── DEV panel ── */
  $("devbtn").onclick = function () { $("dev").classList.toggle("open"); };
  $("devclose").onclick = function () { $("dev").classList.remove("open"); };

  /* ── swap ── */
  document.querySelectorAll("[data-swap]").forEach(function (b) {
    b.onclick = function (e) { e.stopPropagation(); $("stage").classList.toggle("swap"); setTimeout(redrawAll, 20); };
  });

  /* ── zoom ── */
  var zoom = 1;
  document.querySelectorAll("[data-zoom]").forEach(function (b) {
    b.onclick = function () {
      var i = ZOOMS.indexOf(zoom) + parseInt(b.dataset.zoom, 10);
      if (i < 0 || i >= ZOOMS.length) return;
      zoom = ZOOMS[i]; ctl("zoom=" + zoom); paintZoom();
    };
  });
  function paintZoom() { $("v-zval").textContent = zoom.toFixed(1) + "×"; }

  /* ── mode ── */
  function setMode(m) { ctl("mode=" + m); reflectMode(m); }
  function reflectMode(m) {
    document.querySelectorAll("#mode button").forEach(function (b) { b.classList.toggle("active", b.dataset.mode === m); });
    $("trackbtn").classList.toggle("active", m === "track");
  }
  document.querySelectorAll("#mode button").forEach(function (b) { b.onclick = function () { setMode(b.dataset.mode); }; });
  $("trackbtn").onclick = function () { setMode($("trackbtn").classList.contains("active") ? "scan" : "track"); };

  /* ── illuminator ── */
  $("light").onclick = function () {
    var on = $("light").classList.contains("firing");
    if (!on && !confirm("Fire 850nm IR laser? (invisible, eye hazard)")) return;
    ctl("laser=" + (on ? 0 : 1));
  };
  $("autofov").onclick = function () { ctl("autofov=1"); };
  $("s-pow").oninput = function () { $("o-pow").textContent = this.value + "%"; ctl("power=" + Math.round(this.value * 255 / 100)); };
  $("s-fov").oninput = function () { $("o-fov").textContent = this.value + "°"; ctl("fov=" + this.value); };

  /* ── stream ── */
  $("s-fps").oninput = function () { $("o-fps").textContent = this.value; ctl("fps=" + this.value); };
  $("s-q").oninput   = function () { $("o-q").textContent = this.value;   ctl("q=" + this.value); };
  document.querySelectorAll("#presets button").forEach(function (b) {
    b.onclick = function () {
      document.querySelectorAll("#presets button").forEach(function (x) { x.classList.remove("active"); });
      b.classList.add("active"); ctl("preset=" + b.dataset.preset);
    };
  });

  /* ── reserved ── */
  $("rec").onclick = function () { $("rec").classList.toggle("active"); };
  $("restart").onclick = function () { if (confirm("Restart AIRPOC service?")) ctl("restart=1"); };
  $("logs").onclick = function () { var l = $("logs"); l.textContent = "reserved"; setTimeout(function () { l.textContent = "LOGS"; }, 900); };

  /* ── canvas helpers ── */
  function fit(cv) {
    var r = cv.getBoundingClientRect();
    var dpr = window.devicePixelRatio || 1;
    cv.width = Math.max(1, Math.floor(r.width * dpr));
    cv.height = Math.max(1, Math.floor(r.height * dpr));
    return { ctx: cv.getContext("2d"), w: cv.width, h: cv.height, dpr: dpr };
  }

  /* EO reticle — amber tactical crosshair with a center gap + tick marks. */
  function drawReticle() {
    var f = fit($("eo-ovl")), ctx = f.ctx, w = f.w, h = f.h, dpr = f.dpr;
    ctx.clearRect(0, 0, w, h);
    var cx = w / 2, cy = h / 2, amber = css("--amber");
    ctx.strokeStyle = amber; ctx.fillStyle = amber; ctx.lineWidth = 1.4 * dpr;
    ctx.globalAlpha = 0.85;
    var gap = 10 * dpr, arm = 26 * dpr;
    [[0, -1], [0, 1], [-1, 0], [1, 0]].forEach(function (d) {
      ctx.beginPath();
      ctx.moveTo(cx + d[0] * gap, cy + d[1] * gap);
      ctx.lineTo(cx + d[0] * (gap + arm), cy + d[1] * (gap + arm));
      ctx.stroke();
    });
    ctx.globalAlpha = 0.5;
    ctx.beginPath(); ctx.arc(cx, cy, 30 * dpr, 0, 2 * Math.PI); ctx.stroke();
    ctx.globalAlpha = 1; ctx.beginPath(); ctx.arc(cx, cy, 2 * dpr, 0, 2 * Math.PI); ctx.fill();
    /* corner FOV brackets */
    ctx.globalAlpha = 0.35; ctx.lineWidth = 1.2 * dpr;
    var m = 16 * dpr, L = 22 * dpr;
    [[m, m, 1, 1], [w - m, m, -1, 1], [m, h - m, 1, -1], [w - m, h - m, -1, -1]].forEach(function (c) {
      ctx.beginPath();
      ctx.moveTo(c[0], c[1] + c[3] * L); ctx.lineTo(c[0], c[1]); ctx.lineTo(c[0] + c[2] * L, c[1]);
      ctx.stroke();
    });
    ctx.globalAlpha = 1;
  }

  /* Radar polar scope — half-circle, range rings, cyan FOV sector (limited, not
   * 360°), amber reference ring, range labels. Placeholder returns "NO DATA". */
  var FOV_HALF = 60, MAX_M = 500, REF_M = 250;
  function drawRadar(tracks) {
    var f = fit($("radar-cv")), ctx = f.ctx, w = f.w, h = f.h, dpr = f.dpr;
    ctx.clearRect(0, 0, w, h);
    var cx = w / 2, cy = h - 10 * dpr, maxR = Math.max(20, Math.min(h - 16 * dpr, w / 2 - 6 * dpr));
    var cyan = css("--cyan"), amber = css("--amber"), dim = css("--dim");
    ctx.font = (10 * dpr) + "px ui-monospace, monospace";

    for (var i = 1; i <= 4; i++) {
      var ringM = MAX_M * i / 4, ref = Math.abs(ringM - REF_M) < 1;
      ctx.strokeStyle = ref ? amber : cyan;
      ctx.globalAlpha = ref ? 0.5 : 0.16; ctx.lineWidth = ref ? 1.4 * dpr : 1 * dpr;
      ctx.beginPath(); ctx.arc(cx, cy, maxR * i / 4, Math.PI, 2 * Math.PI); ctx.stroke();
      ctx.globalAlpha = ref ? 0.85 : 0.5; ctx.fillStyle = ref ? amber : dim;
      ctx.fillText(ringM.toFixed(0) + " m", cx + 4 * dpr, cy - maxR * i / 4 + 12 * dpr);
    }
    /* FOV sector */
    var fr = FOV_HALF * Math.PI / 180;
    ctx.globalAlpha = 0.07; ctx.fillStyle = cyan;
    ctx.beginPath(); ctx.moveTo(cx, cy);
    ctx.arc(cx, cy, maxR, -Math.PI / 2 - fr, -Math.PI / 2 + fr, false); ctx.closePath(); ctx.fill();
    ctx.globalAlpha = 0.28; ctx.strokeStyle = cyan; ctx.lineWidth = 1 * dpr; ctx.setLineDash([4 * dpr, 4 * dpr]);
    [-1, 1].forEach(function (s) {
      var a = -Math.PI / 2 + s * fr;
      ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx + Math.cos(a) * maxR, cy + Math.sin(a) * maxR); ctx.stroke();
    });
    ctx.setLineDash([]);
    /* boresight + N */
    ctx.globalAlpha = 0.4; ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx, cy - maxR); ctx.stroke();
    ctx.globalAlpha = 0.6; ctx.fillStyle = cyan; ctx.textAlign = "center";
    ctx.fillText("N", cx, cy - maxR + 12 * dpr); ctx.textAlign = "left";

    if (!tracks) {
      ctx.globalAlpha = 0.5; ctx.fillStyle = dim; ctx.textAlign = "center";
      ctx.fillText("NO DATA", cx, cy - maxR * 0.45); ctx.textAlign = "left";
    }
    ctx.globalAlpha = 1;
  }

  function redrawAll() { drawReticle(); drawRadar(null); }
  window.addEventListener("resize", redrawAll);

  /* ── ZULU ── */
  function zulu() { var d = new Date(), p = function (n) { return (n < 10 ? "0" : "") + n; }; $("v-zulu").textContent = p(d.getUTCHours()) + ":" + p(d.getUTCMinutes()); }
  setInterval(zulu, 1000); zulu();

  /* ── telemetry ── */
  function num(v, dp, suf) { return (v === null || v === undefined) ? "—" : v.toFixed(dp) + (suf || ""); }
  function idle(el) { return document.activeElement !== el; }

  function poll() {
    fetch("/stats").then(function (r) { return r.json(); }).then(function (d) {
      $("v-link").textContent = num(d.mbps, 1);
      $("p-link").classList.toggle("on", d.mbps > 0.05);
      $("v-batt").textContent = num(d.batt, 0, "%");
      $("v-alt").textContent  = num(d.alt, 0);
      $("eo-tl").textContent  = "EO · FOV " + num(d.hfov, 1, "°") + " · " + (d.zoom ? d.zoom.toFixed(1) : "1.0") + "×";
      $("eo-tr").textContent  = "BRG " + (d.brg === null ? "—" : num(d.brg, 0, "°")) + "  RNG " + (d.rng === null ? "—" : num(d.rng, 2, " km"));
      $("v-tracks").textContent = d.tracks === null ? "no data" : d.tracks + " TRK";
      $("v-est").textContent  = "EST " + num(d.mbps, 1) + " Mb/s · " + num(d.fps, 0) + " fps";
      $("v-srcfps").textContent = num(d.src_fps, 0);
      $("v-cpu").textContent  = num(d.cpu_c, 0);
      $("v-cam").textContent  = num(d.cam_c, 0);

      if (typeof d.zoom === "number" && ZOOMS.indexOf(d.zoom) >= 0) { zoom = d.zoom; paintZoom(); }
      if (d.mode) reflectMode(d.mode);

      var light = $("light");
      light.classList.toggle("firing", !!d.laser);
      light.classList.toggle("armed", !d.laser && d.lpresent);
      light.style.opacity = d.lpresent ? "1" : ".5";
      if (typeof d.lpower === "number" && idle($("s-pow"))) { var pc = Math.round(d.lpower * 100 / 255); $("s-pow").value = pc; $("o-pow").textContent = pc + "%"; }
      if (typeof d.lfov === "number" && idle($("s-fov"))) { $("s-fov").value = Math.round(d.lfov); $("o-fov").textContent = Math.round(d.lfov) + "°"; }
    }).catch(function () { $("v-link").textContent = "—"; $("p-link").classList.remove("on"); });
  }

  applyTheme();          /* also triggers first redraw */
  setInterval(poll, 160); poll();
})();
