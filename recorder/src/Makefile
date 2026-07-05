# recorder — record & replay daemon. Build on the Jetson (aarch64):
#   make            -> airpoc_recorder, tap_bench
#   make selftest   -> run pack10/tap unit checks host-side
CC      ?= gcc
ARCH    := $(shell uname -m)
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99
ifeq ($(ARCH),aarch64)
CFLAGS  += -march=armv8.2-a+crc
endif
LDFLAGS := -pthread -lrt -ljpeg

SRC := main.c channel.c session.c store.c disk.c events.c http.c \
       pack10.c library.c thumbs.c replay.c
OBJ := $(SRC:.c=.o)

all: airpoc_recorder tap_bench

airpoc_recorder: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c recorder.h ../tap/airpoc_tap.h
	$(CC) $(CFLAGS) -c $< -o $@

tap_bench: ../tools/tap_bench.c ../tap/airpoc_tap.h
	$(CC) $(CFLAGS) -o $@ $< -pthread -lrt

clean:
	rm -f $(OBJ) airpoc_recorder tap_bench

.PHONY: all clean
