/* Temporal multi-target tracker — see cluster.h. Port of the offline-validated
 * radar_tracker (radar/tools). Fixed arrays, no heap on the hot path. */
#include "cluster.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG (M_PI/180.0)

/* ---- occupancy grid geometry (fresh-static channel) ---- */
#define NR 92          /* (184-0)/2 range cells */
#define NA 64          /* (32--32)/1 az cells   */
#define GR_R0 0.0
#define GR_DR 2.0
#define GR_A0 (-32.0)
#define GR_DA 1.0
/* ---- per-track buffers ---- */
#define HMAX 160        /* position-history ring */
#define WMAX 12         /* hit/miss window (== st_conf_N) */
#define MAX_TRK RADAR_MAX_TARGETS

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
#define PARK_TRAVEL 4.0
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
#define MIN_SIZE_M 0.25
#define MAX_SIZE_M 3.0

typedef struct {
    int used, tid;
    double r, az, el, vr, va;
    double jit, mv_ewma, max_mv;
    int misses, age, st_frames, hits_total;
    int confirmed, disp_flag;
    double r0, az0;
    double ht[HMAX], hr[HMAX], ha[HMAX]; int hn, hhead;
    int hit[WMAX], hitn;
    double sx, sy, sz;           /* half-extents (m), from the frame's points */
} Track;

struct RadarClusterer {
    Track tracks[MAX_TRK];
    int   next_tid;
    float occ[NR][NA];
    double t0; int have_t0;
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
};

