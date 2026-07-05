/* Minimal MJPEG-over-HTTP monitor, a drop-in for the bench preview's :8091 feed.
 * The latest processed frame is JPEG-encoded (libjpeg-turbo) and streamed to any
 * number of HTTP clients. This is an operator/monitor convenience — the detector
 * consumes frames in-process, not over HTTP. */
#define _GNU_SOURCE
#include "pipeline.h"
#include "eo.h"
#include "eo_bench.h"
#include "illum.h"
#include <arpa/inet.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <jpeglib.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char  *g_jpeg = NULL;      /* latest encoded frame */
static unsigned long   g_jpeg_len = 0;
static uint64_t        g_seq = 0;          /* increments per publish */
static volatile int    g_zoom = 1;         /* digital zoom 1/2/4/8 (set via /ctl) */

/* Pipeline accounting — raw counters, so a stall shows WHERE frames vanish:
 *   prod = mjpeg_publish calls (frames the producer delivered)
 *   drop = producer frames skipped because the pool had no free slot
 *   pub  = g_seq (frames actually encoded + published)
 * Wire fps is computed from pub-count over a real time window (>=0.5 s), not from
 * inter-arrival EMA — burstiness can't bias a count. */
static uint64_t        g_ctr_prod = 0, g_ctr_drop = 0;   /* under E.lock */
static double          g_wire_fps = 0.0;                 /* under g_lock */
static double          g_win_t0   = 0.0;                 /* fps window start   */
static uint64_t        g_win_pub0 = 0;                   /* pub count at start */

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---- parallel encode pool ----------------------------------------------------
 * HARD REQUIREMENT: 60 fps on the wire at EVERY res/median setting. A single thread
 * cannot JPEG a native 1440x1080 frame in <16.7 ms, so frames round-robin across
 * ENC_WORKERS workers (each gets ENC_WORKERS frame-times per frame) and are published
 * strictly in sequence order. Median (when on) runs in the worker too, so it
 * parallelizes the same way instead of eating the producer's frame budget. */
#define ENC_WORKERS 3
#define ENC_SLOTS   4
static struct {
    pthread_mutex_t lock;
    pthread_cond_t  work;                  /* a slot became ready              */
    pthread_cond_t  done;                  /* a publish completed (order gate) */
    struct { uint8_t *buf; int w, h; uint64_t seq; int state; } s[ENC_SLOTS];
    uint64_t enq_seq;                      /* seq for the next enqueued frame  */
    uint64_t pub_seq;                      /* seq allowed to publish next      */
} E = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
        PTHREAD_COND_INITIALIZER, {{0}}, 0, 0 };

/* Operator-selectable display size — all 4:3 so the GUI's video box never changes.
 * Detection is unaffected (the detector keeps the full-native frame in libeo). */
static const struct { const char *name; int w, h; } RES[4] = {
    { "low",    640, 480 }, { "med",   960, 720 },
    { "high",  1280, 960 }, { "native", 1440, 1080 },
};
static volatile int g_res = 1;             /* med default */

int mjpeg_zoom(void) { return g_zoom; }
void mjpeg_res_dims(int *w, int *h) { int r = g_res; if (w) *w = RES[r].w; if (h) *h = RES[r].h; }
const char *mjpeg_res_name(void) { return RES[g_res].name; }

