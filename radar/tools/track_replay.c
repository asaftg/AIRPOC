/* track_replay — offline replay harness for the cluster.c tracker.
 *
 * Reads a point-cloud fixture (repeated records of:
 *   double t_seconds LE, int32 n, then n * 5 float32 {range, az, el, doppler, snr})
 * and runs cluster_step() over it with the shipping defaults, printing per
 * frame the CONFIRMED tracks and the EMITTED targets:
 *
 *   F <frame_idx> <t>
 *   C <tid> <r_m> <az_deg>        (one per confirmed track, list order)
 *   E <tid> <r_m> <az_deg>        (one per emitted target)
 *
 * Used by parity_check.py to diff track life/death and positions against the
 * Python reference (radar/tools/radar_tracker.py) frame by frame.
 *
 * Build:  make -C radar/tools track_replay
 * Run:    ./track_replay points.bin [elmax_deg]
 */
#define CLUSTER_INTROSPECT
#include "../src/cluster.c"     /* private access to track state (bench tool) */
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s points.bin [elmax_deg]\n", argv[0]); return 2; }
    double elmax = (argc > 2) ? atof(argv[2]) : CLUSTER_DEFAULT_ELMAX;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }

    RadarClusterer *c = cluster_new();
    if (!c) { fclose(f); return 2; }
    cluster_set_gates(c, CLUSTER_DEFAULT_SPEED, CLUSTER_DEFAULT_SNR,
                      CLUSTER_DEFAULT_FOV, elmax, CLUSTER_DEFAULT_DOP);

    static RadarPoint pts[RADAR_MAX_POINTS];
    static RadarTarget tg[RADAR_MAX_TARGETS];
    static int ctid[MAX_TRK]; static double crr[MAX_TRK], caz[MAX_TRK];

    double t, tprev = 0.0;
    int frame = 0, have_prev = 0;
    for (;;) {
        int32_t np;
        if (fread(&t, sizeof(double), 1, f) != 1) break;
        if (fread(&np, sizeof(int32_t), 1, f) != 1) break;
        if (np < 0) break;
        int keep = 0;
        for (int32_t i = 0; i < np; i++) {
            float v[5];
            if (fread(v, sizeof(float), 5, f) != 5) { np = -1; break; }
            if (keep < RADAR_MAX_POINTS) {
                RadarPoint *p = &pts[keep++];
                memset(p, 0, sizeof(*p));
                p->range = v[0]; p->az = v[1]; p->el = v[2];
                p->doppler = v[3]; p->snr = v[4];
            }
        }
        if (np < 0) break;
        double dt = have_prev ? (t - tprev) : 0.0;
        if (dt < 1e-3) dt = 1e-3;               /* == python max(t-tprev, 1e-3) */
        tprev = t; have_prev = 1;

        int nt = cluster_step(c, pts, keep, t, dt, tg, RADAR_MAX_TARGETS);
        int nc = cluster_confirmed(c, ctid, crr, caz, MAX_TRK);

        printf("F %d %.9f\n", frame, t);
        if (getenv("REPLAY_ALL")) {
            /* debug lines: seed counter, channel sizes + flood flag, all tracks */
            printf("N %d\n", c->next_tid);
            printf("M %d %d %d\n", c->dbg_nmv, c->dbg_m,
                   t < c->flood_until ? 1 : 0);
            static int atid[MAX_TRK], aconf[MAX_TRK];
            static double arr2[MAX_TRK], aaz2[MAX_TRK];
            int na = cluster_all(c, atid, arr2, aaz2, aconf, MAX_TRK);
            for (int i = 0; i < na; i++)
                printf("A %d %.6f %.6f %d\n", atid[i], arr2[i], aaz2[i], aconf[i]);
        }
        {
            const char *tw = getenv("REPLAY_TRACE_TID");
            if (tw) {
                double d7[7];
                if (cluster_track_detail(c, atoi(tw), d7))
                    printf("T %.2f %.2f snrp %.1f pass %.0f ever %.0f streak %.0f bad %.0f\n",
                           d7[0], d7[1], d7[2], d7[3], d7[4], d7[5], d7[6]);
            }
        }
        for (int i = 0; i < nc; i++)
            printf("C %d %.6f %.6f\n", ctid[i], crr[i], caz[i]);
        for (int i = 0; i < nt; i++) {
            double r = hypot(tg[i].x, tg[i].y), az = atan2(tg[i].x, tg[i].y) / DEG;
            printf("E %d %.6f %.6f\n", tg[i].tid, r, az);
        }
        frame++;
    }
    fclose(f);
    cluster_free(c);
    return 0;
}
