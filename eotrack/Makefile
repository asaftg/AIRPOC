# eotrack - EO tracker daemon (trackerd) + offline replay bench.
# C11 core + a C++/TensorRT engaged-target lock (lock_nn.cpp, ViTTrack on the GPU). Links
# TensorRT/CUDA + libjpeg (the NN reads the ISP eo_jpeg frames). Builds standalone.

CC       ?= cc
CXX      ?= g++
CFLAGS   ?= -O3 -Wall -Wextra -std=c11 -pthread -Isrc -Itap
CXXFLAGS ?= -O3 -std=c++17 -pthread -Isrc -Itap -I/usr/local/cuda/include
LDFLAGS  ?= -pthread -lm -lrt -L/usr/local/cuda/lib64 -lnvinfer -lcudart -ljpeg -lstdc++

CSRC := src/main.c src/core.c src/emit.c src/http.c src/det_feed.c \
        src/eo_reader.c src/ego.c
OBJ  := $(CSRC:.c=.o) src/lock_nn.o

all: trackerd

trackerd: $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

src/lock_nn.o: src/lock_nn.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Offline benches + unit tests (the core replay needs no GPU; lock_test covers the legacy
# optical-flow lock which lock.c still provides for reference).
tools: tools/replay tools/fake_det

tools/replay: tools/replay.c src/core.c src/emit.c
	$(CC) $(CFLAGS) $^ -o $@ -lm

tools/fake_det: tools/fake_det.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

check: tools/replay
	./tools/replay

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) trackerd tools/replay tools/fake_det

.PHONY: all tools check clean
