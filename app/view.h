/* Display shrink — the GUI's only per-frame compute. One pass over the EO frame:
 * take the centered 1/zoom crop and box-average it down to a small dw x dh 8-bit
 * mono buffer for compression. Read-only on the source; handles GRAY8 and Y10. */
#ifndef AIRPOC_VIEW_H
#define AIRPOC_VIEW_H

#include "eo_frame.h"

void view_shrink(const eo_frame_t *f, int zoom, uint8_t *out, int dw, int dh);

#endif /* AIRPOC_VIEW_H */
