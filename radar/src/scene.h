/* scene — static occupancy layer for the operator picture.
 *
 * Accumulates the STATIONARY returns (|doppler| < 0.1 m/s, which given the
 * 0.108 m/s Doppler quantisation is exactly the zero-Doppler set) into a polar
 * grid, and reports for each cell how often it has been occupied and how strong
 * its echo is. Over tens of seconds the world draws itself: a wall is in its
 * cell nearly every frame, a false alarm appears once in thousands.
 *
 * This is a DISPLAY layer, not a detector. It never feeds tracking or guidance.
 *
 * Two channels are published per cell and they mean different things:
 *   occupancy — how reliably something is there
 *   strength  — how much to trust its BEARING. Measured bearing wander over
 *               100 s: ~2.6 deg at 60 dB, ~3.4 at 53 dB, ~14 at 36-44 dB,
 *               ~20 deg at 28 dB. Faint cells are real returns whose ANGLE is
 *               noise-limited; that is why the far field looks like arcs.
 * Binding opacity to occupancy and colour to strength therefore shows the
 * operator both "is it there" and "can I believe where it is".
 *
 * Accumulated in the SENSOR frame: it smears if the sensor slews. Call
 * scene_reset() on a slew until the gimbal encoders allow a world frame.
 */
#ifndef AIRPOC_SCENE_H
#define AIRPOC_SCENE_H

#include <stddef.h>
#include "radar.h"

/* grid: 0..500 m in native range bins, +/-60 deg in 1 deg cells */
#define SCENE_RSTEP   2.61f
#define SCENE_NR      192
#define SCENE_AZ0    -60.0f
#define SCENE_ASTEP   1.0f
#define SCENE_NA      120
#define SCENE_DOP_MAX 0.1f    /* m/s — "not moving" */

typedef struct SceneMap SceneMap;

SceneMap *scene_new(void);
void      scene_free(SceneMap *);

/* Fold one frame's points in. Cheap: one pass over the stationary points. */
void      scene_step(SceneMap *, const RadarPoint *pts, int n);

/* Clear the accumulation (use on a slew, or from GET /scene?reset=1). */
void      scene_reset(SceneMap *);

/* Accumulation on/off. Default on. */
void      scene_set_enabled(SceneMap *, int on);
int       scene_enabled(const SceneMap *);

/* Serialise the lit cells as JSON into buf. Returns bytes written (0 if it
 * would not fit). Sparse: only cells that have ever been occupied. */
int       scene_json(const SceneMap *, char *buf, size_t cap);

#endif /* AIRPOC_SCENE_H */
