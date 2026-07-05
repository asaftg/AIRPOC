/* Operator console server — a thin PROXY. The app does no capture/ISP/AE/encode: it
 * relays the EO module's JPEG feed and the radar daemon's frames to the browser, and
 * forwards operator controls to each feed. It adds the console: radar scope + EO
 * overlays + tracking selection + styling (in the browser). No websockets.
 *
 *   /            embedded page (index.html/app.css/app.js)
 *   /stream      EO JPEG relayed as MJPEG multipart (fanned to every screen)
 *   /radar       radar daemon frame JSON, verbatim
 *   /stats       console state + the EO feed's /stats (nested "eo") + radar tracks
 *   /ctl         routed: track/engage -> local; radar_* -> daemon; rest -> EO feed
 */
#define _GNU_SOURCE
#include "gui.h"
#include "eo_client.h"
#include "radar.h"
#include "web_assets.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_run = 1;
static pthread_t g_srv_th;
static int       g_listen_fd = -1;

/* console state (not owned by a feed) */
static volatile int g_track_man = 0;      /* tracking select: 0 auto / 1 manual */
static volatile int g_engage    = -1;     /* engaged target tid, -1 = none       */
static volatile unsigned long long g_stream_bytes = 0;  /* total video bytes relayed → true Mb/s meter */
static char            g_wifi_if[32] = "";              /* WiFi iface name, or "" if none associated */
static volatile double g_rssi = 0, g_link_mbps = -1;    /* RSSI dBm + negotiated PHY rate (Mb/s) */
static pthread_t       g_net_th;
static int             g_net_ok = 0;

static double read_temp_c(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1.0;
    long milli = -1;
    if (fscanf(f, "%ld", &milli) != 1) milli = -1;
    fclose(f);
    return milli < 0 ? -1.0 : milli / 1000.0;
}

/* True link throughput: bytes actually relayed since the last /stats call over the elapsed
 * time. Honest (accounts for res/fps/zoom/JPEG), unlike frame-size x fps guessing. */
static double stream_mbps(void)
{
    static unsigned long long prev = 0; static double prevt = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    unsigned long long cur = g_stream_bytes;
    double mbps = (prevt > 0 && now > prevt) ? (double)(cur - prev) * 8.0 / ((now - prevt) * 1e6) : 0.0;
    prev = cur; prevt = now;
    return mbps;
}

/* --- link status: wired vs WiFi, RSSI, and the negotiated PHY rate (the ceiling) --- */
static int read_wifi(char *ifn, size_t n, double *rssi)
{
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) return 0;
    char l[256]; int got = 0;
    while (fgets(l, sizeof l, f)) {                         /* skip the two header rows */
        char name[32]; unsigned st; double q, lvl, noise;
        if (sscanf(l, " %31[^:]: %x %lf %lf %lf", name, &st, &q, &lvl, &noise) == 5) {
            snprintf(ifn, n, "%s", name); if (rssi) *rssi = lvl; got = 1; break;
        }
    }
    fclose(f); return got;
}
static double read_link_rate(const char *ifn)              /* negotiated tx bitrate, Mb/s */
{
    if (!ifn || !*ifn) return -1.0;
    char cmd[96]; snprintf(cmd, sizeof cmd, "iw dev %s link 2>/dev/null", ifn);
    FILE *p = popen(cmd, "r");
    if (!p) return -1.0;
    char l[256]; double r = -1.0;
    while (fgets(l, sizeof l, p)) { char *b = strstr(l, "tx bitrate:"); if (b) { r = atof(b + 11); break; } }
    pclose(p); return r;
}
/* refresh RSSI + rate ~1 Hz (popen(iw) is too heavy to run per /stats request) */
static void *net_poller(void *a)
{
    (void)a;
    while (g_run) {
        char ifn[32] = ""; double rssi = 0;
        if (read_wifi(ifn, sizeof ifn, &rssi)) {
            double lr = read_link_rate(ifn);
            snprintf(g_wifi_if, sizeof g_wifi_if, "%s", ifn); g_rssi = rssi; g_link_mbps = lr;
        } else { g_wifi_if[0] = 0; g_link_mbps = -1.0; }
        for (int i = 0; i < 10 && g_run; i++) usleep(100000);
    }
    return NULL;
}
/* which interface owns a local IPv4 — lets us tell the operator's actual path apart */
static void iface_for_ip(const char *ip, char *name, size_t len)
{
    name[0] = 0;
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) != 0) return;
    for (p = ifa; p; p = p->ifa_next) {
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET) {
            char b[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, b, sizeof b);
            if (!strcmp(b, ip)) { snprintf(name, len, "%s", p->ifa_name); break; }
        }
    }
    freeifaddrs(ifa);
}

