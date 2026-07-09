/* AIRPOC launcher — a tiny always-on control service. It serves one mobile-friendly
 * page with START / STOP / OPEN-CONSOLE, and turns the whole stack on/off by running
 * start.sh / stop.sh (which supervise eo_pipeline + radar + app). It stays up on boot
 * (systemd) so you can flip the system on from any device's browser — no install.
 *
 *   ./airpoc-launcher [port]      (default 8088; runs start.sh/stop.sh from its own cwd)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static int port_up(int p)                       /* is something listening on 127.0.0.1:p ? */
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)p) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

/* Is the recorder's shared-memory tap for <chan> actually connected? A feed's TCP port
 * can be up (live view works) while its /dev/shm tap is gone — then recording captures
 * nothing. This asks the recorder (:8093) the truth so /status can't lie about it. */
static int rec_chan_up(const char *chan)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(8093) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return 0; }
    const char *rq = "GET /stats HTTP/1.0\r\nConnection: close\r\n\r\n";
    if (write(fd, rq, strlen(rq)) < 0) { close(fd); return 0; }
    char buf[4096]; size_t n = 0; ssize_t r;
    while (n < sizeof buf - 1 && (r = read(fd, buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)r;
    close(fd);
    buf[n] = 0;
    char key[64]; snprintf(key, sizeof key, "\"name\":\"%s\"", chan);
    char *p = strstr(buf, key);
    if (!p) return 0;
    p = strstr(p, "\"connected\":");
    return p && p[12] == '1';
}

static const char *WIFI_MODE_FILE   = "/var/lib/airpoc/wifi-mode";     /* we write: auto|ap|home */
static const char *WIFI_STATUS_FILE = "/var/lib/airpoc/wifi-status";   /* autoap writes live state */

static void set_wifi_mode(const char *m)
{
    FILE *f = fopen(WIFI_MODE_FILE, "w");
    if (f) { fputs(m, f); fputc('\n', f); fclose(f); }
}
static int read_file(const char *path, char *buf, size_t n)   /* -> bytes read, 0 on fail */
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return (int)r;
}

