/* stab_ecc.cpp — image-based stabilizer using OpenCV ECC (Euclidean: translation
 * + rotation). Chosen over feature matching because the 850 nm night frames are
 * low-texture and vignetted, where sparse keypoints are unstable but dense ECC on
 * smooth gradients still converges. Warm-started with the previous warp and
 * iteration-capped; on failure returns identity and reports STAB_FAIL so the
 * worker skips that frame's diff.
 *
 * Direction: findTransformECC(templateImage=cur, inputImage=prev) yields the warp
 * that maps prev onto cur — exactly what the worker applies to prev before diffing.
 */
#include "stab.h"
#include <opencv2/opencv.hpp>

struct Ecc {
    int w, h;
    cv::Mat warm;   /* 2x3 CV_32F warm-start */
};

static int ecc_align(void *self, const uint8_t *prev, const uint8_t *cur, float out[6])
{
    Ecc *st = (Ecc *)self;
    cv::Mat mp(st->h, st->w, CV_8U, (void *)prev);
    cv::Mat mc(st->h, st->w, CV_8U, (void *)cur);
    cv::Mat warp = st->warm.clone();
    cv::TermCriteria tc(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 30, 1e-3);
    int rc = STAB_OK;
    try {
        cv::findTransformECC(mc, mp, warp, cv::MOTION_EUCLIDEAN, tc, cv::noArray(), 5);
    } catch (const cv::Exception &) {
        warp = (cv::Mat_<float>(2, 3) << 1, 0, 0, 0, 1, 0);
        rc = STAB_FAIL;
    }
    st->warm = warp.clone();
    for (int i = 0; i < 6; i++) out[i] = warp.at<float>(i / 3, i % 3);
    return rc;
}

static void ecc_destroy(void *self) { delete (Ecc *)self; }

extern "C" Stabilizer stab_ecc_new(int w, int h)
{
    Ecc *st = new Ecc();
    st->w = w; st->h = h;
    st->warm = (cv::Mat_<float>(2, 3) << 1, 0, 0, 0, 1, 0);
    Stabilizer s;
    s.align = ecc_align;
    s.destroy = ecc_destroy;
    s.self = st;
    return s;
}
