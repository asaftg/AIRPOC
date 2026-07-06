# recorder — record & replay daemon. Build on the Jetson (aarch64):
#   make            -> airpoc_recorder, tap_bench
#   make selftest   -> run pack10/tap unit checks host-side
CC      ?= gcc
ARCH    := $(shell uname -m)
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99
ifeq ($(ARCH),aarch64)
CFLAGS  += -march=armv8.2-a+crc
endif
LDFLAGS := -pthread -lrt -ljpeg -lm

# eo_tonemap.c is the recorder's own copy of the EO feed's tone map (kept
# identical, drift-alarmed at runtime) — see eo_tonemap.h. Self-contained; the
# recorder does not reach into the eo/ module tree.
SRC := main.c channel.c session.c store.c disk.c events.c http.c \
       pack10.c library.c thumbs.c replay.c render.c eo_tonemap.c transcode.c radar_movie.c
OBJ := $(SRC:.c=.o)

all: airpoc_recorder tap_bench

airpoc_recorder: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

eo_tonemap.o: eo_tonemap.c eo_tonemap.h
	$(CC) $(CFLAGS) -c eo_tonemap.c -o $@

%.o: %.c recorder.h ../tap/airpoc_tap.h
	$(CC) $(CFLAGS) -c $< -o $@

tap_bench: ../tools/tap_bench.c ../tap/airpoc_tap.h
	$(CC) $(CFLAGS) -o $@ $< -pthread -lrt

clean:
	rm -f $(OBJ) airpoc_recorder tap_bench

.PHONY: all clean
