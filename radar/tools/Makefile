# Bench tools (host-side, not part of the daemon build).
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lm

track_replay: track_replay.c ../src/cluster.c ../src/cluster.h ../src/radar.h
	$(CC) $(CFLAGS) -o $@ track_replay.c $(LDLIBS)

clean:
	rm -f track_replay

.PHONY: clean
