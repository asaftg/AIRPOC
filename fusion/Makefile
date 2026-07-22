# fusion - late-fusion daemon (fusiond) + offline replay benches.
# Pure C11, no external libs beyond pthreads/libm. Same flags as the other
# AIRPOC on-device modules. Builds and runs standalone (guideline 7).

CC      ?= cc
CFLAGS  ?= -O3 -Wall -Wextra -std=c11 -pthread -Isrc -Itap
LDFLAGS ?= -pthread -lm -lrt

SRC := src/main.c src/core.c src/emit.c src/http.c src/trim.c \
       src/sse_client.c src/rad_parse.c src/trk_parse.c
OBJ := $(SRC:.c=.o)

all: fusiond

fusiond: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

# Offline benches + unit tests (no radar/camera/GPU needed).
tools: tools/replay tools/wire_replay tools/fake_feeds

tools/replay: tools/replay.c src/core.c src/emit.c
	$(CC) $(CFLAGS) $^ -o $@ -lm

tools/wire_replay: tools/wire_replay.c src/core.c src/emit.c src/rad_parse.c src/trk_parse.c
	$(CC) $(CFLAGS) $^ -o $@ -lm

tools/fake_feeds: tools/fake_feeds.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Run the offline validation gates (exits nonzero on any failure).
# Tier 1: synthetic scenarios through the pure core.
# Tier 2: a real recorded radar_wire + trk_wire pair through the real parsers.
# Tier 3: two-daemon loopback smoke (fake SSE feeds -> fusiond -> endpoints).
check: tools fusiond
	./tools/replay
	gunzip -kf tools/fixtures/radar_wire.jsonl.gz tools/fixtures/trk_wire.jsonl.gz
	./tools/wire_replay tools/fixtures/radar_wire.jsonl tools/fixtures/trk_wire.jsonl
	./tools/smoke.sh

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) fusiond tools/replay tools/wire_replay tools/fake_feeds \
	      tools/fixtures/radar_wire.jsonl tools/fixtures/trk_wire.jsonl

.PHONY: all tools check clean
