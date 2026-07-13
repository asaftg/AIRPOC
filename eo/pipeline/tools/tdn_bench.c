/* tdn_bench — offline validation of the night denoiser (tdn.c) against a recorded
 * eo_y10 AIREC channel. Runs the EXACT shipped code (links tdn.o + isp.o), so what
 * passes here is what ships. Bench tool, not part of the pipeline.
 *
 *   ./tdn_bench /data/recordings/<sid>/eo_y10 [out_dir]
 *
 * Replays every frame (unpacking the recorder's y10p 4px->5B packing to the pipeline's
 * u16-LE layout), feeds tdn_process with the RECORDED per-frame applied AE meta, and:
 *   - prints per-second noise metrics: raw vs denoised (Laplacian MAD, row-banding std)
 *   - prints tdn cost (ms/frame) and gate/global state transitions
 *   - writes side-by-side tonemapped PGMs (raw | denoised) every ~5 s to out_dir
 */
#include "../tdn.h"
#include "../pipeline.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC_MAGIC 0x30434552u
#pragma pack(push, 1)
typedef struct { uint32_t magic, crc; uint64_t seq, tsrc, tpub; uint32_t len, flags, meta[6]; } RecHdr;
typedef struct { uint64_t seq, tns; uint32_t seg, off, len, flags; } IdxRow;
#pragma pack(pop)

static int W = 1440, H = 1088;

