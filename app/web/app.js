/* AIRPOC operator console — polling + controls. No websockets, no dependencies.
 * Video is the MJPEG <img>; everything else is /stats (poll) and /ctl (one-shot). */
(function () {
  "use strict";
  var $ = function (id) { return document.getElementById(id); };
  var ctl = function (qs) { fetch("/ctl?" + qs).catch(function () {}); };
  var ZOOMS = [1, 2, 4, 8];

  /* ---- theme (persisted; default night) ---- */
  var theme = localStorage.getItem("airpoc-theme") || "night";
  function applyTheme() {
    document.body.className = theme;
    $("theme").textContent = theme === "day" ? "DAY" : "NIGHT";
  }
  $("theme").onclick = function () {
    theme = theme === "day" ? "night" : "day";
    localStorage.setItem("airpoc-theme", theme);
    applyTheme();
  };
  applyTheme();

  /* ---- DEV panel ---- */
  $("devbtn").onclick = function () { $("dev").classList.toggle("open"); };
  $("devclose").onclick = function () { $("dev").classList.remove("open"); };

  /* ---- radar <-> EO swap (client-only view state) ---- */
  document.querySelectorAll("[data-swap]").forEach(function (b) {
    b.onclick = function (e) { e.stopPropagation(); $("stage").classList.toggle("swap"); };
  });

  /* ---- zoom ---- */
  var zoom = 1;
  function sendZoom(next) {
    if (next < 1 || next > ZOOMS.length) return;
    zoom = ZOOMS[next - 1] || zoom;
  }
  document.querySelectorAll("[data-zoom]").forEach(function (b) {
    b.onclick = function () {
      var i = ZOOMS.indexOf(zoom) + parseInt(b.dataset.zoom, 10);
      if (i < 0 || i >= ZOOMS.length) return;
      zoom = ZOOMS[i];
      ctl("zoom=" + zoom);
      $("v-zval").textContent = zoom.toFixed(1) + "×";
      $("v-zoom").textContent = zoom.toFixed(1) + "×";
    };
  });

  /* ---- mode (scan/track), mirrored on the top segmented + the big TRACK ---- */
  function setMode(m) { ctl("mode=" + m); reflectMode(m); }
  function reflectMode(m) {
    document.querySelectorAll("#mode button").forEach(function (b) {
      b.classList.toggle("on", b.dataset.mode === m);
    });
    $("trackbtn").classList.toggle("on", m === "track");
  }
  document.querySelectorAll("#mode button").forEach(function (b) {
    b.onclick = function () { setMode(b.dataset.mode); };
  });
  $("trackbtn").onclick = function () {
    setMode($("trackbtn").classList.contains("on") ? "scan" : "track");
  };

  /* ---- illuminator ---- */
  $("light").onclick = function () {
    var on = $("light").classList.contains("firing");
    if (!on && !confirm("Fire 850nm IR laser? (invisible, eye hazard)")) return;
    ctl("laser=" + (on ? 0 : 1));
  };
  $("autofov").onclick = function () { ctl("autofov=1"); };
  $("s-pow").oninput = function () {
    $("o-pow").textContent = this.value + "%";
    ctl("power=" + Math.round(this.value * 255 / 100));
  };
  $("s-fov").oninput = function () {
    $("o-fov").textContent = this.value + "°";
    ctl("fov=" + this.value);
  };

  /* ---- stream knobs ---- */
  $("s-fps").oninput = function () { $("o-fps").textContent = this.value; ctl("fps=" + this.value); };
  $("s-q").oninput   = function () { $("o-q").textContent = this.value;   ctl("q=" + this.value); };
  document.querySelectorAll("#presets button").forEach(function (b) {
    b.onclick = function () {
      document.querySelectorAll("#presets button").forEach(function (x) { x.classList.remove("on"); });
      b.classList.add("on");
      ctl("preset=" + b.dataset.preset);
    };
  });

  /* ---- reserved placeholders ---- */
  $("rec").onclick = function () { $("rec").classList.toggle("on"); };  /* reserved: needs recorder */
  /* restart/logs are reserved until wired to the systemd unit; harmless no-ops today */
  $("restart").onclick = function () { if (confirm("Restart AIRPOC service?")) ctl("restart=1"); };
  $("logs").onclick = function () { $("logs").textContent = "reserved"; setTimeout(function () { $("logs").textContent = "LOGS"; }, 900); };

  /* ---- ZULU clock (client-side UTC) ---- */
  function zulu() {
    var d = new Date();
    var p = function (n) { return (n < 10 ? "0" : "") + n; };
    $("c-zulu").textContent = p(d.getUTCHours()) + ":" + p(d.getUTCMinutes());
  }
  setInterval(zulu, 1000); zulu();

  /* ---- telemetry poll ---- */
  function num(v, dp, suf) { return (v === null || v === undefined) ? "—" : v.toFixed(dp) + (suf || ""); }
  function setSliderIfIdle(el, v) { if (document.activeElement !== el) el.value = v; }

  function poll() {
    fetch("/stats").then(function (r) { return r.json(); }).then(function (d) {
      $("c-link").textContent  = num(d.mbps, 1);
      $("v-mbps").textContent  = num(d.mbps, 1);
      $("v-encfps").textContent = num(d.fps, 0);
      $("v-srcfps").textContent = num(d.src_fps, 0);
      $("v-fov").textContent   = num(d.hfov, 0, "°");
      $("c-batt").textContent  = num(d.batt, 0, "%");
      $("c-alt").textContent   = num(d.alt, 0);
      $("v-brg").textContent   = d.brg === null ? "—" : num(d.brg, 0, "°");
      $("v-rng").textContent   = d.rng === null ? "—" : num(d.rng, 2, " km");
      $("v-tracks").textContent = d.tracks === null ? "no data" : d.tracks + " TRK";
      $("v-cpu").textContent   = num(d.cpu_c, 0);
      $("v-cam").textContent   = num(d.cam_c, 0);

      if (typeof d.zoom === "number" && ZOOMS.indexOf(d.zoom) >= 0) {
        zoom = d.zoom;
        $("v-zval").textContent = zoom.toFixed(1) + "×";
        $("v-zoom").textContent = zoom.toFixed(1) + "×";
      }
      if (d.mode) reflectMode(d.mode);

      /* illuminator truth */
      var light = $("light");
      light.classList.toggle("firing", !!d.laser);
      light.style.opacity = d.lpresent ? "1" : ".45";
      if (typeof d.lpower === "number") {
        setSliderIfIdle($("s-pow"), Math.round(d.lpower * 100 / 255));
        if (document.activeElement !== $("s-pow")) $("o-pow").textContent = Math.round(d.lpower * 100 / 255) + "%";
      }
      if (typeof d.lfov === "number") {
        setSliderIfIdle($("s-fov"), Math.round(d.lfov));
        if (document.activeElement !== $("s-fov")) $("o-fov").textContent = Math.round(d.lfov) + "°";
      }
    }).catch(function () { $("c-link").textContent = "—"; });
  }
  setInterval(poll, 160); poll();
})();
