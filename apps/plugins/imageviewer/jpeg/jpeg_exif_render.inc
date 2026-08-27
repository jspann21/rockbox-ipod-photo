#if defined(HAVE_LCD_COLOR)

#define JPEG_EXIF_YFAC             128
#define JPEG_EXIF_RVFAC            179
#define JPEG_EXIF_GUFAC            (-43)
#define JPEG_EXIF_GVFAC            (-91)
#define JPEG_EXIF_BUFAC            227
#define JPEG_EXIF_NODITHER_DELTA    (127 * JPEG_EXIF_YFAC)
#define JPEG_EXIF_COMPONENT_SHIFT  15

static bool jpeg_exif_rotates(void)
{
    return jpeg_exif_meta.orientation >= 5;
}

static void jpeg_exif_map_source(int sx, int sy,
                                 int raw_width, int raw_height,
                                 int *dx, int *dy)
{
    switch (jpeg_exif_meta.orientation)
    {
    default:
    case 1: *dx = sx; *dy = sy; break;
    case 2: *dx = raw_width - 1 - sx; *dy = sy; break;
    case 3: *dx = raw_width - 1 - sx; *dy = raw_height - 1 - sy; break;
    case 4: *dx = sx; *dy = raw_height - 1 - sy; break;
    case 5: *dx = sy; *dy = sx; break;
    case 6: *dx = raw_height - 1 - sy; *dy = sx; break;
    case 7: *dx = raw_height - 1 - sy; *dy = raw_width - 1 - sx; break;
    case 8: *dx = sy; *dy = raw_width - 1 - sx; break;
    }
}

static inline int jpeg_exif_clamp_bits(int value, int bits)
{
    int maximum = (1 << bits) - 1;
    if ((unsigned)value > (unsigned)maximum)
        value = value < 0 ? 0 : maximum;
    return value;
}

static inline int jpeg_exif_component(int value, int bits)
{
    return (((1 << bits) - 1) * value +
            (value >> (8 - bits)) + JPEG_EXIF_NODITHER_DELTA) >>
           JPEG_EXIF_COMPONENT_SHIFT;
}

static inline fb_data jpeg_exif_pack_colour(int y, int u, int v)
{
    int yy = JPEG_EXIF_YFAC * y;
    int r = jpeg_exif_component(yy + JPEG_EXIF_RVFAC * v, LCD_RED_BITS);
    int g = jpeg_exif_component(yy + JPEG_EXIF_GUFAC * u +
                                JPEG_EXIF_GVFAC * v, LCD_GREEN_BITS);
    int b = jpeg_exif_component(yy + JPEG_EXIF_BUFAC * u, LCD_BLUE_BITS);
    r = jpeg_exif_clamp_bits(r, LCD_RED_BITS);
    g = jpeg_exif_clamp_bits(g, LCD_GREEN_BITS);
    b = jpeg_exif_clamp_bits(b, LCD_BLUE_BITS);
    return FB_RGBPACK_LCD(r, g, b);
}

static inline fb_data jpeg_exif_pack_gray(int y)
{
    int yy = JPEG_EXIF_YFAC * y;
    int r = jpeg_exif_component(yy, LCD_RED_BITS);
    int g = jpeg_exif_component(yy, LCD_GREEN_BITS);
    int b = jpeg_exif_component(yy, LCD_BLUE_BITS);
    r = jpeg_exif_clamp_bits(r, LCD_RED_BITS);
    g = jpeg_exif_clamp_bits(g, LCD_GREEN_BITS);
    b = jpeg_exif_clamp_bits(b, LCD_BLUE_BITS);
    return FB_RGBPACK_LCD(r, g, b);
}

static bool jpeg_prepare_oriented_cache(const struct image_info *info,
                                        struct t_disp *pdisp,
                                        struct jpeg_rgb_cache *cache,
                                        int ds)
{
    int raw_width = jpg.x_size / ds;
    int raw_height = jpg.y_size / ds;
    int sy;

    if (jpeg_exif_meta.orientation == 1)
        return jpeg_prepare_cache(info, pdisp, cache);

    if (cache == NULL || cache->pixels == NULL ||
        iv->settings->jpeg_dither_mode != DITHER_NONE ||
        raw_width <= 0 || raw_height <= 0)
        return false;

    if (cache->valid)
        return true;

    if (pdisp->csub_y != 0 &&
        ((pdisp->csub_x != 1 && pdisp->csub_x != 2) ||
         (pdisp->csub_y != 1 && pdisp->csub_y != 2)))
        return false;

    for (sy = 0; sy < raw_height; sy++)
    {
        const unsigned char *yrow =
            pdisp->bitmap[0] + sy * pdisp->stride;
        const unsigned char *urow = NULL;
        const unsigned char *vrow = NULL;
        int cstride = 0;
        int sx;

        if (pdisp->csub_y != 0)
        {
            cstride = pdisp->stride / pdisp->csub_x;
            urow = pdisp->bitmap[1] +
                (sy / pdisp->csub_y) * cstride;
            vrow = pdisp->bitmap[2] +
                (sy / pdisp->csub_y) * cstride;
        }

        for (sx = 0; sx < raw_width; sx++)
        {
            fb_data pixel;
            int dx, dy;

            if (iv->settings->jpeg_colour_mode == COLOURMODE_GRAY ||
                pdisp->csub_y == 0)
            {
                pixel = jpeg_exif_pack_gray(yrow[sx]);
            }
            else
            {
                int ci = sx / pdisp->csub_x;
                pixel = jpeg_exif_pack_colour(
                    yrow[sx], urow[ci] - 128, vrow[ci] - 128);
            }

            jpeg_exif_map_source(sx, sy, raw_width, raw_height, &dx, &dy);
            cache->pixels[dy * cache->stride + dx] = pixel;
        }
    }

    cache->valid = true;
    return true;
}

#endif