/* Full-screen video with a live stats overlay (polled from /stats) + zoom buttons. */
static const char *PAGE =
"<!DOCTYPE html><html><head><title>AIRPOC EO</title><meta charset=utf-8><style>"
"body{background:#111;margin:0;font-family:monospace;text-align:center}"
"#wrap{position:relative;display:inline-block}"
"img{max-width:100vw;max-height:100vh;display:block}"
"#ov{position:absolute;left:10px;bottom:10px;color:#0f0;font-size:16px;line-height:1.4;"
"white-space:pre;text-shadow:0 0 3px #000,0 0 3px #000,0 0 3px #000}"
"#roi{position:absolute;left:30%;top:30%;width:40%;height:40%;border:2px solid #0f0;"
"box-sizing:border-box;display:none;pointer-events:none}"
"#bar{position:fixed;top:8px;left:8px;color:#6f6;z-index:2}"
"#bar2{position:fixed;top:44px;left:8px;color:#6f6;z-index:2}"
"button{background:#222;color:#0f0;border:1px solid #0a0;padding:5px 9px;cursor:pointer}"
"button.on{background:#0a0;color:#000}#lon.hot{background:#f00;color:#fff;border-color:#f00}"
"#ls{margin-left:6px}</style></head><body>"
"<div id=bar>zoom "
"<button onclick=z(1) id=z1>1x</button><button onclick=z(2) id=z2>2x</button>"
"<button onclick=z(4) id=z4>4x</button><button onclick=z(8) id=z8>8x</button>"
"&nbsp;&nbsp;<button onclick=f() id=fb>focus</button>"
"&nbsp;&nbsp;<span id=ls>ILLUM</span> "
"<button onclick=L(1) id=lon>ON</button><button onclick=L(0) id=loff>OFF</button> "
"beam <button onclick=P(-32)>pow-</button><button onclick=P(32)>pow+</button> "
"<button onclick=F(-1)>fov-</button><button onclick=F(1)>fov+</button></div>"
"<div id=bar2>exposure <button onclick=A() id=aeb>AUTO</button>"
"&nbsp;fps <button onclick=FP(-1)>fps-</button><button onclick=FP(1)>fps+</button>"
"&nbsp;gain <button onclick=G(-30)>g-</button><button onclick=G(30)>g+</button>"
"&nbsp;exp <button onclick=E(0.77)>exp-</button><button onclick=E(1.3)>exp+</button>"
"&nbsp;auto-cap <button onclick=C(-20)>cap-</button><button onclick=C(20)>cap+</button>"
"&nbsp;<button onclick=M() id=mdb>median</button>"
"&nbsp;&nbsp;quality <button onclick=R('low') id=rlow>480</button>"
"<button onclick=R('med') id=rmed>720</button><button onclick=R('high') id=rhigh>960</button>"
"<button onclick=R('native') id=rnative>1080</button></div>"
"<div id=wrap><img src=/stream><div id=roi></div><div id=ov></div></div><script>"
"var foc=false,peak=0,lpow=64,lfov=70,S={};"
"function z(v){fetch('/ctl?zoom='+v)}"
"function R(r){fetch('/ctl?res='+r)}"
"function A(){fetch('/ctl?ae='+(S.ae?0:1))}"
"function G(d){fetch('/ctl?gain='+Math.max(0,Math.min(480,(S.gain||0)+d)))}"
"function E(f){fetch('/ctl?expms='+Math.max(0.1,(S.exp_ms||16)*f).toFixed(2))}"
"function C(d){fetch('/ctl?gaincap='+Math.max(0,Math.min(480,(S.gaincap||120)+d)))}"
"function M(){fetch('/ctl?median='+(S.median?0:1))}"
"function FP(d){var f=[12,15,20,24,30,48,60],c=Math.round(S.sfps||60);"
"var i=f.reduce((b,v,k)=>Math.abs(v-c)<Math.abs(f[b]-c)?k:b,0);"
"i=Math.max(0,Math.min(f.length-1,i+d));fetch('/ctl?fps='+f[i])}"
"function L(v){if(v&&!confirm('Fire 850nm IR laser? (invisible, eye hazard)'))return;fetch('/ctl?laser='+v)}"
"function P(d){lpow=Math.max(0,Math.min(255,lpow+d));fetch('/ctl?power='+lpow)}"
"function F(dir){var st=lfov>25?5:1;lfov=Math.max(2,Math.min(70,Math.round(lfov+dir*st)));fetch('/ctl?fov='+lfov)}"
"function f(){foc=!foc;peak=0;document.getElementById('roi').style.display=foc?'block':'none';"
"document.getElementById('fb').className=foc?'on':''}"
"async function t(){try{let d=await(await fetch('/stats')).json();S=d;"
"var s='IMX296 Y10  fps '+d.sfps.toFixed(0)+' (fixed, caps exp)  mean='+d.mean+'/1023\\n'+"
"'exp='+d.exp_ms.toFixed(2)+'ms  duty='+d.duty_pct+'%  gain='+d.gain+'/480  '+(d.ae?'AUTO(cap '+d.gaincap+')':'MANUAL')+'\\n'+"
"'FOV '+d.hfov.toFixed(1)+'x'+d.vfov.toFixed(1)+'deg  zoom '+d.zoom+'x  median '+(d.median?'ON':'off')+'\\n'+"
"'stream '+d.res+' '+d.dw+'x'+d.dh+' @'+d.sfps.toFixed(0)+'fps'+"
"(d.eff_w<d.dw?'  (real detail '+d.eff_w+'x'+d.eff_h+' — sensor-limited at '+d.zoom+'x)':'')+"
"'  (detector: 1440x1088 native)';"
"if(foc){if(d.sharp>peak)peak=d.sharp;var p=peak>0?Math.round(100*d.sharp/peak):0;"
"s+='\\nFOCUS  '+Math.round(d.sharp)+'  peak '+Math.round(peak)+'  '+p+'%  (turn ring to max)';}"
"if(d.laser)s+='\\n** LASER ON  pow '+d.lpower+'/255  beam '+d.lfov.toFixed(0)+'deg **';"
"document.getElementById('ov').textContent=s;"
"lpow=d.lpower;lfov=d.lfov;"
"document.getElementById('lon').className=d.laser?'hot':'';"
"var ls=document.getElementById('ls');"
"if(!d.lpresent){ls.textContent='ILLUM(none)';ls.style.color='#666'}"
"else{ls.textContent=d.laser?'LASER ON':'ILLUM';ls.style.color=d.laser?'#f00':'#6f6'}"
"[1,2,4,8].forEach(i=>document.getElementById('z'+i).className=i==d.zoom?'on':'');"
"document.getElementById('aeb').textContent=d.ae?'AUTO':'MANUAL';"
"document.getElementById('aeb').className=d.ae?'on':'';"
"document.getElementById('mdb').className=d.median?'on':'';"
"['low','med','high','native'].forEach(r=>document.getElementById('r'+r).className=r==d.res?'on':'')}catch(e){}}"
"setInterval(t,150);t();</script></body></html>";

