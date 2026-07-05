/* Operator GUI server: the shrink+compress worker and the HTTP endpoints
 * (/, /stream, /stats, /ctl). Reads the EO channel via eo_get_latest(); drives the
 * illuminator via illum.h. No websockets. See app/docs/GUI.md. */
#ifndef AIRPOC_GUI_H
#define AIRPOC_GUI_H

int  gui_start(int port);   /* spawn encoder + HTTP threads; returns 0 on success */
void gui_stop(void);
void gui_set_recorder(const char *host_port);  /* recorder daemon addr for /rec/ pass-through */

#endif /* AIRPOC_GUI_H */