static const char *PAGE =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1,maximum-scale=1'>"
"<title>AIRPOC control</title><style>"
"*{box-sizing:border-box}body{margin:0;height:100vh;display:flex;flex-direction:column;align-items:center;"
"justify-content:center;gap:22px;background:#0a0a0c;color:#e9e5dd;font-family:ui-monospace,Menlo,Consolas,monospace}"
"h1{font-weight:200;letter-spacing:.5em;font-size:30px;margin:0;color:#e9e5dd}h1 i{font-style:normal;font-size:13px;color:#c1a173}"
".st{display:flex;align-items:center;gap:10px;font-size:15px;letter-spacing:1px;color:#9a968e}"
".dot{width:13px;height:13px;border-radius:50%;background:#5f5c56}.dot.up{background:#6cdf8a;box-shadow:0 0 10px #6cdf8a}"
".dot.part{background:#c1a173}"
"button{width:min(86vw,340px);height:64px;border:0;border-radius:8px;font:inherit;font-size:18px;letter-spacing:2px;cursor:pointer}"
".start{background:#6cdf8a;color:#0a0a0c}.stop{background:#5a1a1a;color:#ffb4b4;border:1px solid #7a2a2a}"
".open{background:#131316;color:#c1a173;border:1px solid #3b3b42}button:active{transform:scale(.985)}"
".sub{font-size:11px;color:#5f5c56;letter-spacing:1px}"
".wifi{display:flex;flex-direction:column;align-items:center;gap:8px;margin-top:8px}"
".wlabel{font-size:10px;letter-spacing:3px;color:#5f5c56}"
".wbtns{display:flex;border:1px solid #3b3b42;border-radius:8px;overflow:hidden}"
".wbtns button{width:auto;min-width:100px;height:46px;background:#131316;color:#9a968e;border:0;border-right:1px solid #3b3b42;border-radius:0;font-size:13px;letter-spacing:2px}"
".wbtns button:last-child{border-right:0}.wbtns button.on{background:#c1a173;color:#0a0a0c}"
".wsub{font-size:11px;color:#5f5c56;letter-spacing:1px;min-height:14px;max-width:min(86vw,340px);text-align:center}"
".wsub.switching{color:#c1a173;font-size:13px;line-height:1.5}"
".danger{height:48px;font-size:13px;margin-top:14px;background:#2a0c0c;color:#ff8a8a;border:1px solid #6a2020}"
".recw{font-size:12px;letter-spacing:.5px;color:#e0a94a;max-width:min(86vw,360px);text-align:center;line-height:1.4;min-height:0}</style></head><body>"
"<h1>FAZE <i>CONTROL</i></h1>"
"<div class=st><span id=dot class=dot></span><span id=stat>checking…</span></div>"
"<button class=start onclick=go('start')>START SYSTEM</button>"
"<button class=stop onclick=go('stop')>STOP SYSTEM</button>"
"<button class=open onclick=\"location.href='http://'+location.hostname+':8080/'\">OPEN CONSOLE</button>"
"<div class=sub id=sub></div>"
"<div class=recw id=recw></div>"
"<div class=wifi><div class=wlabel>NETWORK</div><div class=wbtns>"
"<button id=w-auto onclick=setwifi('auto')>AUTO</button>"
"<button id=w-home onclick=setwifi('home')>WIFI</button>"
"<button id=w-ap onclick=setwifi('ap')>AP</button>"
"</div><div class=wsub id=wsub></div></div>"
"<button class=danger onclick=shutdownJetson()>\\u23fb SHUTDOWN JETSON</button>"
"<script>"
"function poll(){fetch('/status').then(r=>r.json()).then(d=>{"
"var n=(d.app?1:0)+(d.eo?1:0)+(d.radar?1:0)+(d.det?1:0);var dot=document.getElementById('dot');"
"var bad=(d.eo&&!d.eo_rec)||(d.radar&&!d.radar_rec);"  /* feed live but recorder tap dead */
"dot.className='dot'+(n===4&&!bad?' up':n>0?' part':'');"
"document.getElementById('stat').textContent=n===0?'SYSTEM OFF':bad?'REC BUS DOWN':n===4?'SYSTEM UP':'STARTING\\u2026';"
"var fx=function(f,r){return f?(r?'\\u2713':'\\u2713 rec\\u2717'):'\\u2717';};"
"document.getElementById('sub').textContent='app '+(d.app?'\\u2713':'\\u2717')+'  \\u00b7  eo '+fx(d.eo,d.eo_rec)+'  \\u00b7  radar '+fx(d.radar,d.radar_rec)+'  \\u00b7  det '+(d.det?'\\u2713':'\\u2717');"
"document.getElementById('recw').textContent=bad?'\\u26a0 feed is live but the recorder tap is DOWN \\u2014 recordings will be EMPTY. Press START to re-attach.':'';"
"}).catch(()=>{document.getElementById('stat').textContent='launcher unreachable';});}"
"function go(a){document.getElementById('stat').textContent=(a==='start'?'STARTING':'STOPPING')+'\\u2026';"
"fetch('/'+a).then(()=>setTimeout(poll,1200));}"
"function shutdownJetson(){if(!confirm('Power OFF the Jetson? The whole system stops and you will need physical access to power it back on.'))return;document.getElementById('stat').textContent='SHUTTING DOWN\\u2026';document.getElementById('dot').className='dot';fetch('/shutdown');}"
"function setwifi(m){"
"[['auto','w-auto'],['home','w-home'],['ap','w-ap']].forEach(function(p){document.getElementById(p[1]).classList.toggle('on',p[0]===m);});"  /* instant: show the press registered */
"var t=m==='ap'?'Switching to AP\\u2026 ~10s. This page will drop \\u2014 then join the \\'AIRPOC\\' WiFi and open 10.42.0.1:8088':m==='home'?'Switching to home WiFi\\u2026 ~10s. Reconnect to your home network, then reopen this page':'Switching to AUTO\\u2026 ~10s';"
"var w=document.getElementById('wsub');w.textContent=t;w.classList.add('switching');"
"fetch('/wifi?mode='+m).then(()=>setTimeout(pollwifi,5000));}"
"function pollwifi(){fetch('/wifi/status').then(r=>r.json()).then(d=>{"
"[['auto','w-auto'],['home','w-home'],['ap','w-ap']].forEach(function(p){document.getElementById(p[1]).classList.toggle('on',d.mode===p[0]);});"
"var w=document.getElementById('wsub');w.classList.remove('switching');"
"w.textContent=d.ap?('AP \\u00b7 '+(d.ip||'10.42.0.1')+' \\u00b7 '+(d.clients||0)+' joined'):(d.net?(d.net+(d.ip?' \\u00b7 '+d.ip:'')):'\\u2014');"
"}).catch(()=>{});}"
"setInterval(poll,2000);poll();setInterval(pollwifi,3000);pollwifi();"
"</script></body></html>";

