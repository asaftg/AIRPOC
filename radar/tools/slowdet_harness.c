/* harness.c — validate slowdet.c + fuse.c against recorded fixtures using the
 * REAL radar.h types.  .bin frame: double t_s, int32 n, then n*float32[r,az,el,dop,snr] */
#define _POSIX_C_SOURCE 199309L
#include "../src/slowdet.h"
#include "../src/fuse.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG (M_PI/180.0)
#define CAP 4096

static int read_frame(FILE *f, double *t, RadarPoint *p, int max) {
    int n;
    if (fread(t, sizeof(double), 1, f) != 1) return -1;
    if (fread(&n, sizeof(int), 1, f) != 1) return -1;
    if (n < 0 || n > CAP) return -1;
    float buf[CAP*5];
    if ((int)fread(buf, sizeof(float)*5, n, f) != n) return -1;
    int m = n < max ? n : max;
    for (int i = 0; i < m; i++) {
        float r=buf[i*5], az=buf[i*5+1], el=buf[i*5+2], dop=buf[i*5+3], snr=buf[i*5+4];
        float horiz = r*cosf(el*(float)DEG);
        p[i].range=r; p[i].az=az; p[i].el=el; p[i].doppler=dop; p[i].snr=snr; p[i].noise=0;
        p[i].x=horiz*sinf(az*(float)DEG); p[i].y=horiz*cosf(az*(float)DEG); p[i].z=r*sinf(el*(float)DEG);
        p[i].tid=255;
    }
    return m;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr,"usage: %s scene.bin [--eljit]\n",argv[0]); return 2; }
    FILE *f = fopen(argv[1],"rb"); if (!f){ perror("open"); return 1; }
    int eljit = argc>2;

    SlowDet *sd = slowdet_new();
    Fuse *fz = fuse_new();
    RadarPoint pts[CAP];
    RadarTarget S[RADAR_MAX_TARGETS], OUT[RADAR_MAX_TARGETS];

    long frames=0, sdecl=0, fout=0;
    double t; int n;
    /* elevation-jitter accumulators: raw S vs fused-and-smoothed */
    double jr=0, js=0; long jn=0;
    static float prev_raw[4096], prev_sm[4096]; static int prev_tid[4096]; int nprev=0;

    struct timespec c0,c1; clock_gettime(CLOCK_MONOTONIC,&c0);
    while ((n = read_frame(f,&t,pts,CAP)) >= 0) {
        int ns = slowdet_step(sd, pts, n, t, S, RADAR_MAX_TARGETS);
        int no = fuse_step(fz, NULL, 0, S, ns, t, OUT, RADAR_MAX_TARGETS);
        frames++; sdecl += ns; fout += no;
        if (eljit) {
            /* match by tid to previous frame, accumulate |d el| raw vs smoothed */
            for (int i=0;i<ns;i++){
                float h=sqrtf(S[i].x*S[i].x+S[i].y*S[i].y);
                float elr=(float)(atan2(S[i].z,h)/DEG);
                for (int k=0;k<no;k++) if (OUT[k].tid==S[i].tid){
                    float h2=sqrtf(OUT[k].x*OUT[k].x+OUT[k].y*OUT[k].y);
                    float els=(float)(atan2(OUT[k].z,h2)/DEG);
                    for (int j=0;j<nprev;j++) if (prev_tid[j]==S[i].tid){
                        jr+=fabs(elr-prev_raw[j]); js+=fabs(els-prev_sm[j]); jn++; break; }
                    prev_raw[i]=elr; prev_sm[i]=els; prev_tid[i]=S[i].tid; break;
                }
            }
            nprev=ns;
        }
    }
    clock_gettime(CLOCK_MONOTONIC,&c1);
    double ms=(c1.tv_sec-c0.tv_sec)*1e3+(c1.tv_nsec-c0.tv_nsec)/1e6;
    printf("frames %ld  S-declared %ld  fused-out %ld  wall %.0fms  %.1f us/frame\n",
           frames, sdecl, fout, ms, frames?ms*1000.0/frames:0.0);
    if (eljit && jn)
        printf("  elevation frame-to-frame jump: raw S %.2f deg -> fused %.2f deg  (n=%ld)\n",
               jr/jn, js/jn, jn);
    slowdet_free(sd); fuse_free(fz); fclose(f);
    return 0;
}
