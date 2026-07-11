/* Temporal multi-target tracker — see cluster.h. Port of the offline-validated
 * radar_tracker.py (reference copy + parity harness: radar/tools). Fixed
 * arrays, no heap on the hot path.
 *
 * 2026-07-11: post-confirmation CONSISTENCY GUARD. A confirmed track must stay
 * physically coherent to live, and must show POSITIVE evidence to emit:
 *   kill   — sustained incoherence (radial random-walk, unphysical path speed,
 *            re-latch teleports, wander, jitter) kills the track (dead, not
 *            parked). Fixes the immortal wandering track (garage multipath)
 *            and ghost tracks riding the denser 16 dB point cloud.
 *   emit   — a track earns emission with a streak of judged-coherent frames
 *            showing net progress, plus BRIGHTNESS evidence: at least one
 *            claimed point with SNR >= req(range) (floor noise is ~16-21 dB at
 *            every range; a real target near the radar is far brighter, R^4).
 *   flood  — when something moves right next to the radar the sidelobes light
 *            the whole hemisphere and angle information is gone; while flooded,
 *            close tracks cannot EARN emission evidence.
 *   assoc  — two-tier association (confirmed tracks claim points first) plus a
 *            tighter miss-growth cap for confirmed tracks stops junk from
 *            shredding an established target under the open elevation diet.
 * All guard thresholds are RANGE-AWARE: range is clean at any distance while
 * azimuth noise scales as radians(err)*range. */
#include "cluster.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG (M_PI/180.0)

/* ---- occupancy grid geometry (fresh-static channel) ---- */
#define NR 200         /* (400-0)/2 range cells */
#define NA 64          /* (32--32)/1 az cells   */
#define GR_R0 0.0
#define GR_DR 2.0
#define GR_A0 (-32.0)
#define GR_DA 1.0
/* ---- per-track buffers ---- */
#define HMAX 160        /* position-history ring */
#define WMAX 12         /* hit/miss window (== st_conf_N) */
#define MAX_TRK 128     /* live tracks incl. tentative (worst measured: 73) */
#define GRAVE_MAX 32    /* emission-evidence graveyard */

/* ---- fixed (non-knob) tracker params (== radar_tracker.py DEFAULTS) ---- */
/* Elevation gating is the elmax knob ONLY (symmetric, radar-frame). The old
 * hard EL_LO -9 / EL_HI +2.5 window was a level-mount ground-bench tuning:
 * on a gimbal it is wrong in every pose, and it silently blinded the tracker
 * to any airborne target above +2.5 deg. Removed 2026-07-10. */
#define AZ_KEEP_CAP 90.0     /* input az also gated by the FOV knob */
#define R_MIN 3.0
#define OCC_FREE 0.35
#define WARMUP_S 8.0
#define LEARN_FAST 0.10
#define LEARN 0.0003
#define DECAY 0.00005
#define ST_MIN_PTS 2
#define ST_SUSTAIN_MV 1.2
#define ST_MV_LO 0.35
#define GATE_R 6.0
#define GATE_CROSS 4.0
#define AZ_GATE_MIN 1.0
#define AZ_GATE_MAX 8.0
#define SPEED_GATE_S 0.45
#define MISS_GROW 0.30
#define MISS_GROW_CAP 2.0
#define CONF_GROW_CAP 1.5    /* confirmed: tighter cap on miss-driven gate growth
                              * (a big-growing gate is how a ghost reaches noise) */
#define MV_RATE_MIN 0.6
#define JIT_MAX 2.6
#define ST_CONF_M 8
#define ST_CONF_N 12
#define ST_JIT_MAX 1.8
#define JIT_ALPHA 0.35
#define TENT_MAX_MISS 3
#define EMIT_MAX_MISS 3
#define PARK_DISP 1.8
#define PARK_WIN 4.0
#define TRK_FPS 26.0     /* A/G profile frame rate: coast/park seconds -> frames */
#define VEL_WIN 0.9
#define VEL_MIN_SPAN 0.22
#define SPEED_MAX 30.0
#define ALPHA 0.55
#define ALPHA_AZ 0.7
#define EL_ALPHA 0.10
#define SEED_LINK_R 5.0
#define SEED_LINK_CROSS 3.5
#define SEED_GUARD_R 5.0
#define SEED_GUARD_AZ 2.5
#define MERGE_R 12.0
#define MERGE_CROSS 3.0
#define DEDUP_R 12.0
#define OUT_R_MAX 500.0
#define EMIT_SPD 1.2
#define EMIT_DISP 2.0
#define DISP_WIN 5.0
#define ANG_FLOOR_DEG 2.5    /* deg  angle-noise floor for cross motion/evidence */
#define ANG_RATE_MIN 3.0     /* deg/s real angular rate = a crosser */
#define MOVE_CONFIRM 8       /* frames of real motion before park-hold is earned */
#define PHANTOM_LEASH_S 1.3  /* s    confirmed track with no motion by then dies */
#define MIN_SIZE_M 0.25
#define MAX_SIZE_M 3.0
/* ---- consistency guard (radar_tracker.py "guard" block) ---- */
#define GUARD_WIN 2.5        /* s   sliding window over measured history */
#define GUARD_NSEG 5         /*     time bins for waypoint averaging */
#define GUARD_MIN_N 8        /*     measured points in window needed to judge */
#define COH_MIN 0.40         /*     net/path below this = random walk */
#define R_PATH_MIN 3.0       /* m   radial-evidence floor at r=0 ... */
#define R_PATH_RK 0.025      /* m/m ... growing with range (noise path thickens) */
#define GSPD_MAX 36.0        /* m/s range path speed above this is unphysical */
#define TELE_SPD 45.0        /* m/s implied range step speed = re-latch teleport */
#define TELE_DM 3.0          /* m   min range step to count at r=0 ... */
#define TELE_RK 0.02         /* m/m ... growing with range (multipath mode flips) */
#define TELE_DT_MAX 0.5      /* s   only judge steps without a long gap */
#define TELE_K 3             /*     teleports in window to flag */
#define JIT_BASE 3.5         /* m   innovation EWMA allowance at r=0 ... */
#define JIT_RK 0.02          /* m/m ... growing with range */
#define CROSS_WANDER_K 2.0   /*     cross path floor factor for wander test */
#define GUARD_KILL 10        /*     bad-frame counter (hysteresis) -> kill */
#define GUARD_EMIT 12        /*     judged-good streak to earn first emission */
#define GUARD_EMIT_RE 3      /*     shorter streak to re-latch a proven target */
#define GUARD_UNLATCH 5      /*     counter level that unlatches emission */
#define POS_NET 2.0          /* m   net window progress = positive evidence */
/* brightness evidence: to emit at range r a track's lifetime PEAK claimed SNR
 * must clear req(r) = clamp(HI - SLOPE*r, LO, HI) for its CURRENT range (not a
 * latch: a far-born floor-noise track that wanders close faces a bar it can
 * never meet; a real approacher brightens R^4-fast). Floor-noise ghosts peak
 * 18-21 dB at 15-120 m, real tracks 21+ dB even at 250 m. */