static unsigned char *encode(const uint8_t *gray, int w, int h, unsigned long *len)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr jerr;
    unsigned char *out = NULL; unsigned long sz = 0;
    c.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &out, &sz);
    c.image_width = w; c.image_height = h;
    c.input_components = 1; c.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, EO_FEED_QUALITY, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < (JDIMENSION)h) {
        JSAMPROW row = (JSAMPROW)(gray + (size_t)c.next_scanline * w);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    *len = sz;
    return out;                            /* caller frees */
}

/* Producer side: copy the finished display frame into a free slot and signal a worker.
 * Never blocks the producer for the encode; the copy (<=1.5 MB) is ~0.2 ms. */
void mjpeg_publish(const uint8_t *gray, int w, int h)
{
    pthread_mutex_lock(&E.lock);
    g_ctr_prod++;
    int fi = -1;
    for (int i = 0; i < ENC_SLOTS; i++) if (E.s[i].state == 0) { fi = i; break; }
    if (fi < 0) { g_ctr_drop++; pthread_mutex_unlock(&E.lock); return; }  /* pool full */
    if (!E.s[fi].buf) E.s[fi].buf = malloc((size_t)EO_WIDTH * EO_HEIGHT);
    if (!E.s[fi].buf) { g_ctr_drop++; pthread_mutex_unlock(&E.lock); return; }
    memcpy(E.s[fi].buf, gray, (size_t)w * h);
    E.s[fi].w = w; E.s[fi].h = h;
    E.s[fi].seq = E.enq_seq++;
    E.s[fi].state = 1;                                       /* ready */
    pthread_cond_signal(&E.work);
    pthread_mutex_unlock(&E.lock);
}

/* Encode worker: take the oldest ready slot, median (if on) + JPEG it, then publish
 * strictly in sequence order (the gate keeps the MJPEG stream monotonic). */
