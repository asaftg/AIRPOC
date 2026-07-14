/* motion.cpp — see motion.h. Rolling-background motion worker.
 * OpenCV confined to this object + stab_ecc. */
#include "motion.h"
#include "stab.h"
#include <opencv2/opencv.hpp>
#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>

#define MOT_MAX_SAMPLES  64      /* ring cap: the background is median of <= this many frames */
#define MOT_MIN_SAMPLES  16      /* need this many before we trust the background */
#define MOT_CONFIRM_MIN  15      /* M-of-N window floor (frames) */
#define MOT_CONFIRM_MAX  60      /* M-of-N window cap — fits in the uint64 hit ring */

/* A persistence track in FULL-res coordinates. `hist` is a rolling bit-ring of the
 * last <=64 frames' hits (bit0 = this frame); a track is reported when its hit
 * count over the confirmation window reaches `m`. */
struct Track {
    float cx, cy, w, h;
    uint64_t hist;
    int age;
    bool matched;
};

struct MotionWorker {
    int fw, fh, down, dw, dh;
    Stabilizer stab;
    std::deque<cv::Mat> ring;   /* down-res luma8 samples spanning the window */
    cv::Mat bg;                 /* current background (down-res luma8); empty until warm */
    long   frame_no;            /* frames seen */
    long   last_recomp;         /* frame_no of last median rebuild */
    long   last_push;           /* frame_no of last ring push */
    std::vector<Track> tracks;
};

extern "C" MotionWorker *motion_new(int full_w, int full_h, int down, int use_ecc)
{
    if (down < 1) down = 1;
    MotionWorker *m = new MotionWorker();
    m->fw = full_w; m->fh = full_h; m->down = down;
    m->dw = full_w / down; m->dh = full_h / down;
    m->stab = use_ecc ? stab_ecc_new(m->dw, m->dh) : stab_identity_new(m->dw, m->dh);
    m->frame_no = 0; m->last_recomp = -1000000; m->last_push = -1000000;
    return m;
}

/* Y10 (16-bit LE words, data in high 10 bits) -> down-res 8-bit luma via INTER_AREA. */
static void to_luma_down(const uint8_t *y10, int w, int h, int down, cv::Mat &out)
{
    cv::Mat m16(h, w, CV_16U, (void *)y10);
    cv::Mat m8;
    m16.convertTo(m8, CV_8U, 1.0 / 256.0);   /* word>>8 : 10-bit -> 8-bit */
    cv::resize(m8, out, cv::Size(w / down, h / down), 0, 0, cv::INTER_AREA);
}

/* Subtract each row's median from a float image: cancels the per-row differential
 * read-noise (the night "line" noise) that a plain difference leaves behind. */
