struct jpeg_strip_state
{
    bool decoded;
    bool strip_only;
    bool cache_ok;
    bool lcd_ok;
    bool frame_pending;
    bool test_enabled;
    bool test_reference;
    bool logged;
    int strips;
    int strip_height;
    uint32_t started_us;
    uint32_t total_us;
    uint32_t first_strip_us;
    uint32_t lcd_us;
    uint32_t yuv_crc;
    size_t used_bytes;
    size_t full_yuv_bytes;
    size_t strip_bytes;
    size_t cache_bytes;
    char filename[MAX_PATH];
};

static struct jpeg_strip_state jpeg_strip;

static uint32_t jpeg_now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (uint32_t)*rb->current_tick * (1000000u / HZ);
#endif
}

static bool jpeg_path_exists(const char *path)
{
    int fd = rb->open(path, O_RDONLY);

    if (fd < 0)
        return false;
    rb->close(fd);
    return true;
}

static void jpeg_strip_reset(char *filename)
{
    rb->memset(&jpeg_strip, 0, sizeof(jpeg_strip));
    jpeg_strip.test_enabled =
        jpeg_path_exists(ROCKBOX_DIR "/jpeg7mem.enabled");
    jpeg_strip.test_reference = jpeg_strip.test_enabled &&
        jpeg_path_exists(ROCKBOX_DIR "/jpeg7mem.reference");
    jpeg_strip.yuv_crc = 0xffffffff;
    rb->snprintf(jpeg_strip.filename, sizeof(jpeg_strip.filename),
                 "%s", filename);
}

static void jpeg_reset_decoded_pool(void)
{
    int i;

    for (i = 1; i <= 8; i++)
        rb->memset(&disp[i], 0, sizeof(disp[i]));

    jpeg_cache_clear();
    buf_images = buf_root;
    buf_images_size = root_size;

    jpeg_strip.decoded = false;
    jpeg_strip.strip_only = false;
    jpeg_strip.cache_ok = false;
    jpeg_strip.lcd_ok = false;
    jpeg_strip.frame_pending = false;
    jpeg_strip.strips = 0;
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

static bool jpeg_allocate_stream_buffers(struct image_info *info, int ds,
                                         bool strip_only)
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
    int y_rows = strip_only ? strip_height : jpg.y_phys;
    int c_rows = y_rows >> 1;
    size_t cache_bytes = (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data);
    size_t y_bytes = (size_t)y_stride * y_rows;
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

    jpeg_strip.strip_height = strip_height;
    jpeg_strip.cache_bytes = cache_bytes;
    jpeg_strip.strip_bytes =
        (size_t)y_stride * strip_height +
        2u * (size_t)c_stride * (strip_height >> 1);
    jpeg_strip.full_yuv_bytes =
        (size_t)y_stride * jpg.y_phys +
        2u * (size_t)c_stride * (jpg.y_phys >> 1);
    jpeg_strip.used_bytes = (size_t)(root_size - buf_images_size);
    return true;
}

static uint32_t jpeg_crc_rows(const unsigned char *src, int stride,
                              int width, int height, uint32_t crc)
{
    int row;

    for (row = 0; row < height; row++)
    {
        crc = rb->crc_32(src, width, crc);
        src += stride;
    }
    return crc;
}

static void jpeg_strip_update_yuv_crc(unsigned char * const row[3],
                                      int height, int stride)
{
    int cstride = stride >> 1;
    int cheight = height >> 1;

    jpeg_strip.yuv_crc = jpeg_crc_rows(row[0], stride,
                                      LCD_WIDTH, height,
                                      jpeg_strip.yuv_crc);
    jpeg_strip.yuv_crc = jpeg_crc_rows(row[1], cstride,
                                      LCD_WIDTH >> 1, cheight,
                                      jpeg_strip.yuv_crc);
    jpeg_strip.yuv_crc = jpeg_crc_rows(row[2], cstride,
                                      LCD_WIDTH >> 1, cheight,
                                      jpeg_strip.yuv_crc);
}

static uint32_t jpeg_framebuffer_crc(void)
{
    struct viewport *vp_main =
        *(rb->screens[SCREEN_MAIN]->current_viewport);
    uint32_t crc = 0xffffffff;
    int row;

    if (vp_main == NULL || vp_main->buffer == NULL ||
        vp_main->buffer->fb_ptr == NULL ||
        vp_main->x != 0 || vp_main->y != 0 ||
        vp_main->width != LCD_WIDTH || vp_main->height != LCD_HEIGHT ||
        vp_main->buffer->stride != LCD_WIDTH)
        return 0;

    for (row = 0; row < LCD_HEIGHT; row++)
    {
        const fb_data *src = vp_main->buffer->fb_ptr + row * LCD_WIDTH;
        crc = rb->crc_32(src, LCD_WIDTH * sizeof(fb_data), crc);
    }
    return crc;
}

static void jpeg_strip_log(const struct image_info *info)
{
    const char *name;
    struct jpeg_rgb_cache *cache = &rgb_cache[1];
    uint32_t cache_crc;
    uint32_t fb_crc;
    int fd;

    if (!jpeg_strip.test_enabled || jpeg_strip.logged ||
        !cache->valid || info->width != LCD_WIDTH ||
        info->height != LCD_HEIGHT)
        return;

    cache_crc = rb->crc_32(cache->pixels,
        (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data), 0xffffffff);
    fb_crc = jpeg_framebuffer_crc();

    fd = rb->open(ROCKBOX_DIR "/jpeg7mem.csv",
                  O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    if (rb->filesize(fd) == 0)
        rb->fdprintf(fd,
            "mode,file,total_us,first_strip_us,lcd_us,strips,"
            "yuv_crc,cache_crc,fb_crc,used_bytes,full_yuv_bytes,"
            "strip_bytes,cache_bytes,strip_only,lcd_streamed\n");

    name = rb->strrchr(jpeg_strip.filename, '/');
    name = name != NULL ? name + 1 : jpeg_strip.filename;

    rb->fdprintf(fd,
        "%s,%s,%lu,%lu,%lu,%d,%08lx,%08lx,%08lx,"
        "%lu,%lu,%lu,%lu,%d,%d\n",
        jpeg_strip.test_reference ? "reference" : "accelerated",
        name,
        (unsigned long)jpeg_strip.total_us,
        (unsigned long)jpeg_strip.first_strip_us,
        (unsigned long)jpeg_strip.lcd_us,
        jpeg_strip.strips,
        (unsigned long)jpeg_strip.yuv_crc,
        (unsigned long)cache_crc,
        (unsigned long)fb_crc,
        (unsigned long)jpeg_strip.used_bytes,
        (unsigned long)jpeg_strip.full_yuv_bytes,
        (unsigned long)jpeg_strip.strip_bytes,
        (unsigned long)jpeg_strip.cache_bytes,
        jpeg_strip.strip_only ? 1 : 0,
        jpeg_strip.lcd_ok ? 1 : 0);

    rb->close(fd);
    jpeg_strip.logged = true;
}
