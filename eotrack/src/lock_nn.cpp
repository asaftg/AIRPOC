/* lock_nn.cpp - the engaged-target lock as a learned SOT tracker (ViTTrack), run on the
 * Jetson GPU via TensorRT. Replaces the optical-flow lock (lock.c): optical flow has no
 * model of WHICH object is the target, so it slides onto neighbouring similar vehicles
 * during a pan (proven in the offline rig). ViTTrack matches the target's learned
 * appearance and holds it through clutter - validated on recording 113321 where it stayed
 * on the sedan while optical flow drifted off.
 *
 * Same C interface as lock.h so the daemon is unchanged: lock_anchor() = tracker init on
 * the detector's engaged box; lock_track() = tracker update, returns the box centre + a
 * confidence score. The detector anchors ACQUISITION; the NN owns HOLDING.
 *
 * Algorithm ported exactly from OpenCV's TrackerVit (Apache-2.0): template 128x128 crop at
 * factor 2, search 256x256 crop at factor 4 around the last box, ImageNet normalisation,
 * three 16x16 heads (confidence/offset/size), Hann-weighted argmax, box decode. Inference
 * ~1 ms on the Orin GPU; only runs while a target is engaged.
 */
#include "lock.h"
#include "config.h"
#include "airpoc_tap.h"
#include <NvInfer.h>
#include <cuda_runtime.h>
#include <jpeglib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <vector>
#include <string>
#include <csetjmp>

namespace nv = nvinfer1;

static const int TEMPL = 128, SEARCH = 256, OW = 16;   /* head grids are 16x16 */
static const float MEANV[3] = {0.485f, 0.456f, 0.406f};
static const float STDV[3]  = {0.229f, 0.224f, 0.225f};
static const float SCORE_THRESH = 0.20f;

class Logger : public nv::ILogger {
    void log(Severity s, const char *msg) noexcept override {
        if (s <= Severity::kWARNING) fprintf(stderr, "[trt] %s\n", msg);
    }
};

struct Lock {
    Logger logger;
    nv::IRuntime *runtime = nullptr;
    nv::ICudaEngine *engine = nullptr;
    nv::IExecutionContext *ctx = nullptr;
    cudaStream_t stream = nullptr;
    /* device buffers */
    float *d_templ = nullptr, *d_search = nullptr;   /* inputs  */
    float *d_o1 = nullptr, *d_o2 = nullptr, *d_o3 = nullptr; /* outputs (conf/size/offset) */
    /* host staging */
    std::vector<float> h_templ, h_search, h_o1, h_o2, h_o3;
    float hann[OW * OW];
    /* tracker state (box in frame px) */
    double rx, ry, rw, rh;   /* rect top-left + size */
    int has = 0;
    float last_score = 0;
};

/* ---- bilinear sample of the uint16 Y10 frame, scaled to 8-bit-ish [0,255] ---- */
static inline float y10_at(const uint16_t *f, int w, int h, double x, double y)
{
    if (x < 0) x = 0; else if (x > w - 1) x = w - 1;
    if (y < 0) y = 0; else if (y > h - 1) y = h - 1;
    int x0 = (int)x, y0 = (int)y;
    int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    double ax = x - x0, ay = y - y0;
    const uint16_t *r0 = f + (size_t)y0 * w, *r1 = f + (size_t)y1 * w;
    double a = r0[x0] + ((double)r0[x1] - r0[x0]) * ax;
    double b = r1[x0] + ((double)r1[x1] - r1[x0]) * ax;
    /* input is the ISP-processed 8-bit gray frame (from the eo_jpeg tap, decoded), widened
     * to uint16 (values 0-255) - the tonemapped frames the NN was validated on. Raw eo_y10
     * (16-bit linear) does NOT work: the NN needs the ISP look. */
    return (float)(a + (b - a) * ay);
}

