#include "cluster.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── Tunables (match radar/clustering.py:ClusterParams) ── */
/* eps spacing and min-samples are runtime fields on RadarClusterer (live via
 * /ctl, seeded from CLUSTER_DEFAULT_*); the rest are compile-time. */
#define EPS_DOP_MPS      3.0f
/* Range-adaptive spacing: a target's returns spread out with distance (the
 * radar's cross-range footprint = range x angular spread), so the "how close
 * counts as one object" distance must grow with range or far targets fail to
 * cluster. The eps_pos slider is the NEAR-field base; effective eps =
 * eps_pos + EPS_RANGE_SLOPE * range. At 0.06: +1.8 m @ 30 m, +6 m @ 100 m. */
#define EPS_RANGE_SLOPE  0.06f
/* Dynamic-only + SNR gates are runtime fields (live via /ctl), seeded from
 * CLUSTER_DEFAULT_SPEED / _SNR: points too slow (static clutter) or too weak
 * never seed or join a cluster. See pt_ok(). */
#define MIN_SIZE_M       0.25f
#define MAX_SIZE_M       3.0f
#define ASSOC_GATE_M     5.0f
#define GATE_GROWTH      2.0f     /* m per s since last detection */
#define MERGE_OVERLAP_M  5.0f
#define CONFIRM_MIN_HITS 2
#define CONFIRM_WINDOW   3
#define Q_ACCEL_MPS2     3.0
#define R_POS_M          0.4
/* Internal-only: how many missed frames a track survives (frozen, NEVER
 * published) so its id/confirm-state stay stable across a brief dropout.
 * This is NOT coasting — undetected targets are never emitted. Real
 * motion-model coasting is the future tracking module's job. */
#define TRACK_MAX_MISSES 5

#define MAX_TRACKS   RADAR_MAX_TARGETS

typedef struct {
    int    used;
    int    tid;
    double p[3], v[3];       /* state per axis */
    double P[3][2][2];       /* per-axis covariance */
    double size_half[3];
    int    hits, misses;
    unsigned hist_bits;      /* rolling M-of-N window (bit0 = most recent) */
    int    confirmed;
    double last_hit_t;
} Track;

struct RadarClusterer {
    Track  tracks[MAX_TRACKS];
    int    next_tid;
    float  eps_pos;          /* DBSCAN spacing, m (live via /ctl) */
    int    min_samples;      /* DBSCAN min-samples (live via /ctl) */
    float  speed_min;        /* dynamic-only gate, m/s (live via /ctl) */
    float  snr_min;          /* min per-point SNR, dB; 0 = off (live via /ctl) */
};

RadarClusterer *cluster_new(void) {
    RadarClusterer *c = calloc(1, sizeof(RadarClusterer));
    if (c) {
        c->eps_pos = (float)CLUSTER_DEFAULT_EPS_M;
        c->min_samples = CLUSTER_DEFAULT_MIN_PTS;
        c->speed_min = (float)CLUSTER_DEFAULT_SPEED;
        c->snr_min = (float)CLUSTER_DEFAULT_SNR;
    }
    return c;
}
void cluster_free(RadarClusterer *c) { free(c); }

void cluster_set_dbscan(RadarClusterer *c, double eps_m, int min_pts) {
    if (!c) return;
    if (eps_m < CLUSTER_EPS_MIN_M) eps_m = CLUSTER_EPS_MIN_M;
    if (eps_m > CLUSTER_EPS_MAX_M) eps_m = CLUSTER_EPS_MAX_M;
    if (min_pts < CLUSTER_MIN_PTS_MIN) min_pts = CLUSTER_MIN_PTS_MIN;
    if (min_pts > CLUSTER_MIN_PTS_MAX) min_pts = CLUSTER_MIN_PTS_MAX;
    c->eps_pos = (float)eps_m;
    c->min_samples = min_pts;
}

void cluster_set_gates(RadarClusterer *c, double speed_min_mps, double snr_min_db) {
    if (!c) return;
    if (speed_min_mps < CLUSTER_SPEED_MIN) speed_min_mps = CLUSTER_SPEED_MIN;
    if (speed_min_mps > CLUSTER_SPEED_MAX) speed_min_mps = CLUSTER_SPEED_MAX;
    if (snr_min_db < CLUSTER_SNR_MIN) snr_min_db = CLUSTER_SNR_MIN;
    if (snr_min_db > CLUSTER_SNR_MAX) snr_min_db = CLUSTER_SNR_MAX;
    c->speed_min = (float)speed_min_mps;
    c->snr_min = (float)snr_min_db;
}

