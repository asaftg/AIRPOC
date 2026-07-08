/* infer_probe.cpp — offline sanity check for the detector's model path.
 *
 *   ./infer_probe <engine> <image.jpg> [conf=0.3]
 *
 * Runs a single still image through the EXACT runtime path — infer.cpp's
 * preprocess (preproc.cu) + TensorRT engine + raw-head decode + NMS — and prints
 * the boxes it produces. It's the tool that answers "is the model + our decode
 * actually correct?" without needing a person or vehicle in front of the live
 * camera: point it at a known image (e.g. mmdetection's demo.jpg) and confirm it
 * finds the obvious cars/people at sensible confidence and box positions.
 *
 * The image is read as MONO (our sensor is mono) and stretched to the model input
 * size, then packed into Y10 words exactly as the camera tap delivers them, so the
 * probe exercises the same code the daemon runs. It prints the underlying COCO
 * class and the mapped AIRPOC target class for each surviving box (infer_run only
 * returns the target classes — human/vehicle — so non-targets are already dropped).
 *
 * Build:  make tools      (links against infer.o + preproc.o + OpenCV)
 * Note: this is a bench diagnostic, not part of the shipping daemon.
 */
#include "infer.h"
#include "coco.h"
#include <opencv2/opencv.hpp>
#include <vector>
#include <cstdio>
#include <cstdlib>

#define MODEL_W 1440
#define MODEL_H 1088

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <engine> <image> [conf=0.3]\n", argv[0]);
        return 2;
    }
    float conf = argc > 3 ? (float)atof(argv[3]) : 0.3f;

    char err[128] = {0};
    InferEngine *e = infer_open(argv[1], nullptr, err, sizeof err);
    if (!e) { fprintf(stderr, "infer_probe: engine open failed: %s\n", err); return 1; }

    cv::Mat img = cv::imread(argv[2], cv::IMREAD_GRAYSCALE);   /* mono, like the sensor */
    if (img.empty()) { fprintf(stderr, "infer_probe: cannot read image: %s\n", argv[2]); return 1; }
    cv::Mat r;
    cv::resize(img, r, cv::Size(MODEL_W, MODEL_H));

    /* luma L -> Y10 word so the runtime unpack ((word>>6)*0.25) reconstructs L. */
    std::vector<uint16_t> y10((size_t)MODEL_W * MODEL_H);
    for (size_t i = 0; i < y10.size(); i++) y10[i] = (uint16_t)(r.data[i] << 8);

    InferBox boxes[300];
    double ms = 0;
    int n = infer_run(e, (const uint8_t *)y10.data(), MODEL_W, MODEL_H,
                      conf, 0.65f, 300, boxes, 300, &ms);
    printf("model=%s  infer_ms=%.2f (first call incl. warmup)  boxes>%.2f: %d\n",
           infer_model_name(e), ms, conf, n);
    for (int i = 0; i < n && i < 40; i++) {
        const char *coco = (boxes[i].cls >= 0 && boxes[i].cls < COCO_NUM_CLASSES)
                           ? COCO_CLASSES[boxes[i].cls] : "?";
        const char *tgt = coco_to_airpoc(boxes[i].cls);
        printf("  %-8s (%-14s) %.2f  px=(%.0f,%.0f,%.0f,%.0f)\n",
               tgt ? tgt : "-", coco, boxes[i].conf,
               boxes[i].cx, boxes[i].cy, boxes[i].w, boxes[i].h);
    }
    infer_close(e);
    return 0;
}
