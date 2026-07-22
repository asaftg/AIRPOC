/* wire_replay.c - tier-2 gates: feed recorded radar_wire + trk_wire JSONL
 * (lines "<t_pub_ns> <json>") through the REAL parsers + core + emitter and
 * check the wire invariants on every emitted frame. Exits nonzero on failure.
 *
 * usage: wire_replay <radar_wire.jsonl> <trk_wire.jsonl>
 */
#define _GNU_SOURCE
#include "../src/core.h"
#include "../src/config.h"
#include "../src/emit.h"
#include "../src/rad_parse.h"
#include "../src/trk_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { FILE *f; char *line; size_t cap; uint64_t t; int eof; } Src;

static int src_next(Src *s)
{
    if (s->eof) return 0;
    ssize_t r = getline(&s->line, &s->cap, s->f);
    if (r <= 0) { s->eof = 1; return 0; }
    char *sp = strchr(s->line, ' ');
    if (!sp) return src_next(s);
    *sp = 0;
    s->t = strtoull(s->line, NULL, 10);
    memmove(s->line, sp + 1, strlen(sp + 1) + 1);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s radar.jsonl trk.jsonl\n", argv[0]); return 2; }
    Src rs = { .f = fopen(argv[1], "r") }, es = { .f = fopen(argv[2], "r") };
    if (!rs.f || !es.f) { fprintf(stderr, "wire_replay: cannot open inputs\n"); return 2; }

    FusCore *core = fus_core_new();
    /* fixtures predate a measured mount trim - association gates are what we
     * exercise here; run with the shipped defaults */
    FusRadTgt rt[FUS_MAX_RAD]; FusEoTrk et[FUS_MAX_EO]; FusOut rows[FUS_MAX_OUT];
    static char json[256 * 1024];

    int have_r = src_next(&rs), have_e = src_next(&es);
    if (!have_r || !have_e) { fprintf(stderr, "wire_replay: empty input\n"); return 2; }

    long frames = 0, viol = 0, fused_frames = 0, rows_total = 0;
    int max_fused = 0;
    double proc_us_max = 0, proc_us_sum = 0;
    long over_budget = 0;
    uint64_t out_seq = 0;

    while (have_r || have_e) {
        int take_r = have_r && (!have_e || rs.t <= es.t);
        int n;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        if (take_r) {
            RadMeta m;
            int nr = rad_parse(rs.line, rt, FUS_MAX_RAD, &m);
            n = fus_core_step_rad(core, rt, nr, rs.t, rows, FUS_MAX_OUT);
            have_r = src_next(&rs);
        } else {
            TrkMeta m;
            int ne = trk_parse(es.line, et, FUS_MAX_EO, &m);
            if (m.ifov_rad > 0) fus_core_set_eo_geom(core, m.img_w, m.img_h, m.ifov_rad);
            n = fus_core_step_eo(core, et, ne, es.t, rows, FUS_MAX_OUT);
            have_e = src_next(&es);
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        double us = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1e3;
        proc_us_sum += us; if (us > proc_us_max) proc_us_max = us;
        if (us > 300.0) over_budget++;

        /* wire invariant: no constituent published twice */
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                if ((rows[i].eo_tid >= 0 && rows[i].eo_tid == rows[j].eo_tid) ||
                    (rows[i].rad_tid >= 0 && rows[i].rad_tid == rows[j].rad_tid))
                    viol++;
            }
        /* emit round-trip: well-formed, finite */
        FusHdr h = { .rad_connected = 1, .trk_connected = 1, .frame_id = ++out_seq,
                     .eo_engaged = -1, .t_out_ns = take_r ? rs.t : es.t };
        size_t len = fus_frame_json(json, sizeof json, &h, rows, n);
        if (len == 0 || len >= sizeof json || strstr(json, "nan") || strstr(json, "inf")) viol++;
        int bal = 0;
        for (size_t i2 = 0; i2 < len; i2++) { if (json[i2]=='{') bal++; if (json[i2]=='}') bal--; }
        if (bal != 0) viol++;

        int f, e, r;
        fus_core_counts(core, &f, &e, &r);
        if (f > 0) fused_frames++;
        if (f > max_fused) max_fused = f;
        rows_total += n;
        frames++;
    }

    printf("wire_replay: %ld frames, %ld rows, fused on %ld frames (max %d), "
           "%ld violations, %.1f us/pass avg, %.1f max, %ld over 300 us\n",
           frames, rows_total, fused_frames, max_fused, viol,
           frames ? proc_us_sum / frames : 0, proc_us_max, over_budget);
    if (viol) { printf("WIRE_REPLAY: FAIL\n"); return 1; }
    if (frames < 10) { printf("WIRE_REPLAY: FAIL (input too short)\n"); return 1; }
    /* budget gate: average well inside, and outliers (host scheduler preemption
     * on a shared bench box) at most 0.5% of passes. The authoritative on-target
     * timing is the Jetson bench measurement. */
    if (frames && proc_us_sum / frames > 300.0) { printf("WIRE_REPLAY: FAIL (avg perf)\n"); return 1; }
    if (over_budget * 200 > frames) { printf("WIRE_REPLAY: FAIL (perf outliers)\n"); return 1; }
    printf("WIRE_REPLAY: PASS\n");
    return 0;
}