#define SNR_EVID_HI 24.5
#define SNR_EVID_LO 17.5
#define SNR_EVID_SLOPE 0.025
/* near-field flood: moving cloud below FLOOD_R covering FLOOD_AZ of azimuth */
#define FLOOD_R 40.0
#define FLOOD_N 25
#define FLOOD_AZ 80.0
#define FLOOD_HOLD 2.0
#define FLOOD_MARGIN 10.0
/* emission-evidence handoff (re-acquired target after a dropout death) */
#define INHERIT_S 2.0
#define INHERIT_R 10.0
#define INHERIT_CROSS 6.0

typedef struct {
    int used, tid;
    double r, az, el, vr, va;
    double jit, mv_ewma, max_mv;
    int misses, age, st_frames, hits_total;
    int confirmed;
    double ht[HMAX], hr[HMAX], ha[HMAX]; int hn, hhead;
    int hit[WMAX], hitn;
    double sx, sy, sz;           /* half-extents (m), from the frame's points */
    /* motion test */
    int moved_frames, moving;
    double last_moved_t;
    /* consistency guard */
    int guard_bad, ok_streak, guard_pass, ever_passed, coh_bad;
    double snr_peak;         /* lifetime max claimed SNR (brightness evidence) */
    int snr_unknown;         /* saw a point without SNR (no TLV7) -> fail open */
} Track;

typedef struct { double t, r, az, snr_peak; } Grave;

struct RadarClusterer {
    Track tracks[MAX_TRK];
    int   ord[MAX_TRK];  /* live slots in python-list order (assoc/merge/emit) */
    int   nord;
    int   next_tid;
    float occ[NR][NA];
    double t0; int have_t0;
    double flood_until;
    Grave grave[GRAVE_MAX]; int ngrave;
    /* live knobs */
    float dedup_cross;   /* eps knob   */
    int   min_pts;       /* min_pts    */
    float vmin;          /* speed knob */
    float snr_mv;        /* snr knob (static = +3) */
    float fov_half;      /* fov knob   */
    float el_max;        /* elmax knob (elevation half-angle, 90 = off) */
    float merge_dv;      /* doppler knob */
    int   conf_m;        /* confirm knob (M-of-N, window = M+1) */
    double coast_s;      /* coast knob (seconds) */
    double park_s;       /* park-hold knob (seconds) */
    int dbg_nmv, dbg_m;  /* bench introspection */
};

RadarClusterer *cluster_new(void) {
    RadarClusterer *c = calloc(1, sizeof(RadarClusterer));
    if (c) {
        c->next_tid    = 1;
        c->dedup_cross = (float)CLUSTER_DEFAULT_EPS_M;
        c->min_pts     = CLUSTER_DEFAULT_MIN_PTS;
        c->vmin        = (float)CLUSTER_DEFAULT_SPEED;
        c->snr_mv      = (float)CLUSTER_DEFAULT_SNR;
        c->fov_half    = (float)CLUSTER_DEFAULT_FOV;
        c->el_max      = (float)CLUSTER_DEFAULT_ELMAX;
        c->merge_dv    = (float)CLUSTER_DEFAULT_DOP;
        c->conf_m      = CLUSTER_DEFAULT_CONFIRM;
        c->coast_s     = CLUSTER_DEFAULT_COAST_S;
        c->park_s      = CLUSTER_DEFAULT_PARK_S;
        c->flood_until = -1e18;
    }
    return c;
}
void cluster_free(RadarClusterer *c) { free(c); }
double cluster_fov(const RadarClusterer *c) { return c ? c->fov_half : CLUSTER_DEFAULT_FOV; }

