/* airpoc_recorder — record & replay daemon (:8093).
 *
 * Consumes: shm taps (airpoc.eo_y10 / eo_jpeg / radar_raw / radar_wire) and
 * the modules' documented /stats surfaces. Produces: AIREC v1 sessions under
 * /data/recordings and the HTTP surface the operator console proxies as /rec/.
 * Degrades gracefully: absent taps/NVMe never crash anything — they show in
 * /stats and rec=start refuses politely.
 *
 *   airpoc_recorder [-p 8093] [-r /data/recordings]
 */
#include "recorder.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "p:r:h")) != -1) {
        if (opt == 'p') g_rec.port = atoi(optarg);
        else if (opt == 'r') snprintf(g_rec.root, sizeof g_rec.root, "%s", optarg);
        else {
            fprintf(stderr, "usage: airpoc_recorder [-p port] [-r recordings_root]\n");
            return 1;
        }
    }

    if (pack10_selftest() != 0) {
        fprintf(stderr, "rec: pack10 selftest FAILED\n");
        return 1;
    }
    if (render_selftest() != 0) {
        fprintf(stderr, "rec: render selftest FAILED\n");
        return 1;
    }

    if (disk_present(g_rec.root) || !access(g_rec.root, F_OK)) {
        /* root exists (or its volume is mounted): ensure the directory */
        mkdir(g_rec.root, 0755);
    }

    if (chan_init_all() != 0) {
        fprintf(stderr, "rec: channel init failed (memory?)\n");
        return 1;
    }
    session_recover_all();
    transcode_cleanup_tmp();          /* clear encode temps orphaned by a crash/kill */
    session_guard_start();
    events_start();
    transcode_autobuild_start();   /* fills in missing HD movies while idle */

    if (httpd_start(g_rec.port) != 0) return 1;

    fprintf(stderr, "airpoc_recorder: :%d root=%s disk=%s free=%.0fGB\n",
            g_rec.port, g_rec.root,
            disk_present(g_rec.root) ? "present" : "ABSENT",
            disk_free_gb(g_rec.root));

    for (;;) pause();
    return 0;
}