/* ── per-axis Kalman ── */
static void kf_predict(Track *t, double dt) {
    double q = Q_ACCEL_MPS2 * Q_ACCEL_MPS2;
    double dt2 = dt * dt, dt3 = dt2 * dt, dt4 = dt2 * dt2;
    double Qpp = 0.25 * dt4 * q, Qpv = 0.5 * dt3 * q, Qvv = dt2 * q;
    for (int a = 0; a < 3; a++) {
        t->p[a] += t->v[a] * dt;
        /* F=[[1,dt],[0,1]]; P = F P F^T + Q */
        double p00 = t->P[a][0][0], p01 = t->P[a][0][1];
        double p10 = t->P[a][1][0], p11 = t->P[a][1][1];
        double n00 = p00 + dt * (p10 + p01) + dt2 * p11 + Qpp;
        double n01 = p01 + dt * p11 + Qpv;
        double n10 = p10 + dt * p11 + Qpv;
        double n11 = p11 + Qvv;
        t->P[a][0][0] = n00; t->P[a][0][1] = n01;
        t->P[a][1][0] = n10; t->P[a][1][1] = n11;
    }
}

static void kf_update(Track *t, const double z[3]) {
    double R = R_POS_M * R_POS_M;
    for (int a = 0; a < 3; a++) {
        double p00 = t->P[a][0][0], p01 = t->P[a][0][1];
        double p10 = t->P[a][1][0], p11 = t->P[a][1][1];
        double S = p00 + R;
        double K0 = p00 / S, K1 = p10 / S;
        double y = z[a] - t->p[a];
        t->p[a] += K0 * y;
        t->v[a] += K1 * y;
        /* P = (I - K H) P, H = [1 0] */
        t->P[a][0][0] = (1 - K0) * p00;
        t->P[a][0][1] = (1 - K0) * p01;
        t->P[a][1][0] = p10 - K1 * p00;
        t->P[a][1][1] = p11 - K1 * p01;
    }
}

static void track_init(Track *t, int tid, const double c[3], const double v[3],
                       const double half[3], double now_t) {
    memset(t, 0, sizeof(*t));
    t->used = 1; t->tid = tid;
    for (int a = 0; a < 3; a++) {
        t->p[a] = c[a]; t->v[a] = v[a];
        t->P[a][0][0] = R_POS_M * R_POS_M;
        t->P[a][1][1] = 25.0;         /* (5 m/s)^2 — unknown */
        t->size_half[a] = half[a];
    }
    t->hits = 1; t->misses = 0; t->hist_bits = 1; t->confirmed = 0;
    t->last_hit_t = now_t;
}

static void hist_push(Track *t, int hit) {
    t->hist_bits = (t->hist_bits << 1) | (hit ? 1u : 0u);
    unsigned mask = (1u << CONFIRM_WINDOW) - 1u;
    if (hit) {
        t->hits++; t->misses = 0;
        if (!t->confirmed &&
            __builtin_popcount(t->hist_bits & mask) >= CONFIRM_MIN_HITS)
            t->confirmed = 1;
    } else {
        t->misses++;
    }
}

/* Squared range-adaptive spacing between two points (see EPS_RANGE_SLOPE). */
static inline float eps2_pair(float eps_base, float ri, float rj) {
    float e = eps_base + EPS_RANGE_SLOPE * 0.5f * (ri + rj);
    return e * e;
}

/* A point is eligible to seed/join a cluster if it's dynamic enough AND (when
 * an SNR gate is set and SNR is known) strong enough. Static clutter and weak
 * returns are excluded. Unknown SNR (NaN, no SideInfo) is never gated out. */
static inline int pt_ok(const RadarPoint *p, float speed_min, float snr_min) {
    if (fabsf(p->doppler) < speed_min) return 0;
    if (snr_min > 0.0f && !isnan(p->snr) && p->snr < snr_min) return 0;
    return 1;
}

