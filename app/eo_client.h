/* EO feed consumer — subscribe to the EO module's MJPEG feed (eo/pipeline, :8091),
 * keep the latest JPEG + its /stats, forward controls to its /ctl. The app does no
 * capture/ISP/AE/encode — it proxies. See eo_client.c. */
#ifndef AIRPOC_EO_CLIENT_H
#define AIRPOC_EO_CLIENT_H
#include <stdint.h>

int  eo_start(const char *host_port);   /* e.g. "127.0.0.1:8091"; NULL = default */
void eo_stop(void);

/* Latest JPEG frame: copies it into buf (cap bytes), returns its length or 0 if none.
 * *seq increments per new frame (for the /stream fan-out). */
int  eo_get_jpeg(unsigned char *buf, int cap, uint64_t *seq);
int  eo_connected(void);                /* 1 if the EO feed is up + delivering */
int  eo_last_len(void);                 /* size of the latest JPEG (for a link estimate) */
int  eo_get_stats(char *buf, int cap);  /* the EO feed's /stats JSON (fov/fps/zoom/illum) */
void eo_ctl(const char *query);         /* forward "zoom=4"/"laser=1"/... to the feed */

#endif /* AIRPOC_EO_CLIENT_H */
