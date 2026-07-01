# AIRPOC EO pipeline — on-device build (native aarch64 on the Jetson).
# Deps: libjpeg-turbo dev headers  ->  sudo apt-get install -y libjpeg-turbo8-dev
ILLUM   = ../../illuminator/src
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -pthread -I$(ILLUM)
LDLIBS   = -ljpeg -lpthread -lm

SRC  = main.c capture.c sensor.c ae.c isp.c mjpeg.c illum.c
HDRS = pipeline.h illum.h $(ILLUM)/sg_ir850.h
OBJ  = $(SRC:.c=.o) sg_ir850.o          # sg_ir850.o built from the illuminator module
BIN  = eo_pipeline

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

sg_ir850.o: $(ILLUM)/sg_ir850.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJ)

.PHONY: all clean
