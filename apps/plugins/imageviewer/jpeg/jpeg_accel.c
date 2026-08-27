/***************************************************************************
 * JPEG imageviewer integration for the iPod Photo acceleration path.
 *
 * The original jpeg.c remains unchanged and is included with its public
 * entry points renamed. This file adds fit-to-screen RGB565 caching, direct
 * full-range LCD output, and reusable MCU-row decoding for exact-screen JPEGs.
 ****************************************************************************/

#include "plugin.h"
#include "../imageviewer.h"
#include "jpeg_decoder.h"
#ifdef HAVE_LCD_COLOR
#include "yuv2rgb.h"
#endif
#if defined(IPOD_COLOR)
#include "jpeg_lcd_fullrange.h"
#endif

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
    bool valid;
};

static struct jpeg_rgb_cache rgb_cache[9];

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
    buf_images += bytes;
    buf_images_size -= bytes;
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

    return cache->valid;
}
#endif

#if defined(IPOD_COLOR) && defined(HAVE_LCD_COLOR)
#include "jpeg_strip_memory_state.inc"
#include "jpeg_strip_memory_decode.inc"
#endif

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize)
{
    int status;

#ifdef HAVE_LCD_COLOR
    jpeg_cache_clear();
    iv->skip_next_clear = false;
    iv->skip_next_update = false;
#endif
#if defined(IPOD_COLOR) && defined(HAVE_LCD_COLOR)
    jpeg_decode_set_mcu_row_callback(NULL, NULL);
    jpeg_decode_set_mcu_row_reuse(false);
    jpeg_lcd_stream_abort();
    jpeg_strip_reset(filename);
#endif

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
#endif

    if (ds < 1 || ds > 8)
        return PLUGIN_ERROR;

#ifdef HAVE_LCD_COLOR
    already_decoded = disp[ds].bitmap[0] != NULL;
#endif

#if defined(IPOD_COLOR) && defined(HAVE_LCD_COLOR)
    if (ds == 1 && jpeg_strip.decoded && rgb_cache[1].valid)
    {
        info->width = LCD_WIDTH;
        info->height = LCD_HEIGHT;
        info->data = &disp[1];
        return PLUGIN_OK;
    }

    if (jpeg_stream_eligible(info, ds, already_decoded))
    {
        bool strip_only = !jpeg_strip.test_reference;

        status = jpeg_stream_get_image(info, frame, ds, strip_only);
        if (status == PLUGIN_OUTOFMEM)
        {
            jpeg_reset_decoded_pool();
            status = jpeg_stream_get_image(info, frame, ds, strip_only);
        }

        if (status == PLUGIN_OK)
            return PLUGIN_OK;

        jpeg_reset_decoded_pool();
        already_decoded = false;
    }
#endif

#ifdef HAVE_LCD_COLOR
    if (!already_decoded && buf_images_size <= img_mem(ds))
        jpeg_cache_clear();
#endif

    status = jpeg_legacy_get_image(info, frame, ds);
    if (status != PLUGIN_OK)
        return status;

#ifdef HAVE_LCD_COLOR
    jpeg_allocate_cache(info, ds);
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
    if (jpeg_strip.frame_pending)
    {
        bool exact_frame =
            info->width == LCD_WIDTH && info->height == LCD_HEIGHT &&
            info->x == 0 && info->y == 0 &&
            x == 0 && y == 0 && width == LCD_WIDTH && height == LCD_HEIGHT;

        jpeg_strip.frame_pending = false;
        if (exact_frame)
            return;

        iv->skip_next_update = false;
        rb->lcd_clear_display();
    }

    if (jpeg_strip.decoded && jpeg_strip.strip_only && ds == 1)
    {
        if (colour_fast && cache != NULL && cache->valid)
        {
            rb->lcd_bitmap_part(cache->pixels,
                                info->x + x, info->y + y,
                                cache->stride,
                                dst_x, dst_y, width, height);
            return;
        }

        if (!jpeg_materialize_planar(info, ds))
            return;

        pdisp = (struct t_disp *)info->data;
        cache = &rgb_cache[ds];
    }

    if (colour_fast &&
        pdisp->csub_x == 2 && pdisp->csub_y == 2 &&
        info->width == LCD_WIDTH && info->height == LCD_HEIGHT &&
        info->x == 0 && info->y == 0 &&
        x == 0 && y == 0 && width == LCD_WIDTH && height == LCD_HEIGHT)
    {
        bool direct_ok;

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        rb->cpu_boost(true);
#endif
        direct_ok = jpeg_lcd_blit_yuv420_fullrange(
            pdisp->bitmap, 0, 0, pdisp->stride,
            0, 0, LCD_WIDTH, LCD_HEIGHT);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        rb->cpu_boost(false);
#endif

        if (direct_ok)
        {
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