static void reply(int fd, const char *ctype, const char *body)
{
    dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nCache-Control: no-cache\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", ctype, strlen(body), body);
}
static void reply204(int fd)   /* what Android's captivity check wants: no internet != dead */
{
    dprintf(fd, "HTTP/1.0 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

static void handle_conn(int fd)
{
    char req[512];
    ssize_t n = read(fd, req, sizeof req - 1);
    if (n <= 0) return;
    req[n] = 0;

    /* Captive-portal / connectivity checks: make the phone treat this open AP as a real
     * network so it stops preferring cellular. Android wants a 204; Apple wants "Success". */
    if (strstr(req, "generate_204") || strstr(req, "gen_204")) {
        reply204(fd);
    } else if (strstr(req, "hotspot-detect") || strstr(req, "ncsi.txt") || strstr(req, "connecttest")) {
        reply(fd, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    } else if (!strncmp(req, "GET /status", 11)) {
        int eo = port_up(8091), rad = port_up(8092);
        char b[320];
        snprintf(b, sizeof b,
                 "{\"app\":%s,\"eo\":%s,\"radar\":%s,\"det\":%s,\"eo_rec\":%s,\"radar_rec\":%s}",
                 port_up(8080) ? "true" : "false", eo ? "true" : "false", rad ? "true" : "false",
                 port_up(8094) ? "true" : "false",
                 (eo && rec_chan_up("eo_y10")) ? "true" : "false",
                 (rad && rec_chan_up("radar_raw")) ? "true" : "false");
        reply(fd, "application/json", b);
    } else if (!strncmp(req, "GET /start", 10)) {
        if (system("bash ./start.sh >/tmp/airpoc-start.log 2>&1 &") == -1) {}
        reply(fd, "application/json", "{\"ok\":1}");
    } else if (!strncmp(req, "GET /stop", 9)) {
        if (system("bash ./stop.sh >/tmp/airpoc-stop.log 2>&1") == -1) {}
        reply(fd, "application/json", "{\"ok\":1}");
    } else if (!strncmp(req, "GET /reattach", 13)) {
        /* Re-bind the recorder's shm taps to the live feeds, on demand: the operator hits
         * REC, the console sees a recorder tap is DOWN (feed live but recorder detached),
         * and asks us to heal BEFORE recording so it never records 0 bytes. This runs the
         * SAME ordered heal as START (start.sh) — restart the producer whose tap is gone,
         * THEN bounce the recorder so it attaches to the fresh shm. A recorder-only restart
         * is NOT enough (and can orphan a working attach): the producers pin the old shm, so
         * the new recorder ring has no writer and stays connected=0. Verified on-device. */
        if (system("bash ./start.sh >/tmp/airpoc-reattach.log 2>&1 &") == -1) {}
        reply(fd, "application/json", "{\"ok\":1}");
    } else if (!strncmp(req, "GET /suspend", 12)) {
        /* Reviewing a recording needs no live sensors. FREEZE the producers (SIGSTOP —
         * instant, resumable, no camera/board re-init, shm mappings kept) so the native-mp4
         * transcode + smooth playback get the whole CPU/power budget and the box stops
         * browning out under the combined load. Recorder + console + launcher keep running.
         * Paired with /resume on replay close; SIGCONT is idempotent so /resume is safe to
         * call liberally. A STOP/START also clears a stuck-suspended state (start.sh sees the
         * frozen producer as unhealthy and relaunches it). */
        if (system("pkill -STOP -x eo_pipeline; pkill -STOP -x radar_preview; pkill -STOP -x detectiond") == -1) {}
        reply(fd, "application/json", "{\"ok\":1,\"live\":\"suspended\"}");
    } else if (!strncmp(req, "GET /resume", 11)) {
        if (system("pkill -CONT -x eo_pipeline; pkill -CONT -x radar_preview; pkill -CONT -x detectiond") == -1) {}
        reply(fd, "application/json", "{\"ok\":1,\"live\":\"running\"}");
    } else if (!strncmp(req, "GET /shutdown", 13)) {
        reply(fd, "application/json", "{\"ok\":1}");   /* answer first — the box is about to go down */
        if (system("sudo -n systemctl poweroff >/dev/null 2>&1 &") == -1) {}

    } else if (!strncmp(req, "GET /wifi/status", 16)) {
        char b[256];
        if (read_file(WIFI_STATUS_FILE, b, sizeof b) <= 0)
            snprintf(b, sizeof b, "{\"mode\":\"auto\",\"ap\":false,\"net\":\"\",\"ip\":\"\",\"clients\":0}");
        reply(fd, "application/json", b);
    } else if (!strncmp(req, "GET /wifi", 9)) {
        const char *m = strstr(req, "mode="), *set = "auto";
        if (m) { m += 5; if (!strncmp(m, "ap", 2)) set = "ap"; else if (!strncmp(m, "home", 4)) set = "home"; }
        set_wifi_mode(set);
        char b[64]; snprintf(b, sizeof b, "{\"ok\":1,\"mode\":\"%s\"}", set);
        reply(fd, "application/json", b);
    } else {
        reply(fd, "text/html", PAGE);
    }
}

static int make_listener(int port)   /* -> listening fd, or -1 */
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons((uint16_t)port) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    listen(s, 16);
    return s;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 8088;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);                   /* auto-reap start.sh/stop.sh */

    int sctl = make_listener(port);
    if (sctl < 0) { perror("bind control"); return 1; }
    int s80 = make_listener(80);                /* captive-portal helper; needs CAP_NET_BIND_SERVICE */
    fprintf(stderr, "airpoc-launcher: control http://0.0.0.0:%d/  captive-portal:%s\n",
            port, s80 >= 0 ? "on :80" : "off (no :80 bind)");

    struct pollfd pfds[2];
    pfds[0].fd = sctl; pfds[0].events = POLLIN;
    int nf = 1;
    if (s80 >= 0) { pfds[1].fd = s80; pfds[1].events = POLLIN; nf = 2; }

    for (;;) {
        if (poll(pfds, nf, -1) <= 0) continue;
        for (int i = 0; i < nf; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            int fd = accept(pfds[i].fd, NULL, NULL);
            if (fd < 0) continue;
            handle_conn(fd);
            close(fd);
        }
    }
    return 0;
}
