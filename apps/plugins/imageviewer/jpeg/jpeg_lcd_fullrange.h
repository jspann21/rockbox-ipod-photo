/***************************************************************************
 * iPod Color full-range JPEG YCbCr420 LCD writer.
 ****************************************************************************/
#ifndef JPEG_LCD_FULLRANGE_H
#define JPEG_LCD_FULLRANGE_H

#include "plugin.h"

#if defined(IPOD_COLOR)
#include "cpu.h"
#include "hwcompat.h"

bool jpeg_lcd_stream_begin(int x, int y, int width, int height);
bool jpeg_lcd_stream_write_yuv420(unsigned char * const src[3],
                                  int stride, int rows);
bool jpeg_lcd_stream_write_rgb565(const fb_data *src, int stride, int rows);
bool jpeg_lcd_stream_end(void);
void jpeg_lcd_stream_abort(void);

bool jpeg_lcd_blit_yuv420_fullrange(unsigned char * const src[3],
                                    int src_x, int src_y, int stride,
                                    int x, int y, int width, int height);
#endif

#endif /* JPEG_LCD_FULLRANGE_H */
