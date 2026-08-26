/***************************************************************************
 * Pull Rockbox's existing classic-ARM JPEG IDCT into the JPEG decoder
 * overlay. The accelerated decoder uses the assembly vertical passes and
 * exact contiguous-plane C horizontal passes.
 ****************************************************************************/
#include "recorder/jpeg_idct_arm.S"