/* ---------------------------------------------------------------- http -------- */
static int has(const char *req, const char *path)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "GET %s", path);
    return strncmp(req, buf, (size_t)n) == 0;
}

static void send_asset(int fd, const char *ctype, const unsigned char *body, unsigned len)
{
    dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: %s\r\n"
                "Cache-Control: no-cache, must-revalidate\r\n"
                "Content-Length: %u\r\nConnection: close\r\n\r\n", ctype, len);
    ssize_t wr = write(fd, body, len); (void)wr;
}

static void handle_stats(int fd)
{
    char eostats[512]; int en = eo_get_stats(eostats, sizeof eostats);
    int eoc = eo_connected();
    double mbps = stream_mbps();

    char tracks_s[16];
    if (radar_connected()) snprintf(tracks_s, sizeof tracks_s, "%d", radar_num_targets());
    else snprintf(tracks_s, sizeof tracks_s, "null");

    double cpu = read_temp_c("/sys/class/thermal/thermal_zone0/temp");
    char cpu_s[16]; if (cpu < 0) snprintf(cpu_s, sizeof cpu_s, "null"); else snprintf(cpu_s, sizeof cpu_s, "%.0f", cpu);

    /* link type from the interface the operator's browser actually connected through */
    char lip[INET_ADDRSTRLEN] = "", lif[32] = "";
    struct sockaddr_in sa; socklen_t sl = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0) inet_ntop(AF_INET, &sa.sin_addr, lip, sizeof lip);
    iface_for_ip(lip, lif, sizeof lif);
    const char *ltype = "wired"; char rssi_s[16] = "null", lrate_s[16] = "null";
    if (g_wifi_if[0] && !strcmp(lif, g_wifi_if)) {
        ltype = "wifi"; snprintf(rssi_s, sizeof rssi_s, "%.0f", g_rssi);
        if (g_link_mbps > 0) snprintf(lrate_s, sizeof lrate_s, "%.0f", g_link_mbps);
    } else if (!strncmp(lip, "192.168.55.", 11) || !strncmp(lif, "usb", 3) || !strncmp(lif, "l4tbr", 5)) {
        ltype = "usb";
    }

    char body[960];
    int bl = snprintf(body, sizeof body,
        "{\"eo_connected\":%d,\"mbps\":%.2f,\"track\":\"%s\",\"engage\":%d,"
        "\"tracks\":%s,\"cpu_c\":%s,\"link_type\":\"%s\",\"rssi_dbm\":%s,\"link_mbps\":%s,"
        "\"batt\":null,\"alt\":null,\"brg\":null,\"rng\":null,\"eo\":%s}\n",
        eoc, mbps, g_track_man ? "man" : "auto", g_engage,
        tracks_s, cpu_s, ltype, rssi_s, lrate_s,
        (en > 0 ? eostats : "null"));
    dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n%s", bl, body);
}

static void handle_radar(int fd)
{
    int cap = 131072;
    char *b = malloc(cap);
    if (!b) { close(fd); return; }
    int n = radar_get_frame_json(b, cap);
    if (n <= 0)
        n = snprintf(b, cap, "{\"connected\":false,\"num_points\":0,\"num_targets\":0,"
                             "\"points\":[],\"targets\":[]}\n");
    dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    ssize_t w = write(fd, b, n); (void)w;
    free(b);
}

/* the daemon's /stats (its 6 control values + fps/drops), for slider init + readback */
static void handle_rstats(int fd)
{
    char s[512]; int n = radar_get_stats(s, sizeof s);
    if (n <= 0) { n = snprintf(s, sizeof s, "{}"); }
    dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n%s", n, s);
}

static const char *EO_KEYS[] = { "zoom=", "laser=", "power=", "fov=", "ae=", "gain=",
                                 "expms=", "gaincap=", "median=", "fps=", "res=" };
/* the daemon's six live controls; the GUI sends them namespaced as radar_<key>= */
static const char *RADAR_KEYS[] = { "eps", "minpts", "speed", "snrmin", "fov", "doppler" };

