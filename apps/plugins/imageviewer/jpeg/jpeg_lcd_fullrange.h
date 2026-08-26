/***************************************************************************
 * iPod Color full-range JPEG YCbCr420 LCD writer.
 ****************************************************************************/
#ifndef JPEG_LCD_FULLRANGE_H
#define JPEG_LCD_FULLRANGE_H

#include "plugin.h"

#if defined(IPOD_COLOR)
#include "cpu.h"
#include "hwcompat.h"

bool jpeg_lcd_blit_yuv420_fullrange(unsigned char * const src[3],
                                    int src_x, int src_y, int stride,
                                    int x, int y, int width, int height,
                                    uint32_t *rgb_crc);
#endif

#endif /* JPEG_LCD_FULLRANGE_H */
