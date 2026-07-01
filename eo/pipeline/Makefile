# AIRPOC EO pipeline — on-device build (native aarch64 on the Jetson).
# Deps: libjpeg-turbo dev headers  ->  sudo apt-get install -y libjpeg-turbo8-dev
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -pthread
LDLIBS   = -ljpeg -lpthread -lm

SRC = main.c capture.c sensor.c ae.c isp.c mjpeg.c
OBJ = $(SRC:.c=.o)
BIN = eo_pipeline

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c pipeline.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJ)

.PHONY: all clean
