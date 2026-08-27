/***************************************************************************
 * Adaptive exact DC-only shortcut for progressive JPEG IDCT.
 *
 * The original transform remains in idct.c. At image start, DC-run mode is
 * enabled. Consecutive DC-only blocks use the exact direct-fill result; the
 * first block with any AC coefficient permanently disables the shortcut for
 * the rest of that image so photographic content stays on the proven legacy
 * transform without repeated sparsity-test overhead.
 ****************************************************************************/

#include "idct_accel.h"

#define idct_sq idct_sq_legacy
#define idct_s  idct_s_legacy
#include "idct.c"
#undef idct_sq
#undef idct_s

static bool jpegp_dc_run;

void jpegp_idct_reset(void)
{
    jpegp_dc_run = true;
}

void idct_sq(short *coef, int *sq)
{
    int i;

    if (jpegp_dc_run)
    {
        for (i = 1; i < 64; i++)
            if (coef[i] != 0)
                break;

        if (i == 64)
        {
            short value = CLIP[
                (coef[0] * sq[0] + ((1024 + 4) << 12)) >> 15];

            for (i = 0; i < 64; i++)
                coef[i] = value;
            return;
        }

        jpegp_dc_run = false;
    }

    idct_sq_legacy(coef, sq);
}

void idct_s(int *t, short *y)
{
    idct_s_legacy(t, y);
}
