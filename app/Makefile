# AIRPOC application — operator console + supervisor (production C). Build on the
# Jetson (V4L2/pthreads/libjpeg-turbo are Linux). `make` produces a single self-
# contained binary `app` with the web front-end embedded.
#
#   sudo apt-get install -y libjpeg-turbo8-dev xxd
#   make
#   ./app -d /dev/video0 -p 8080 -i /dev/sg-ir850

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra -pthread
ILLUM    := ../illuminator/src
EOP      := ../eo/pipeline
CPPFLAGS += -I$(ILLUM) -I$(EOP)
LDLIBS   += -ljpeg -lm

OBJ := main.o gui.o view.o eo_frame_stub.o illum.o sg_ir850.o

app: web_assets.h $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

web_assets.h: web/index.html web/app.css web/app.js gen_assets.sh
	sh gen_assets.sh

gui.o: gui.c gui.h eo_frame.h view.h $(EOP)/illum.h web_assets.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ gui.c
main.o: main.c eo_frame.h gui.h $(EOP)/illum.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ main.c
view.o: view.c view.h eo_frame.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ view.c
eo_frame_stub.o: eo_frame_stub.c eo_frame.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ eo_frame_stub.c
illum.o: $(EOP)/illum.c $(EOP)/illum.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $(EOP)/illum.c
sg_ir850.o: $(ILLUM)/sg_ir850.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $(ILLUM)/sg_ir850.c

clean:
	rm -f app $(OBJ) web_assets.h

.PHONY: clean
