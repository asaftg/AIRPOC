/* refit.c - bench tool: re-run a recorded session's radar_wire + trk_wire
 * through the CURRENT fusion core and print the fus wire it would publish,
 * as JSONL ("<t_pub_ns> <frame-json>"). Used to regenerate a recording's
 * fusion layer after a core change so replay shows the new behaviour.
 *
 * usage: refit <radar.jsonl> <trk.jsonl> <trim_az_deg> <trim_el_deg>
 */
#define _GNU_SOURCE
#include "../src/core.h"
#include "../src/config.h"
#include "../src/emit.h"
#include "../src/rad_parse.h"
#include "../src/trk_parse.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { FILE *f; char *line; size_t cap; uint64_t t; int eof; } Src;
static int nx(Src *s)
{
    if (s->eof) return 0;
    ssize_t r = getline(&s->line, &s->cap, s->f);
    if (r <= 0) { s->eof = 1; return 0; }
    char *sp = strchr(s->line, ' ');
    if (!sp) return nx(s);
    *sp = 0;
    s->t = strtoull(s->line, NULL, 10);
    memmove(s->line, sp + 1, strlen(sp + 1) + 1);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: %s radar.jsonl trk.jsonl trim_az trim_el\n", argv[0]); return 2; }
    Src rs = { .f = fopen(argv[1], "r") }, es = { .f = fopen(argv[2], "r") };
    if (!rs.f || !es.f) { fprintf(stderr, "refit: cannot open inputs\n"); return 2; }
    FusCore *c = fus_core_new();
    FusKnobs k; fus_core_get_knobs(c, &k);
    k.trim_az = atof(argv[3]) * M_PI / 180.0;
    k.trim_el = atof(argv[4]) * M_PI / 180.0;
    fus_core_set_knobs(c, &k);

    FusRadTgt rt[FUS_MAX_RAD]; FusEoTrk et[FUS_MAX_EO]; FusOut rows[FUS_MAX_OUT];
    static char json[256 * 1024];
    uint64_t out_seq = 0, rad_fid = 0, eo_fid = 0;
    int engaged = -1;
    int hr = nx(&rs), he = nx(&es);
    while (hr || he) {
        int takr = hr && (!he || rs.t <= es.t);
        uint64_t t; int n;
        if (takr) {
            RadMeta m; int nr = rad_parse(rs.line, rt, FUS_MAX_RAD, &m);
            t = rs.t; rad_fid = m.frame_id;
            n = fus_core_step_rad(c, rt, nr, t, rows, FUS_MAX_OUT);
            hr = nx(&rs);
        } else {
            TrkMeta m; int ne = trk_parse(es.line, et, FUS_MAX_EO, &m);
            if (m.ifov_rad > 0) fus_core_set_eo_geom(c, m.img_w, m.img_h, m.ifov_rad);
            t = es.t; eo_fid = m.frame_id; engaged = m.engaged;
            n = fus_core_step_eo(c, et, ne, t, rows, FUS_MAX_OUT);
            he = nx(&es);
        }
        double ea, ee; int en = fus_core_trim_est(c, &ea, &ee);
        FusHdr h = {
            .rad_connected = 1, .trk_connected = 1,
            .frame_id = ++out_seq, .rad_frame_id = rad_fid, .eo_frame_id = eo_fid,
            .eo_engaged = engaged, .t_out_ns = t,
            .trim_az_deg = k.trim_az * 180.0 / M_PI,
            .trim_el_deg = k.trim_el * 180.0 / M_PI,
            .est_az_deg = ea * 180.0 / M_PI, .est_el_deg = ee * 180.0 / M_PI,
            .est_n = en,
        };
        size_t len = fus_frame_json(json, sizeof json, &h, rows, n);
        int nf, neo, nrad; fus_core_counts(c, &nf, &neo, &nrad);
        if (len > 0)
            printf("%llu %u %d %d %d %s\n", (unsigned long long)t,
                   (unsigned)out_seq, nf, neo, nrad, json);
    }
    return 0;
}
