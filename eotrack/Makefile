# eotrack - EO tracker daemon (trackerd) + offline replay bench.
# Pure C11, no external libs beyond pthreads/libm. Same flags as the other AIRPOC
# on-device modules. Builds and runs standalone (guideline 7).

CC      ?= cc
CFLAGS  ?= -O3 -Wall -Wextra -std=c11 -pthread -Isrc -Itap
LDFLAGS ?= -pthread -lm -lrt

SRC := src/main.c src/core.c src/emit.c src/http.c src/det_feed.c \
       src/eo_reader.c src/ego.c src/lock.c
OBJ := $(SRC:.c=.o)

all: trackerd

trackerd: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

# Offline benches + unit tests (no camera/detector/GPU needed).
tools: tools/replay tools/lock_test tools/fake_det

tools/replay: tools/replay.c src/core.c src/emit.c
	$(CC) $(CFLAGS) $^ -o $@ -lm

tools/lock_test: tools/lock_test.c src/lock.c src/ego.c
	$(CC) $(CFLAGS) $^ -o $@ -lm

tools/fake_det: tools/fake_det.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Run the offline validation gates (exits nonzero on any failure).
check: tools/replay tools/lock_test
	./tools/replay
	./tools/lock_test

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) trackerd tools/replay tools/lock_test tools/fake_det

.PHONY: all tools check clean