/* ── DBSCAN (hand-rolled, O(N^2), N is a few hundred) ── */
static int dbscan(const RadarPoint *pts, int n, int *labels,
                  float eps_pos, int min_samples, float speed_min, float snr_min) {
    static uint8_t nbr[RADAR_MAX_POINTS];   /* scratch neighbour row */
    uint8_t *visited = calloc(n, 1);
    if (!visited) return 0;
    for (int i = 0; i < n; i++) labels[i] = -1;
    int next = 0;

    /* FIFO of indices — bounded by initial neighbours (<=n) plus each
     * point added at most once on expansion (<=n), so 2*N is a safe cap. */
    static int queue[2 * RADAR_MAX_POINTS];
    const int QCAP = 2 * RADAR_MAX_POINTS;
    for (int i = 0; i < n; i++) {
        if (visited[i]) continue;
        visited[i] = 1;
        if (!pt_ok(&pts[i], speed_min, snr_min)) continue;   /* ineligible: never seeds */
        /* count neighbours of i (ineligible points can't be neighbours) */
        int cnt = 0;
        for (int j = 0; j < n; j++) {
            float dx = pts[i].x - pts[j].x, dy = pts[i].y - pts[j].y, dz = pts[i].z - pts[j].z;
            int ok = pt_ok(&pts[j], speed_min, snr_min) &&
                     (dx*dx + dy*dy + dz*dz) <= eps2_pair(eps_pos, pts[i].range, pts[j].range) &&
                     fabsf(pts[i].doppler - pts[j].doppler) <= EPS_DOP_MPS;
            nbr[j] = ok; cnt += ok;
        }
        if (cnt < min_samples) continue;
        labels[i] = next;
        int qn = 0;
        for (int j = 0; j < n; j++) if (nbr[j]) { if (labels[j] == -1) labels[j] = next; if (qn < QCAP) queue[qn++] = j; }
        int qi = 0;
        while (qi < qn) {
            int jj = queue[qi++];
            if (visited[jj]) continue;
            visited[jj] = 1;
            int cnt2 = 0;
            static uint8_t nbr2[RADAR_MAX_POINTS];
            for (int k = 0; k < n; k++) {
                float dx = pts[jj].x - pts[k].x, dy = pts[jj].y - pts[k].y, dz = pts[jj].z - pts[k].z;
                int ok = pt_ok(&pts[k], speed_min, snr_min) &&
                         (dx*dx + dy*dy + dz*dz) <= eps2_pair(eps_pos, pts[jj].range, pts[k].range) &&
                         fabsf(pts[jj].doppler - pts[k].doppler) <= EPS_DOP_MPS;
                nbr2[k] = ok; cnt2 += ok;
            }
            if (cnt2 >= min_samples) {
                for (int k = 0; k < n; k++)
                    if (nbr2[k] && labels[k] == -1) { labels[k] = next; if (qn < QCAP) queue[qn++] = k; }
            } else if (labels[jj] == -1) {
                labels[jj] = next;
            }
        }
        next++;
    }
    free(visited);
    return next;
}

/* Cluster summary for association. */
typedef struct { double c[3], v[3], half[3]; int npts; int cid; } Cl;

static int find_best_track(RadarClusterer *R, const double c[3], double now_t,
                           const int *excluded) {
    double best = INFINITY; int best_tid = -1;
    for (int i = 0; i < MAX_TRACKS; i++) {
        Track *t = &R->tracks[i];
        if (!t->used || excluded[i]) continue;
        double dt_since = now_t - t->last_hit_t; if (dt_since < 0) dt_since = 0;
        double speed = sqrt(t->v[0]*t->v[0] + t->v[1]*t->v[1] + t->v[2]*t->v[2]);
        double gate = ASSOC_GATE_M + (GATE_GROWTH + speed) * dt_since;
        double dx = t->p[0]-c[0], dy = t->p[1]-c[1], dz = t->p[2]-c[2];
        double d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < gate*gate && d2 < best) { best = d2; best_tid = i; }
    }
    return best_tid;
}

static int overlaps_matched(RadarClusterer *R, const double c[3], const int *matched) {
    double g2 = MERGE_OVERLAP_M * MERGE_OVERLAP_M;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (!matched[i]) continue;
        Track *t = &R->tracks[i];
        double dx = t->p[0]-c[0], dy = t->p[1]-c[1], dz = t->p[2]-c[2];
        if (dx*dx + dy*dy + dz*dz < g2) return 1;
    }
    return 0;
}

static Track *alloc_track(RadarClusterer *R) {
    for (int i = 0; i < MAX_TRACKS; i++) if (!R->tracks[i].used) return &R->tracks[i];
    return NULL;
}

static void reap(RadarClusterer *R) {
    for (int i = 0; i < MAX_TRACKS; i++) {
        Track *t = &R->tracks[i];
        if (t->used && t->misses > TRACK_MAX_MISSES) t->used = 0;
    }
}

/* Emit ONLY tracks detected (matched) this frame and confirmed. A track not
 * matched this frame is never published — no coasting, no dead-reckoning. */
static int publish(RadarClusterer *R, RadarTarget *out, int max_out) {
    int n = 0;
    for (int i = 0; i < MAX_TRACKS && n < max_out; i++) {
        Track *t = &R->tracks[i];
        if (!t->used || !t->confirmed || t->misses > 0) continue;
        double conf = t->hits / 10.0; if (conf > 1.0) conf = 1.0;
        RadarTarget *o = &out[n++];
        o->tid = t->tid;
        o->x = t->p[0]; o->y = t->p[1]; o->z = t->p[2];
        o->vx = t->v[0]; o->vy = t->v[1]; o->vz = t->v[2];
        o->sx = t->size_half[0]; o->sy = t->size_half[1]; o->sz = t->size_half[2];
        o->conf = (float)conf; o->num_points = t->hits;
    }
    return n;
}

