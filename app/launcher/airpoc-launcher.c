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
".wsub.switching{color:#c1a173;font-size:13px;line-height:1.5}</style></head><body>"
"<h1>FAZE <i>CONTROL</i></h1>"
"<div class=st><span id=dot class=dot></span><span id=stat>checking…</span></div>"
"<button class=start onclick=go('start')>START SYSTEM</button>"
"<button class=stop onclick=go('stop')>STOP SYSTEM</button>"
"<button class=open onclick=\"location.href='http://'+location.hostname+':8080/'\">OPEN CONSOLE</button>"
"<div class=sub id=sub></div>"
"<div class=wifi><div class=wlabel>NETWORK</div><div class=wbtns>"
"<button id=w-auto onclick=setwifi('auto')>AUTO</button>"
"<button id=w-home onclick=setwifi('home')>WIFI</button>"
"<button id=w-ap onclick=setwifi('ap')>AP</button>"
"</div><div class=wsub id=wsub></div></div>"
"<script>"
"function poll(){fetch('/status').then(r=>r.json()).then(d=>{"
"var n=(d.app?1:0)+(d.eo?1:0)+(d.radar?1:0);var dot=document.getElementById('dot');"
"dot.className='dot'+(n===3?' up':n>0?' part':'');"
"document.getElementById('stat').textContent=n===3?'SYSTEM UP':n===0?'SYSTEM OFF':'STARTING…';"
"document.getElementById('sub').textContent='app '+(d.app?'\\u2713':'\\u2717')+'  \\u00b7  eo '+(d.eo?'\\u2713':'\\u2717')+'  \\u00b7  radar '+(d.radar?'\\u2713':'\\u2717');"
"}).catch(()=>{document.getElementById('stat').textContent='launcher unreachable';});}"
"function go(a){document.getElementById('stat').textContent=(a==='start'?'STARTING':'STOPPING')+'\\u2026';"
"fetch('/'+a).then(()=>setTimeout(poll,1200));}"
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
        char b[128];
        snprintf(b, sizeof b, "{\"app\":%s,\"eo\":%s,\"radar\":%s}",
                 port_up(8080) ? "true" : "false", port_up(8091) ? "true" : "false", port_up(8092) ? "true" : "false");
        reply(fd, "application/json", b);
    } else if (!strncmp(req, "GET /start", 10)) {
        if (system("bash ./start.sh >/tmp/airpoc-start.log 2>&1 &") == -1) {}
        reply(fd, "application/json", "{\"ok\":1}");
    } else if (!strncmp(req, "GET /stop", 9)) {
        if (system("bash ./stop.sh >/tmp/airpoc-stop.log 2>&1") == -1) {}
        reply(fd, "application/json", "{\"ok\":1}");
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