static void *enc_worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&E.lock);
        int mi = -1;
        for (;;) {
            uint64_t best = 0;
            mi = -1;
            for (int i = 0; i < ENC_SLOTS; i++)
                if (E.s[i].state == 1 && (mi < 0 || E.s[i].seq < best)) { mi = i; best = E.s[i].seq; }
            if (mi >= 0) break;
            pthread_cond_wait(&E.work, &E.lock);
        }
        E.s[mi].state = 2;                                   /* busy */
        uint64_t myseq = E.s[mi].seq;
        int w = E.s[mi].w, h = E.s[mi].h;
        uint8_t *buf = E.s[mi].buf;
        pthread_mutex_unlock(&E.lock);

        if (eo_median_on()) isp_median3(buf, w, h);          /* parallelizes with encode */
        unsigned long len = 0;
        unsigned char *j = encode(buf, w, h, &len);

        pthread_mutex_lock(&E.lock);                         /* in-order publish gate */
        while (E.pub_seq != myseq) pthread_cond_wait(&E.done, &E.lock);
        pthread_mutex_unlock(&E.lock);

        if (j) {
            pthread_mutex_lock(&g_lock);
            free(g_jpeg);
            g_jpeg = j; g_jpeg_len = len; g_seq++;
            /* count-based wire fps over a >=0.5 s window — bursts can't bias a count */
            double t = now_s();
            if (g_win_t0 <= 0.0) { g_win_t0 = t; g_win_pub0 = g_seq; }
            else if (t - g_win_t0 >= 0.5) {
                g_wire_fps = (double)(g_seq - g_win_pub0) / (t - g_win_t0);
                g_win_t0 = t; g_win_pub0 = g_seq;
            }
            pthread_mutex_unlock(&g_lock);
        }

        pthread_mutex_lock(&E.lock);
        E.s[mi].state = 0;                                   /* slot free again */
        E.pub_seq = myseq + 1;
        pthread_cond_broadcast(&E.done);                     /* wake order-waiters */
        pthread_cond_signal(&E.work);
        pthread_mutex_unlock(&E.lock);
    }
    return NULL;
}

