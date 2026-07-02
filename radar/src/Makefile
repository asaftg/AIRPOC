# AIRPOC radar previewer — on-device build (native aarch64 on the Jetson).
# No external deps: pure C + pthreads + libm. Data over UART, no DCA.
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -pthread
LDLIBS  = -lpthread -lm

SRC = main.c serial.c cfg_push.c tlv.c cluster.c wire.c http.c sim.c
HDR = radar.h serial.h cfg_push.h tlv.h cluster.h wire.h http.h sim.h
OBJ = $(SRC:.c=.o)
BIN = radar_preview

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJ)

.PHONY: all clean