void cluster_set_dbscan(RadarClusterer *c, double eps_m, int min_pts) {
    if (!c) return;
    if (eps_m < CLUSTER_EPS_MIN_M) eps_m = CLUSTER_EPS_MIN_M;
    if (eps_m > CLUSTER_EPS_MAX_M) eps_m = CLUSTER_EPS_MAX_M;
    if (min_pts < CLUSTER_MIN_PTS_MIN) min_pts = CLUSTER_MIN_PTS_MIN;
    if (min_pts > CLUSTER_MIN_PTS_MAX) min_pts = CLUSTER_MIN_PTS_MAX;
    c->dedup_cross = (float)eps_m; c->min_pts = min_pts;
}
void cluster_set_gates(RadarClusterer *c, double speed_min_mps, double snr_min_db,
                       double fov_half_deg, double el_max_deg, double doppler_gate_mps) {
    if (!c) return;
    if (speed_min_mps < CLUSTER_SPEED_MIN) speed_min_mps = CLUSTER_SPEED_MIN;
    if (speed_min_mps > CLUSTER_SPEED_MAX) speed_min_mps = CLUSTER_SPEED_MAX;
    if (snr_min_db < CLUSTER_SNR_MIN) snr_min_db = CLUSTER_SNR_MIN;
    if (snr_min_db > CLUSTER_SNR_MAX) snr_min_db = CLUSTER_SNR_MAX;
    if (fov_half_deg < CLUSTER_FOV_MIN) fov_half_deg = CLUSTER_FOV_MIN;
    if (fov_half_deg > CLUSTER_FOV_MAX) fov_half_deg = CLUSTER_FOV_MAX;
    if (el_max_deg < CLUSTER_ELMAX_MIN) el_max_deg = CLUSTER_ELMAX_MIN;
    if (el_max_deg > CLUSTER_ELMAX_MAX) el_max_deg = CLUSTER_ELMAX_MAX;
    if (doppler_gate_mps < CLUSTER_DOP_MIN) doppler_gate_mps = CLUSTER_DOP_MIN;
    if (doppler_gate_mps > CLUSTER_DOP_MAX) doppler_gate_mps = CLUSTER_DOP_MAX;
    c->vmin = (float)speed_min_mps; c->snr_mv = (float)snr_min_db;
    c->fov_half = (float)fov_half_deg; c->el_max = (float)el_max_deg;
    c->merge_dv = (float)doppler_gate_mps;
}
void cluster_set_track(RadarClusterer *c, int confirm_m, double coast_s, double park_s) {
    if (!c) return;
    if (confirm_m < CLUSTER_CONFIRM_MIN) confirm_m = CLUSTER_CONFIRM_MIN;
    if (confirm_m > CLUSTER_CONFIRM_MAX) confirm_m = CLUSTER_CONFIRM_MAX;
    if (coast_s < CLUSTER_COAST_MIN) coast_s = CLUSTER_COAST_MIN;
    if (coast_s > CLUSTER_COAST_MAX) coast_s = CLUSTER_COAST_MAX;
    if (park_s < CLUSTER_PARK_MIN) park_s = CLUSTER_PARK_MIN;
    if (park_s > CLUSTER_PARK_MAX) park_s = CLUSTER_PARK_MAX;
    c->conf_m = confirm_m; c->coast_s = coast_s; c->park_s = park_s;
}

/* ---- helpers ---- */
static double clampd(double v, double lo, double hi){ return v<lo?lo:(v>hi?hi:v); }
static int dcmp(const void *a, const void *b){ double d=*(const double*)a-*(const double*)b; return (d>0)-(d<0); }
static double med(const double *v, int n){
    static double a[RADAR_MAX_POINTS]; memcpy(a, v, (size_t)n*sizeof(double));
    qsort(a, (size_t)n, sizeof(double), dcmp);
    return (n&1) ? a[n/2] : 0.5*(a[n/2-1]+a[n/2]);
}
static void hist_push(Track *t, double tm, double r, double a){
    t->ht[t->hhead]=tm; t->hr[t->hhead]=r; t->ha[t->hhead]=a;
    t->hhead=(t->hhead+1)%HMAX; if(t->hn<HMAX) t->hn++;
}
/* copy the in-window tail of the history ring in CHRONOLOGICAL order */
static int hist_window(const Track *t, double tnow, double win,
                       double *ht, double *hr, double *ha){
    int n=0;
    for(int i=0;i<t->hn;i++){
        int idx=(t->hhead - t->hn + i + HMAX)%HMAX;      /* oldest -> newest */
        if(tnow - t->ht[idx] <= win){ ht[n]=t->ht[idx]; hr[n]=t->hr[idx]; ha[n]=t->ha[idx]; n++; }
    }
    return n;
}
static void hit_push(Track *t, int v){
    if(t->hitn<WMAX) t->hit[t->hitn++]=v;
    else { memmove(t->hit, t->hit+1, (WMAX-1)*sizeof(int)); t->hit[WMAX-1]=v; }
}
static int hit_sum_last(Track *t, int k){
    int s=0, n=t->hitn; if(k>n)k=n;
    for(int i=n-k;i<n;i++) s+=t->hit[i];
    return s;
}
/* (radial, cross) coherent displacement over the window: first-third vs
 * last-third mean of MEASURED history (radar_tracker.py recent_motion) */
static void recent_motion(const Track *t, double tnow, double win,
                          double *rad, double *cross){
    double ht[HMAX], hr[HMAX], ha[HMAX];
    int n = hist_window(t, tnow, win, ht, hr, ha);
    *rad = 0.0; *cross = 0.0;
    if(n<9) return;
    int k=n/3; double r1=0,r2=0,a1=0,a2=0;
    for(int i=0;i<k;i++){ r1+=hr[i]; a1+=ha[i]; }
    for(int i=n-k;i<n;i++){ r2+=hr[i]; a2+=ha[i]; }
    r1/=k; a1/=k; r2/=k; a2/=k;
    *rad = fabs(r2-r1);
    *cross = fabs((a2-a1)*DEG)*0.5*(r1+r2);
}
static void refit_vel(Track *t, double tnow){
    double tt[HMAX], rr[HMAX], aa[HMAX];
    int n = hist_window(t, tnow, VEL_WIN, tt, rr, aa);
    if(n<3) return;
    if(tt[n-1]-tt[0] < VEL_MIN_SPAN) return;
    double mt=0,mr=0,ma=0; for(int i=0;i<n;i++){ mt+=tt[i]; mr+=rr[i]; ma+=aa[i]; } mt/=n; mr/=n; ma/=n;
    double den=0,nr=0,na=0;
    for(int i=0;i<n;i++){ double dm=tt[i]-mt; den+=dm*dm; nr+=dm*(rr[i]-mr); na+=dm*(aa[i]-ma); }
    if(den<=0) return;
    t->vr=clampd(nr/den, -SPEED_MAX, SPEED_MAX);
    double valim=(SPEED_MAX/(t->r>10.0?t->r:10.0))/DEG;
    t->va=clampd(na/den, -valim, valim);
}
/* physical-coherence evidence over the trailing window of MEASURED positions
 * (radar_tracker.py guard_metrics): time-binned waypoints suppress per-frame
 * noise; teleports count raw impossible range steps. */
