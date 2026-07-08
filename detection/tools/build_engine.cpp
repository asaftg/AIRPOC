/* build_engine.cpp — on-device TensorRT engine builder for the EO detector.
 *
 *   build_engine --onnx m.onnx --out e.engine [--fp16] [--int8 --calib <dir>]
 *
 * FP16 is the baseline; INT8 adds ~2x throughput and is the compute-budget lever
 * for the closing-phase high frame rate. INT8 uses a real entropy calibrator fed
 * the captured Y10 frames through preproc.cu — the SAME preprocessing the runtime
 * uses — so the 8-bit dynamic ranges are faithful (unlike trtexec's blind INT8).
 * The calibration cache is written next to the engine for fast rebuilds.
 * Engines are per-device / per-TRT-version artifacts, kept under /data (gitignored).
 */
#include "preproc.h"
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <dirent.h>

namespace nv = nvinfer1;

/* RTMDet normalization (BGR, 0..255) + Y10->255 scale — identical to infer.cpp. */
static const float MEAN[3] = {103.53f, 116.28f, 123.675f};
static const float STD[3]  = {57.375f, 57.12f, 58.395f};
static const float SCALE   = 0.25f;
#define IN_W 1440
#define IN_H 1088
#define IN_C 3

class Logger : public nv::ILogger {
    void log(Severity s, const char *m) noexcept override {
        if (s <= Severity::kWARNING) fprintf(stderr, "[TRT] %s\n", m);
    }
} gLogger;

/* Feeds captured Y10 frames to the INT8 calibrator, preprocessed on the GPU
 * exactly as the runtime does. One image per batch. */
class Calib : public nv::IInt8EntropyCalibrator2 {
    std::vector<std::string> files;
    size_t idx = 0;
    uint16_t *d_y10 = nullptr;
    float *d_input = nullptr;
    std::vector<uint8_t> host;
    std::string cachePath;
    std::vector<char> cache;
public:
    Calib(std::vector<std::string> f, std::string cp) : files(std::move(f)), cachePath(std::move(cp)) {
        cudaMalloc((void **)&d_y10, (size_t)IN_W * IN_H * 2);
        cudaMalloc((void **)&d_input, (size_t)IN_C * IN_H * IN_W * sizeof(float));
        host.resize((size_t)IN_W * IN_H * 2);
    }
    ~Calib() override { if (d_y10) cudaFree(d_y10); if (d_input) cudaFree(d_input); }
    int getBatchSize() const noexcept override { return 1; }
    bool getBatch(void *bindings[], const char *names[], int nb) noexcept override {
        (void)names; (void)nb;
        if (idx >= files.size()) return false;
        std::ifstream f(files[idx], std::ios::binary);
        if (!f.read((char *)host.data(), (std::streamsize)host.size())) return false;
        cudaMemcpy(d_y10, host.data(), host.size(), cudaMemcpyHostToDevice);
        preproc_y10(d_y10, IN_W, IN_H, d_input, IN_C, IN_H, IN_W, 0, MEAN, STD, SCALE, 0);
        cudaDeviceSynchronize();
        bindings[0] = d_input;
        if ((idx % 25) == 0) fprintf(stderr, "  calib %zu/%zu\n", idx, files.size());
        idx++;
        return true;
    }
    const void *readCalibrationCache(size_t &len) noexcept override {
        std::ifstream f(cachePath, std::ios::binary);
        if (!f) { len = 0; return nullptr; }
        cache.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        len = cache.size();
        return len ? cache.data() : nullptr;
    }
    void writeCalibrationCache(const void *ptr, size_t len) noexcept override {
        std::ofstream f(cachePath, std::ios::binary);
        f.write((const char *)ptr, (std::streamsize)len);
    }
};

static std::vector<std::string> list_y10(const char *dir)
{
    std::vector<std::string> v;
    DIR *d = opendir(dir);
    if (!d) return v;
    struct dirent *e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".y10") v.push_back(std::string(dir) + "/" + n);
    }
    closedir(d);
    std::sort(v.begin(), v.end());
    return v;
}

int main(int argc, char **argv)
{
    const char *onnx = nullptr, *out = nullptr, *calibdir = nullptr;
    bool int8 = false, fp16 = true;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--onnx" && i + 1 < argc) onnx = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--calib" && i + 1 < argc) calibdir = argv[++i];
        else if (a == "--int8") int8 = true;
        else if (a == "--fp16") fp16 = true;
        else if (a == "--no-fp16") fp16 = false;
    }
    if (!onnx || !out) {
        fprintf(stderr, "usage: %s --onnx m.onnx --out e.engine [--fp16] [--int8 --calib dir]\n", argv[0]);
        return 2;
    }

    auto builder = nv::createInferBuilder(gLogger);
    auto network = builder->createNetworkV2(0);   /* explicit batch (TRT10 default) */
    auto parser = nvonnxparser::createParser(*network, gLogger);
    if (!parser->parseFromFile(onnx, (int)nv::ILogger::Severity::kWARNING)) {
        fprintf(stderr, "build_engine: ONNX parse failed for %s\n", onnx);
        return 1;
    }
    auto config = builder->createBuilderConfig();
    config->setMemoryPoolLimit(nv::MemoryPoolType::kWORKSPACE, 1ULL << 30);
    if (fp16) config->setFlag(nv::BuilderFlag::kFP16);

    Calib *calib = nullptr;
    if (int8) {
        if (!calibdir) { fprintf(stderr, "build_engine: --int8 requires --calib <dir>\n"); return 2; }
        std::vector<std::string> files = list_y10(calibdir);
        if (files.empty()) { fprintf(stderr, "build_engine: no *.y10 frames in %s\n", calibdir); return 1; }
        fprintf(stderr, "build_engine: INT8 calibration on %zu frames\n", files.size());
        calib = new Calib(files, std::string(out) + ".calib");
        config->setFlag(nv::BuilderFlag::kINT8);
        config->setInt8Calibrator(calib);
    }

    fprintf(stderr, "build_engine: building (%s%s) — this takes a few minutes...\n",
            fp16 ? "fp16" : "fp32", int8 ? "+int8" : "");
    auto serialized = builder->buildSerializedNetwork(*network, *config);
    if (!serialized) { fprintf(stderr, "build_engine: build failed\n"); return 1; }

    std::ofstream f(out, std::ios::binary);
    f.write((const char *)serialized->data(), (std::streamsize)serialized->size());
    fprintf(stderr, "build_engine: wrote %s (%zu bytes)\n", out, serialized->size());

    delete serialized; delete calib; delete config; delete parser; delete network; delete builder;
    return 0;
}
