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
#include <jpeglib.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char  *g_jpeg = NULL;      /* latest encoded frame */
static unsigned long   g_jpeg_len = 0;
static uint64_t        g_seq = 0;          /* increments per publish */
static volatile int    g_zoom = 1;         /* digital zoom 1/2/4/8 (set via /ctl) */

int mjpeg_zoom(void) { return g_zoom; }

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
"&nbsp;gain <button onclick=G(-30)>g-</button><button onclick=G(30)>g+</button>"
"&nbsp;exp <button onclick=E(0.77)>exp-</button><button onclick=E(1.3)>exp+</button>"
"&nbsp;auto-cap <button onclick=C(-20)>cap-</button><button onclick=C(20)>cap+</button>"
"&nbsp;<button onclick=M() id=mdb>median</button></div>"
"<div id=wrap><img src=/stream><div id=roi></div><div id=ov></div></div><script>"
"var foc=false,peak=0,lpow=64,lfov=70,S={};"
"function z(v){fetch('/ctl?zoom='+v)}"
"function A(){fetch('/ctl?ae='+(S.ae?0:1))}"
"function G(d){fetch('/ctl?gain='+Math.max(0,Math.min(480,(S.gain||0)+d)))}"
"function E(f){fetch('/ctl?expms='+Math.max(0.1,(S.exp_ms||16)*f).toFixed(2))}"
"function C(d){fetch('/ctl?gaincap='+Math.max(0,Math.min(480,(S.gaincap||120)+d)))}"
"function M(){fetch('/ctl?median='+(S.median?0:1))}"
"function L(v){if(v&&!confirm('Fire 850nm IR laser? (invisible, eye hazard)'))return;fetch('/ctl?laser='+v)}"
"function P(d){lpow=Math.max(0,Math.min(255,lpow+d));fetch('/ctl?power='+lpow)}"
"function F(dir){var st=lfov>25?5:1;lfov=Math.max(2,Math.min(70,Math.round(lfov+dir*st)));fetch('/ctl?fov='+lfov)}"
"function f(){foc=!foc;peak=0;document.getElementById('roi').style.display=foc?'block':'none';"
"document.getElementById('fb').className=foc?'on':''}"
"async function t(){try{let d=await(await fetch('/stats')).json();S=d;"
"var s='IMX296 Y10  disp '+d.fps.toFixed(0)+'fps  sensor '+d.sfps.toFixed(0)+'fps  mean='+d.mean+'/1023\\n'+"
"'exp='+d.exp_ms.toFixed(2)+'ms  duty='+d.duty_pct+'%  gain='+d.gain+'/480  '+(d.ae?'AUTO(cap '+d.gaincap+')':'MANUAL')+'\\n'+"
"'FOV '+d.hfov.toFixed(1)+'x'+d.vfov.toFixed(1)+'deg  zoom '+d.zoom+'x  median '+(d.median?'ON':'off');"
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
"document.getElementById('mdb').className=d.median?'on':''}catch(e){}}"
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

void mjpeg_publish(const uint8_t *gray, int w, int h)
{
    unsigned long len = 0;
    unsigned char *j = encode(gray, w, h, &len);
    if (!j) return;
    pthread_mutex_lock(&g_lock);
    free(g_jpeg);
    g_jpeg = j; g_jpeg_len = len; g_seq++;
    pthread_mutex_unlock(&g_lock);
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
        char body[560];
        int bl = snprintf(body, sizeof(body),
            "{\"fps\":%.1f,\"mean\":%.0f,\"exp_ms\":%.2f,\"duty_pct\":%.0f,\"gain\":%d,\"sfps\":%.1f,"
            "\"zoom\":%d,\"hfov\":%.2f,\"vfov\":%.2f,\"sharp\":%.0f,"
            "\"ae\":%d,\"gaincap\":%d,\"median\":%d,\"connected\":%d,"
            "\"laser\":%d,\"lpower\":%d,\"lfov\":%.1f,\"lpresent\":%d}\n",
            st.fps, st.mean, st.exp_ms, st.duty_pct, st.gain, st.sfps,
            z, hf, vf, st.focus,
            st.ae_on, st.gaincap, st.median, st.connected,
            lon, lpw, lfov, lpr);
        dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n%s", bl, body);
        close(fd);
        return NULL;
    }

    if (strncmp(req, "GET /ctl", 8) == 0) {
        char *q;
        if ((q = strstr(req, "zoom="))) { int v = atoi(q + 5); if (v==1||v==2||v==4||v==8) g_zoom = v; }
        if ((q = strstr(req, "laser=")))  illum_set_on(atoi(q + 6));      /* 0/1        */
        if ((q = strstr(req, "power=")))  illum_set_power(atoi(q + 6));   /* 0..255     */
        if ((q = strstr(req, "fov=")))    illum_set_fov(atof(q + 4));     /* 1.96..70   */
        /* exposure/gain override -> libeo bench API. Setting a manual value drops to
         * manual mode; ae=1 returns to auto. Exposure derives the frame length (fps). */
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
    if (pthread_create(&t, NULL, server, (void *)(long)port)) return -1;
    pthread_detach(t);
    return 0;
}
