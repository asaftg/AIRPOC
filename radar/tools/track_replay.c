/* track_replay — offline replay harness for the cluster.c tracker.
 *
 * Reads a point-cloud fixture (repeated records of:
 *   double t_seconds LE, int32 n, then n * 5 float32 {range, az, el, doppler, snr})
 * and runs cluster_step() over it, printing per frame the CONFIRMED tracks and
 * the EMITTED targets:
 *
 *   K eps=.. minpts=.. speed=.. snrmin=.. fov=.. elmax=.. doppler=.. confirm=.. coast=.. park=..
 *   F <frame_idx> <t>
 *   C <tid> <r_m> <az_deg>                          (one per confirmed track, list order)
 *   E <tid> <r_m> <az_deg> <vr_mps> <snr_peak_db> <sus> <mv>  (one per emitted
 *       target; sus=1 = suspected reflection copy, mv = walk-guard class 0/1/2)
 *
 * The single K header echoes the EFFECTIVE knob state (after clamping), so a
 * replay log always says what it actually ran with. E lines are the wire — the
 * subset of confirmed tracks the daemon would emit. Validation must score E
 * lines, never C lines (see radar/docs/VALIDATION.md).
 *
 * Knobs — all 10 live tracker knobs can be overridden, two equivalent ways:
 *   argv:  ./track_replay points.bin snrmin=18 confirm=4
 *   env:   REPLAY_SNRMIN=18 REPLAY_CONFIRM=4 ./track_replay points.bin
 * argv wins over env; anything not given uses the compiled default. The knob
 * names are: eps minpts speed snrmin fov elmax doppler confirm coast park —
 * they map 1:1 onto cluster_set_dbscan / cluster_set_gates / cluster_set_track.
 * Backward compat: a bare number as argv[2] is still elmax_deg.
 *
 * Used by parity_check.py (frame diff vs the Python reference) and by
 * regression/tracker_gates.py (the validation bench).
 *
 * Build:  make -C radar/tools track_replay
 * Run:    ./track_replay points.bin [elmax_deg] [knob=value ...]
 */
#define CLUSTER_INTROSPECT
#include "../src/cluster.c"     /* private access to track state (bench tool) */
#include <stdio.h>
#include <ctype.h>

/* ---- knob plumbing ------------------------------------------------------ */
typedef struct { const char *name; double val; } Knob;
enum { K_EPS, K_MINPTS, K_SPEED, K_SNRMIN, K_FOV, K_ELMAX, K_DOP,
       K_CONFIRM, K_COAST, K_PARK, K_N };

static Knob knobs[K_N] = {
    [K_EPS]     = { "eps",     CLUSTER_DEFAULT_EPS_M   },
    [K_MINPTS]  = { "minpts",  CLUSTER_DEFAULT_MIN_PTS },
    [K_SPEED]   = { "speed",   CLUSTER_DEFAULT_SPEED   },
    [K_SNRMIN]  = { "snrmin",  CLUSTER_DEFAULT_SNR     },
    [K_FOV]     = { "fov",     CLUSTER_DEFAULT_FOV     },
    [K_ELMAX]   = { "elmax",   CLUSTER_DEFAULT_ELMAX   },
    [K_DOP]     = { "doppler", CLUSTER_DEFAULT_DOP     },
    [K_CONFIRM] = { "confirm", CLUSTER_DEFAULT_CONFIRM },
    [K_COAST]   = { "coast",   CLUSTER_DEFAULT_COAST_S },
    [K_PARK]    = { "park",    CLUSTER_DEFAULT_PARK_S  },
};

static int knob_index(const char *name, size_t len)
{
    for (int i = 0; i < K_N; i++)
        if (strlen(knobs[i].name) == len && !strncmp(knobs[i].name, name, len))
            return i;
    return -1;
}

/* env REPLAY_EPS / REPLAY_MINPTS / ... (uppercased knob name) */
static void knobs_from_env(void)
{
    for (int i = 0; i < K_N; i++) {
        char var[32] = "REPLAY_";
        size_t off = strlen(var);
        for (const char *p = knobs[i].name; *p && off < sizeof(var) - 1; p++)
            var[off++] = (char)toupper((unsigned char)*p);
        var[off] = 0;
        const char *v = getenv(var);
        if (v && *v) knobs[i].val = atof(v);
    }
}