static void *client(void *arg)
{
    int fd = (int)(long)arg;
    char req[1024];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = 0;

    if (strncmp(req, "GET /stats", 10) == 0) {
        EoStats st; eo_stats(&st);
        int z = g_zoom;
        double fmm = eo_focal_mm(), pum = eo_pixel_um();
        double hf = 2 * atan((EO_WIDTH  * pum / 1000.0 / z) / (2 * fmm)) * 180.0 / M_PI;
        double vf = 2 * atan((EO_HEIGHT * pum / 1000.0 / z) / (2 * fmm)) * 180.0 / M_PI;
        int lon, lpw, lpr; double lfov;      /* cached illuminator state (no serial here) */
        illum_snapshot(&lon, &lpw, &lfov, &lpr);
        int dw, dh; mjpeg_res_dims(&dw, &dh);
        /* Effective resolution = the REAL detail the operator sees = min(display size,
         * sensor crop). Zoom crops the native sensor to 1440/z wide (1080/z high, 4:3),
         * so beyond that the display size is just upscaling. At high zoom eff_w collapses
         * to the crop regardless of the res button — that's sensor-limited, not a bug. */
        int cw = EO_WIDTH / z, ch = (EO_WIDTH * 3 / 4) / z;   /* native 4:3 crop at this zoom */
        int eff_w = dw < cw ? dw : cw, eff_h = dh < ch ? dh : ch;
        pthread_mutex_lock(&E.lock);
        uint64_t c_prod = g_ctr_prod, c_drop = g_ctr_drop;
        pthread_mutex_unlock(&E.lock);
        pthread_mutex_lock(&g_lock);
        double wfps = g_wire_fps; uint64_t c_pub = g_seq;
        pthread_mutex_unlock(&g_lock);
        char body[760];
        int bl = snprintf(body, sizeof(body),
            "{\"fps\":%.1f,\"mean\":%.0f,\"exp_ms\":%.2f,\"duty_pct\":%.0f,\"gain\":%d,"
            "\"sfps\":%.1f,\"fps_cap\":%.0f,"
            "\"prod\":%llu,\"drop\":%llu,\"pub\":%llu,"
            "\"zoom\":%d,\"hfov\":%.2f,\"vfov\":%.2f,\"sharp\":%.0f,"
            "\"ae\":%d,\"gaincap\":%d,\"median\":%d,\"connected\":%d,"
            "\"res\":\"%s\",\"dw\":%d,\"dh\":%d,\"eff_w\":%d,\"eff_h\":%d,"
            "\"laser\":%d,\"lpower\":%d,\"lfov\":%.1f,\"lpresent\":%d}\n",
            wfps, st.mean, st.exp_ms, st.duty_pct, st.gain,
            st.sfps, st.sfps,
            (unsigned long long)c_prod, (unsigned long long)c_drop, (unsigned long long)c_pub,
            z, hf, vf, st.focus,
            st.ae_on, st.gaincap, st.median, st.connected,
            mjpeg_res_name(), dw, dh, eff_w, eff_h,
            lon, lpw, lfov, lpr);
        dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n%s", bl, body);
        close(fd);
        return NULL;
    }

    if (strncmp(req, "GET /ctl", 8) == 0) {
        char *q;
        if ((q = strstr(req, "zoom="))) { int v = atoi(q + 5); if (v==1||v==2||v==4||v==8) g_zoom = v; }
        if ((q = strstr(req, "res="))) {                                 /* display size */
            char *r = q + 4;
            if (!strncmp(r, "low", 3))         g_res = 0;
            else if (!strncmp(r, "med", 3))    g_res = 1;
            else if (!strncmp(r, "high", 4))   g_res = 2;
            else if (!strncmp(r, "native", 6)) g_res = 3;
        }
        if ((q = strstr(req, "laser=")))  illum_set_on(atoi(q + 6));      /* 0/1        */
        if ((q = strstr(req, "power=")))  illum_set_power(atoi(q + 6));   /* 0..255     */
        if ((q = strstr(req, "fov=")))    illum_set_fov(atof(q + 4));     /* 1.96..70   */
        /* exposure/gain override -> libeo bench API. fps is the FIXED operating rate
         * that caps exposure (AE never changes it); a manual gain/exp drops to manual. */
        if ((q = strstr(req, "fps=")))     eo_set_fps(atof(q + 4));
        if ((q = strstr(req, "ae=")))      eo_set_ae(atoi(q + 3));
        if ((q = strstr(req, "gain=")))    eo_set_gain(atoi(q + 5));
        if ((q = strstr(req, "expms=")))   eo_set_expms(atof(q + 6));
        if ((q = strstr(req, "gaincap="))) eo_set_gaincap(atoi(q + 8));
        if ((q = strstr(req, "median=")))  eo_set_median(atoi(q + 7));
        const char *ok = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        ssize_t wr = write(fd, ok, strlen(ok)); (void)wr; close(fd); return NULL;
    }

    if (strncmp(req, "GET /stream", 11) != 0) {   /* "/" and anything else: the page */
        dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                    "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(PAGE), PAGE);
        close(fd); return NULL;
    }

    /* /stream: MJPEG multipart */
    const char *hdr = "HTTP/1.0 200 OK\r\n"
                      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
    if (write(fd, hdr, strlen(hdr)) < 0) { close(fd); return NULL; }

    uint64_t last = 0;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        if (g_seq == last || !g_jpeg) { pthread_mutex_unlock(&g_lock); usleep(2000); continue; }
        last = g_seq;
        unsigned long len = g_jpeg_len;
        unsigned char *copy = malloc(len);
        if (copy) memcpy(copy, g_jpeg, len);
        pthread_mutex_unlock(&g_lock);
        if (!copy) break;

        char part[128];
        int pl = snprintf(part, sizeof(part),
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n", len);
        if (write(fd, part, pl) < 0 || write(fd, copy, len) < 0 || write(fd, "\r\n", 2) < 0) {
            free(copy); break;
        }
        free(copy);
    }
    close(fd);
    return NULL;
}

static void *server(void *arg)
{
    int port = (int)(long)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons(port) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("mjpeg: bind"); return NULL; }
    listen(s, 8);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) continue;
        pthread_t t;
        pthread_create(&t, NULL, client, (void *)(long)fd);
        pthread_detach(t);
    }
    return NULL;
}

int mjpeg_start(int port)
{
    pthread_t t;
    for (int i = 0; i < ENC_WORKERS; i++) {              /* the 60fps-guarantee pool */
        if (pthread_create(&t, NULL, enc_worker, NULL)) return -1;
        pthread_detach(t);
    }
    if (pthread_create(&t, NULL, server, (void *)(long)port)) return -1;
    pthread_detach(t);
    return 0;
}
