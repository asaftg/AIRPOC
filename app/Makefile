# AIRPOC application — operator console + supervisor (production C). Build on the
# Jetson. `make` produces a single self-contained binary `app` with the web front-end
# embedded.
#
#   sudo apt-get install -y libjpeg-turbo8-dev xxd
#   make                 # V4L2 EO (via eo/pipeline) + radar + illuminator
#   ./app -d /dev/video0 -p 8080 -i /dev/sg-ir850 -r 127.0.0.1:8092
# No camera present? the app still runs and the GUI shows EO NOT CONNECTED.

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra -pthread
ILLUM    := ../illuminator/src
EOP      := ../eo/pipeline
CPPFLAGS += -I$(ILLUM) -I$(EOP)
LDLIBS   += -ljpeg -lm

OBJ := main.o gui.o radar_client.o illum.o sg_ir850.o \
       eo_frame_v4l2.o capture.o ae.o isp.o sensor.o

app: web_assets.h $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

web_assets.h: web/index.html web/app.css web/app.js gen_assets.sh
	sh gen_assets.sh

gui.o: gui.c gui.h eo_frame.h radar.h $(EOP)/pipeline.h $(EOP)/illum.h web_assets.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ gui.c
main.o: main.c eo_frame.h radar.h gui.h $(EOP)/illum.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ main.c
radar_client.o: radar_client.c radar.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ radar_client.c
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