/* "name=value" argv token; returns 0 if not a knob assignment */
static int knob_from_arg(const char *arg)
{
    const char *eq = strchr(arg, '=');
    if (!eq || eq == arg) return 0;
    int i = knob_index(arg, (size_t)(eq - arg));
    if (i < 0) return 0;
    knobs[i].val = atof(eq + 1);
    return 1;
}

/* find the live Track behind an emitted tid (private access via the include;
 * cluster.c's API is untouched) */
static const Track *find_track(const RadarClusterer *R, int tid)
{
    for (int oi = 0; oi < R->nord; oi++) {
        const Track *t = &R->tracks[R->ord[oi]];
        if (t->tid == tid) return t;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s points.bin [elmax_deg] [knob=value ...]\n"
            "knobs: eps minpts speed snrmin fov elmax doppler confirm coast park\n"
            "env:   REPLAY_<KNOB>=value (argv wins)\n", argv[0]);
        return 2;
    }
    knobs_from_env();
    for (int a = 2; a < argc; a++) {
        if (knob_from_arg(argv[a])) continue;
        if (a == 2 && strchr("0123456789.-+", argv[a][0])) {  /* legacy [elmax] */
            knobs[K_ELMAX].val = atof(argv[a]);
            continue;
        }
        fprintf(stderr, "unknown arg: %s\n", argv[a]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }

    RadarClusterer *c = cluster_new();
    if (!c) { fclose(f); return 2; }
    cluster_set_dbscan(c, knobs[K_EPS].val, (int)knobs[K_MINPTS].val);
    cluster_set_gates(c, knobs[K_SPEED].val, knobs[K_SNRMIN].val,
                      knobs[K_FOV].val, knobs[K_ELMAX].val, knobs[K_DOP].val);
    cluster_set_track(c, (int)knobs[K_CONFIRM].val, knobs[K_COAST].val,
                      knobs[K_PARK].val);

    /* echo the EFFECTIVE state (read back after clamping) — a replay log must
     * say what it actually ran with, not what was asked for */
    printf("K eps=%.3f minpts=%d speed=%.3f snrmin=%.3f fov=%.3f elmax=%.3f "
           "doppler=%.3f confirm=%d coast=%.3f park=%.3f\n",
           c->dedup_cross, c->min_pts, c->vmin, c->snr_mv, c->fov_half,
           c->el_max, c->merge_dv, c->conf_m, c->coast_s, c->park_s);

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
            /* debug lines: seed counter, channel sizes + flood flag + chain
             * counters (chains_active, chains_confirmed_total), all tracks */
            printf("N %d\n", c->next_tid);
            printf("M %d %d %d %d %lu\n", c->dbg_nmv, c->dbg_m,
                   t < c->flood_until ? 1 : 0,
                   c->chains_active, c->chains_total);
            static int atid[MAX_TRK], aconf[MAX_TRK];
            static double arr2[MAX_TRK], aaz2[MAX_TRK];
            int na = cluster_all(c, atid, arr2, aaz2, aconf, MAX_TRK);
            for (int i = 0; i < na; i++)
                printf("A %d %.6f %.6f %d\n", atid[i], arr2[i], aaz2[i], aconf[i]);
        }
        {
            const char *tw = getenv("REPLAY_TRACE_TID");
            if (tw) {
                double d[16];
                if (cluster_track_detail(c, atoi(tw), d))
                    printf("T %.2f %.2f snrp %.1f pass %.0f ever %.0f streak %.0f "
                           "bad %.0f liar %.0f wlatch %.0f chain %.0f doperr %.2f "
                           "mv %.0f wbad %.0f\n",
                           d[0], d[1], d[2], d[3], d[4], d[5], d[6],
                           d[12], d[13], d[14], d[15], d[11], d[10]);
            }
        }
        for (int i = 0; i < nc; i++)
            printf("C %d %.6f %.6f\n", ctid[i], crr[i], caz[i]);
        for (int i = 0; i < nt; i++) {
            double r = hypot(tg[i].x, tg[i].y), az = atan2(tg[i].x, tg[i].y) / DEG;
            const Track *tk = find_track(c, tg[i].tid);
            printf("E %d %.6f %.6f %.3f %.1f %d %d\n", tg[i].tid, r, az,
                   tk ? tk->vr : 0.0, tk ? tk->snr_peak : 0.0,
                   tg[i].suspect, tg[i].mv_class);
        }
        frame++;
    }
    fclose(f);
    cluster_free(c);
    return 0;
}
