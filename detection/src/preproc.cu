/* preproc.cu — see preproc.h. One kernel, no resize (frame == model input size). */
#include "preproc.h"
#include <cuda_runtime.h>
#include <cuda_fp16.h>

/* Per output pixel: unpack Y10 (word>>6, 0..1023), scale to 0..255, then for each
 * of the 3 channels write (v - mean[c]) / std[c]. The mono value is replicated to
 * all channels; only mean/std differ per channel. NCHW layout. */
template <typename T>
__global__ void preproc_kernel(const uint16_t *y10, int src_w,
                               T *dst, int W, int H,
                               float m0, float m1, float m2,
                               float s0, float s1, float s2, float scale)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= W || y >= H) return;

    float v = (float)(y10[(size_t)y * src_w + x] >> 6) * scale;   /* 0..255 */
    int hw = W * H;
    int p = y * W + x;
    dst[0 * hw + p] = (T)((v - m0) / s0);
    dst[1 * hw + p] = (T)((v - m1) / s1);
    dst[2 * hw + p] = (T)((v - m2) / s2);
}

extern "C" int preproc_y10(const uint16_t *d_y10, int src_w, int src_h,
                           void *d_dst, int dst_c, int dst_h, int dst_w, int fp16,
                           const float mean[3], const float std[3], float scale,
                           void *stream)
{
    if (dst_c != 3 || src_w != dst_w || src_h != dst_h) return -1;   /* no resize path */
    cudaStream_t s = (cudaStream_t)stream;
    dim3 block(32, 8);
    dim3 grid((dst_w + block.x - 1) / block.x, (dst_h + block.y - 1) / block.y);
    if (fp16)
        preproc_kernel<__half><<<grid, block, 0, s>>>(
            d_y10, src_w, (__half *)d_dst, dst_w, dst_h,
            mean[0], mean[1], mean[2], std[0], std[1], std[2], scale);
    else
        preproc_kernel<float><<<grid, block, 0, s>>>(
            d_y10, src_w, (float *)d_dst, dst_w, dst_h,
            mean[0], mean[1], mean[2], std[0], std[1], std[2], scale);
    return (cudaGetLastError() == cudaSuccess) ? 0 : -2;
}