static int guard_metrics(const Track *t, double tnow, double tele_dm,
                         double *r_path, double *r_net, double *r_span,
                         double *c_path, double *c_net, int *tele){
    double ht[HMAX], hr[HMAX], ha[HMAX];
    int n = hist_window(t, tnow, GUARD_WIN, ht, hr, ha);
    *r_path=*r_net=*r_span=*c_path=*c_net=0.0; *tele=0;
    if(n<2) return n;
    for(int i=1;i<n;i++){
        double sdt=ht[i]-ht[i-1], dr=fabs(hr[i]-hr[i-1]);
        if(sdt>0.0 && sdt<=TELE_DT_MAX && dr>tele_dm && dr/sdt>TELE_SPD) (*tele)++;
    }
    double tlo=ht[0];
    double seg_dt=(ht[n-1]-tlo)/GUARD_NSEG; if(seg_dt<1e-9) seg_dt=1e-9;
    double wt[GUARD_NSEG+1], wr[GUARD_NSEG+1], wa[GUARD_NSEG+1]; int nwp=0;
    double bt=0,br=0,ba=0; int bc=0, cur=0;
    for(int i=0;i<n;i++){
        int k=(int)((ht[i]-tlo)/seg_dt); if(k>GUARD_NSEG-1) k=GUARD_NSEG-1;
        if(k!=cur && bc){
            wt[nwp]=bt/bc; wr[nwp]=br/bc; wa[nwp]=ba/bc; nwp++;
            bt=br=ba=0; bc=0; cur=k;
        }
        bt+=ht[i]; br+=hr[i]; ba+=ha[i]; bc++;
    }
    if(bc){ wt[nwp]=bt/bc; wr[nwp]=br/bc; wa[nwp]=ba/bc; nwp++; }
    if(nwp<3) return n;
    for(int i=1;i<nwp;i++){
        *r_path += fabs(wr[i]-wr[i-1]);
        *c_path += fabs((wa[i]-wa[i-1])*DEG)*0.5*(wr[i]+wr[i-1]);
    }
    *r_net = fabs(wr[nwp-1]-wr[0]);
    *c_net = fabs((wa[nwp-1]-wa[0])*DEG)*0.5*(wr[0]+wr[nwp-1]);
    *r_span = wt[nwp-1]-wt[0];
    return n;
}
/* flood-fill cluster on (r,az); fills label[], returns ncluster */
static int cluster_pts(const double *r, const double *az, int n,
                       double link_r, double link_cross, int *label){
    for(int i=0;i<n;i++) label[i]=-1;
    static int stk[RADAR_MAX_POINTS]; int nc=0;
    for(int i=0;i<n;i++){
        if(label[i]>=0) continue;
        int sp=0; stk[sp++]=i; label[i]=nc;
        while(sp){
            int j=stk[--sp];
            for(int k=0;k<n;k++){
                if(label[k]>=0) continue;
                if(fabs(r[k]-r[j])<link_r){
                    double cross=fabs((az[k]-az[j])*DEG)*0.5*(r[k]+r[j]);
                    if(cross<link_cross){ label[k]=nc; stk[sp++]=k; }
                }
            }
        }
        nc++;
    }
    return nc;
}
static double snr_req(double r){
    return clampd(SNR_EVID_HI - SNR_EVID_SLOPE*r, SNR_EVID_LO, SNR_EVID_HI);
}
static void ord_remove(RadarClusterer *R, int pos){
    R->tracks[R->ord[pos]].used = 0;
    memmove(R->ord+pos, R->ord+pos+1, (size_t)(R->nord-pos-1)*sizeof(int));
    R->nord--;
}

