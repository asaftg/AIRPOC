/* trim.h - mount-trim persistence (degrees on disk, mirrors the knob surface). */
#ifndef FUS_TRIM_H
#define FUS_TRIM_H

/* Load trim from the state file. Returns 1 if a file supplied the values
 * (az/el filled), 0 if defaults should stand. */
int trim_load(double *az_deg, double *el_deg);

/* Persist atomically (tmp + rename); falls back next to the binary when the
 * system path is not writable. Returns 0 on success. */
int trim_save(double az_deg, double el_deg);

#endif
