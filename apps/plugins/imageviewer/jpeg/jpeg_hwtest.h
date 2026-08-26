/***************************************************************************
 * Temporary same-build A/B controls for JPEG IDCT/LCD hardware validation.
 *
 * These declarations are intentionally isolated so the test controls can be
 * removed cleanly after the one hardware acceptance session.
 ****************************************************************************/
#ifndef JPEG_HWTEST_H
#define JPEG_HWTEST_H

#include <stdbool.h>

extern bool jpeg_hwtest_reference_mode;
extern bool jpeg_hwtest_enabled;

#endif /* JPEG_HWTEST_H */
