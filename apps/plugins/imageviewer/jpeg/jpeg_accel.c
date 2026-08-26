/***************************************************************************
 * JPEG imageviewer integration for the iPod Photo acceleration path.
 *
 * The original jpeg.c remains unchanged and is included with its public
 * entry points renamed. This file adds an opportunistic screen-sized RGB565
 * cache plus the iPod Color full-range direct LCD path.
 ****************************************************************************/

#include "plugin.h"
#include "../imageviewer.h"
#include "jpeg_decoder.h"
#include "jpeg_hwtest.h"
#ifdef HAVE_LCD_COLOR
#include "yuv2rgb.h"
#endif
#if defined(IPOD_COLOR)
#include "jpeg_lcd_fullrange.h"
#endif

bool jpeg_hwtest_reference_mode;
bool jpeg_hwtest_enabled;

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
    int ds;
    bool valid;
    bool crc_valid;
    bool logged;
    uint32_t decode_us;
    uint32_t yuv_crc;
    uint32_t rgb_crc;
};

static struct jpeg_rgb_cache rgb_cache[9];
#endif

static char jpeg_hwtest_filename[MAX_PATH];

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

static void jpeg_hwtest_refresh(void)
{
    jpeg_hwtest_enabled =
        jpeg_path_exists(ROCKBOX_DIR "/jpeg56.enabled");
    jpeg_hwtest_reference_mode = jpeg_hwtest_enabled &&
        jpeg_path_exists(ROCKBOX_DIR "/jpeg56.reference");
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

static bool jpeg_prepare_cache(const struct image_info *info,
                               struct t_disp *pdisp,
                               struct jpeg_rgb_cache *cache)
{
    if (cache == NULL || cache->pixels == NULL)
        return false;

    if (!cache->valid)
        cache->valid = yuv_bitmap_part_to_buffer(
            pdisp->bitmap, pdisp->csub_x, pdisp->csub_y,
            0, 0, pdisp->stride, info->width, info->height,
            cache->pixels, cache->stride);

    if (cache->valid && jpeg_hwtest_enabled && !cache->crc_valid)
    {
        cache->yuv_crc = jpeg_yuv_crc(info, pdisp);
        cache->rgb_crc = rb->crc_32(cache->pixels,
            info->width * info->height * sizeof(fb_data), 0xffffffff);
        cache->crc_valid = true;
    }

    return cache->valid;
}

static void jpeg_hwtest_log(const struct image_info *info,
                            struct jpeg_rgb_cache *cache,
                            bool direct_lcd, uint32_t lcd_crc,
                            uint32_t direct_us)
{
    const char *name;
    int fd;

    if (!jpeg_hwtest_enabled || cache == NULL || cache->logged ||
        !cache->valid || !cache->crc_valid)
        return;

    fd = rb->open(ROCKBOX_DIR "/jpeg56.csv",
                  O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    if (rb->filesize(fd) == 0)
        rb->fdprintf(fd,
            "mode,file,ds,width,height,decode_us,yuv_crc,rgb_crc,"
            "lcd_rgb_crc,direct_us,direct_lcd\n");

    name = rb->strrchr(jpeg_hwtest_filename, '/');
    name = name != NULL ? name + 1 : jpeg_hwtest_filename;

    rb->fdprintf(fd,
        "%s,%s,%d,%d,%d,%lu,%08lx,%08lx,%08lx,%lu,%d\n",
        jpeg_hwtest_reference_mode ? "reference" : "accelerated",
        name, cache->ds, info->width, info->height,
        (unsigned long)cache->decode_us,
        (unsigned long)cache->yuv_crc,
        (unsigned long)cache->rgb_crc,
        (unsigned long)lcd_crc,
        (unsigned long)direct_us,
        direct_lcd ? 1 : 0);

    rb->close(fd);
    cache->logged = true;
}
#endif

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize)
{
    int status;

#ifdef HAVE_LCD_COLOR
    jpeg_cache_clear();
    iv->skip_next_update = false;
#endif
    jpeg_hwtest_refresh();
    rb->snprintf(jpeg_hwtest_filename, sizeof(jpeg_hwtest_filename),
                 "%s", filename);

    status = jpeg_legacy_load_image(filename, info, buf, buf_size,
                                    offset, filesize);
    if (status == PLUGIN_OK && jpg.table_error != 0)
    {
        rb->splashf(HZ, "JPEG table error %d", jpg.table_error);
        return PLUGIN_ERROR;
    }

    return status;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    int status;
#ifdef HAVE_LCD_COLOR
    bool already_decoded;
    uint32_t started;
#endif

    if (ds < 1 || ds > 8)
        return PLUGIN_ERROR;

#ifdef HAVE_LCD_COLOR
    already_decoded = disp[ds].bitmap[0] != NULL;
    if (!already_decoded && buf_images_size <= img_mem(ds))
        jpeg_cache_clear();
    started = jpeg_now_us();
#endif

    status = jpeg_legacy_get_image(info, frame, ds);
    if (status != PLUGIN_OK)
        return status;

#ifdef HAVE_LCD_COLOR
    jpeg_allocate_cache(info, ds);
    if (!already_decoded)
        rgb_cache[ds].decode_us = jpeg_now_us() - started;
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
    bool colour_fast =
        iv->settings->jpeg_colour_mode == COLOURMODE_COLOUR &&
        iv->settings->jpeg_dither_mode == DITHER_NONE;

#if defined(IPOD_COLOR)
    if (!jpeg_hwtest_reference_mode && colour_fast &&
        pdisp->csub_x == 2 && pdisp->csub_y == 2 &&
        info->width == LCD_WIDTH && info->height == LCD_HEIGHT &&
        info->x == 0 && info->y == 0 &&
        x == 0 && y == 0 && width == LCD_WIDTH && height == LCD_HEIGHT)
    {
        uint32_t lcd_crc = 0;
        uint32_t direct_us;
        uint32_t started;
        bool direct_ok;

        if (jpeg_hwtest_enabled)
            jpeg_prepare_cache(info, pdisp, cache);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        rb->cpu_boost(true);
#endif
        started = jpeg_now_us();
        direct_ok = jpeg_lcd_blit_yuv420_fullrange(
            pdisp->bitmap, 0, 0, pdisp->stride,
            0, 0, LCD_WIDTH, LCD_HEIGHT,
            jpeg_hwtest_enabled ? &lcd_crc : NULL);
        direct_us = jpeg_now_us() - started;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        rb->cpu_boost(false);
#endif

        if (direct_ok)
        {
            if (jpeg_hwtest_enabled)
                jpeg_hwtest_log(info, cache, true, lcd_crc, direct_us);
            imgdec_skip_next_lcd_update();
            return;
        }
    }
#endif

    if (colour_fast && jpeg_prepare_cache(info, pdisp, cache))
    {
        rb->lcd_bitmap_part(cache->pixels,
                            info->x + x, info->y + y,
                            cache->stride,
                            dst_x, dst_y, width, height);
        jpeg_hwtest_log(info, cache, false, cache->rgb_crc, 0);
        return;
    }

    jpeg_legacy_draw_image_rect(info, x, y, width, height);
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

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
const struct plugin_api *rb DATA_ATTR;
struct imgdec_api *iv DATA_ATTR;
const struct imgdec_header __header
__attribute__ ((section (".header"))) = {
    { PLUGIN_MAGIC, TARGET_ID, IMGDEC_API_VERSION,
      plugin_start_addr, plugin_end_addr, }, &image_decoder,
      &rb, PLUGIN_API_VERSION, sizeof(struct plugin_api),
      &iv, sizeof(struct imgdec_api)
};
#else
const struct plugin_api *rb DATA_ATTR;
struct imgdec_api *iv DATA_ATTR;
const struct imgdec_header __header
__attribute__((visibility("default"))) = {
    { PLUGIN_MAGIC, TARGET_ID, IMGDEC_API_VERSION, NULL, NULL },
    &image_decoder, &rb, PLUGIN_API_VERSION, sizeof(struct plugin_api),
    &iv, sizeof(struct imgdec_api),
};
#endif