static void handle_ctl(const char *req)
{
    const char *qs = strstr(req, "/ctl?");
    if (!qs) return;
    qs += 5;
    char query[256]; int i = 0;
    while (qs[i] && qs[i] != ' ' && qs[i] != '\r' && qs[i] != '\n' && i < (int)sizeof(query) - 1) { query[i] = qs[i]; i++; }
    query[i] = 0;

    char *p;
    if ((p = strstr(query, "track=")))  { g_track_man = (strncmp(p + 6, "man", 3) == 0); return; }
    if ((p = strstr(query, "engage="))) { g_engage = atoi(p + 7); return; }

    /* radar controls: strip the radar_ namespace and forward to the daemon's /ctl */
    char dq[256]; int dn = 0;
    for (unsigned k = 0; k < sizeof(RADAR_KEYS) / sizeof(RADAR_KEYS[0]); k++) {
        char pref[24]; int pl = snprintf(pref, sizeof pref, "radar_%s=", RADAR_KEYS[k]);
        char *rp = strstr(query, pref);
        if (!rp) continue;
        rp += pl;
        char val[24]; int vi = 0;
        while (rp[vi] && rp[vi] != '&' && vi < (int)sizeof(val) - 1) { val[vi] = rp[vi]; vi++; }
        val[vi] = 0;
        dn += snprintf(dq + dn, sizeof(dq) - (size_t)dn, "%s%s=%s", dn ? "&" : "", RADAR_KEYS[k], val);
    }
    if (dn > 0) { radar_ctl(dq); return; }

    for (unsigned k = 0; k < sizeof(EO_KEYS) / sizeof(EO_KEYS[0]); k++)
        if (strstr(query, EO_KEYS[k])) { eo_ctl(query); break; }   /* forward EO controls */
}

static void stream_mjpeg(int fd)
{
    const char *hdr = "HTTP/1.0 200 OK\r\n"
                      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
    if (write(fd, hdr, strlen(hdr)) < 0) return;
    int cap = 512 * 1024;
    unsigned char *buf = malloc(cap);
    if (!buf) return;
    uint64_t last = 0;
    while (g_run) {
        uint64_t seq = 0;
        int n = eo_get_jpeg(buf, cap, &seq);
        if (n <= 0 || seq == last) { usleep(5000); continue; }
        last = seq;
        char part[128];
        int pl = snprintf(part, sizeof part,
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", n);
        if (write(fd, part, pl) < 0 || write(fd, buf, n) < 0 || write(fd, "\r\n", 2) < 0) break;
        g_stream_bytes += (unsigned long long)(pl + n + 2);   /* true throughput meter */
    }
    free(buf);
}

static void *client(void *arg)
{
    int fd = (int)(long)arg;
    char req[1024];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = 0;

    if (has(req, "/rstats"))         handle_rstats(fd);
    else if (has(req, "/stats"))     handle_stats(fd);
    else if (has(req, "/radar"))     handle_radar(fd);
    else if (has(req, "/ctl")) {
        handle_ctl(req);
        const char *ok = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        ssize_t wr = write(fd, ok, strlen(ok)); (void)wr;
    }
    else if (has(req, "/stream"))    stream_mjpeg(fd);
    else if (has(req, "/app.css"))   send_asset(fd, "text/css", asset_app_css, asset_app_css_len);
    else if (has(req, "/app.js"))    send_asset(fd, "application/javascript", asset_app_js, asset_app_js_len);
    else                             send_asset(fd, "text/html", asset_index_html, asset_index_html_len);

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
                             .sin_port = htons((uint16_t)port) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("gui: bind"); return NULL; }
    listen(s, 16);
    g_listen_fd = s;
    while (g_run) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) { if (!g_run) break; continue; }
        pthread_t t;
        if (pthread_create(&t, NULL, client, (void *)(long)fd) == 0) pthread_detach(t);
        else close(fd);
    }
    close(s);
    return NULL;
}

int gui_start(int port)
{
    g_run = 1;
    if (pthread_create(&g_srv_th, NULL, server, (void *)(long)port) != 0) return -1;
    g_net_ok = (pthread_create(&g_net_th, NULL, net_poller, NULL) == 0);
    return 0;
}

void gui_stop(void)
{
    g_run = 0;
    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
    pthread_join(g_srv_th, NULL);
    if (g_net_ok) pthread_join(g_net_th, NULL);
}
