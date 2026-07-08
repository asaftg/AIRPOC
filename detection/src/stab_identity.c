/* stab_identity.c — no-op stabilizer: assumes the platform isn't moving (or that
 * ego-motion is handled elsewhere). Returns the identity warp. First bring-up stub
 * and the graceful fallback when ECC fails. */
#include "stab.h"

static int id_align(void *self, const uint8_t *prev, const uint8_t *cur, float w[6])
{
    (void)self; (void)prev; (void)cur;
    w[0] = 1; w[1] = 0; w[2] = 0;
    w[3] = 0; w[4] = 1; w[5] = 0;
    return STAB_OK;
}

static void id_destroy(void *self) { (void)self; }

Stabilizer stab_identity_new(int w, int h)
{
    (void)w; (void)h;
    Stabilizer s;
    s.align = id_align;
    s.destroy = id_destroy;
    s.self = 0;
    return s;
}