int cluster_step(RadarClusterer *R, RadarPoint *pts, int n,
                 double now_t, double dt, RadarTarget *out, int max_out) {
    if (dt <= 0) dt = 0.05;
    /* Predict only tracks detected last frame (to gate this frame's clusters).
     * Missed tracks freeze — no motion-model extrapolation of undetected
     * targets. */
    for (int i = 0; i < MAX_TRACKS; i++)
        if (R->tracks[i].used && R->tracks[i].misses == 0) kf_predict(&R->tracks[i], dt);

    if (n <= 0) {
        for (int i = 0; i < MAX_TRACKS; i++) if (R->tracks[i].used) hist_push(&R->tracks[i], 0);
        reap(R);
        return publish(R, out, max_out);
    }

    static int labels[RADAR_MAX_POINTS];
    int nlab = dbscan(pts, n, labels, R->eps_pos, R->min_samples,
                      R->speed_min, R->snr_min);

    /* per-cluster stats */
    static Cl cls[RADAR_MAX_TARGETS];
    int ncl = 0;
    for (int lbl = 0; lbl < nlab && ncl < RADAR_MAX_TARGETS; lbl++) {
        double sx=0,sy=0,sz=0,sd=0; int cnt=0;
        double sxx=0,syy=0,szz=0;
        for (int i = 0; i < n; i++) if (labels[i] == lbl) {
            sx+=pts[i].x; sy+=pts[i].y; sz+=pts[i].z; sd+=pts[i].doppler; cnt++;
        }
        if (cnt == 0) continue;
        Cl *c = &cls[ncl++];
        c->cid = lbl; c->npts = cnt;
        c->c[0]=sx/cnt; c->c[1]=sy/cnt; c->c[2]=sz/cnt;
        for (int i = 0; i < n; i++) if (labels[i] == lbl) {
            double dx=pts[i].x-c->c[0], dy=pts[i].y-c->c[1], dz=pts[i].z-c->c[2];
            sxx+=dx*dx; syy+=dy*dy; szz+=dz*dz;
        }
        double stdx = cnt>1 ? sqrt(sxx/(cnt-1)) : MIN_SIZE_M;
        double stdy = cnt>1 ? sqrt(syy/(cnt-1)) : MIN_SIZE_M;
        double stdz = cnt>1 ? sqrt(szz/(cnt-1)) : MIN_SIZE_M;
        double h[3] = {1.5*stdx, 1.5*stdy, 1.5*stdz};
        for (int a=0;a<3;a++){ if(h[a]<MIN_SIZE_M)h[a]=MIN_SIZE_M; if(h[a]>MAX_SIZE_M)h[a]=MAX_SIZE_M; c->half[a]=h[a]; }
        double dmean = sd/cnt;
        double r = sqrt(c->c[0]*c->c[0]+c->c[1]*c->c[1]+c->c[2]*c->c[2]); if (r<1e-3) r=1e-3;
        for (int a=0;a<3;a++) c->v[a] = c->c[a]/r * dmean;
    }

    /* largest clusters first (greedy) */
    for (int i = 0; i < ncl; i++) for (int j = i+1; j < ncl; j++)
        if (cls[j].npts > cls[i].npts) { Cl tmp = cls[i]; cls[i] = cls[j]; cls[j] = tmp; }

    int matched[MAX_TRACKS]; memset(matched, 0, sizeof(matched));
    int cid_to_track[RADAR_MAX_TARGETS];
    for (int i = 0; i < RADAR_MAX_TARGETS; i++) cid_to_track[i] = -1;

    for (int i = 0; i < ncl; i++) {
        Cl *c = &cls[i];
        int ti = find_best_track(R, c->c, now_t, matched);
        if (ti < 0) {
            if (overlaps_matched(R, c->c, matched)) continue;
            Track *t = alloc_track(R);
            if (!t) continue;
            track_init(t, R->next_tid++, c->c, c->v, c->half, now_t);
            ti = (int)(t - R->tracks);
        } else {
            Track *t = &R->tracks[ti];
            kf_update(t, c->c);
            for (int a=0;a<3;a++) t->size_half[a] = 0.5*c->half[a] + 0.5*t->size_half[a];
            hist_push(t, 1);
            t->last_hit_t = now_t;
            matched[ti] = 1;
        }
        if (c->cid >= 0 && c->cid < RADAR_MAX_TARGETS) cid_to_track[c->cid] = R->tracks[ti].tid;
    }

    for (int i = 0; i < MAX_TRACKS; i++)
        if (R->tracks[i].used && !matched[i]) hist_push(&R->tracks[i], 0);

    reap(R);

    for (int i = 0; i < n; i++) {
        int lbl = labels[i];
        pts[i].tid = (lbl >= 0 && lbl < RADAR_MAX_TARGETS && cid_to_track[lbl] >= 0)
                     ? cid_to_track[lbl] : 255;
    }
    return publish(R, out, max_out);
}
