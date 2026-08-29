static inline fb_data jpeg_pack_rgb565(int y, int u, int v)
{
    int yy = YFAC * y;
    int r = component_to_lcd(yy + RVFAC * v,
                             LCD_RED_BITS, NODITHER_DELTA);
    int g = component_to_lcd(yy + GUFAC * u + GVFAC * v,
                             LCD_GREEN_BITS, NODITHER_DELTA);
    int b = component_to_lcd(yy + BUFAC * u,
                             LCD_BLUE_BITS, NODITHER_DELTA);

    r = clamp_component_bits(r, LCD_RED_BITS);
    g = clamp_component_bits(g, LCD_GREEN_BITS);
    b = clamp_component_bits(b, LCD_BLUE_BITS);
    return FB_RGBPACK_LCD(r, g, b);
}

static bool jpeg_yuv_to_rgb565(unsigned char *src[3], int csub_x, int csub_y,
                               int src_x, int src_y, int stride,
                               int width, int height,
                               fb_data *dst, int dst_stride)
{
    int row;

    if (width <= 0 || height <= 0 || dst == NULL || dst_stride < width)
        return false;

    if (csub_y == 0) /* grayscale JPEG */
    {
        for (row = 0; row < height; row++)
        {
            const unsigned char *ysrc = src[0] +
                (src_y + row) * stride + src_x;
            fb_data *d = dst + row * dst_stride;
            int col;

            for (col = 0; col < width; col++)
                d[col] = jpeg_pack_rgb565(ysrc[col], 0, 0);
        }
        return true;
    }

    if ((csub_x != 1 && csub_x != 2) ||
        (csub_y != 1 && csub_y != 2))
        return false;

    for (row = 0; row < height; row++)
    {
        const unsigned char *ysrc = src[0] +
            (src_y + row) * stride + src_x;
        int cstride = stride / csub_x;
        int crow = (src_y + row) / csub_y;
        const unsigned char *usrc = src[1] + crow * cstride;
        const unsigned char *vsrc = src[2] + crow * cstride;
        fb_data *d = dst + row * dst_stride;
        int col = 0;

        if (csub_x == 2)
        {
            int sx = src_x;

            if ((sx & 1) && col < width)
            {
                int ci = sx >> 1;
                d[col] = jpeg_pack_rgb565(ysrc[col],
                                           usrc[ci] - 128,
                                           vsrc[ci] - 128);
                sx++;
                col++;
            }

            for (; col + 1 < width; col += 2, sx += 2)
            {
                int ci = sx >> 1;
                int u = usrc[ci] - 128;
                int v = vsrc[ci] - 128;
                d[col] = jpeg_pack_rgb565(ysrc[col], u, v);
                d[col + 1] = jpeg_pack_rgb565(ysrc[col + 1], u, v);
            }

            if (col < width)
            {
                int ci = sx >> 1;
                d[col] = jpeg_pack_rgb565(ysrc[col],
                                           usrc[ci] - 128,
                                           vsrc[ci] - 128);
            }
        }
        else
        {
            const unsigned char *u = usrc + src_x;
            const unsigned char *v = vsrc + src_x;
            for (col = 0; col < width; col++)
                d[col] = jpeg_pack_rgb565(ysrc[col],
                                           u[col] - 128,
                                           v[col] - 128);
        }
    }

    return true;
}

bool yuv_bitmap_part_to_buffer(unsigned char *src[3], int csub_x, int csub_y,
                               int src_x, int src_y, int stride,
                               int width, int height,
                               fb_data *dst, int dst_stride)
{
    return jpeg_yuv_to_rgb565(src, csub_x, csub_y,
                              src_x, src_y, stride,
                              width, height, dst, dst_stride);
}
