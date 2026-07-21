/* lock_test.c - unit test for the NCC lock loop and the ego-motion estimator on
 * synthetic frames (no camera). Builds a 1440x1088 frame with a bright square,
 * sets a template, moves the square, and asserts the correlator follows it with a
 * high score; then pans the whole frame and asserts ego_update recovers the shift. */
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

static void clear(void){ memset(F, 0, (size_t)W*H*2); for (size_t i=0;i<(size_t)W*H;i++) F[i]=100; }
static void square(double cx, double cy, double s, uint16_t val)
{
    int x0=(int)(cx-s/2), x1=(int)(cx+s/2), y0=(int)(cy-s/2), y1=(int)(cy+s/2);
    for (int y=y0;y<y1;y++) for (int x=x0;x<x1;x++)
        if (x>=0&&x<W&&y>=0&&y<H) F[(size_t)y*W+x]=val;
}

int main(void)
{
    int fails = 0;
    F = malloc((size_t)W*H*2);

    /* --- lock: follow a moving TEXTURED target ---
     * A real target box has internal structure; a flat patch has no NCC signal.
     * Model that with a bright object (40 px) inside a larger sampled window (60 px)
     * so the template captures the object edges against the background. */
    Lock *l = lock_new();
    clear(); square(700, 540, 40, 4000);
    lock_set_template(l, F, W, H, 700, 540, 60, 60);
    if (!lock_has_template(l)) { printf("FAIL: template set\n"); fails++; }

    /* move the object +10,+6; predict at old centre; expect the match near new pos */
    clear(); square(710, 546, 40, 4000);
    double ox, oy, sc;
    lock_track(l, F, W, H, 700, 540, &ox, &oy, &sc);
    printf("lock: matched (%.0f,%.0f) score %.3f (truth 710,546)\n", ox, oy, sc);
    if (fabs(ox - 710) > 4 || fabs(oy - 546) > 4) { printf("FAIL: lock position\n"); fails++; }
    else printf("ok:   lock follows the moving target\n");
    if (sc < 0.9) { printf("FAIL: lock score low (%.3f)\n", sc); fails++; }
    else printf("ok:   lock NCC score high on a clean match\n");

    /* a scaled (grown) object should still match via the multi-scale search */
    clear(); square(700, 540, 44, 4000);   /* 10% bigger */
    lock_track(l, F, W, H, 700, 540, &ox, &oy, &sc);
    printf("lock (grown): score %.3f\n", sc);
    if (sc < 0.85) { printf("FAIL: multi-scale match on growth\n"); fails++; }
    else printf("ok:   multi-scale absorbs a closing (growing) target\n");
    lock_free(l);

    /* --- ego: recover a global pan --- */
    Ego *e = ego_new();
    double dx, dy;
    clear(); square(400, 300, 200, 3000); square(900, 700, 150, 5000);
    ego_update(e, F, W, H, &dx, &dy);   /* first frame: (0,0) */
    /* shift the whole scene right by 24, down by 16 */
    clear(); square(424, 316, 200, 3000); square(924, 716, 150, 5000);
    ego_update(e, F, W, H, &dx, &dy);
    printf("ego: recovered (%.0f,%.0f) (truth ~24,16)\n", dx, dy);
    if (fabs(dx - 24) > 8 || fabs(dy - 16) > 8) { printf("FAIL: ego shift\n"); fails++; }
    else printf("ok:   ego-motion recovers a global pan\n");
    ego_free(e);

    free(F);
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails==1?"":"s");
    return fails ? 1 : 0;
}