/* Crop a square region of side ceil(sqrt(rw*rh)*factor) centred on the box, resize to `out`
 * (128 or 256), normalise (ImageNet), write an NCHW float blob. Returns the crop side in
 * frame px (needed to map the box back). Out-of-frame samples clamp to the edge (~=0-pad
 * after normalisation is close enough; edge-clamp avoids hard borders on partial targets). */
static int crop_blob(const uint16_t *frame, int w, int h,
                     double bx, double by, double bw, double bh, int factor,
                     int out, std::vector<float> &blob)
{
    int crop_sz = (int)ceil(sqrt(bw * bh) * factor);
    if (crop_sz < 8) crop_sz = 8;
    double cx = bx + bw / 2.0, cy = by + bh / 2.0;
    double x1 = cx - crop_sz / 2.0, y1 = cy - crop_sz / 2.0;
    double step = (double)crop_sz / out;
    blob.assign((size_t)3 * out * out, 0.f);
    for (int oy = 0; oy < out; oy++) {
        double sy = y1 + (oy + 0.5) * step;
        for (int ox = 0; ox < out; ox++) {
            double sx = x1 + (ox + 0.5) * step;
            float g = y10_at(frame, w, h, sx, sy) / 255.0f;    /* [0,1] */
            for (int c = 0; c < 3; c++)
                blob[(size_t)c * out * out + (size_t)oy * out + ox] = (g - MEANV[c]) / STDV[c];
        }
    }
    return crop_sz;
}

static void hann2d(float *win, int n)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            float wi = 0.5f * (1.f - cosf(2.f * (float)M_PI / (n + 1) * (i + 1)));
            float wj = 0.5f * (1.f - cosf(2.f * (float)M_PI / (n + 1) * (j + 1)));
            win[i * n + j] = wi * wj;
        }
}

static std::string find_engine()
{
    const char *env = getenv("AIRPOC_VIT_ENGINE");
    if (env && *env) return env;
    const char *cands[] = { "models/vit_fp16.engine", "/opt/airpoc/vit_fp16.engine",
                            "/tmp/vit_fp16.engine" };
    for (const char *c : cands) { std::ifstream f(c); if (f.good()) return c; }
    return "models/vit_fp16.engine";
}

extern "C" {

Lock *lock_new(void)
{
    Lock *l = new Lock();
    std::string path = find_engine();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.good()) { fprintf(stderr, "lock_nn: engine not found: %s\n", path.c_str()); delete l; return nullptr; }
    std::streamsize sz = f.tellg(); f.seekg(0);
    std::vector<char> blob(sz);
    if (!f.read(blob.data(), sz)) { delete l; return nullptr; }
    l->runtime = nv::createInferRuntime(l->logger);
    l->engine = l->runtime->deserializeCudaEngine(blob.data(), blob.size());
    if (!l->engine) { fprintf(stderr, "lock_nn: deserialize failed\n"); delete l; return nullptr; }
    l->ctx = l->engine->createExecutionContext();
    cudaStreamCreate(&l->stream);
    l->h_templ.resize(3 * TEMPL * TEMPL);
    l->h_search.resize(3 * SEARCH * SEARCH);
    l->h_o1.resize(OW * OW); l->h_o2.resize(2 * OW * OW); l->h_o3.resize(2 * OW * OW);
    cudaMalloc(&l->d_templ, l->h_templ.size() * 4);
    cudaMalloc(&l->d_search, l->h_search.size() * 4);
    cudaMalloc(&l->d_o1, l->h_o1.size() * 4);
    cudaMalloc(&l->d_o2, l->h_o2.size() * 4);
    cudaMalloc(&l->d_o3, l->h_o3.size() * 4);
    l->ctx->setTensorAddress("template", l->d_templ);
    l->ctx->setTensorAddress("search",   l->d_search);
    l->ctx->setTensorAddress("output1",  l->d_o1);
    l->ctx->setTensorAddress("output2",  l->d_o2);
    l->ctx->setTensorAddress("output3",  l->d_o3);
    hann2d(l->hann, OW);
    return l;
}

