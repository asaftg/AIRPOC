/* AIRPOC application — the main process. Supervisor only: it starts the EO channel,
 * the (optional) illuminator, and the operator GUI, then waits for a signal. It does
 * NOT implement capture/AE/ISP (that is the EO channel) and it never touches frames
 * on the capture path — the GUI reads them read-only via eo_get_latest().
 *
 *   ./app [-d /dev/video0] [-p 8080] [-i /dev/sg-ir850]
 */
#include "eo_frame.h"
#include "radar.h"
#include "gui.h"
#include "illum.h"
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

int main(int argc, char **argv)
{
    const char *dev = "/dev/video0";
    const char *iport = "/dev/sg-ir850";   /* SG-IR850 illuminator (optional) */
    int port = 8080, opt;
    while ((opt = getopt(argc, argv, "d:p:i:")) != -1) {
        if      (opt == 'd') dev = optarg;
        else if (opt == 'p') port = atoi(optarg);
        else if (opt == 'i') iport = optarg;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);              /* dropped screens must not kill us */

    if (eo_start(dev) != 0) {
        fprintf(stderr, "app: EO channel failed to start\n");
        return 1;
    }
    illum_start(iport);                     /* optional; no-ops if absent */
    radar_start(NULL);                      /* synthetic until the AWR module lands */

    if (gui_start(port) != 0) {
        fprintf(stderr, "app: GUI server failed to start\n");
        radar_stop();
        eo_stop();
        return 1;
    }
    fprintf(stderr, "app: operator console at http://0.0.0.0:%d/\n", port);

    while (g_run) pause();

    fprintf(stderr, "app: shutting down\n");
    gui_stop();
    radar_stop();
    eo_stop();
    return 0;
}
