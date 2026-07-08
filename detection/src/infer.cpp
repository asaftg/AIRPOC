/* infer.cpp — TensorRT 10 appearance detector. See infer.h.
 *
 * Generic engine plumbing (works for any single-input engine): enumerate I/O
 * tensors, size device+host buffers from their shapes, run preprocess + enqueue,
 * copy outputs back. The DECODE step is model-specific and lives in decode_boxes()
 * — it targets the RTMDet-tiny export: two outputs, boxes [1,N,4] (xyxy, input
 * pixels) and scores [1,N,num_classes] (post-sigmoid) — then greedy NMS.
 */
#include "infer.h"
#include "preproc.h"
#include "coco.h"
#include <NvInfer.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>

namespace nv = nvinfer1;

/* RTMDet default normalization (mmdet data_preprocessor), BGR order, pixel 0..255.
 * Our mono value maps 0..1023 -> 0..255 via scale 0.25 and is replicated to 3ch. */
static const float RTMDET_MEAN[3] = {103.53f, 116.28f, 123.675f};
static const float RTMDET_STD[3]  = {57.375f, 57.12f, 58.395f};
static const float Y10_TO_255     = 0.25f;

class TrtLogger : public nv::ILogger {
    void log(Severity s, const char *msg) noexcept override {
        if (s <= Severity::kWARNING) fprintf(stderr, "[TRT] %s\n", msg);
    }
};

struct IoBuf {
    std::string name;
    nv::Dims dims;
    size_t vol;          /* element count */
    bool fp16;
    void *dev;           /* device buffer */
    std::vector<float> host;   /* host mirror (fp32) for outputs */
};

struct InferEngine {
    TrtLogger logger;
    nv::IRuntime *runtime = nullptr;
    nv::ICudaEngine *engine = nullptr;
    nv::IExecutionContext *ctx = nullptr;
    cudaStream_t stream = nullptr;

    int in_c = 0, in_h = 0, in_w = 0;
    bool in_fp16 = false;
    IoBuf input;
    uint16_t *d_y10 = nullptr;

    std::vector<IoBuf> outputs;
    int idx_boxes = -1, idx_scores = -1;
    int num_classes = 0;

    char model_name[64] = "rtmdet-t";
    char precision[16] = "fp16";
};

static size_t dims_vol(const nv::Dims &d)
{
    size_t v = 1;
    for (int i = 0; i < d.nbDims; i++) v *= (d.d[i] > 0 ? (size_t)d.d[i] : 1);
    return v;
}

static bool read_file(const char *path, std::vector<char> &buf)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    f.seekg(0);
    buf.resize((size_t)n);
    return (bool)f.read(buf.data(), n);
}