int cluster_step(RadarClusterer *R, RadarPoint *pts, int n,
                 double now_t, double dt, RadarTarget *out, int max_out) {
    if (dt <= 0) dt = 0.05;
    if (!R->have_t0){ R->t0 = now_t; R->have_t0 = 1; R->flood_until = now_t - 1.0; }
    int warm = (now_t - R->t0) < WARMUP_S;
    double snr_st = R->snr_mv + 3.0f;
    double fov = R->fov_half; if (fov > AZ_KEEP_CAP) fov = AZ_KEEP_CAP;
    double el_max = R->el_max;                            /* live elmax knob   */
    int conf_m = R->conf_m, conf_n = conf_m + 1;          /* live confirm knob */
    int coast_frames = (int)lround(R->coast_s * TRK_FPS); /* live coast knob   */
    int park_frames  = (int)lround(R->park_s * TRK_FPS);  /* live park knob    */

    for (int i=0;i<n;i++) pts[i].tid = 255;

    /* ---- channel split (srcpt keeps pts index for tid tagging) ---- */
    static double rs[RADAR_MAX_POINTS], azs[RADAR_MAX_POINTS], els[RADAR_MAX_POINTS], snrs[RADAR_MAX_POINTS];
    static int ismv[RADAR_MAX_POINTS], srcpt[RADAR_MAX_POINTS], snrok[RADAR_MAX_POINTS];
    static double ar[RADAR_MAX_POINTS], aa_[RADAR_MAX_POINTS];
    int m=0, nmv=0, nall=0;
    static int st_idx[RADAR_MAX_POINTS];
    static double st_r[RADAR_MAX_POINTS], st_a[RADAR_MAX_POINTS], st_e[RADAR_MAX_POINTS], st_s[RADAR_MAX_POINTS];
    int nst=0;
    for (int i=0;i<n;i++){
        double r=pts[i].range, az=pts[i].az, el=pts[i].el, v=pts[i].doppler, snr=pts[i].snr;
        if (r<R_MIN || fabs(az)>fov || fabs(el)>el_max) continue;
        ar[nall]=r; aa_[nall]=az; nall++;
        int snr_known = !isnan(snr);
        if (fabs(v)>=R->vmin && (!snr_known || snr>=R->snr_mv)) {
            rs[m]=r; azs[m]=az; els[m]=el; snrs[m]=snr_known?snr:0.0;
            snrok[m]=snr_known; ismv[m]=1; srcpt[m]=i; m++; nmv++;
        } else if (snr_known && snr>=snr_st) {
            st_idx[nst]=i; st_r[nst]=r; st_a[nst]=az; st_e[nst]=el; st_s[nst]=snr; nst++;
        }
    }
    /* ---- near-field flood detection: moving cloud below FLOOD_R spanning
     * most of the azimuth axis = something is right next to the radar and
     * angle information is physically gone (sidelobe hemisphere). ---- */
    {
        static double caz[RADAR_MAX_POINTS]; int ncz=0;
        for(int i=0;i<nmv;i++) if(rs[i]<FLOOD_R) caz[ncz++]=azs[i];
        if(ncz>=FLOOD_N){
            qsort(caz,(size_t)ncz,sizeof(double),dcmp);
            int i10=(int)(0.10*(ncz-1)), i90=(int)(0.90*(ncz-1));
            if(caz[i90]-caz[i10] >= FLOOD_AZ) R->flood_until = now_t + FLOOD_HOLD;
        }
    }
    /* ---- fresh-static: high-SNR returns in historically-empty cells ---- */
    if (!warm && nst){
        static double cr[RADAR_MAX_POINTS], ca[RADAR_MAX_POINTS], ce[RADAR_MAX_POINTS], cs_[RADAR_MAX_POINTS];
        static int cidx[RADAR_MAX_POINTS]; int nc=0;
        for (int i=0;i<nst;i++){
            int ir=(int)((st_r[i]-GR_R0)/GR_DR), ia=(int)((st_a[i]-GR_A0)/GR_DA);
            if (ir>=0&&ir<NR&&ia>=0&&ia<NA){
                float nb=0; int i0=ir-1<0?0:ir-1,i1=ir+1>=NR?NR-1:ir+1,j0=ia-1<0?0:ia-1,j1=ia+1>=NA?NA-1:ia+1;
                for(int a=i0;a<=i1;a++) for(int b=j0;b<=j1;b++) if(R->occ[a][b]>nb) nb=R->occ[a][b];
                if (nb<OCC_FREE){ cr[nc]=st_r[i]; ca[nc]=st_a[i]; ce[nc]=st_e[i]; cs_[nc]=st_s[i]; cidx[nc]=st_idx[i]; nc++; }
            }
        }
        if (nc){
            static int lbl[RADAR_MAX_POINTS]; int k=cluster_pts(cr,ca,nc,SEED_LINK_R,SEED_LINK_CROSS,lbl);
            static int cnt[RADAR_MAX_POINTS]; for(int c=0;c<k;c++) cnt[c]=0;
            for(int i=0;i<nc;i++) cnt[lbl[i]]++;
            for(int i=0;i<nc;i++) if(cnt[lbl[i]]>=ST_MIN_PTS && m<RADAR_MAX_POINTS){
                rs[m]=cr[i]; azs[m]=ca[i]; els[m]=ce[i]; snrs[m]=cs_[i];
                snrok[m]=1; ismv[m]=0; srcpt[m]=cidx[i]; m++;
            }
        }
    }

    R->dbg_nmv = nmv; R->dbg_m = m;
    /* ---- predictions (python-list order) ---- */
    static double PR[MAX_TRK], PAZ[MAX_TRK], RG[MAX_TRK], AZG[MAX_TRK];
    for (int oi=0;oi<R->nord;oi++){
        Track *t=&R->tracks[R->ord[oi]];
        double pr=t->r+t->vr*dt, paz=t->az+t->va*dt;
        double gcap = t->confirmed ? CONF_GROW_CAP : MISS_GROW_CAP;
        double g=1.0+MISS_GROW*t->misses; if(g>gcap)g=gcap;
        double rg, cg;
        if (t->coh_bad){
            /* incoherent confirmed track: its velocity fit is fed by noise, so
             * neither the speed term nor miss-growth may inflate the gate. */
            g = 1.0; rg = GATE_R; cg = GATE_CROSS;
        } else {
            double cs=fabs(t->va*DEG)*pr;
            rg=(GATE_R+fabs(t->vr)*SPEED_GATE_S)*g;
            cg=(GATE_CROSS+cs*SPEED_GATE_S)*g;
        }
        double azg=atan2(cg, pr>5.0?pr:5.0)/DEG;
        double lo=AZ_GATE_MIN*g; if(azg<lo)azg=lo; if(azg>AZ_GATE_MAX)azg=AZ_GATE_MAX;
        int ti=R->ord[oi];
        PR[ti]=pr; PAZ[ti]=paz; RG[ti]=rg; AZG[ti]=azg;
    }
    /* ---- two-tier per-point nearest-track association: confirmed tracks
     * claim their points first; tentative tracks compete for leftovers ---- */
    static int owner[RADAR_MAX_POINTS], tier1[RADAR_MAX_POINTS];
    static double bestd[RADAR_MAX_POINTS];
    for(int i=0;i<m;i++){ owner[i]=-1; bestd[i]=1e9; }
    for(int tier=1;tier>=0;tier--){
        for(int oi=0;oi<R->nord;oi++){
            int ti=R->ord[oi]; Track *t=&R->tracks[ti];
            if(t->confirmed != tier) continue;
            int st_ok = t->max_mv >= ST_SUSTAIN_MV;
            for(int i=0;i<m;i++){
                if(!tier && tier1[i]) continue;          /* leftovers only */
                if(!ismv[i] && !st_ok) continue;
                double dr=fabs(rs[i]-PR[ti])/RG[ti], da=fabs(azs[i]-PAZ[ti])/AZG[ti];
                if(dr<1.0 && da<1.0){ double d=dr+da; if(d<bestd[i]){ bestd[i]=d; owner[i]=ti; } }
            }
        }
        if(tier){
            for(int i=0;i<m;i++){ tier1[i] = owner[i]>=0; if(!tier1[i]) bestd[i]=1e9; }
        }
    }
    /* ---- update tracks ---- */
    static double gr[RADAR_MAX_POINTS], ga[RADAR_MAX_POINTS], ge[RADAR_MAX_POINTS];
    for(int oi=0;oi<R->nord;oi++){
        int ti=R->ord[oi]; Track *t=&R->tracks[ti];
        int ng=0, nmv_hit=0, any_unk=0; double smax=-1e9;
        for(int i=0;i<m;i++) if(owner[i]==ti){
            gr[ng]=rs[i]; ga[ng]=azs[i]; ge[ng]=els[i];
            if(ismv[i]){                       /* brightness: MOVING points only */
                nmv_hit++;
                if(snrok[i]){ if(snrs[i]>smax) smax=snrs[i]; } else any_unk=1;
            }
            if(srcpt[i]>=0&&srcpt[i]<n) pts[srcpt[i]].tid=t->tid;
            ng++;
        }
        if(ng){
            double mr=med(gr,ng), maz=med(ga,ng), mel=med(ge,ng);
            double jd=hypot(mr-PR[ti], (maz-PAZ[ti])*DEG*mr);
            t->jit=(1-JIT_ALPHA)*t->jit + JIT_ALPHA*jd;
            t->mv_ewma=0.85*t->mv_ewma + 0.15*(double)nmv_hit;
            if(t->mv_ewma>t->max_mv) t->max_mv=t->mv_ewma;
            t->st_frames = (t->mv_ewma>=ST_MV_LO)?0:t->st_frames+1;
            t->r=ALPHA*mr+(1-ALPHA)*PR[ti];
            t->az=ALPHA_AZ*maz+(1-ALPHA_AZ)*PAZ[ti];
            t->el=(1-EL_ALPHA)*t->el + EL_ALPHA*mel;
            if(any_unk) t->snr_unknown=1;      /* no TLV7 -> fail open */
            /* flood brightness belongs to the flood, not to any track */
            if(smax>t->snr_peak
               && !(now_t < R->flood_until && t->r < FLOOD_R+FLOOD_MARGIN))
                t->snr_peak=smax;
            /* half-extents from the frame's points (for the wire box) */
            double mnx=1e9,mxx=-1e9,mny=1e9,mxy=-1e9,mnz=1e9,mxz=-1e9;
            for(int i=0;i<m;i++) if(owner[i]==ti){
                double rr=rs[i], a=azs[i]*DEG, e=els[i]*DEG, rh=rr*cos(e);
                double x=rh*sin(a), y=rh*cos(a), z=rr*sin(e);
                mnx=fmin(mnx,x); mxx=fmax(mxx,x);
                mny=fmin(mny,y); mxy=fmax(mxy,y);
                mnz=fmin(mnz,z); mxz=fmax(mxz,z);
            }
            t->sx=clampd((mxx-mnx)/2, MIN_SIZE_M, MAX_SIZE_M);
            t->sy=clampd((mxy-mny)/2, MIN_SIZE_M, MAX_SIZE_M);
            t->sz=clampd((mxz-mnz)/2, MIN_SIZE_M, MAX_SIZE_M);
            hist_push(t, now_t, t->r, t->az);
            refit_vel(t, now_t);
            hit_push(t, 1); t->misses=0; t->age++; t->hits_total++;
        } else {
            double rad, cross;
            recent_motion(t, now_t, PARK_WIN, &rad, &cross);
            if(t->confirmed && t->moved_frames>=MOVE_CONFIRM
               && (now_t - t->last_moved_t) <= R->park_s
               && rad < PARK_DISP){
                t->vr=0; t->va=0;    /* genuinely-moved target that stopped: hold */
            } else { t->r=PR[ti]; t->az=PAZ[ti]; }
            hit_push(t, 0); t->misses++; t->age++;
            t->mv_ewma*=0.85; if(t->mv_ewma<ST_MV_LO) t->st_frames++;
        }
    }
    /* ---- motion test (range-scaled, NON-latching) ---- */
    for(int oi=0;oi<R->nord;oi++){
        Track *t=&R->tracks[R->ord[oi]];
        double rad, cross;
        recent_motion(t, now_t, DISP_WIN, &rad, &cross);
        double cross_floor = EMIT_DISP;
        { double f = (ANG_FLOOR_DEG*DEG)*t->r; if(f>cross_floor) cross_floor=f; }
        t->moving = (fabs(t->vr)>=EMIT_SPD
                     || fabs(t->va)>=ANG_RATE_MIN
                     || rad>=EMIT_DISP
                     || cross>=cross_floor);
        if(t->moving){ t->moved_frames++; t->last_moved_t=now_t; }
    }
    /* ---- post-confirmation consistency guard ---- */
    for(int oi=0;oi<R->nord;oi++){
        Track *t=&R->tracks[R->ord[oi]];
        if(!t->confirmed) continue;
        double r_path,r_net,r_span,c_path,c_net; int tele;
        int gn = guard_metrics(t, now_t, TELE_DM + TELE_RK*t->r,
                               &r_path,&r_net,&r_span,&c_path,&c_net,&tele);
        int judged = gn>=GUARD_MIN_N && r_span>1e-9;
        double c_move_floor = POS_NET;
        { double f=(ANG_FLOOR_DEG*DEG)*t->r; if(f>c_move_floor) c_move_floor=f; }
        int r_bad=0, spd_bad=0, tele_bad=0, c_bad=0;
        if(judged){
            double r_floor = R_PATH_MIN + R_PATH_RK*t->r;
            r_bad = (r_path>=r_floor && r_net/r_path<COH_MIN);
            spd_bad = (r_path/r_span > GSPD_MAX);
            tele_bad = (tele>=TELE_K);
            double c_floor = CROSS_WANDER_K*(ANG_FLOOR_DEG*DEG)*t->r;
            if(c_floor<r_floor) c_floor=r_floor;
            c_bad = (c_path>=c_floor && c_net/c_path<COH_MIN);
        }
        int jit_bad = t->jit > JIT_BASE + JIT_RK*t->r;
        int bad = r_bad||spd_bad||tele_bad||c_bad||jit_bad;
        t->coh_bad = r_bad||c_bad||spd_bad;
        if(bad){ t->guard_bad++; t->ok_streak=0; }
        else {
            if(t->guard_bad>0) t->guard_bad--;
            int frozen = (now_t < R->flood_until) && (t->r < FLOOD_R+FLOOD_MARGIN);
            if(judged && !frozen && (r_net>=POS_NET || c_net>=c_move_floor))
                t->ok_streak++;
        }
        int need = t->ever_passed ? GUARD_EMIT_RE : GUARD_EMIT;
        if(t->ok_streak>=need){ t->guard_pass=1; t->ever_passed=1; }
        if(t->guard_bad>=GUARD_UNLATCH) t->guard_pass=0;
    }
    /* ---- lifecycle + confirmation (python order) ---- */
    for(int oi=0;oi<R->nord;){
        Track *t=&R->tracks[R->ord[oi]];
        if(!t->confirmed && t->misses>TENT_MAX_MISS){ ord_remove(R,oi); continue; }
        if(t->confirmed && t->guard_bad>=GUARD_KILL){ ord_remove(R,oi); continue; }
        int moved_recently = t->moved_frames>=MOVE_CONFIRM
                             && (now_t - t->last_moved_t) <= R->park_s;
        if(t->confirmed && t->misses>coast_frames){
            if(!(moved_recently && t->misses<=park_frames)){
                if(t->guard_pass && R->ngrave<GRAVE_MAX){
                    Grave *g=&R->grave[R->ngrave++];
                    g->t=now_t; g->r=t->r; g->az=t->az; g->snr_peak=t->snr_peak;
                }
                ord_remove(R,oi); continue;
            }
        }
        /* phantom cull: confirmed but never genuinely moved -> false-alarm cluster */
        if(t->confirmed && !t->moving && !moved_recently
           && (now_t - t->last_moved_t) > PHANTOM_LEASH_S){ ord_remove(R,oi); continue; }
        if(!t->confirmed){
            if(hit_sum_last(t,conf_n)>=conf_m && t->mv_ewma>=MV_RATE_MIN && t->jit<JIT_MAX)
                t->confirmed=1;
            else if(t->hitn>=ST_CONF_N && hit_sum_last(t,ST_CONF_N)>=ST_CONF_M && t->jit<ST_JIT_MAX)
                t->confirmed=1;
        }
        oi++;
    }
    /* purge graveyard */
    {
        int w=0;
        for(int i=0;i<R->ngrave;i++)
            if(now_t - R->grave[i].t <= INHERIT_S) R->grave[w++]=R->grave[i];
        R->ngrave=w;
    }
    /* ---- merge duplicates (keep the elder): stable sort by
     * (confirmed desc, age desc), then pairwise kill later entries ---- */
    for(int i=1;i<R->nord;i++){
        int key=R->ord[i]; Track *tk=&R->tracks[key];
        int j=i-1;
        while(j>=0){
            Track *tj=&R->tracks[R->ord[j]];
            int before = (tk->confirmed>tj->confirmed) ||
                         (tk->confirmed==tj->confirmed && tk->age>tj->age);
            if(before){ R->ord[j+1]=R->ord[j]; j--; } else break;
        }
        R->ord[j+1]=key;
    }
    for(int i=0;i<R->nord;i++){
        Track *a=&R->tracks[R->ord[i]];
        for(int j=i+1;j<R->nord;){
            Track *b=&R->tracks[R->ord[j]];
            double cross=fabs((a->az-b->az)*DEG)*0.5*(a->r+b->r);
            if(fabs(a->r-b->r)<MERGE_R && cross<MERGE_CROSS && fabs(a->vr-b->vr)<R->merge_dv)
                ord_remove(R,j);
            else j++;
        }
    }
    /* ---- seed from unclaimed MOVING points ---- */
    if(m){
        static double sr[RADAR_MAX_POINTS], sa[RADAR_MAX_POINTS], se[RADAR_MAX_POINTS], ss[RADAR_MAX_POINTS];
        static int sk[RADAR_MAX_POINTS]; int ns=0;
        for(int i=0;i<m;i++) if(owner[i]<0 && ismv[i]){
            sr[ns]=rs[i]; sa[ns]=azs[i]; se[ns]=els[i]; ss[ns]=snrs[i]; sk[ns]=snrok[i]; ns++;
        }
        if(ns){
            static int lbl[RADAR_MAX_POINTS]; int k=cluster_pts(sr,sa,ns,SEED_LINK_R,SEED_LINK_CROSS,lbl);
            for(int c=0;c<k;c++){
                double sumr=0,suma=0,smax=-1e9; int cnt=0, any_unk=0;
                static double eb[RADAR_MAX_POINTS]; int ne=0;
                for(int i=0;i<ns;i++) if(lbl[i]==c){
                    sumr+=sr[i]; suma+=sa[i]; eb[ne++]=se[i]; cnt++;
                    if(sk[i]){ if(ss[i]>smax) smax=ss[i]; } else any_unk=1;
                }
                if(cnt<R->min_pts) continue;
                double crr=sumr/cnt, caz=suma/cnt, cel=med(eb,ne);
                int guarded=0;
                for(int oi=0;oi<R->nord;oi++){
                    Track *t=&R->tracks[R->ord[oi]];
                    if(fabs(t->r-crr)<SEED_GUARD_R && fabs(t->az-caz)<SEED_GUARD_AZ){ guarded=1; break; }
                }
                if(guarded) continue;
                int slot=-1;
                for(int ti=0;ti<MAX_TRK;ti++) if(!R->tracks[ti].used){ slot=ti; break; }
                if(slot<0) continue;
                Track *t=&R->tracks[slot]; memset(t,0,sizeof(*t));
                t->used=1; t->tid=R->next_tid++; t->r=crr; t->az=caz; t->el=cel;
                t->sx=t->sy=t->sz=MIN_SIZE_M;
                t->last_moved_t=now_t;
                if(any_unk) t->snr_unknown=1;
                if(smax>-1e9
                   && !(now_t < R->flood_until && crr < FLOOD_R+FLOOD_MARGIN))
                    t->snr_peak=smax;
                /* emission-evidence handoff from a just-died emitting track */
                for(int gi=0;gi<R->ngrave;gi++){
                    Grave *g=&R->grave[gi];
                    double cross=fabs((g->az-caz)*DEG)*0.5*(g->r+crr);
                    if(fabs(g->r-crr)<INHERIT_R && cross<INHERIT_CROSS){
                        t->guard_pass=1; t->ever_passed=1;
                        if(g->snr_peak>t->snr_peak) t->snr_peak=g->snr_peak;
                        break;
                    }
                }
                hist_push(t,now_t,crr,caz); hit_push(t,1); t->age=1; t->hits_total=1;
                R->ord[R->nord++]=slot;
            }
        }
    }
    /* ---- occupancy update ---- */
    double lr = warm ? LEARN_FAST : LEARN;
    static char hitcell[NR][NA]; memset(hitcell,0,sizeof(hitcell));
    for(int i=0;i<nall;i++){ int ir=(int)((ar[i]-GR_R0)/GR_DR), ia=(int)((aa_[i]-GR_A0)/GR_DA);
        if(ir>=0&&ir<NR&&ia>=0&&ia<NA) hitcell[ir][ia]=1; }
    for(int a=0;a<NR;a++) for(int b=0;b<NA;b++){ R->occ[a][b]*=(float)(1.0-DECAY); if(hitcell[a][b]) R->occ[a][b]=(float)(R->occ[a][b]*(1-lr)+lr); }

    /* ---- emit: coherent, bright, genuinely-moving confirmed tracks only ---- */
    static int ci[MAX_TRK]; int ncand=0;
    for(int oi=0;oi<R->nord;oi++){
        int ti=R->ord[oi]; Track *t=&R->tracks[ti];
        if(!t->confirmed) continue;
        if(!t->guard_pass) continue;            /* no positive coherence yet */
        if(!t->snr_unknown && t->snr_peak < snr_req(t->r))
            continue;                           /* not target-bright for THIS range */
        int moved_recently = t->moved_frames>=MOVE_CONFIRM
                             && (now_t - t->last_moved_t) <= R->park_s;
        if(!(t->moving || moved_recently)) continue;   /* static/phantom */
        if(t->misses>EMIT_MAX_MISS && !moved_recently) continue;
        if(fabs(t->az)>R->fov_half || t->r>OUT_R_MAX) continue;
        ci[ncand++]=ti;
    }
    /* stable sort by strength: mv_ewma + 0.001*min(age,500), descending */
    for(int i=1;i<ncand;i++){
        int key=ci[i]; Track *tk=&R->tracks[key];
        double ks=tk->mv_ewma+0.001*(tk->age<500?tk->age:500); int j=i-1;
        while(j>=0){ Track *tj=&R->tracks[ci[j]]; double js=tj->mv_ewma+0.001*(tj->age<500?tj->age:500);
            if(js<ks){ci[j+1]=ci[j];j--;}else break; } ci[j+1]=key;
    }
    int nout=0;
    for(int i=0;i<ncand && nout<max_out;i++){
        Track *t=&R->tracks[ci[i]]; int dup=0;
        /* spatial dedup against already-emitted (recover r,az from x,y) */
        for(int e=0;e<nout;e++){
            double ex=out[e].x, ey=out[e].y, er=hypot(ex,ey), eazd=atan2(ex,ey)/DEG;
            double cross=fabs((eazd-t->az)*DEG)*0.5*(er+t->r);
            if(cross<R->dedup_cross && fabs(er-t->r)<DEDUP_R){ dup=1; break; }
        }
        if(dup) continue;
        double rr=t->r, a=t->az*DEG, e=t->el*DEG, rh=rr*cos(e);
        double vrad=t->vr, vaz=t->va*DEG;
        RadarTarget *o=&out[nout++];
        o->tid=t->tid;
        o->x=(float)(rh*sin(a)); o->y=(float)(rh*cos(a)); o->z=(float)(rr*sin(e));
        o->vx=(float)(vrad*sin(a)+rr*cos(a)*vaz);
        o->vy=(float)(vrad*cos(a)-rr*sin(a)*vaz);
        o->vz=0.0f;
        o->sx=(float)t->sx; o->sy=(float)t->sy; o->sz=(float)t->sz;
        double conf=t->hits_total/10.0; if(conf>1.0)conf=1.0;
        o->conf=(float)conf; o->num_points=t->hits_total;
    }
    return nout;
}

