/***************************************************************************
 * Full-range JPEG YCbCr420 writer for the iPod Color LCD2 interface.
 *
 * The stock target blitter is intentionally video-range. This target-local
 * JPEG path keeps that MPEG behaviour untouched and applies the same
 * full-range coefficients/rounding as imageviewer's validated RGB565 path.
 ****************************************************************************/

#include "plugin.h"
#include "jpeg_lcd_fullrange.h"

#if defined(IPOD_COLOR)

#define JPEG_LCD_POLL_LIMIT 1000000
#define JPEG_YFAC            128
#define JPEG_RVFAC           179
#define JPEG_GUFAC           (-43)
#define JPEG_GVFAC           (-91)
#define JPEG_BUFAC           227
#define JPEG_NODITHER_DELTA   (127 * JPEG_YFAC)
#define JPEG_COMPONENT_SHIFT 15

#define LCD_CNTL_RAM_ADDR_SET       0x21
#define LCD_CNTL_WRITE_TO_GRAM      0x22
#define LCD_CNTL_HORIZ_RAM_ADDR_POS 0x44
#define LCD_CNTL_VERT_RAM_ADDR_POS  0x45

struct jpeg_lcd_stream_state
{
    bool active;
    int width;
    int remaining_rows;
    int block_rows;
    fb_data *fb_dst;
    int fb_stride;
};

static struct jpeg_lcd_stream_state jpeg_lcd_stream;

static inline bool jpeg_lcd_wait_write(void)
{
    unsigned int count = JPEG_LCD_POLL_LIMIT;

    while (LCD2_PORT & LCD2_BUSY_MASK)
        if (--count == 0)
            return false;
    return true;
}

static inline bool jpeg_lcd_wait_block(unsigned long mask)
{
    unsigned int count = JPEG_LCD_POLL_LIMIT;

    while (!(LCD2_BLOCK_CTRL & mask))
        if (--count == 0)
            return false;
    return true;
}

static int jpeg_lcd_type(void)
{
    if (IPOD_HW_REVISION == 0x60000)
        return 0;
    return (GPIOA_INPUT_VAL & 0x2) | ((GPIOA_INPUT_VAL & 0x10) >> 4);
}

static bool jpeg_lcd_cmd_data(int type, unsigned cmd, unsigned data)
{
    if ((type & 1) == 0)
    {
        if (!jpeg_lcd_wait_write())
            return false;
        LCD2_PORT = LCD2_CMD_MASK | cmd;
        if (!jpeg_lcd_wait_write())
            return false;
        LCD2_PORT = LCD2_CMD_MASK | data;
    }
    else
    {
        if (!jpeg_lcd_wait_write())
            return false;
        LCD2_PORT = LCD2_CMD_MASK;
        LCD2_PORT = LCD2_CMD_MASK | cmd;
        if (!jpeg_lcd_wait_write())
            return false;
        LCD2_PORT = LCD2_DATA_MASK | (data >> 8);
        LCD2_PORT = LCD2_DATA_MASK | (data & 0xff);
    }
    return true;
}

static bool jpeg_lcd_setup_region(int x, int y, int width, int height)
{
    int type = jpeg_lcd_type();
    int y0 = y;
    int y1 = y + height - 1;
    int x1 = LCD_WIDTH - 1 - x;
    int x0 = x1 - width + 1;

    if ((type & 1) == 0)
    {
        return jpeg_lcd_cmd_data(type, 0x12, y0) &&
               jpeg_lcd_cmd_data(type, 0x13, x1) &&
               jpeg_lcd_cmd_data(type, 0x15, y1) &&
               jpeg_lcd_cmd_data(type, 0x16, x0);
    }

    if (!jpeg_lcd_cmd_data(type, LCD_CNTL_HORIZ_RAM_ADDR_POS,
                           (y1 << 8) | y0) ||
        !jpeg_lcd_cmd_data(type, LCD_CNTL_VERT_RAM_ADDR_POS,
                           (x1 << 8) | x0) ||
        !jpeg_lcd_cmd_data(type, LCD_CNTL_RAM_ADDR_SET,
                           (x1 << 8) | y0) ||
        !jpeg_lcd_wait_write())
        return false;

    LCD2_PORT = LCD2_CMD_MASK;
    LCD2_PORT = LCD2_CMD_MASK | LCD_CNTL_WRITE_TO_GRAM;
    return true;
}