RadarClusterer *cluster_new(void) {
    RadarClusterer *c = calloc(1, sizeof(RadarClusterer));
    if (c) {
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
static double med(double *v, int n){
    static double a[RADAR_MAX_POINTS]; memcpy(a, v, n*sizeof(double));
    qsort(a, n, sizeof(double), dcmp);
    return (n&1) ? a[n/2] : 0.5*(a[n/2-1]+a[n/2]);
}
static void hist_push(Track *t, double tm, double r, double a){
    t->ht[t->hhead]=tm; t->hr[t->hhead]=r; t->ha[t->hhead]=a;
    t->hhead=(t->hhead+1)%HMAX; if(t->hn<HMAX) t->hn++;
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
static double trk_travel(Track *t){
    return hypot(t->r - t->r0, (t->az - t->az0)*DEG * t->r);
}
static double recent_disp(Track *t, double tnow, double win){
    double hr[HMAX], ha[HMAX]; int n=0;
    for(int i=0;i<t->hn;i++){ int idx=(t->hhead-1-i+HMAX)%HMAX; if(tnow-t->ht[idx]<=win){ hr[n]=t->hr[idx]; ha[n]=t->ha[idx]; n++; } }
    if(n<9) return 0.0;
    for(int i=0;i<n/2;i++){ double tr=hr[i]; hr[i]=hr[n-1-i]; hr[n-1-i]=tr; double ta=ha[i]; ha[i]=ha[n-1-i]; ha[n-1-i]=ta; }
    int k=n/3; double r1=0,r2=0,a1=0,a2=0;
    for(int i=0;i<k;i++){ r1+=hr[i]; a1+=ha[i]; }
    for(int i=n-k;i<n;i++){ r2+=hr[i]; a2+=ha[i]; }
    r1/=k; a1/=k; r2/=k; a2/=k;
    double cross=(a2-a1)*DEG*0.5*(r1+r2);
    return hypot(r2-r1, cross);
}
static int trk_displaced(Track *t, double tnow){
    if(t->disp_flag) return 1;
    if(hypot(t->vr, t->va*DEG*t->r) >= EMIT_SPD){ t->disp_flag=1; return 1; }
    if(recent_disp(t, tnow, DISP_WIN) >= EMIT_DISP) t->disp_flag=1;
    return t->disp_flag;
}
static void refit_vel(Track *t, double tnow){
    double tt[HMAX], rr[HMAX], aa[HMAX]; int n=0;
    for(int i=0;i<t->hn;i++){ int idx=(t->hhead-1-i+HMAX)%HMAX; if(tnow-t->ht[idx]<=VEL_WIN){ tt[n]=t->ht[idx]; rr[n]=t->hr[idx]; aa[n]=t->ha[idx]; n++; } }
    if(n<3) return;
    if(tt[0]-tt[n-1] < VEL_MIN_SPAN) return;     /* newest-first: tt[0] latest */
    double mt=0,mr=0,ma=0; for(int i=0;i<n;i++){ mt+=tt[i]; mr+=rr[i]; ma+=aa[i]; } mt/=n; mr/=n; ma/=n;
    double den=0,nr=0,na=0;
    for(int i=0;i<n;i++){ double dm=tt[i]-mt; den+=dm*dm; nr+=dm*(rr[i]-mr); na+=dm*(aa[i]-ma); }
    if(den<=0) return;
    t->vr=clampd(nr/den, -SPEED_MAX, SPEED_MAX);
    double valim=(SPEED_MAX/(t->r>10.0?t->r:10.0))/DEG;
    t->va=clampd(na/den, -valim, valim);
}
/* flood-fill cluster on (r,az); fills label[], returns ncluster */
static int cluster_pts(double *r, double *az, int n, double link_r, double link_cross, int *label){
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

int cluster_step(RadarClusterer *R, RadarPoint *pts, int n,
                 double now_t, double dt, RadarTarget *out, int max_out) {
    if (dt <= 0) dt = 0.05;
    if (!R->have_t0){ R->t0 = now_t; R->have_t0 = 1; }
    int warm = (now_t - R->t0) < WARMUP_S;
    double snr_st = R->snr_mv + 3.0f;
    double fov = R->fov_half; if (fov > AZ_KEEP_CAP) fov = AZ_KEEP_CAP;
    double el_max = R->el_max;                            /* live elmax knob   */
    int conf_m = R->conf_m, conf_n = conf_m + 1;          /* live confirm knob */
    int coast_frames = (int)lround(R->coast_s * TRK_FPS); /* live coast knob   */
    int park_frames  = (int)lround(R->park_s * TRK_FPS);  /* live park knob    */

    for (int i=0;i<n;i++) pts[i].tid = 255;

    /* ---- channel split (index-parallel to pts for tid tagging) ---- */
    static double rs[RADAR_MAX_POINTS], azs[RADAR_MAX_POINTS], els[RADAR_MAX_POINTS];
    static int ismv[RADAR_MAX_POINTS], srcpt[RADAR_MAX_POINTS];
    static double ar[RADAR_MAX_POINTS], aa_[RADAR_MAX_POINTS];
    int m=0, nmv=0, nall=0;
    /* moving first (so ismv split is contiguous), then fresh-static */
    static int st_idx[RADAR_MAX_POINTS]; static double st_r[RADAR_MAX_POINTS], st_a[RADAR_MAX_POINTS], st_e[RADAR_MAX_POINTS]; int nst=0;
    for (int i=0;i<n;i++){
        double r=pts[i].range, az=pts[i].az, el=pts[i].el, v=pts[i].doppler, snr=pts[i].snr;
        if (r<R_MIN || fabs(az)>fov || fabs(el)>el_max) continue;
        ar[nall]=r; aa_[nall]=az; nall++;
        int snr_known = !isnan(snr);
        if (fabs(v)>=R->vmin && (!snr_known || snr>=R->snr_mv)) {
            rs[m]=r; azs[m]=az; els[m]=el; ismv[m]=1; srcpt[m]=i; m++; nmv++;
        } else if (snr_known && snr>=snr_st) {
            st_idx[nst]=i; st_r[nst]=r; st_a[nst]=az; st_e[nst]=el; nst++;
        }
    }
    /* ---- fresh-static: high-SNR returns in historically-empty cells ---- */
    if (!warm && nst){
        static double cr[RADAR_MAX_POINTS], ca[RADAR_MAX_POINTS]; static int cidx[RADAR_MAX_POINTS]; static double ce[RADAR_MAX_POINTS]; int nc=0;
        for (int i=0;i<nst;i++){
            int ir=(int)((st_r[i]-GR_R0)/GR_DR), ia=(int)((st_a[i]-GR_A0)/GR_DA);
            if (ir>=0&&ir<NR&&ia>=0&&ia<NA){
                float nb=0; int i0=ir-1<0?0:ir-1,i1=ir+1>=NR?NR-1:ir+1,j0=ia-1<0?0:ia-1,j1=ia+1>=NA?NA-1:ia+1;
                for(int a=i0;a<=i1;a++) for(int b=j0;b<=j1;b++) if(R->occ[a][b]>nb) nb=R->occ[a][b];
                if (nb<OCC_FREE){ cr[nc]=st_r[i]; ca[nc]=st_a[i]; ce[nc]=st_e[i]; cidx[nc]=st_idx[i]; nc++; }
            }
        }
        if (nc){
            static int lbl[RADAR_MAX_POINTS]; int k=cluster_pts(cr,ca,nc,SEED_LINK_R,SEED_LINK_CROSS,lbl);
            static int cnt[RADAR_MAX_POINTS]; for(int c=0;c<k;c++) cnt[c]=0; for(int i=0;i<nc;i++) cnt[lbl[i]]++;
            for(int i=0;i<nc;i++) if(cnt[lbl[i]]>=ST_MIN_PTS && m<RADAR_MAX_POINTS){
                rs[m]=cr[i]; azs[m]=ca[i]; els[m]=ce[i]; ismv[m]=0; srcpt[m]=cidx[i]; m++;
            }
        }
    }

    /* ---- predictions ---- */
    static double PR[MAX_TRK], PAZ[MAX_TRK], RG[MAX_TRK], AZG[MAX_TRK];
    for (int ti=0;ti<MAX_TRK;ti++){
        if(!R->tracks[ti].used) continue;
        Track *t=&R->tracks[ti];
        double pr=t->r+t->vr*dt, paz=t->az+t->va*dt;
        double g=1.0+MISS_GROW*t->misses; if(g>MISS_GROW_CAP)g=MISS_GROW_CAP;
        double cs=fabs(t->va*DEG)*pr;
        double rg=(GATE_R+fabs(t->vr)*SPEED_GATE_S)*g;
        double cg=(GATE_CROSS+cs*SPEED_GATE_S)*g;
        double azg=atan2(cg, pr>5.0?pr:5.0)/DEG;
        double lo=AZ_GATE_MIN*g; if(azg<lo)azg=lo; if(azg>AZ_GATE_MAX)azg=AZ_GATE_MAX;
        PR[ti]=pr; PAZ[ti]=paz; RG[ti]=rg; AZG[ti]=azg;
    }
    /* ---- per-point nearest-track association ---- */
    static int owner[RADAR_MAX_POINTS]; static double bestd[RADAR_MAX_POINTS];
    for(int i=0;i<m;i++){ owner[i]=-1; bestd[i]=1e9; }
    for(int ti=0;ti<MAX_TRK;ti++){
        if(!R->tracks[ti].used) continue;
        int st_ok = R->tracks[ti].max_mv >= ST_SUSTAIN_MV;
        for(int i=0;i<m;i++){
            if(!ismv[i] && !st_ok) continue;
            double dr=fabs(rs[i]-PR[ti])/RG[ti], da=fabs(azs[i]-PAZ[ti])/AZG[ti];
            if(dr<1.0 && da<1.0){ double d=dr+da; if(d<bestd[i]){ bestd[i]=d; owner[i]=ti; } }
        }
    }
    /* ---- update tracks ---- */
    static double gr[RADAR_MAX_POINTS], ga[RADAR_MAX_POINTS], ge[RADAR_MAX_POINTS];
    for(int ti=0;ti<MAX_TRK;ti++){
        if(!R->tracks[ti].used) continue;
        Track *t=&R->tracks[ti]; int ng=0, nmv_hit=0;
        for(int i=0;i<m;i++) if(owner[i]==ti){ gr[ng]=rs[i]; ga[ng]=azs[i]; ge[ng]=els[i]; if(ismv[i])nmv_hit++; if(srcpt[i]>=0&&srcpt[i]<n) pts[srcpt[i]].tid=t->tid; ng++; }
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
            if(t->confirmed && t->disp_flag && trk_travel(t)>=PARK_TRAVEL
               && recent_disp(t,now_t,PARK_WIN)<PARK_DISP){ t->vr=0; t->va=0; }
            else { t->r=PR[ti]; t->az=PAZ[ti]; }
            hit_push(t, 0); t->misses++; t->age++;
            t->mv_ewma*=0.85; if(t->mv_ewma<ST_MV_LO) t->st_frames++;
        }
    }
    /* ---- lifecycle + confirmation ---- */
    for(int ti=0;ti<MAX_TRK;ti++){
        Track *t=&R->tracks[ti]; if(!t->used) continue;
        if(!t->confirmed && t->misses>TENT_MAX_MISS){ t->used=0; continue; }
        if(t->confirmed && t->misses>coast_frames){
            int parked = t->disp_flag && t->vr==0.0 && t->va==0.0 && t->misses<=park_frames;
            if(!parked){ t->used=0; continue; }
        }
        if(!t->confirmed){
            if(hit_sum_last(t,conf_n)>=conf_m && t->mv_ewma>=MV_RATE_MIN && t->jit<JIT_MAX) t->confirmed=1;
            else if(t->hitn>=ST_CONF_N && hit_sum_last(t,ST_CONF_N)>=ST_CONF_M && t->jit<ST_JIT_MAX) t->confirmed=1;
        }
    }
    /* ---- merge duplicates (keep the elder: confirmed first, then oldest) ---- */
    for(int i=0;i<MAX_TRK;i++){
        Track *a=&R->tracks[i]; if(!a->used) continue;
        for(int j=0;j<MAX_TRK;j++){
            if(i==j) continue;
            Track *b=&R->tracks[j]; if(!b->used) continue;
            /* keep a if a is (confirmed>b) or (same conf and older/equal age) */
            int a_better = (a->confirmed>b->confirmed) ||
                           (a->confirmed==b->confirmed && (a->age>b->age || (a->age==b->age && i<j)));
            if(!a_better) continue;
            double cross=fabs((a->az-b->az)*DEG)*0.5*(a->r+b->r);
            if(fabs(a->r-b->r)<MERGE_R && cross<MERGE_CROSS && fabs(a->vr-b->vr)<R->merge_dv)
                b->used=0;
        }
    }
    /* ---- seed from unclaimed MOVING points ---- */
    if(m){
        static double sr[RADAR_MAX_POINTS], sa[RADAR_MAX_POINTS], se[RADAR_MAX_POINTS]; int ns=0;
        for(int i=0;i<m;i++) if(owner[i]<0 && ismv[i]){ sr[ns]=rs[i]; sa[ns]=azs[i]; se[ns]=els[i]; ns++; }
        if(ns){
            static int lbl[RADAR_MAX_POINTS]; int k=cluster_pts(sr,sa,ns,SEED_LINK_R,SEED_LINK_CROSS,lbl);
            for(int c=0;c<k;c++){
                double sumr=0,suma=0; int cnt=0; static double eb[RADAR_MAX_POINTS]; int ne=0;
                for(int i=0;i<ns;i++) if(lbl[i]==c){ sumr+=sr[i]; suma+=sa[i]; eb[ne++]=se[i]; cnt++; }
                if(cnt<R->min_pts) continue;
                double crr=sumr/cnt, caz=suma/cnt, cel=med(eb,ne);
                int guarded=0;
                for(int ti=0;ti<MAX_TRK;ti++) if(R->tracks[ti].used && fabs(R->tracks[ti].r-crr)<SEED_GUARD_R && fabs(R->tracks[ti].az-caz)<SEED_GUARD_AZ){ guarded=1; break; }
                if(guarded) continue;
                for(int ti=0;ti<MAX_TRK;ti++) if(!R->tracks[ti].used){
                    Track *t=&R->tracks[ti]; memset(t,0,sizeof(*t));
                    t->used=1; t->tid=R->next_tid++; t->r=crr; t->az=caz; t->el=cel; t->r0=crr; t->az0=caz;
                    t->sx=t->sy=t->sz=MIN_SIZE_M;
                    hist_push(t,now_t,crr,caz); hit_push(t,1); t->age=1; t->hits_total=1;
                    break;
                }
            }
        }
    }
    /* ---- occupancy update ---- */
    double lr = warm ? LEARN_FAST : LEARN;
    static char hitcell[NR][NA]; memset(hitcell,0,sizeof(hitcell));
    for(int i=0;i<nall;i++){ int ir=(int)((ar[i]-GR_R0)/GR_DR), ia=(int)((aa_[i]-GR_A0)/GR_DA);
        if(ir>=0&&ir<NR&&ia>=0&&ia<NA) hitcell[ir][ia]=1; }
    for(int a=0;a<NR;a++) for(int b=0;b<NA;b++){ R->occ[a][b]*=(float)(1.0-DECAY); if(hitcell[a][b]) R->occ[a][b]=(float)(R->occ[a][b]*(1-lr)+lr); }

    /* ---- emit: confirmed + in-band, strength-sorted, spatial dedup ---- */
    static int ci[MAX_TRK]; int ncand=0;
    for(int ti=0;ti<MAX_TRK;ti++){
        Track *t=&R->tracks[ti]; if(!t->used) continue;
        int okmiss = t->misses<=EMIT_MAX_MISS || (t->disp_flag && t->vr==0.0 && t->va==0.0 && t->misses<=park_frames);
        if(t->confirmed && okmiss && trk_displaced(t,now_t)
           && fabs(t->az)<=R->fov_half && t->r<=OUT_R_MAX) ci[ncand++]=ti;
    }
    for(int i=1;i<ncand;i++){ int key=ci[i]; Track *tk=&R->tracks[key];
        double ks=tk->mv_ewma+0.001*(tk->age<500?tk->age:500); int j=i-1;
        while(j>=0){ Track *tj=&R->tracks[ci[j]]; double js=tj->mv_ewma+0.001*(tj->age<500?tj->age:500);
            if(js<ks){ci[j+1]=ci[j];j--;}else break; } ci[j+1]=key; }
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
