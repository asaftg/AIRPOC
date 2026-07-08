/* capture_calib.c — grab N raw Y10 frames from the EO tap into a directory, for
 * INT8 calibration. The frames are the exact bytes the runtime preprocesses, so
 * the calibrator sees the real input distribution. Spread over time for variety.
 *
 *   ./capture_calib [N=200] [dir=/data/detection/calib] [tap=airpoc.eo_y10]
 */
#define _GNU_SOURCE
#include "airpoc_tap.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200;
    const char *dir = argc > 2 ? argv[2] : "/data/detection/calib";
    const char *tap = argc > 3 ? argv[3] : EO_TAP_NAME;

    AirTapSub s;
    int opened = 0;
    for (int i = 0; i < 50 && !opened; i++) {
        if (tap_open(&s, tap) == 0) opened = 1; else usleep(100000);
    }
    if (!opened) { fprintf(stderr, "capture_calib: tap %s not available\n", tap); return 1; }

    static uint8_t buf[EO_FRAME_BYTES];
    AirTapRec r;
    int got = 0;
    char path[512];
    while (got < n) {
        if (tap_read(&s, buf, (uint32_t)sizeof buf, &r) == 1) {
            snprintf(path, sizeof path, "%s/frame_%04d.y10", dir, got);
            FILE *f = fopen(path, "wb");
            if (!f) { fprintf(stderr, "capture_calib: cannot write %s\n", path); return 1; }
            fwrite(buf, 1, EO_FRAME_BYTES, f);
            fclose(f);
            got++;
            usleep(40000);   /* ~every few frames, for temporal variety */
        } else {
            usleep(5000);
        }
    }
    printf("capture_calib: wrote %d frames to %s\n", got, dir);
    tap_close(&s);
    return 0;
}