static inline int jpeg_clamp_bits(int value, int bits)
{
    int maximum = (1 << bits) - 1;

    if ((unsigned)value > (unsigned)maximum)
        value = value < 0 ? 0 : maximum;
    return value;
}

static inline int jpeg_component_to_lcd(int value, int bits)
{
    return (((1 << bits) - 1) * value +
            (value >> (8 - bits)) + JPEG_NODITHER_DELTA) >>
           JPEG_COMPONENT_SHIFT;
}

static inline fb_data jpeg_pack_rgb565(unsigned int y, int cb, int cr)
{
    int yy = JPEG_YFAC * (int)y;
    int r = jpeg_component_to_lcd(yy + JPEG_RVFAC * cr, LCD_RED_BITS);
    int g = jpeg_component_to_lcd(yy + JPEG_GUFAC * cb +
                                  JPEG_GVFAC * cr, LCD_GREEN_BITS);
    int b = jpeg_component_to_lcd(yy + JPEG_BUFAC * cb, LCD_BLUE_BITS);

    r = jpeg_clamp_bits(r, LCD_RED_BITS);
    g = jpeg_clamp_bits(g, LCD_GREEN_BITS);
    b = jpeg_clamp_bits(b, LCD_BLUE_BITS);
    return FB_RGBPACK_LCD(r, g, b);
}

static bool jpeg_lcd_write_two_lines(unsigned char const * const src[3],
                                     int width, int stride,
                                     fb_data *dst, int dst_stride)
{
    int row;

    for (row = 0; row < 2; row++)
    {
        const unsigned char *ysrc = src[0] + row * stride;
        const unsigned char *cb = src[1];
        const unsigned char *cr = src[2];
        fb_data *out = dst + row * dst_stride;
        int col;

        for (col = 0; col < width; col += 2)
        {
            fb_data pair[2];
            uint32_t packed;

            pair[0] = jpeg_pack_rgb565(ysrc[col],
                                       (int)*cb - 128, (int)*cr - 128);
            pair[1] = jpeg_pack_rgb565(ysrc[col + 1],
                                       (int)*cb - 128, (int)*cr - 128);
            cb++;
            cr++;

            out[col] = pair[0];
            out[col + 1] = pair[1];

            packed = (uint16_t)pair[0] |
                     ((uint32_t)(uint16_t)pair[1] << 16);
            if (!jpeg_lcd_wait_block(LCD2_BLOCK_TXOK))
                return false;
            LCD2_BLOCK_DATA = packed;
        }
    }

    return true;
}

static bool jpeg_lcd_stream_start_block(void)
{
    int rows = jpeg_lcd_stream.remaining_rows;
    int maximum_rows = ((0x10000 / 2) / jpeg_lcd_stream.width) & ~1;
    int bytes;

    if (rows > maximum_rows)
        rows = maximum_rows;
    if (rows <= 0 || (rows & 1))
        return false;

    bytes = jpeg_lcd_stream.width * rows * 2;
    LCD2_BLOCK_CTRL = 0x10000080;
    LCD2_BLOCK_CONFIG = 0xc0010000 | (bytes - 1);
    LCD2_BLOCK_CTRL = 0x34000000;
    jpeg_lcd_stream.block_rows = rows;
    return true;
}

static bool jpeg_lcd_stream_finish_block(void)
{
    if (!jpeg_lcd_wait_block(LCD2_BLOCK_READY))
        return false;

    LCD2_BLOCK_CONFIG = 0;
    jpeg_lcd_stream.block_rows = 0;
    return true;
}

void jpeg_lcd_stream_abort(void)
{
    LCD2_BLOCK_CONFIG = 0;
    jpeg_lcd_stream.active = false;
    jpeg_lcd_stream.width = 0;
    jpeg_lcd_stream.remaining_rows = 0;
    jpeg_lcd_stream.block_rows = 0;
    jpeg_lcd_stream.fb_dst = NULL;
    jpeg_lcd_stream.fb_stride = 0;
}