/* recorder y10p (4 px -> 5 B, p0 = bits 0..9) -> pipeline u16 LE (10-bit in [15:6]) */
static void unpack_y10p(const uint8_t *in, int stride, uint8_t *out)
{
    for (int y = 0; y < H; y++) {
        const uint8_t *r = in + (size_t)y * stride;
        uint16_t *o = (uint16_t *)(out + (size_t)y * W * 2);
        for (int g = 0; g < W / 4; g++) {
            const uint8_t *p = r + g * 5;
            uint64_t v = (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16
                       | (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32;
            o[g*4+0] = (uint16_t)(((v >>  0) & 0x3FF) << 6);
            o[g*4+1] = (uint16_t)(((v >> 10) & 0x3FF) << 6);
            o[g*4+2] = (uint16_t)(((v >> 20) & 0x3FF) << 6);
            o[g*4+3] = (uint16_t)(((v >> 30) & 0x3FF) << 6);
        }
    }
}

/* Laplacian MAD in a centre patch + row-banding std, on 10-bit values */
static void noise_metrics(const uint16_t *img, int q5, double *lap_mad, double *row_std)
{
    int shr = q5 ? 5 : 6;                       /* -> 10-bit ints */
    int x0 = W/2 - 100, y0 = H/2 - 100, n = 0;
    static float lap[198 * 198];
    for (int y = 1; y < 199; y++)
        for (int x = 1; x < 199; x++) {
            #define PX(dx,dy) (int)(img[(size_t)(y0+y+(dy))*W + x0+x+(dx)] >> shr)
            lap[n++] = (float)(4*PX(0,0) - PX(-1,0) - PX(1,0) - PX(0,-1) - PX(0,1));
            #undef PX
        }
    /* median-of-absolute-deviations, coarse (histogram) */
    double mean = 0; for (int i = 0; i < n; i++) mean += lap[i]; mean /= n;
    static int hist[4096];
    memset(hist, 0, sizeof hist);
    for (int i = 0; i < n; i++) { int a = (int)fabsf(lap[i] - (float)mean); hist[a > 4095 ? 4095 : a]++; }
    int acc = 0, med = 0;
    for (int v = 0; v < 4096; v++) { acc += hist[v]; if (acc >= n / 2) { med = v; break; } }
    *lap_mad = med;
    /* row banding: std of per-row mean minus 31-row boxcar */
    static double rm[2048];
    for (int y = 0; y < H; y++) {
        double s = 0; for (int x = 0; x < W; x += 4) s += img[(size_t)y*W + x] >> shr;
        rm[y] = s / (W / 4);
    }
    double sum = 0, sum2 = 0; int cnt = 0;
    for (int y = 16; y < H - 16; y++) {
        double b = 0; for (int k = -15; k <= 15; k++) b += rm[y + k];
        double d = rm[y] - b / 31; sum += d; sum2 += d * d; cnt++;
    }
    *row_std = sqrt(sum2 / cnt - (sum / cnt) * (sum / cnt));
}

static void write_pgm(const char *path, const uint8_t *a, const uint8_t *b)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", W * 2 + 8, H);
    for (int y = 0; y < H; y++) {
        fwrite(a + (size_t)y * W, 1, W, f);
        static const uint8_t gap[8] = {255,255,255,255,255,255,255,255};
        fwrite(gap, 1, 8, f);
        fwrite(b + (size_t)y * W, 1, W, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <session>/eo_y10 [out_dir]\n", argv[0]); return 1; }
    const char *dir = argv[1], *out = argc > 2 ? argv[2] : "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/index.bin", dir);
    FILE *fi = fopen(path, "rb");
    if (!fi) { perror("index.bin"); return 1; }

    uint8_t  *raw   = malloc((size_t)W * H * 2);      /* pipeline-layout raw        */
    uint16_t *dn    = malloc((size_t)W * H * 2);      /* denoised Q10.5             */
    uint8_t  *tm_a  = malloc((size_t)W * H);          /* tonemapped raw             */
    uint8_t  *tm_b  = malloc((size_t)W * H);          /* tonemapped denoised        */
    uint8_t  *pay   = malloc(4u << 20);
    FILE *fs = NULL; int cur_seg = -1;

    long nframes = 0, nactive = 0, nglobal_est = 0;
    double ms_sum = 0, ms_max = 0;
    IdxRow row;
    while (fread(&row, sizeof row, 1, fi) == 1) {
        if ((int)row.seg != cur_seg) {
            if (fs) fclose(fs);
            snprintf(path, sizeof path, "%s/data.%05u.airec", dir, row.seg);
            fs = fopen(path, "rb");
            if (!fs) { perror(path); break; }
            cur_seg = (int)row.seg;
        }
        fseek(fs, row.off, SEEK_SET);
        RecHdr h;
        if (fread(&h, sizeof h, 1, fs) != 1 || h.magic != REC_MAGIC) continue;
        if (h.len > (4u << 20) || fread(pay, 1, h.len, fs) != h.len) continue;

        unpack_y10p(pay, (int)(h.len / H), raw);
        int exp_lines = (int)h.meta[1], gain = (int)h.meta[2];
        int q5 = tdn_process(raw, W * 2, W, H, dn, exp_lines, gain);
        double ms = tdn_last_ms();
        if (q5) { nactive++; ms_sum += ms; if (ms > ms_max) ms_max = ms; }
        nframes++;

        if (nframes % 60 == 0) {                       /* once per recorded second */
            double lm_a, rs_a, lm_b = 0, rs_b = 0;
            noise_metrics((const uint16_t *)raw, 0, &lm_a, &rs_a);
            if (q5) noise_metrics(dn, 1, &lm_b, &rs_b);
            printf("f=%6ld gain=%3d exp=%5d active=%d  lapMAD %5.1f -> %5.1f   rowband %5.3f -> %5.3f   tdn %.2f ms\n",
                   nframes, gain, exp_lines, q5, lm_a, lm_b, rs_a, rs_b, ms);
        }
        if (nframes % 300 == 0 && q5) {                /* side-by-side every ~5 s */
            isp_scale_tonemap(raw, W * 2, 0, 0, W, H, tm_a, W, H, 0);
            isp_scale_tonemap((const uint8_t *)dn, W * 2, 0, 0, W, H, tm_b, W, H, 1);
            snprintf(path, sizeof path, "%s/tdn_%06ld.pgm", out, nframes);
            write_pgm(path, tm_a, tm_b);
            printf("  wrote %s (L=raw R=denoised, same tonemap)\n", path);
        }
    }
    printf("\nframes=%ld  denoiser active=%ld (%.0f%%)  cost avg %.2f ms  max %.2f ms  (est globals %ld)\n",
           nframes, nactive, nframes ? 100.0 * nactive / nframes : 0, nactive ? ms_sum / nactive : 0, ms_max, nglobal_est);
    if (fs) fclose(fs);
    fclose(fi);
    return 0;
}
