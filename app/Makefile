# AIRPOC operator console — a self-contained proxy (no libjpeg, no eo/pipeline, no
# illuminator link; it consumes the sensor modules' feeds over HTTP). Build anywhere.
#
#   sudo apt-get install -y xxd
#   make
#   ./app -p 8080 -e 127.0.0.1:8091 -r 127.0.0.1:8092
# EO / radar feed down? the console shows NOT CONNECTED (no synthetic data).

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -pthread

OBJ := main.o gui.o eo_client.o radar_client.o det_client.o trk_client.o

app: web_assets.h $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

web_assets.h: web/index.html web/app.css web/app.js gen_assets.sh
	sh gen_assets.sh

gui.o: gui.c gui.h eo_client.h radar.h det.h trk.h web_assets.h
	$(CC) $(CFLAGS) -c -o $@ gui.c
main.o: main.c eo_client.h radar.h det.h trk.h gui.h
	$(CC) $(CFLAGS) -c -o $@ main.c
eo_client.o: eo_client.c eo_client.h
	$(CC) $(CFLAGS) -c -o $@ eo_client.c
radar_client.o: radar_client.c radar.h
	$(CC) $(CFLAGS) -c -o $@ radar_client.c
det_client.o: det_client.c det.h
trk_client.o: trk_client.c trk.h
	$(CC) $(CFLAGS) -c -o $@ det_client.c

clean:
	rm -f app *.o web_assets.h

.PHONY: clean
