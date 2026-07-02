# AIRPOC application — operator console + supervisor (production C). Build on the
# Jetson. `make` produces a single self-contained binary `app` with the web front-end
# embedded.
#
#   sudo apt-get install -y libjpeg-turbo8-dev xxd
#   make                 # real camera (V4L2 EO via eo/pipeline) + radar + illuminator
#   make EO_SRC=stub     # deskside dev: synthetic EO source (no camera needed)
#   ./app -d /dev/video0 -p 8080 -i /dev/sg-ir850

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra -pthread
ILLUM    := ../illuminator/src
EOP      := ../eo/pipeline
CPPFLAGS += -I$(ILLUM) -I$(EOP)
LDLIBS   += -ljpeg -lm

# EO source: real V4L2 datapath (default) or synthetic stub for no-camera dev.
EO_SRC ?= v4l2
ifeq ($(EO_SRC),stub)
  EO_OBJ := eo_frame_stub.o
else
  EO_OBJ := eo_frame_v4l2.o capture.o ae.o isp.o sensor.o
endif

OBJ := main.o gui.o view.o radar_client.o illum.o sg_ir850.o $(EO_OBJ)

app: web_assets.h $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

web_assets.h: web/index.html web/app.css web/app.js gen_assets.sh
	sh gen_assets.sh

gui.o: gui.c gui.h eo_frame.h radar.h view.h $(EOP)/illum.h web_assets.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ gui.c
main.o: main.c eo_frame.h radar.h gui.h $(EOP)/illum.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ main.c
view.o: view.c view.h eo_frame.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ view.c
radar_client.o: radar_client.c radar.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ radar_client.c
eo_frame_stub.o: eo_frame_stub.c eo_frame.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ eo_frame_stub.c
eo_frame_v4l2.o: eo_frame_v4l2.c eo_frame.h $(EOP)/pipeline.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ eo_frame_v4l2.c

# EO datapath objects reused from the eo/pipeline module (built into app/).
capture.o: $(EOP)/capture.c $(EOP)/pipeline.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $(EOP)/capture.c
ae.o: $(EOP)/ae.c $(EOP)/pipeline.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $(EOP)/ae.c
isp.o: $(EOP)/isp.c $(EOP)/pipeline.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $(EOP)/isp.c
sensor.o: $(EOP)/sensor.c $(EOP)/pipeline.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $(EOP)/sensor.c

illum.o: $(EOP)/illum.c $(EOP)/illum.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $(EOP)/illum.c
sg_ir850.o: $(ILLUM)/sg_ir850.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $(ILLUM)/sg_ir850.c

clean:
	rm -f app *.o web_assets.h

.PHONY: clean
