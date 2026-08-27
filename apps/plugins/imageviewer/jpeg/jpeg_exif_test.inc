#if defined(HAVE_LCD_COLOR)

static bool jpeg_exif_test_enabled;
static bool jpeg_exif_test_logged[9];
static char jpeg_exif_test_filename[MAX_PATH];
static fb_data jpeg_exif_crc_row[LCD_WIDTH];

static void jpeg_exif_test_reset(const char *filename)
{
    int fd;

    rb->memset(jpeg_exif_test_logged, 0, sizeof(jpeg_exif_test_logged));
    rb->strlcpy(jpeg_exif_test_filename, filename,
                sizeof(jpeg_exif_test_filename));

    fd = rb->open(ROCKBOX_DIR "/jpegexif.enabled", O_RDONLY);
    jpeg_exif_test_enabled = fd >= 0;
    if (fd >= 0)
        rb->close(fd);
}

static uint32_t jpeg_exif_canonical_crc(
    const struct jpeg_rgb_cache *cache, int ds)
{
    int raw_width = jpg.x_size / ds;
    int raw_height = jpg.y_size / ds;
    uint32_t crc = 0xffffffff;
    int sy;

    if (raw_width <= 0 || raw_width > LCD_WIDTH ||
        raw_height <= 0 || cache == NULL ||
        cache->pixels == NULL || !cache->valid)
        return 0;

    for (sy = 0; sy < raw_height; sy++)
    {
        int sx;
        for (sx = 0; sx < raw_width; sx++)
        {
            int dx, dy;
            jpeg_exif_map_source(sx, sy, raw_width, raw_height, &dx, &dy);
            jpeg_exif_crc_row[sx] =
                cache->pixels[dy * cache->stride + dx];
        }
        crc = rb->crc_32(jpeg_exif_crc_row,
                         raw_width * sizeof(fb_data), crc);
    }

    return crc;
}

static void jpeg_exif_test_log(const struct image_info *info,
                               const struct jpeg_rgb_cache *cache,
                               int ds)
{
    const char *name;
    uint32_t canonical_crc;
    uint32_t oriented_crc;
    int raw_width;
    int raw_height;
    int fd;

    if (!jpeg_exif_test_enabled || ds < 1 || ds > 8 ||
        jpeg_exif_test_logged[ds] || cache == NULL || !cache->valid)
        return;

    raw_width = jpg.x_size / ds;
    raw_height = jpg.y_size / ds;
    canonical_crc = jpeg_exif_canonical_crc(cache, ds);
    if (canonical_crc == 0)
        return;

    oriented_crc = rb->crc_32(
        cache->pixels,
        (size_t)info->width * info->height * sizeof(fb_data),
        0xffffffff);

    fd = rb->open(ROCKBOX_DIR "/jpegexif.csv",
                  O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    if (rb->filesize(fd) == 0)
        rb->fdprintf(fd,
            "file,orientation,thumb_offset,thumb_length,"
            "raw_width,raw_height,width,height,ds,"
            "canonical_crc,oriented_crc\n");

    name = rb->strrchr(jpeg_exif_test_filename, '/');
    name = name != NULL ? name + 1 : jpeg_exif_test_filename;

    rb->fdprintf(fd,
        "%s,%u,%lu,%lu,%d,%d,%d,%d,%d,%08lx,%08lx\n",
        name, jpeg_exif_meta.orientation,
        jpeg_exif_meta.thumbnail_offset,
        jpeg_exif_meta.thumbnail_length,
        raw_width, raw_height, info->width, info->height, ds,
        (unsigned long)canonical_crc,
        (unsigned long)oriented_crc);

    rb->close(fd);
    jpeg_exif_test_logged[ds] = true;
}

#endif
