/* infer.h — TensorRT appearance detector: Y10 frame in, class boxes out.
 *
 * Loads a prebuilt engine (+ its JSON sidecar: input size, normalization, class
 * names, output layout), runs preprocess (preproc.cu) + enqueue + decode + NMS,
 * and returns boxes in full-resolution pixel coordinates. Engines are built
 * on-device by tools/build_engine and never committed. C-callable so the C main
 * loop drives it.
 */
#ifndef DET_INFER_H
#define DET_INFER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   cls;                 /* class index into the sidecar's class_names */
    float conf;
    float cx, cy, w, h;        /* full-res pixels, centre form */
} InferBox;

typedef struct InferEngine InferEngine;

/* Open an engine + sidecar. Returns NULL on failure (missing/incompatible engine,
 * bad sidecar). *err (if non-NULL) gets a short reason. */
InferEngine *infer_open(const char *engine_path, const char *sidecar_path, char *err, int errcap);

/* Names for /stats + the /stream "model" field. */
const char *infer_model_name(const InferEngine *);
const char *infer_precision(const InferEngine *);
int         infer_num_classes(const InferEngine *);
const char *infer_class_name(const InferEngine *, int cls);

/* Run one frame. host_y10 = the tap payload (Y10 as little-endian 16-bit words,
 * passed as raw bytes; w*h*2 bytes, tightly packed). Writes up to max_out boxes
 * (>= conf_thresh, after NMS), returns the count, or -1 on a CUDA/TRT error.
 * infer_ms (if non-NULL) gets the GPU section time. */
int infer_run(InferEngine *, const uint8_t *host_y10, int w, int h,
              float conf_thresh, float nms_iou, int max_out,
              InferBox *out, int max_boxes, double *infer_ms);

void infer_close(InferEngine *);

#ifdef __cplusplus
}
#endif

#endif /* DET_INFER_H */
