/* lock_test.c - unit test for the Lucas-Kanade optical-flow lock and the ego-motion
 * estimator on synthetic frames (no camera). Builds a 1440x1088 frame with a TEXTURED
 * target patch (optical flow needs texture, not a flat blob), anchors on it, translates
 * the patch by a known amount, and asserts the flow recovers the motion sub-pixel; then
 * pans the whole frame and asserts ego_update recovers the shift. */
#include "lock.h"
#include "ego.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint16_t *F;
#define W EO_IMG_W
#define H EO_IMG_H

static void clear(void){ for (size_t i=0;i<(size_t)W*H;i++) F[i]=100; }

/* A checkerboard patch keyed to the patch centre, so it translates RIGIDLY when (cx,cy)
 * moves - i.e. brightness-constancy holds, exactly what optical flow assumes. 8 px checks
 * give many trackable corners across the box. */
static void patch(double cx, double cy, double half)
{
    int x0=(int)(cx-half), x1=(int)(cx+half), y0=(int)(cy-half), y1=(int)(cy+half);
    for (int y=y0;y<y1;y++) for (int x=x0;x<x1;x++) {
        if (x<0||x>=W||y<0||y>=H) continue;
        int rx = x - (int)cx, ry = y - (int)cy;
        int c = ((rx>>3) + (ry>>3)) & 1;
        F[(size_t)y*W+x] = c ? 3800 : 900;
    }
}

int main(void)
{
    int fails = 0;
    F = malloc((size_t)W*H*2);

    /* --- LK lock: follow a moving TEXTURED target --- */
    Lock *l = lock_new();
    clear(); patch(700, 540, 30);                 /* 60 px box of texture */
    lock_anchor(l, F, W, H, 700, 540, 60, 60);
    if (!lock_has_template(l)) { printf("FAIL: anchor set points\n"); fails++; }
    else printf("ok:   anchor detected corner points\n");

    /* move the patch +10,+6; give the flow a ZERO ego guess so we test that the pyramid
     * finds the motion on its own; expect the tracked centre near the new position. */
    double ox, oy, sc;
    clear(); patch(710, 546, 30);
    lock_track(l, F, W, H, 0, 0, &ox, &oy, &sc);
    printf("lock: tracked (%.2f,%.2f) frac %.3f (truth 710,546)\n", ox, oy, sc);
    if (fabs(ox - 710) > 1.5 || fabs(oy - 546) > 1.5) { printf("FAIL: lock position\n"); fails++; }
    else printf("ok:   flow follows the moving target sub-pixel\n");
    if (sc < 0.5) { printf("FAIL: surviving fraction low (%.3f)\n", sc); fails++; }
    else printf("ok:   most points tracked (frac %.2f)\n", sc);

    /* a second consecutive frame: move -6,+5 from the last position. This exercises the
     * current->previous roll and re-tracking from the survivor points. */
    clear(); patch(704, 551, 30);
    lock_track(l, F, W, H, 0, 0, &ox, &oy, &sc);
    printf("lock: tracked (%.2f,%.2f) frac %.3f (truth 704,551)\n", ox, oy, sc);
    if (fabs(ox - 704) > 1.5 || fabs(oy - 551) > 1.5) { printf("FAIL: second-frame position\n"); fails++; }
    else printf("ok:   flow rolls prev->cur across frames\n");

    /* a bigger shake (+28,-20) with the true ego as the guess: the pyramid + guess must
     * still land it (this is the shake case NCC used to lose). */
    lock_anchor(l, F, W, H, 704, 551, 60, 60);    /* re-anchor (detector fix) */
    clear(); patch(732, 531, 30);
    lock_track(l, F, W, H, 28, -20, &ox, &oy, &sc);
    printf("lock: tracked (%.2f,%.2f) frac %.3f (truth 732,531, shake)\n", ox, oy, sc);
    if (fabs(ox - 732) > 2.0 || fabs(oy - 531) > 2.0) { printf("FAIL: shake position\n"); fails++; }
    else printf("ok:   flow holds through a big shake\n");

    /* reset drops everything */
    lock_reset(l);
    if (lock_has_template(l)) { printf("FAIL: reset\n"); fails++; }
    else printf("ok:   reset clears the lock\n");
    lock_free(l);

    /* --- ego: recover a global pan --- */
    Ego *e = ego_new();
    double dx, dy;
    clear(); patch(400, 300, 100); patch(900, 700, 80);
    ego_update(e, F, W, H, &dx, &dy);   /* first frame: (0,0) */
    clear(); patch(424, 316, 100); patch(924, 716, 80);   /* shift +24,+16 */
    ego_update(e, F, W, H, &dx, &dy);
    printf("ego: recovered (%.0f,%.0f) (truth ~24,16)\n", dx, dy);
    if (fabs(dx - 24) > 8 || fabs(dy - 16) > 8) { printf("FAIL: ego shift\n"); fails++; }
    else printf("ok:   ego-motion recovers a global pan\n");
    ego_free(e);

    free(F);
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails==1?"":"s");
    return fails ? 1 : 0;
}