void lock_free(Lock *l)
{
    if (!l) return;
    cudaFree(l->d_templ); cudaFree(l->d_search);
    cudaFree(l->d_o1); cudaFree(l->d_o2); cudaFree(l->d_o3);
    if (l->stream) cudaStreamDestroy(l->stream);
    delete l->ctx; delete l->engine; delete l->runtime;
    delete l;
}

void lock_reset(Lock *l) { if (l) l->has = 0; }
int  lock_has_template(const Lock *l) { return l && l->has; }

/* init the tracker on the detector's engaged box (cx,cy,bw,bh centre+size) */
void lock_anchor(Lock *l, const uint16_t *frame, int w, int h,
                 double cx, double cy, double bw, double bh)
{
    if (!l) return;
    l->rx = cx - bw / 2.0; l->ry = cy - bh / 2.0; l->rw = bw; l->rh = bh;
    crop_blob(frame, w, h, l->rx, l->ry, l->rw, l->rh, 2, TEMPL, l->h_templ);
    cudaMemcpyAsync(l->d_templ, l->h_templ.data(), l->h_templ.size() * 4, cudaMemcpyHostToDevice, l->stream);
    cudaStreamSynchronize(l->stream);
    l->has = 1;
}

/* update: run ViTTrack, write the matched centre (ox,oy) + score in [0,1]. Returns 1. The
 * ego args are unused (the NN tracks appearance directly, no motion prior needed). */
int lock_track(Lock *l, const uint16_t *frame, int w, int h,
               double ego_dx, double ego_dy, double *ox, double *oy, double *score)
{
    (void)ego_dx; (void)ego_dy;
    if (!l || !l->has) return 0;
    int crop_sz = crop_blob(frame, w, h, l->rx, l->ry, l->rw, l->rh, 4, SEARCH, l->h_search);
    cudaMemcpyAsync(l->d_search, l->h_search.data(), l->h_search.size() * 4, cudaMemcpyHostToDevice, l->stream);
    if (!l->ctx->enqueueV3(l->stream)) { if (score) *score = 0; return 1; }
    cudaMemcpyAsync(l->h_o1.data(), l->d_o1, l->h_o1.size() * 4, cudaMemcpyDeviceToHost, l->stream);
    cudaMemcpyAsync(l->h_o2.data(), l->d_o2, l->h_o2.size() * 4, cudaMemcpyDeviceToHost, l->stream);
    cudaMemcpyAsync(l->h_o3.data(), l->d_o3, l->h_o3.size() * 4, cudaMemcpyDeviceToHost, l->stream);
    cudaStreamSynchronize(l->stream);

    /* conf * Hann, argmax */
    int bi = 0; float bv = -1e9f;
    for (int i = 0; i < OW * OW; i++) {
        float v = l->h_o1[i] * l->hann[i];
        if (v > bv) { bv = v; bi = i; }
    }
    l->last_score = l->h_o1[bi];   /* raw confidence at the peak (pre-Hann) */
    int mr = bi / OW, mc = bi % OW;
    /* output2 = size, output3 = offset (per OpenCV TrackerVit) */
    float off_x = l->h_o3[0 * OW * OW + bi];
    float off_y = l->h_o3[1 * OW * OW + bi];
    float sw    = l->h_o2[0 * OW * OW + bi];
    float sh    = l->h_o2[1 * OW * OW + bi];
    float ncx = (mc + off_x) / (float)OW;   /* normalised [0,1] in the search crop */
    float ncy = (mr + off_y) / (float)OW;

    /* Only update the box when the match is confident. On a low score the target is
     * occluded/left; keep the last box (OpenCV returns false here) so a bad decode can't
     * blow the box up and run it off-screen. The caller HOLDs on the low score. */
    if (l->last_score >= SCORE_THRESH) {
        double x0 = l->rx + (l->rw - crop_sz) / 2.0;
        double y0 = l->ry + (l->rh - crop_sz) / 2.0;
        l->rx = (ncx - sw / 2.0) * crop_sz + x0;
        l->ry = (ncy - sh / 2.0) * crop_sz + y0;
        l->rw = sw * crop_sz; l->rh = sh * crop_sz;
    }
    if (ox) *ox = l->rx + l->rw / 2.0;
    if (oy) *oy = l->ry + l->rh / 2.0;
    if (score) *score = l->last_score;
    return 1;
}

