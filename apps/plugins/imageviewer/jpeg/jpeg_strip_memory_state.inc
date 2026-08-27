struct jpeg_strip_state
{
    bool decoded;
    bool strip_only;
    bool cache_ok;
    bool lcd_ok;
    bool frame_pending;
    int strips;
};

static struct jpeg_strip_state jpeg_strip;

static void jpeg_strip_reset(void)
{
    rb->memset(&jpeg_strip, 0, sizeof(jpeg_strip));
}

static void jpeg_reset_decoded_pool(void)
{
    int i;

    for (i = 1; i <= 8; i++)
        rb->memset(&disp[i], 0, sizeof(disp[i]));

    jpeg_cache_clear();
    buf_images = buf_root;
    buf_images_size = root_size;
    jpeg_strip_reset();
}

static bool jpeg_stream_eligible(const struct image_info *info, int ds,
                                 bool already_decoded)
{
    return !already_decoded && ds == 1 &&
        (iv->settings->hide_info || iv->running_slideshow) &&
        iv->settings->jpeg_colour_mode == COLOURMODE_COLOUR &&
        iv->settings->jpeg_dither_mode == DITHER_NONE &&
        info->x_size == LCD_WIDTH && info->y_size == LCD_HEIGHT &&
        jpg.components == 3 && jpg.blocks > 1 &&
        jpg.x_phys >= LCD_WIDTH && jpg.y_phys == LCD_HEIGHT &&
        jpg.y_mbl > 0 &&
        jpg.subsample_x[0] == 1 && jpg.subsample_y[0] == 1 &&
        jpg.subsample_x[1] == 2 && jpg.subsample_y[1] == 2 &&
        jpg.subsample_x[2] == 2 && jpg.subsample_y[2] == 2;
}

static bool jpeg_allocate_stream_buffers(struct image_info *info, int ds)
{
    unsigned char *cursor = buf_images;
    ssize_t remaining = buf_images_size;
    struct jpeg_rgb_cache *cache = &rgb_cache[ds];
    struct t_disp *pdisp = &disp[ds];
    uintptr_t address, aligned;
    size_t padding;
    int y_stride = jpg.x_phys;
    int c_stride = y_stride >> 1;
    int strip_height = jpg.y_phys / jpg.y_mbl;
    int c_rows = strip_height >> 1;
    size_t cache_bytes =
        (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data);
    size_t y_bytes = (size_t)y_stride * strip_height;
    size_t c_bytes = (size_t)c_stride * c_rows;

    if (ds != 1 || y_stride < LCD_WIDTH || (y_stride & 1) ||
        strip_height <= 0 || (strip_height & 1) ||
        remaining <= 0)
        return false;

    address = (uintptr_t)cursor;
    aligned = (address + sizeof(fb_data) - 1) &
              ~((uintptr_t)sizeof(fb_data) - 1);
    padding = aligned - address;

    if (remaining < (ssize_t)(padding + cache_bytes +
                              y_bytes + c_bytes + c_bytes))
        return false;

    cursor += padding;
    remaining -= padding;

    cache->pixels = (fb_data *)cursor;
    cache->stride = LCD_WIDTH;
    cache->valid = false;
    cursor += cache_bytes;
    remaining -= cache_bytes;

    pdisp->bitmap[0] = cursor;
    cursor += y_bytes;
    remaining -= y_bytes;
    pdisp->bitmap[1] = cursor;
    cursor += c_bytes;
    remaining -= c_bytes;
    pdisp->bitmap[2] = cursor;
    cursor += c_bytes;
    remaining -= c_bytes;

    pdisp->csub_x = 2;
    pdisp->csub_y = 2;
    pdisp->stride = y_stride;

    info->width = LCD_WIDTH;
    info->height = LCD_HEIGHT;
    info->data = pdisp;

    buf_images = cursor;
    buf_images_size = remaining;
    return true;
}
