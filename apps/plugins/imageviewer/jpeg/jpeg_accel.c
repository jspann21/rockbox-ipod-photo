/***************************************************************************
 * JPEG imageviewer integration for the low-risk acceleration set.
 *
 * The original jpeg.c remains untouched and is included with its entry
 * points renamed.  This file adds a screen-sized RGB565 cache and an
 * opt-in microsecond/CRC logger.  A .rockbox/jpegbench.reference sentinel
 * selects the legacy decode/conversion loops, so reference and accelerated
 * measurements can be collected from the same installed build.
 ****************************************************************************/

#include "plugin.h"
#include "../imageviewer.h"
#include "jpeg_decoder.h"
#include "jpeg_accel.h"
#ifdef HAVE_LCD_COLOR
#include "yuv2rgb.h"
#endif

bool jpeg_accel_reference_mode;

/*
 * Renaming the legacy decoder object also renames its struct tag while the
 * source is included.  Provide a layout-identical private tag so the legacy
 * initializer remains type-correct without changing imageviewer.h.
 */
struct jpeg_legacy_decoder
{
    const bool unscaled_avail;
    int (*img_mem)(int ds);
    int (*load_image)(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize);
    int (*get_image)(struct image_info *info, int frame, int ds);
    void (*draw_image_rect)(struct image_info *info,
                            int x, int y, int width, int height);
};

/* Prevent the included legacy integration from emitting the plugin header. */
#undef IMGDEC_HEADER
#define IMGDEC_HEADER
#define draw_image_rect jpeg_legacy_draw_image_rect
#define load_image      jpeg_legacy_load_image
#define get_image       jpeg_legacy_get_image
#define image_decoder   jpeg_legacy_decoder
#include "jpeg.c"
#undef draw_image_rect
#undef load_image
#undef get_image
#undef image_decoder
#undef IMGDEC_HEADER

#ifdef HAVE_LCD_COLOR
struct jpeg_rgb_cache
{
    fb_data *pixels;
    int stride;
    int width;
    int height;
    int ds;
    bool valid;
    bool verified;
    bool logged;
    uint32_t decode_us;
    uint32_t conversion_us;
    uint32_t draw_us;
    uint32_t yuv_crc;
    uint32_t rgb_crc;
    uint32_t reference_rgb_crc;
    uint32_t mismatches;
    uint32_t decoded_blocks;
    uint32_t dc_only_blocks;
};

static struct jpeg_rgb_cache rgb_cache[9];
#endif

static bool jpegbench_configured;
static bool jpegbench_enabled;
static char jpegbench_filename[MAX_PATH];
static uint32_t jpegbench_load_us;

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

#ifdef HAVE_LCD_COLOR
static void jpeg_cache_clear(void)
{
    rb->memset(rgb_cache, 0, sizeof(rgb_cache));
}

static int jpeg_find_ds(const struct t_disp *pdisp)
{
    static const unsigned char scales[] = {1, 2, 4, 8};
    unsigned int i;

    for (i = 0; i < ARRAYLEN(scales); i++)
    {
        int ds = scales[i];
        if (pdisp == &disp[ds])
            return ds;
    }
    return 0;
}