static void destripe_rows(cv::Mat &d)
{
    std::vector<float> row(d.cols);
    for (int y = 0; y < d.rows; y++) {
        float *p = d.ptr<float>(y);
        std::copy(p, p + d.cols, row.begin());
        std::nth_element(row.begin(), row.begin() + d.cols / 2, row.end());
        float med = row[d.cols / 2];
        for (int x = 0; x < d.cols; x++) p[x] -= med;
    }
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

/* Per-pixel median of the ring -> background (down-res luma8). */
static void rebuild_bg(MotionWorker *m)
{
    int n = (int)m->ring.size();
    if (n == 0) return;
    m->bg.create(m->dh, m->dw, CV_8U);
    std::vector<uint8_t> col(n);
    for (int y = 0; y < m->dh; y++) {
        uint8_t *bo = m->bg.ptr<uint8_t>(y);
        for (int x = 0; x < m->dw; x++) {
            for (int i = 0; i < n; i++) col[i] = m->ring[i].ptr<uint8_t>(y)[x];
            std::nth_element(col.begin(), col.begin() + n / 2, col.end());
            bo[x] = col[n / 2];
        }
    }
}

extern "C" int motion_process(MotionWorker *m, const uint8_t *y10, int w, int h,
                              const MotionParams *p, Mover *out, int max_out, int *stab_fail)
{
    if (stab_fail) *stab_fail = 0;
    if (w != m->fw || h != m->fh || !p) return 0;

    cv::Mat cur;
    to_luma_down(y10, w, h, m->down, cur);
    m->frame_no++;

    /* --- rolling background: push a sample every `stride` frames so the ring
     * spans `window_s` seconds with ~MOT_MAX_SAMPLES samples, then rebuild the
     * median a couple times a second. --- */
    double fps = p->fps > 1.0 ? p->fps : 60.0;
    double window_s = p->window_s > 0.1 ? p->window_s : 5.0;
    int stride = (int)std::lround(window_s * fps / MOT_MAX_SAMPLES);
    if (stride < 1) stride = 1;
    if (m->frame_no - m->last_push >= stride) {
        m->ring.push_back(cur.clone());
        while ((int)m->ring.size() > MOT_MAX_SAMPLES) m->ring.pop_front();
        m->last_push = m->frame_no;
    }
    int recomp = (int)std::lround(fps / 2.0); if (recomp < 1) recomp = 1;
    if ((int)m->ring.size() >= MOT_MIN_SAMPLES &&
        (m->bg.empty() || m->frame_no - m->last_recomp >= recomp)) {
        rebuild_bg(m);
        m->last_recomp = m->frame_no;
    }
    if (m->bg.empty()) return 0;   /* background still warming up */

    /* Align the background onto the current frame (identity on a static mount;
     * ECC/ego-motion cancels platform motion on a slewing gimbal). */
    float warp[6];
    int rc = m->stab.align(m->stab.self, m->bg.data, cur.data, warp);
    if (stab_fail) *stab_fail = (rc == STAB_FAIL);
    cv::Mat M = (cv::Mat_<float>(2, 3) << warp[0], warp[1], warp[2], warp[3], warp[4], warp[5]);
    cv::Mat bg_aligned;
    cv::warpAffine(m->bg, bg_aligned, M, cur.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);

    /* Signed difference -> destripe rows -> magnitude. */
    cv::Mat curf, bgf, d;
    cur.convertTo(curf, CV_32F);
    bg_aligned.convertTo(bgf, CV_32F);
    d = curf - bgf;
    destripe_rows(d);
    d = cv::abs(d);
    cv::Mat diff8;
    d.convertTo(diff8, CV_8U);

    /* Kill the invalid border the warp introduces (proportional to translation). */
    int bw = (int)std::ceil(std::fabs(warp[2]) + std::fabs(warp[5])) + 2;
    if (bw > 0 && bw * 2 < diff8.cols && bw * 2 < diff8.rows) {
        diff8(cv::Rect(0, 0, diff8.cols, bw)).setTo(0);
        diff8(cv::Rect(0, diff8.rows - bw, diff8.cols, bw)).setTo(0);
        diff8(cv::Rect(0, 0, bw, diff8.rows)).setTo(0);
        diff8(cv::Rect(diff8.cols - bw, 0, bw, diff8.rows)).setTo(0);
    }

    int med, mad;
    median_mad(diff8, med, mad);
    int thr = (int)(med + p->k_mad * (mad > 0 ? mad : 1));
    if (thr < 8) thr = 8;
    if (thr > 240) thr = 240;

    cv::Mat bin;
    cv::threshold(diff8, bin, thr, 255, cv::THRESH_BINARY);
    cv::morphologyEx(bin, bin, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));

    cv::Mat labels, stats, centroids;
    int nlab = cv::connectedComponentsWithStats(bin, labels, stats, centroids, 8, CV_32S);

    /* --- M-of-N confirmation window sized to ~1 s; hits required scale with the
     * `persist` knob (1..5 -> 20..100 % of the window). --- */
    int N = (int)std::lround(fps);
    if (N < MOT_CONFIRM_MIN) N = MOT_CONFIRM_MIN;
    if (N > MOT_CONFIRM_MAX) N = MOT_CONFIRM_MAX;
    uint64_t nmask = (N >= 64) ? ~0ull : ((1ull << N) - 1);
    int persist = p->persist < 1 ? 1 : p->persist > 5 ? 5 : p->persist;
    int Mreq = (int)std::lround((double)persist / 5.0 * N);
    if (Mreq < 1) Mreq = 1;
    if (Mreq > N) Mreq = N;

    /* age tracks: shift the hit ring, unmatch */
    for (auto &t : m->tracks) { t.hist = (t.hist << 1) & nmask; t.age++; t.matched = false; }

    const float scale = (float)m->down;
    for (int i = 1; i < nlab; i++) {   /* 0 = background */
        int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area < 2) continue;        /* single-pixel specks out (keep small far targets) */
        float bwid = stats.at<int>(i, cv::CC_STAT_WIDTH) * scale;
        float bhei = stats.at<int>(i, cv::CC_STAT_HEIGHT) * scale;
        float cx = (float)centroids.at<double>(i, 0) * scale;
        float cy = (float)centroids.at<double>(i, 1) * scale;
        /* per-frame association gate. The 20 px base is a max target speed at 60 fps;
         * scale it by 60/fps so the SAME real speed is tracked at any process rate
         * (at 15 fps a target moves ~4x more between frames). */
        float gate = std::max(20.0f * (60.0f / (float)fps), 0.5f * std::max(bwid, bhei));

        int best = -1; float bestd = gate;
        for (size_t k = 0; k < m->tracks.size(); k++) {
            if (m->tracks[k].matched) continue;
            float dd = std::hypot(m->tracks[k].cx - cx, m->tracks[k].cy - cy);
            if (dd < bestd) { bestd = dd; best = (int)k; }
        }
        if (best >= 0) {
            Track &t = m->tracks[best];
            t.cx = 0.5f * t.cx + 0.5f * cx; t.cy = 0.5f * t.cy + 0.5f * cy;
            t.w = bwid; t.h = bhei; t.hist |= 1ull; t.matched = true;
        } else {
            Track t; t.cx = cx; t.cy = cy; t.w = bwid; t.h = bhei;
            t.hist = 1ull; t.age = 0; t.matched = true;
            m->tracks.push_back(t);
        }
    }

    /* drop dead tracks (nothing in the confirmation window) */
    m->tracks.erase(std::remove_if(m->tracks.begin(), m->tracks.end(),
                    [nmask](const Track &t) { return (t.hist & nmask) == 0; }), m->tracks.end());

    /* emit confirmed tracks */
    int n = 0;
    for (auto &t : m->tracks) {
        int hits = __builtin_popcountll(t.hist & nmask);
        if (hits >= Mreq && n < max_out) {
            out[n].cx = t.cx; out[n].cy = t.cy; out[n].w = t.w; out[n].h = t.h;
            out[n].conf = (float)hits / (float)N; out[n].age = t.age;
            n++;
        }
    }
    return n;
}

extern "C" void motion_free(MotionWorker *m)
{
    if (!m) return;
    if (m->stab.destroy) m->stab.destroy(m->stab.self);
    delete m;
}
