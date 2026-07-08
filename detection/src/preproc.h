/* preproc.h — GPU preprocessing: Y10 camera frame -> model input tensor.
 *
 * The IMX296 frame is Y10 (10-bit) packed in 16-bit LE words, 1440x1088 — which
 * is exactly the model's input size, so there is NO resize: a single per-pixel
 * kernel unpacks (word>>6 -> 0..1023), scales to the model's 0..255 range, and
 * normalizes (x-mean)/std per channel, replicating the single mono channel to
 * the 3 input channels. Output is fp16 or fp32 NCHW, written to a device buffer
 * (the TensorRT input binding). Constants come from the model sidecar.
 *
 * C-clean header (no CUDA types) so it is includable from C and C++. The stream
 * is passed as void* (a cudaStream_t) and cast internally.
 */
#ifndef DET_PREPROC_H
#define DET_PREPROC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Launch the preprocess kernel. Returns 0 on success, non-zero on a CUDA error.
 *   d_y10  : device pointer to the Y10 frame (uint16, src_w*src_h, tightly packed)
 *   d_dst  : device pointer to the model input (dst_c*dst_h*dst_w elements)
 *   fp16   : 1 => write __half, 0 => write float
 *   mean/std : per-channel normalization (model sidecar), applied to the 0..255 value
 *   scale  : maps the 10-bit value to the model range (0.25 for 0..1023 -> 0..255)
 * Requires src dims == dst spatial dims (no resize path in phase-2 bring-up). */
int preproc_y10(const uint16_t *d_y10, int src_w, int src_h,
                void *d_dst, int dst_c, int dst_h, int dst_w, int fp16,
                const float mean[3], const float std[3], float scale,
                void *stream);

#ifdef __cplusplus
}
#endif

#endif /* DET_PREPROC_H */
