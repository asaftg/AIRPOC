/* det_to_trk.c - bench tool: run a recorded det_wire JSONL through the REAL
 * eotrack core and emit the trk wire it would have published, as JSONL.
 * This is how the fixture trk_wire is produced for recordings made before the
 * EO tracker shipped: real tracker code on real detector data.
 *
 * Build (from fusion/): needs the eotrack sources next to this repo checkout:
 *   gcc -O3 -Wall -std=c11 -I../eotrack/src tools/det_to_trk.c \
 *       ../eotrack/src/core.c ../eotrack/src/emit.c ../eotrack/src/det_feed.c \
 *       -o tools/det_to_trk -lm -lpthread
 * usage: det_to_trk <det_wire.jsonl >trk_wire.jsonl
 */
#define _GNU_SOURCE
#include "core.h"
#include "config.h"
#include "emit.h"
#include "det_feed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    static char line[512 * 1024];
    static char out[256 * 1024];
    TrkCore *core = trk_core_new();
    if (!core) return 1;
    uint64_t last_pub = 0;
    uint64_t frame_id = 0;
    while (fgets(line, sizeof line, stdin)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        uint64_t t_pub = strtoull(line, NULL, 10);
        const char *json = sp + 1;

        TrkDet dets[TRK_MAX_IN];
        DetMeta m; memset(&m, 0, sizeof m);
        int n = det_parse(json, dets, TRK_MAX_IN, &m);
        double dt = 1.0 / TRK_GATE_REF_FPS;
        if (last_pub && t_pub > last_pub) dt = (double)(t_pub - last_pub) / 1e9;
        last_pub = t_pub;
        frame_id = m.frame_id ? m.frame_id : frame_id + 1;

        TrkOut to[TRK_MAX_TRACKS];
        int no = trk_core_step(core, dets, n, m.t_src_ns, dt, 0, 0, -1,
                               to, TRK_MAX_TRACKS);
        TrkHdr h = {
            .frame_id = frame_id,
            .t_src_ns = m.t_src_ns, .t_pub_ns = t_pub, .t_out_ns = t_pub,
            .connected = 1, .mode = "stare", .engaged = -1,
            .img_w = EO_IMG_W, .img_h = EO_IMG_H,
            .ifov_rad = m.have_ifov && m.ifov_rad > 0 ? m.ifov_rad : EO_IFOV_RAD_DEFAULT,
        };
        size_t len = trk_frame_json(out, sizeof out, &h, to, no);
        if (len > 0) printf("%llu %s\n", (unsigned long long)t_pub, out);
    }
    return 0;
}
