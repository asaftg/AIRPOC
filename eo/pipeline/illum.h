/* Thread-safe shim over the SG-IR850 IR illuminator controller for the monitor
 * GUI. The illuminator is OPTIONAL: if not attached, illum_start() logs and every
 * setter no-ops so the EO pipeline runs normally. See
 * illuminator/docs/PREVIEW_INTEGRATION.md. */
#ifndef AIRPOC_EO_ILLUM_H
#define AIRPOC_EO_ILLUM_H

int  illum_start(const char *port);   /* open once at startup; returns 1 if present */
void illum_set_on(int on);            /* laser on/off (restores commanded power on on) */
void illum_set_power(int level);      /* optical drive 0..255 (applied when on)       */
void illum_set_fov(double deg);       /* beam angle 1.96..70 deg (light beam, not cam) */
void illum_snapshot(int *on, int *power, double *fov, int *present);  /* for /stats   */

#endif /* AIRPOC_EO_ILLUM_H */
