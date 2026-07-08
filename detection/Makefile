# AIRPOC EO detector — on-device build (native aarch64 on the Jetson Orin Nano Super).
#
# Mixed C / C++ / CUDA:
#   C   : main, tap consumer, HTTP/SSE, emit, identity stabilizer   (cc)
#   CUDA: preproc kernel (Y10 -> model input)                        (nvcc)
#   C++ : TensorRT inference (infer), motion worker + ECC (opencv)   (g++)
# OpenCV is linked only into the motion objects; TensorRT/CUDA only where needed.
# Override CUDA_DIR if CUDA isn't at /usr/local/cuda.
CUDA_DIR ?= /usr/local/cuda
NVCC     ?= $(CUDA_DIR)/bin/nvcc
CC       ?= cc
CXX      ?= g++

INC       = -Itap -Isrc
CFLAGS   ?= -O3 -Wall -Wextra -pthread
CFLAGS   += $(INC)
CXXFLAGS ?= -O3 -Wall -Wextra -pthread -std=c++17
CXXFLAGS += $(INC) -I$(CUDA_DIR)/include
NVCCFLAGS ?= -O3 $(INC) -gencode arch=compute_87,code=sm_87

OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4)
OPENCV_LIBS   := $(shell pkg-config --libs opencv4)

LDLIBS = -L$(CUDA_DIR)/lib64 -lnvinfer -lnvinfer_plugin -lcudart \
         -lpthread -lm -lrt $(OPENCV_LIBS)

CSRC   = main.c tap_source.c http.c emit.c stab_identity.c
CXXSRC = infer.cpp motion.cpp stab_ecc.cpp
CUSRC  = preproc.cu
OBJ    = $(CSRC:.c=.o) $(CXXSRC:.cpp=.o) $(CUSRC:.cu=.o)
DEPS   = $(wildcard src/*.h) tap/airpoc_tap.h
BIN    = detectiond

VPATH = src

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)

# C sources (found via VPATH in src/)
%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

# TensorRT/CUDA C++ (no OpenCV)
infer.o: src/infer.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# OpenCV C++ (motion path only)
motion.o: src/motion.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) -c -o $@ $<
stab_ecc.o: src/stab_ecc.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) -c -o $@ $<

# CUDA kernel
preproc.o: src/preproc.cu $(DEPS)
	$(NVCC) $(NVCCFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJ)

.PHONY: all clean
