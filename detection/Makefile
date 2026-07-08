# AIRPOC EO detector — on-device build (native aarch64 on the Jetson Orin Nano).
#
# Phase 1 is pure C + pthreads (tap consumer + HTTP/SSE), no external deps.
# Later phases extend this file:
#   - preproc.cu   -> nvcc, TensorRT preprocess kernel
#   - infer.cpp    -> -lnvinfer -lnvinfer_plugin -lcudart  (TRT engine + NMS)
#   - motion.c / stab_ecc.cpp -> `pkg-config --libs opencv4` (motion worker only)
# Keep OpenCV/CUDA confined to the objects that need them so the core stays lean.
CC     ?= cc
CFLAGS ?= -O3 -Wall -Wextra -pthread
CFLAGS += -I../tap                 # vendored airpoc_tap.h (recorder tap protocol)
LDLIBS  = -lpthread -lm -lrt       # -lrt: POSIX shm for the tap

SRC = main.c tap_source.c http.c emit.c
HDR = config.h source.h http.h emit.h ../tap/airpoc_tap.h
OBJ = $(SRC:.c=.o)
BIN = detectiond

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJ)

.PHONY: all clean