/* ---- eo_jpeg frame source: the NN needs the ISP-tonemapped 8-bit frames (raw eo_y10 does
 * not work). The eo module publishes them JPEG-encoded on airpoc.eo_jpeg; read + decode to
 * gray, widened to uint16 so it drops into the same lock loop that fed eo_y10. ---- */
struct JpegSrc {
    AirTapSub sub;
    int ok = 0;
    std::vector<uint8_t> jbuf;   /* raw JPEG bytes */
    std::vector<uint8_t> gray;   /* decoded gray8  */
};

struct JpegErr { struct jpeg_error_mgr mgr; jmp_buf jb; };
static void jpeg_err_exit(j_common_ptr c) { longjmp(((JpegErr*)c->err)->jb, 1); }

JpegSrc *jpeg_src_open(const char *tap)
{
    JpegSrc *s = new JpegSrc();
    s->jbuf.resize(4u * 1024 * 1024);          /* generous JPEG cap */
    s->gray.resize((size_t)EO_IMG_W * EO_IMG_H);
    s->ok = tap_open(&s->sub, tap);
    if (!s->ok) fprintf(stderr, "jpeg_src: tap_open %s failed\n", tap);
    return s;
}
void jpeg_src_close(JpegSrc *s) { if (s) delete s; }
int  jpeg_src_connected(const JpegSrc *s) { return s && s->ok; }

/* Copy the newest JPEG frame, decode to gray, widen into `out` (uint16, values 0-255).
 * Mirrors eo_latest(): returns 1 on a new frame, 0 if none new. */
int jpeg_src_latest(JpegSrc *s, uint16_t *out, size_t cap,
                    int *w, int *h, uint64_t *seq, uint64_t *t_src_ns, uint32_t meta[6])
{
    if (!s || !s->ok) return 0;
    AirTapRec rec; int got = 0;
    /* drain to the newest record */
    while (tap_read(&s->sub, s->jbuf.data(), (uint32_t)s->jbuf.size(), &rec) == 1) got = 1;
    if (!got) return 0;

    JpegErr jerr; struct jpeg_decompress_struct ci;
    ci.err = jpeg_std_error(&jerr.mgr); jerr.mgr.error_exit = jpeg_err_exit;
    if (setjmp(jerr.jb)) { jpeg_destroy_decompress(&ci); return 0; }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, s->jbuf.data(), rec.payload_len);
    if (jpeg_read_header(&ci, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&ci); return 0; }
    ci.out_color_space = JCS_GRAYSCALE;
    jpeg_start_decompress(&ci);
    int iw = ci.output_width, ih = ci.output_height;
    if ((size_t)iw * ih > s->gray.size()) { jpeg_abort_decompress(&ci); jpeg_destroy_decompress(&ci); return 0; }
    while ((int)ci.output_scanline < ih) {
        uint8_t *row = s->gray.data() + (size_t)ci.output_scanline * iw;
        jpeg_read_scanlines(&ci, &row, 1);
    }
    jpeg_finish_decompress(&ci); jpeg_destroy_decompress(&ci);

    size_t n = (size_t)iw * ih; if (n > cap) n = cap;
    for (size_t k = 0; k < n; k++) out[k] = s->gray[k];   /* widen 8->16, no shift */
    if (w) *w = iw; if (h) *h = ih;
    if (seq) *seq = rec.seq;
    if (t_src_ns) *t_src_ns = rec.t_src_ns;
    if (meta) memcpy(meta, rec.meta, 6 * sizeof(uint32_t));
    return 1;
}

} /* extern "C" */