#ifdef CLUSTER_INTROSPECT
/* Bench-tool hook (radar/tools/track_replay.c): confirmed tracks in list
 * order, for parity diffing against the Python reference. Not compiled into
 * the daemon. */
int cluster_confirmed(const RadarClusterer *R, int *tid, double *r, double *az, int max)
{
    int k=0;
    for(int oi=0;oi<R->nord && k<max;oi++){
        const Track *t=&R->tracks[R->ord[oi]];
        if(t->confirmed){ tid[k]=t->tid; r[k]=t->r; az[k]=t->az; k++; }
    }
    return k;
}
int cluster_all(const RadarClusterer *R, int *tid, double *r, double *az, int *conf, int max)
{
    int k=0;
    for(int oi=0;oi<R->nord && k<max;oi++){
        const Track *t=&R->tracks[R->ord[oi]];
        tid[k]=t->tid; r[k]=t->r; az[k]=t->az; conf[k]=t->confirmed; k++;
    }
    return k;
}
int cluster_track_detail(const RadarClusterer *R, int want_tid, double *out7)
{
    for(int oi=0;oi<R->nord;oi++){
        const Track *t=&R->tracks[R->ord[oi]];
        if(t->tid==want_tid){
            out7[0]=t->r; out7[1]=t->az; out7[2]=t->snr_peak;
            out7[3]=t->guard_pass; out7[4]=t->ever_passed;
            out7[5]=t->ok_streak; out7[6]=t->guard_bad;
            return 1;
        }
    }
    return 0;
}
#endif