InferEngine *infer_open(const char *engine_path, const char *sidecar_path, char *err, int errcap)
{
    (void)sidecar_path;   /* stock bring-up hardcodes RTMDet norm + COCO names */
    auto fail = [&](const char *m) -> InferEngine * {
        if (err) snprintf(err, errcap, "%s", m);
        return nullptr;
    };

    std::vector<char> blob;
    if (!read_file(engine_path, blob)) return fail("engine file unreadable");

    InferEngine *e = new InferEngine();
    e->runtime = nv::createInferRuntime(e->logger);
    if (!e->runtime) { delete e; return fail("createInferRuntime failed"); }
    e->engine = e->runtime->deserializeCudaEngine(blob.data(), blob.size());
    if (!e->engine) { infer_close(e); return fail("deserialize failed (rebuild engine for this TRT/device)"); }
    e->ctx = e->engine->createExecutionContext();
    if (!e->ctx) { infer_close(e); return fail("createExecutionContext failed"); }
    if (cudaStreamCreate(&e->stream) != cudaSuccess) { infer_close(e); return fail("cudaStreamCreate failed"); }

    int nio = e->engine->getNbIOTensors();
    for (int i = 0; i < nio; i++) {
        const char *name = e->engine->getIOTensorName(i);
        nv::Dims dims = e->engine->getTensorShape(name);
        bool isIn = e->engine->getTensorIOMode(name) == nv::TensorIOMode::kINPUT;
        bool fp16 = e->engine->getTensorDataType(name) == nv::DataType::kHALF;
        if (isIn) {
            e->input.name = name; e->input.dims = dims; e->input.fp16 = fp16;
            e->input.vol = dims_vol(dims);
            /* expect NCHW */
            e->in_c = dims.d[1]; e->in_h = dims.d[2]; e->in_w = dims.d[3];
            e->in_fp16 = fp16;
            snprintf(e->precision, sizeof e->precision, "%s", fp16 ? "fp16" : "fp32");
        } else {
            IoBuf b; b.name = name; b.dims = dims; b.fp16 = fp16; b.vol = dims_vol(dims);
            e->outputs.push_back(b);
        }
    }
    if (e->in_c != 3) { infer_close(e); return fail("engine input is not 3-channel"); }

    /* Identify boxes vs scores outputs by last-dim: 4 => boxes, >4 => scores. */
    for (size_t i = 0; i < e->outputs.size(); i++) {
        int last = e->outputs[i].dims.d[e->outputs[i].dims.nbDims - 1];
        if (last == 4) e->idx_boxes = (int)i;
        else if (last > 4) { e->idx_scores = (int)i; e->num_classes = last; }
    }
    if (e->idx_boxes < 0 || e->idx_scores < 0) { infer_close(e); return fail("expected boxes[.,4]+scores[.,C] outputs"); }

    /* Allocate device buffers + host mirrors; wire tensor addresses. */
    if (cudaMalloc(&e->input.dev, e->input.vol * (e->in_fp16 ? 2 : 4)) != cudaSuccess) { infer_close(e); return fail("cudaMalloc input"); }
    if (cudaMalloc((void **)&e->d_y10, (size_t)e->in_w * e->in_h * 2) != cudaSuccess) { infer_close(e); return fail("cudaMalloc y10"); }
    e->ctx->setTensorAddress(e->input.name.c_str(), e->input.dev);
    for (auto &b : e->outputs) {
        if (cudaMalloc(&b.dev, b.vol * (b.fp16 ? 2 : 4)) != cudaSuccess) { infer_close(e); return fail("cudaMalloc output"); }
        b.host.resize(b.vol);
        e->ctx->setTensorAddress(b.name.c_str(), b.dev);
    }
    return e;
}

/* fp16 (IEEE half) -> float, for host-side output conversion. */
static inline float half_to_float(uint16_t h)
{
    uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, f;
    if (exp == 0) { if (man == 0) f = sign; else { exp = 127 - 15 + 1; while (!(man & 0x400)) { man <<= 1; exp--; } man &= 0x3FF; f = sign | (exp << 23) | (man << 13); } }
    else if (exp == 0x1F) f = sign | 0x7F800000u | (man << 13);
    else f = sign | ((exp + 127 - 15) << 23) | (man << 13);
    float out; memcpy(&out, &f, 4); return out;
}

static void copy_output_to_host(IoBuf &b, cudaStream_t s)
{
    if (b.fp16) {
        std::vector<uint16_t> tmp(b.vol);
        cudaMemcpyAsync(tmp.data(), b.dev, b.vol * 2, cudaMemcpyDeviceToHost, s);
        cudaStreamSynchronize(s);
        for (size_t i = 0; i < b.vol; i++) b.host[i] = half_to_float(tmp[i]);
    } else {
        cudaMemcpyAsync(b.host.data(), b.dev, b.vol * 4, cudaMemcpyDeviceToHost, s);
        cudaStreamSynchronize(s);
    }
}

/* Greedy per-class NMS over decoded candidates. */
struct Cand { int cls; float conf, x1, y1, x2, y2; };

static float iou(const Cand &a, const Cand &b)
{
    float xx1 = std::max(a.x1, b.x1), yy1 = std::max(a.y1, b.y1);
    float xx2 = std::min(a.x2, b.x2), yy2 = std::min(a.y2, b.y2);
    float w = std::max(0.f, xx2 - xx1), h = std::max(0.f, yy2 - yy1);
    float inter = w * h;
    float ua = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return ua > 0 ? inter / ua : 0.f;
}

