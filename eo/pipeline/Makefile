# AIRPOC EO pipeline — on-device build (native aarch64 on the Jetson).
# Deps: libjpeg-turbo dev headers  ->  sudo apt-get install -y libjpeg-turbo8-dev
#
#   make            -> libeo.a (the module) + eo_pipeline (the preview/bench tool)
#   make libeo.a    -> just the linkable module (what the GUI links against)
#
# A consumer (e.g. the GUI) links libeo.a and includes eo.h — nothing else.
ILLUM   = ../../illuminator/src
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -pthread -I$(ILLUM)
LDLIBS   = -ljpeg -lpthread -lm

# libeo = the frozen EO module: camera + capture + AE + ISP behind eo.h.
LIBEO_SRC = libeo.c capture.c sensor.c ae.c isp.c
LIBEO_OBJ = $(LIBEO_SRC:.c=.o)
LIBEO     = libeo.a

# preview/bench tool: consumes libeo, adds MJPEG server + illuminator controls.
APP_SRC = main.c mjpeg.c illum.c
APP_OBJ = $(APP_SRC:.c=.o) sg_ir850.o
BIN     = eo_pipeline

HDRS = eo.h eo_bench.h pipeline.h illum.h $(ILLUM)/sg_ir850.h

all: $(LIBEO) $(BIN)

$(LIBEO): $(LIBEO_OBJ)
	ar rcs $@ $^

$(BIN): $(APP_OBJ) $(LIBEO)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJ) $(LIBEO) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

sg_ir850.o: $(ILLUM)/sg_ir850.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(LIBEO) $(LIBEO_OBJ) $(APP_OBJ)

.PHONY: all clean
