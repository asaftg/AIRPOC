/* motion.cpp — see motion.h. OpenCV confined to this object + stab_ecc. */
#include "motion.h"
#include "stab.h"
#include <opencv2/opencv.hpp>
#include <vector>
#include <cmath>

/* A persistence track in FULL-res coordinates. hist is a 5-bit ring of the last
 * five frames' hits; a track is reported when its hit count reaches `persist`. */
struct Track {
    float cx, cy, w, h;
    uint8_t hist;     /* bit0 = this frame */
    int age;          /* frames since created */
    bool matched;     /* claimed by a blob this frame */
};

struct MotionWorker {
    int fw, fh, down, dw, dh;
    Stabilizer stab;
    cv::Mat prev;     /* down-res luma8 */
    bool have_prev;
    std::vector<Track> tracks;
};

extern "C" MotionWorker *motion_new(int full_w, int full_h, int down, int use_ecc)
{
    if (down < 1) down = 1;
    MotionWorker *m = new MotionWorker();
    m->fw = full_w; m->fh = full_h; m->down = down;
    m->dw = full_w / down; m->dh = full_h / down;
    m->stab = use_ecc ? stab_ecc_new(m->dw, m->dh) : stab_identity_new(m->dw, m->dh);
    m->have_prev = false;
    return m;
}

/* Y10 (16-bit LE words in a byte buffer, data in high 10 bits) -> down-res 8-bit
 * luma via INTER_AREA. */
static void to_luma_down(const uint8_t *y10, int w, int h, int down, cv::Mat &out)
{
    cv::Mat m16(h, w, CV_16U, (void *)y10);
    cv::Mat m8;
    m16.convertTo(m8, CV_8U, 1.0 / 256.0);   /* word>>8 : 10-bit -> 8-bit */
    cv::resize(m8, out, cv::Size(w / down, h / down), 0, 0, cv::INTER_AREA);
}

/* Median and MAD of an 8-bit single-channel image via a 256-bin histogram. */
static void median_mad(const cv::Mat &img, int &median, int &mad)
{
    int hist[256] = {0};
    int n = img.rows * img.cols;
    for (int y = 0; y < img.rows; y++) {
        const uint8_t *r = img.ptr<uint8_t>(y);
        for (int x = 0; x < img.cols; x++) hist[r[x]]++;
    }
    int acc = 0; median = 0;
    for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= n / 2) { median = i; break; } }
    int mhist[256] = {0};
    for (int i = 0; i < 256; i++) mhist[std::abs(i - median)] += hist[i];
    acc = 0; mad = 0;
    for (int i = 0; i < 256; i++) { acc += mhist[i]; if (acc >= n / 2) { mad = i; break; } }
}

extern "C" int motion_process(MotionWorker *m, const uint8_t *y10, int w, int h,
                              double k_mad, int persist, Mover *out, int max_out, int *stab_fail)
{
    if (stab_fail) *stab_fail = 0;
    if (w != m->fw || h != m->fh) return 0;

    cv::Mat cur;
    to_luma_down(y10, w, h, m->down, cur);
    if (!m->have_prev) { m->prev = cur.clone(); m->have_prev = true; return 0; }

    float warp[6];
    int rc = m->stab.align(m->stab.self, m->prev.data, cur.data, warp);
    if (stab_fail) *stab_fail = (rc == STAB_FAIL);

    cv::Mat M = (cv::Mat_<float>(2, 3) << warp[0], warp[1], warp[2], warp[3], warp[4], warp[5]);
    cv::Mat prev_aligned;
    cv::warpAffine(m->prev, prev_aligned, M, cur.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);

    cv::Mat diff;
    cv::absdiff(cur, prev_aligned, diff);

    /* Kill the invalid border the warp introduces (proportional to translation). */
    int bw = (int)std::ceil(std::fabs(warp[2]) + std::fabs(warp[5])) + 2;
    if (bw > 0 && bw * 2 < diff.cols && bw * 2 < diff.rows) {
        diff(cv::Rect(0, 0, diff.cols, bw)).setTo(0);
        diff(cv::Rect(0, diff.rows - bw, diff.cols, bw)).setTo(0);
        diff(cv::Rect(0, 0, bw, diff.rows)).setTo(0);
        diff(cv::Rect(diff.cols - bw, 0, bw, diff.rows)).setTo(0);
    }

    int med, mad;
    median_mad(diff, med, mad);
    int thr = (int)(med + k_mad * (mad > 0 ? mad : 1));
    if (thr < 8) thr = 8;
    if (thr > 240) thr = 240;

    cv::Mat bin;
    cv::threshold(diff, bin, thr, 255, cv::THRESH_BINARY);
    cv::morphologyEx(bin, bin, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));

    cv::Mat labels, stats, centroids;
    int nlab = cv::connectedComponentsWithStats(bin, labels, stats, centroids, 8, CV_32S);

    /* age tracks: shift history, unmatch */
    for (auto &t : m->tracks) { t.hist = (uint8_t)((t.hist << 1) & 0x1F); t.age++; t.matched = false; }

    const float scale = (float)m->down;
    for (int i = 1; i < nlab; i++) {   /* 0 = background */
        int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area < 2) continue;
        float bx = stats.at<int>(i, cv::CC_STAT_LEFT) * scale;
        float by = stats.at<int>(i, cv::CC_STAT_TOP) * scale;
        float bwid = stats.at<int>(i, cv::CC_STAT_WIDTH) * scale;
        float bhei = stats.at<int>(i, cv::CC_STAT_HEIGHT) * scale;
        float cx = (float)centroids.at<double>(i, 0) * scale;
        float cy = (float)centroids.at<double>(i, 1) * scale;
        float gate = std::max(20.0f, 0.5f * std::max(bwid, bhei));

        int best = -1; float bestd = gate;
        for (size_t k = 0; k < m->tracks.size(); k++) {
            if (m->tracks[k].matched) continue;
            float d = std::hypot(m->tracks[k].cx - cx, m->tracks[k].cy - cy);
            if (d < bestd) { bestd = d; best = (int)k; }
        }
        if (best >= 0) {
            Track &t = m->tracks[best];
            t.cx = 0.5f * t.cx + 0.5f * cx; t.cy = 0.5f * t.cy + 0.5f * cy;
            t.w = bwid; t.h = bhei; t.hist |= 1; t.matched = true;
            (void)bx; (void)by;
        } else {
            Track t; t.cx = cx; t.cy = cy; t.w = bwid; t.h = bhei;
            t.hist = 1; t.age = 0; t.matched = true;
            m->tracks.push_back(t);
        }
    }

    /* drop dead tracks (nothing in the last 5 frames) */
    m->tracks.erase(std::remove_if(m->tracks.begin(), m->tracks.end(),
                    [](const Track &t) { return (t.hist & 0x1F) == 0; }), m->tracks.end());

    /* emit confirmed tracks */
    int n = 0;
    for (auto &t : m->tracks) {
        int hits = __builtin_popcount(t.hist & 0x1F);
        if (hits >= persist && n < max_out) {
            out[n].cx = t.cx; out[n].cy = t.cy; out[n].w = t.w; out[n].h = t.h;
            out[n].conf = hits / 5.0f; out[n].age = t.age;
            n++;
        }
    }

    m->prev = cur.clone();
    return n;
}

extern "C" void motion_free(MotionWorker *m)
{
    if (!m) return;
    if (m->stab.destroy) m->stab.destroy(m->stab.self);
    delete m;
}