static int decode_boxes(InferEngine *e, float conf_thresh, float nms_iou,
                        InferBox *out, int max_boxes)
{
    IoBuf &B = e->outputs[e->idx_boxes];   /* [1,N,4] xyxy input-px */
    IoBuf &S = e->outputs[e->idx_scores];  /* [1,N,C] post-sigmoid  */
    int N = (int)(S.vol / e->num_classes);
    int C = e->num_classes;

    std::vector<Cand> cands;
    for (int i = 0; i < N; i++) {
        int best = -1; float bestc = conf_thresh;
        const float *sc = &S.host[(size_t)i * C];
        for (int c = 0; c < C; c++) if (sc[c] > bestc) { bestc = sc[c]; best = c; }
        if (best < 0) continue;
        const float *bx = &B.host[(size_t)i * 4];
        cands.push_back({best, bestc, bx[0], bx[1], bx[2], bx[3]});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.conf > b.conf; });

    std::vector<char> dead(cands.size(), 0);
    int n = 0;
    for (size_t i = 0; i < cands.size() && n < max_boxes; i++) {
        if (dead[i]) continue;
        Cand &a = cands[i];
        for (size_t j = i + 1; j < cands.size(); j++)
            if (!dead[j] && cands[j].cls == a.cls && iou(a, cands[j]) > nms_iou) dead[j] = 1;
        out[n].cls = a.cls;
        out[n].conf = a.conf;
        out[n].cx = 0.5f * (a.x1 + a.x2);
        out[n].cy = 0.5f * (a.y1 + a.y2);
        out[n].w = a.x2 - a.x1;
        out[n].h = a.y2 - a.y1;
        n++;
    }
    return n;
}

int infer_run(InferEngine *e, const uint8_t *host_y10, int w, int h,
              float conf_thresh, float nms_iou, int max_out,
              InferBox *out, int max_boxes, double *infer_ms)
{
    if (w != e->in_w || h != e->in_h) return -1;
    auto t0 = std::chrono::steady_clock::now();

    /* host_y10 is Y10 as little-endian 16-bit words in a byte buffer. */
    cudaMemcpyAsync(e->d_y10, host_y10, (size_t)w * h * 2, cudaMemcpyHostToDevice, e->stream);
    if (preproc_y10(e->d_y10, w, h, e->input.dev, e->in_c, e->in_h, e->in_w,
                    e->in_fp16, RTMDET_MEAN, RTMDET_STD, Y10_TO_255, e->stream) != 0)
        return -1;
    if (!e->ctx->enqueueV3(e->stream)) return -1;
    for (auto &b : e->outputs) copy_output_to_host(b, e->stream);

    auto t1 = std::chrono::steady_clock::now();
    if (infer_ms) *infer_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    int cap = max_out < max_boxes ? max_out : max_boxes;
    return decode_boxes(e, conf_thresh, nms_iou, out, cap);
}

const char *infer_model_name(const InferEngine *e) { return e->model_name; }
const char *infer_precision(const InferEngine *e) { return e->precision; }
int infer_num_classes(const InferEngine *e) { return e->num_classes; }
const char *infer_class_name(const InferEngine *e, int cls)
{
    (void)e;
    return (cls >= 0 && cls < COCO_NUM_CLASSES) ? COCO_CLASSES[cls] : "obj";
}

void infer_close(InferEngine *e)
{
    if (!e) return;
    if (e->d_y10) cudaFree(e->d_y10);
    if (e->input.dev) cudaFree(e->input.dev);
    for (auto &b : e->outputs) if (b.dev) cudaFree(b.dev);
    if (e->stream) cudaStreamDestroy(e->stream);
    if (e->ctx) delete e->ctx;
    if (e->engine) delete e->engine;
    if (e->runtime) delete e->runtime;
    delete e;
}
