/* AIRPOC operator console — the main process. A thin console that CONSUMES the sensor
 * modules' feeds and serves one integrated operator picture. It does no capture, no
 * ISP, no AE, no encode, no illuminator serial — each module owns its domain:
 *   EO module (eo/pipeline)  -> serves the MJPEG video + zoom/AE/illuminator on :8091
 *   radar module             -> serves the SSE point cloud + targets on :8092
 * The app proxies both, forwards operator controls, and adds the radar scope,
 * EO overlays, tracking selection, and styling. Feeds report NOT CONNECTED when down;
 * there is no synthetic data.
 *
 *   ./app [-p 8080] [-e 127.0.0.1:8091] [-r 127.0.0.1:8092]
 */
#include "eo_client.h"
#include "radar.h"
#include "gui.h"
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

int main(int argc, char **argv)
{
    const char *eo    = "127.0.0.1:8091";   /* EO module feed  */
    const char *rport = "127.0.0.1:8092";   /* radar daemon    */
    int port = 8080, opt;
    while ((opt = getopt(argc, argv, "p:e:r:")) != -1) {
        if      (opt == 'p') port = atoi(optarg);
        else if (opt == 'e') eo = optarg;
        else if (opt == 'r') rport = optarg;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    eo_start(eo);          /* consume the EO feed; NOT CONNECTED if down */
    radar_start(rport);    /* consume the radar daemon; NOT CONNECTED if down */

    if (gui_start(port) != 0) {
        fprintf(stderr, "app: GUI server failed to start\n");
        radar_stop(); eo_stop();
        return 1;
    }
    fprintf(stderr, "app: operator console http://0.0.0.0:%d/  (proxy: EO %s, radar %s)\n",
            port, eo, rport);

    while (g_run) pause();

    fprintf(stderr, "app: shutting down\n");
    gui_stop();
    radar_stop();
    eo_stop();
    return 0;
}