bool jpeg_lcd_stream_begin(int x, int y, int width, int height)
{
    struct viewport *vp_main;

    if (jpeg_lcd_stream.active || width <= 0 || height <= 0 ||
        (width & 1) || (height & 1) || x < 0 || y < 0 ||
        x + width > LCD_WIDTH || y + height > LCD_HEIGHT)
        return false;

    vp_main = *(rb->screens[SCREEN_MAIN]->current_viewport);
    if (vp_main == NULL || vp_main->buffer == NULL ||
        vp_main->buffer->fb_ptr == NULL ||
        vp_main->x != 0 || vp_main->y != 0 ||
        vp_main->width != LCD_WIDTH || vp_main->height != LCD_HEIGHT ||
        vp_main->buffer->stride != LCD_WIDTH)
        return false;

    if (!jpeg_lcd_setup_region(x, y, width, height))
        return false;

    jpeg_lcd_stream.active = true;
    jpeg_lcd_stream.width = width;
    jpeg_lcd_stream.remaining_rows = height;
    jpeg_lcd_stream.block_rows = 0;
    jpeg_lcd_stream.fb_dst = vp_main->buffer->fb_ptr + y * LCD_WIDTH + x;
    jpeg_lcd_stream.fb_stride = LCD_WIDTH;

    if (!jpeg_lcd_stream_start_block())
    {
        jpeg_lcd_stream_abort();
        return false;
    }

    return true;
}

bool jpeg_lcd_stream_write_yuv420(unsigned char * const src[3],
                                  int stride, int rows)
{
    unsigned char const *yuv_src[3];

    if (!jpeg_lcd_stream.active || src == NULL ||
        src[0] == NULL || src[1] == NULL || src[2] == NULL ||
        stride <= 0 || (stride & 1) || rows <= 0 || (rows & 1) ||
        rows > jpeg_lcd_stream.remaining_rows)
        return false;

    yuv_src[0] = src[0];
    yuv_src[1] = src[1];
    yuv_src[2] = src[2];

    while (rows > 0)
    {
        if (jpeg_lcd_stream.block_rows == 0)
        {
            if (!jpeg_lcd_stream_start_block())
            {
                jpeg_lcd_stream_abort();
                return false;
            }
        }

        if (!jpeg_lcd_write_two_lines(yuv_src, jpeg_lcd_stream.width, stride,
                                      jpeg_lcd_stream.fb_dst,
                                      jpeg_lcd_stream.fb_stride))
        {
            jpeg_lcd_stream_abort();
            return false;
        }

        yuv_src[0] += stride << 1;
        yuv_src[1] += stride >> 1;
        yuv_src[2] += stride >> 1;
        jpeg_lcd_stream.fb_dst += jpeg_lcd_stream.fb_stride << 1;
        jpeg_lcd_stream.remaining_rows -= 2;
        jpeg_lcd_stream.block_rows -= 2;
        rows -= 2;

        if (jpeg_lcd_stream.block_rows == 0)
        {
            if (!jpeg_lcd_stream_finish_block())
            {
                jpeg_lcd_stream_abort();
                return false;
            }
        }
    }

    return true;
}

bool jpeg_lcd_stream_end(void)
{
    if (!jpeg_lcd_stream.active || jpeg_lcd_stream.remaining_rows != 0 ||
        jpeg_lcd_stream.block_rows != 0)
    {
        jpeg_lcd_stream_abort();
        return false;
    }

    jpeg_lcd_stream.active = false;
    jpeg_lcd_stream.width = 0;
    jpeg_lcd_stream.fb_dst = NULL;
    jpeg_lcd_stream.fb_stride = 0;
    return true;
}

bool jpeg_lcd_blit_yuv420_fullrange(unsigned char * const src[3],
                                    int src_x, int src_y, int stride,
                                    int x, int y, int width, int height)
{
    unsigned char *yuv_src[3];

    if (src == NULL || src[0] == NULL || src[1] == NULL || src[2] == NULL ||
        width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        (src_x & 1) || (src_y & 1))
        return false;

    yuv_src[0] = src[0] + src_y * stride + src_x;
    yuv_src[1] = src[1] + (src_y >> 1) * (stride >> 1) + (src_x >> 1);
    yuv_src[2] = src[2] + (src_y >> 1) * (stride >> 1) + (src_x >> 1);

    if (!jpeg_lcd_stream_begin(x, y, width, height))
        return false;

    if (!jpeg_lcd_stream_write_yuv420(yuv_src, stride, height))
        return false;

    return jpeg_lcd_stream_end();
}
#endif /* IPOD_COLOR */
