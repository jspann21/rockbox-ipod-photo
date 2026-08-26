/***************************************************************************
 * Fast no-dither JPEG YUV-to-RGB565 conversion.
 *
 * The original converter remains the fallback for clipping, grayscale mode,
 * ordered/error-diffusion dithering, and unusual subsampling. This wrapper
 * specializes the common no-dither paths and exposes a buffer target for the
 * viewport cache.
 ****************************************************************************/

#include "plugin.h"
#include "yuv2rgb.h"

#define yuv_bitmap_part yuv_bitmap_part_legacy
#include "yuv2rgb.c"
#undef yuv_bitmap_part

#include "yuv2rgb_fast.inc"
#include "yuv2rgb_dispatch.inc"