static void jpeg_allocate_cache(const struct image_info *info, int ds)
{
    struct jpeg_rgb_cache *cache;
    uintptr_t address, aligned;
    size_t padding, bytes;

    if (ds < 1 || ds > 8 || info->width <= 0 || info->height <= 0 ||
        info->width > LCD_WIDTH || info->height > LCD_HEIGHT)
        return;

    cache = &rgb_cache[ds];
    if (cache->pixels != NULL)
        return;

    address = (uintptr_t)buf_images;
    aligned = (address + sizeof(fb_data) - 1) &
              ~((uintptr_t)sizeof(fb_data) - 1);
    padding = aligned - address;
    bytes = (size_t)info->width * info->height * sizeof(fb_data);

    if (buf_images_size <= (ssize_t)(padding + bytes))
        return;

    buf_images += padding;
    buf_images_size -= padding;
    cache->pixels = (fb_data *)buf_images;
    cache->stride = info->width;
    cache->width = info->width;
    cache->height = info->height;
    cache->ds = ds;
    buf_images += bytes;
    buf_images_size -= bytes;
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

static uint32_t jpeg_yuv_crc(const struct image_info *info,
                             const struct t_disp *pdisp)
{
    uint32_t crc = 0xffffffff;

    crc = jpeg_crc_rows(pdisp->bitmap[0], pdisp->stride,
                         info->width, info->height, crc);

    if (pdisp->csub_x > 0 && pdisp->csub_y > 0)
    {
        int cwidth = (info->width + pdisp->csub_x - 1) / pdisp->csub_x;
        int cheight = (info->height + pdisp->csub_y - 1) / pdisp->csub_y;
        int cstride = pdisp->stride / pdisp->csub_x;

        crc = jpeg_crc_rows(pdisp->bitmap[1], cstride,
                             cwidth, cheight, crc);
        crc = jpeg_crc_rows(pdisp->bitmap[2], cstride,
                             cwidth, cheight, crc);
    }

    return crc;
}

static void jpeg_prepare_crc(const struct image_info *info,
                             struct t_disp *pdisp,
                             struct jpeg_rgb_cache *cache)
{
    if (!jpegbench_enabled || cache->pixels == NULL || !cache->valid ||
        cache->verified)
        return;

    cache->yuv_crc = jpeg_yuv_crc(info, pdisp);
    cache->rgb_crc = rb->crc_32(cache->pixels,
        info->width * info->height * sizeof(fb_data), 0xffffffff);
    cache->verified = yuv_bitmap_verify_rgb565(
        pdisp->bitmap, pdisp->csub_x, pdisp->csub_y,
        0, 0, pdisp->stride, info->width, info->height,
        cache->pixels, cache->stride,
        &cache->reference_rgb_crc, &cache->mismatches);
}

static void jpegbench_log(const struct image_info *info,
                          struct jpeg_rgb_cache *cache)
{
    const char *name;
    int fd;

    if (!jpegbench_enabled || cache->logged || !cache->valid)
        return;

    fd = rb->open(ROCKBOX_DIR "/jpegbench.csv",
                  O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    if (rb->filesize(fd) == 0)
    {
        rb->fdprintf(fd,
            "mode,file,ds,width,height,load_us,decode_us,conversion_us,"
            "draw_us,blocks,dc_only_blocks,yuv_crc,rgb_crc,"
            "reference_rgb_crc,mismatches,cache_bytes\n");
    }

    name = rb->strrchr(jpegbench_filename, '/');
    name = name != NULL ? name + 1 : jpegbench_filename;
    rb->fdprintf(fd,
        "%s,%s,%d,%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%08lx,%08lx,"
        "%08lx,%lu,%lu\n",
        jpeg_accel_reference_mode ? "reference" : "accelerated",
        name, cache->ds, info->width, info->height,
        (unsigned long)jpegbench_load_us,
        (unsigned long)cache->decode_us,
        (unsigned long)cache->conversion_us,
        (unsigned long)cache->draw_us,
        (unsigned long)cache->decoded_blocks,
        (unsigned long)cache->dc_only_blocks,
        (unsigned long)cache->yuv_crc,
        (unsigned long)cache->rgb_crc,
        (unsigned long)cache->reference_rgb_crc,
        (unsigned long)cache->mismatches,
        (unsigned long)(info->width * info->height * sizeof(fb_data)));
    rb->close(fd);
    cache->logged = true;
}
#endif /* HAVE_LCD_COLOR */

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize)
{
    uint32_t start;
    int status;

#ifdef HAVE_LCD_COLOR
    jpeg_cache_clear();
#endif
    if (!jpegbench_configured)
    {
        jpegbench_enabled =
            jpeg_path_exists(ROCKBOX_DIR "/jpegbench.enabled");
        jpeg_accel_reference_mode = jpegbench_enabled &&
            jpeg_path_exists(ROCKBOX_DIR "/jpegbench.reference");
        jpegbench_configured = true;
    }
    rb->snprintf(jpegbench_filename, sizeof(jpegbench_filename),
                 "%s", filename);

    start = jpeg_now_us();
    status = jpeg_legacy_load_image(filename, info, buf, buf_size,
                                    offset, filesize);
    jpegbench_load_us = jpeg_now_us() - start;

    if (status == PLUGIN_OK && jpg.table_error != 0)
    {
        rb->splashf(HZ, "JPEG table error %d", jpg.table_error);
        return PLUGIN_ERROR;
    }

    return status;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    uint32_t start;
    bool already_decoded;
    int status;

    if (ds < 1 || ds > 8)
        return PLUGIN_ERROR;

    already_decoded = disp[ds].bitmap[0] != NULL;
#ifdef HAVE_LCD_COLOR
    if (!already_decoded && buf_images_size <= img_mem(ds))
        jpeg_cache_clear();
#endif

    start = jpeg_now_us();
    status = jpeg_legacy_get_image(info, frame, ds);
    if (status != PLUGIN_OK)
        return status;

#ifdef HAVE_LCD_COLOR
    if (!already_decoded)
    {
        struct jpeg_rgb_cache *cache = &rgb_cache[ds];
        cache->decode_us = jpeg_now_us() - start;
        cache->decoded_blocks = jpg.decoded_blocks;
        cache->dc_only_blocks = jpg.dc_only_blocks;
    }
    jpeg_allocate_cache(info, ds);
#else
    (void)start;
#endif

    return PLUGIN_OK;
}

static void draw_image_rect(struct image_info *info,
                            int x, int y, int width, int height)
{
#ifdef HAVE_LCD_COLOR
    struct t_disp *pdisp = (struct t_disp *)info->data;
    int ds = jpeg_find_ds(pdisp);
    struct jpeg_rgb_cache *cache = ds ? &rgb_cache[ds] : NULL;
    int dst_x = x + MAX(0, (LCD_WIDTH - info->width) / 2);
    int dst_y = y + MAX(0, (LCD_HEIGHT - info->height) / 2);
    uint32_t start;

    if (jpeg_accel_reference_mode)
    {
        start = jpeg_now_us();
        jpeg_legacy_draw_image_rect(info, x, y, width, height);
        if (cache != NULL)
            cache->draw_us += jpeg_now_us() - start;

        if (cache != NULL && cache->pixels != NULL && !cache->valid &&
            iv->settings->jpeg_colour_mode == COLOURMODE_COLOUR &&
            iv->settings->jpeg_dither_mode == DITHER_NONE)
        {
            start = jpeg_now_us();
            cache->valid = yuv_bitmap_part_to_buffer(
                pdisp->bitmap, pdisp->csub_x, pdisp->csub_y,
                0, 0, pdisp->stride, info->width, info->height,
                cache->pixels, cache->stride);
            cache->conversion_us = jpeg_now_us() - start;
            jpeg_prepare_crc(info, pdisp, cache);
            jpegbench_log(info, cache);
        }
        return;
    }

    if (cache != NULL && cache->pixels != NULL &&
        iv->settings->jpeg_colour_mode == COLOURMODE_COLOUR &&
        iv->settings->jpeg_dither_mode == DITHER_NONE)
    {
        if (!cache->valid)
        {
            start = jpeg_now_us();
            cache->valid = yuv_bitmap_part_to_buffer(
                pdisp->bitmap, pdisp->csub_x, pdisp->csub_y,
                0, 0, pdisp->stride, info->width, info->height,
                cache->pixels, cache->stride);
            cache->conversion_us = jpeg_now_us() - start;
            jpeg_prepare_crc(info, pdisp, cache);
        }

        if (cache->valid)
        {
            start = jpeg_now_us();
            rb->lcd_bitmap_part(cache->pixels,
                                info->x + x, info->y + y,
                                cache->stride,
                                dst_x, dst_y, width, height);
            cache->draw_us += jpeg_now_us() - start;
            jpegbench_log(info, cache);
            return;
        }
    }

    start = jpeg_now_us();
    jpeg_legacy_draw_image_rect(info, x, y, width, height);
    if (cache != NULL)
        cache->draw_us += jpeg_now_us() - start;
#else
    jpeg_legacy_draw_image_rect(info, x, y, width, height);
#endif
}

const struct image_decoder image_decoder = {
    false,
    img_mem,
    load_image,
    get_image,
    draw_image_rect,
};

/* Equivalent to IMGDEC_HEADER, emitted here after the public decoder above. */
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
const struct plugin_api *rb DATA_ATTR;
const struct imgdec_api *iv DATA_ATTR;
const struct imgdec_header __header
__attribute__ ((section (".header"))) = {
    { PLUGIN_MAGIC, TARGET_ID, IMGDEC_API_VERSION,
      plugin_start_addr, plugin_end_addr, }, &image_decoder,
      &rb, PLUGIN_API_VERSION, sizeof(struct plugin_api),
      &iv, sizeof(struct imgdec_api)
};
#else
const struct plugin_api *rb DATA_ATTR;
const struct imgdec_api *iv DATA_ATTR;
const struct imgdec_header __header
__attribute__((visibility("default"))) = {
    { PLUGIN_MAGIC, TARGET_ID, IMGDEC_API_VERSION, NULL, NULL },
    &image_decoder, &rb, PLUGIN_API_VERSION, sizeof(struct plugin_api),
    &iv, sizeof(struct imgdec_api),
};
#endif
