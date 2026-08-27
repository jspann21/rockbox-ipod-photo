/***************************************************************************
 * EXIF metadata reader with embedded-JPEG thumbnail preview.
 *
 * The bounded metadata parser remains in jpeg_exif.c. This wrapper reuses it,
 * then opportunistically decodes a validated baseline-JPEG IFD1 thumbnail into
 * the current framebuffer before the full image load/decode begins.
 ****************************************************************************/

#include "plugin.h"
#include "../imageviewer.h"
#include "jpeg_decoder.h"
#include "jpeg_exif.h"

extern struct imgdec_api *iv;
#ifdef HAVE_LCD_COLOR
#include "yuv2rgb.h"
#endif

#define jpeg_exif_reset jpeg_exif_reset_metadata
#define jpeg_exif_read  jpeg_exif_read_metadata
#include "jpeg_exif.c"
#undef jpeg_exif_reset
#undef jpeg_exif_read

#ifdef HAVE_LCD_COLOR

#define JPEG_PREVIEW_MIN_SOURCE_PIXELS \
    ((unsigned long)LCD_WIDTH * LCD_HEIGHT * 4UL)

static struct jpeg preview_jpg;

static bool preview_main_large_enough(const unsigned char *src, size_t size)
{
    size_t pos = 2;

    if (src == NULL || size < 4 || src[0] != 0xff || src[1] != 0xd8)
        return false;

    while (pos + 4 <= size)
    {
        unsigned int marker;
        unsigned int length;
        size_t payload;

        while (pos < size && src[pos] == 0xff)
            pos++;
        if (pos >= size)
            break;

        marker = src[pos++];
        if (marker == 0xda || marker == 0xd9)
            break;
        if (marker == 0x00 || marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd8))
            continue;

        if (pos + 2 > size)
            break;

        length = ((unsigned int)src[pos] << 8) | src[pos + 1];
        if (length < 2)
            break;

        payload = pos + 2;
        if (payload > size || length - 2 > size - payload)
            break;

        if ((marker == 0xc0 || marker == 0xc1 || marker == 0xc2) &&
            length >= 8)
        {
            unsigned int height =
                ((unsigned int)src[payload + 1] << 8) | src[payload + 2];
            unsigned int width =
                ((unsigned int)src[payload + 3] << 8) | src[payload + 4];
            unsigned long pixels = (unsigned long)width * height;

            return width != 0 && height != 0 &&
                   pixels >= JPEG_PREVIEW_MIN_SOURCE_PIXELS;
        }

        pos = payload + length - 2;
    }

    /* Unknown dimensions mean no speculative preview work. */
    return false;
}

static void preview_map(int orientation, int sx, int sy,
                        int raw_width, int raw_height,
                        int *dx, int *dy)
{
    switch (orientation)
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

static bool preview_alloc_planes(unsigned char *scratch, size_t scratch_size,
                                 size_t compressed_size, int ds,
                                 unsigned char *planes[3],
                                 fb_data **rgb)
{
    unsigned char *cursor = scratch + compressed_size;
    unsigned char *end = scratch + scratch_size;
    int i;
    size_t size;
    size_t rgb_pixels;
    int stride = preview_jpg.x_phys / ds;
    int raw_width = preview_jpg.x_size / ds;
    int raw_height = preview_jpg.y_size / ds;
    uintptr_t aligned;

    if (stride <= 0 || raw_width <= 0 || raw_height <= 0 ||
        cursor > end)
        return false;

    if (preview_jpg.blocks > 1)
    {
        for (i = 1; i < 3; i++)
        {
            int sx = preview_jpg.subsample_x[i];
            int sy = preview_jpg.subsample_y[i];

            if (sx <= 0 || sy <= 0)
                return false;

            size = (size_t)(preview_jpg.x_phys / ds / sx) *
                   (preview_jpg.y_phys / ds / sy);
            if (size > (size_t)(end - cursor))
                return false;

            planes[i] = cursor;
            cursor += size;
        }
    }
    else
    {
        planes[1] = planes[2] = cursor;
    }

    size = (size_t)stride * raw_height;
    if (size > (size_t)(end - cursor))
        return false;

    planes[0] = cursor;
    cursor += size;

    aligned = ((uintptr_t)cursor + sizeof(fb_data) - 1) &
              ~((uintptr_t)sizeof(fb_data) - 1);
    if (aligned < (uintptr_t)cursor || aligned > (uintptr_t)end)
        return false;

    cursor = (unsigned char *)aligned;
    rgb_pixels = (size_t)raw_width * raw_height;
    if (rgb_pixels > (size_t)(end - cursor) / sizeof(fb_data))
        return false;

    *rgb = (fb_data *)cursor;
    return true;
}

static int preview_choose_ds(const struct jpeg_exif *exif)
{
    int ds = 1;

    while (ds < 8)
    {
        int width = preview_jpg.x_size / ds;
        int height = preview_jpg.y_size / ds;
        int shown_width = exif->orientation >= 5 ? height : width;
        int shown_height = exif->orientation >= 5 ? width : height;

        if (shown_width <= LCD_WIDTH && shown_height <= LCD_HEIGHT)
            break;
        ds <<= 1;
    }

    return ds;
}

static bool jpeg_exif_try_preview(const char *filename, int offset,
                                  unsigned char *scratch,
                                  size_t scratch_size,
                                  struct jpeg_exif *exif)
{
    unsigned long relative;
    size_t length;
    unsigned char *planes[3] = { NULL, NULL, NULL };
    fb_data *rgb = NULL;
    struct viewport *vp;
    int status;
    int ds;
    int raw_width;
    int raw_height;
    int shown_width;
    int shown_height;
    int x0;
    int y0;
    int sy;

    (void)filename;

    if (exif->thumbnail_length < 4 ||
        exif->thumbnail_offset < (unsigned long)offset ||
        scratch == NULL || scratch_size < 1024 ||
        iv == NULL || iv->settings == NULL ||
        !(iv->settings->hide_info || iv->running_slideshow) ||
        iv->settings->jpeg_colour_mode != COLOURMODE_COLOUR ||
        iv->settings->jpeg_dither_mode != DITHER_NONE)
        return false;

    relative = exif->thumbnail_offset - (unsigned long)offset;
    length = exif->thumbnail_length;

    if (relative > scratch_size || length > scratch_size - relative ||
        length >= scratch_size)
        return false;

    rb->memmove(scratch, scratch + relative, length);
    rb->memset(&preview_jpg, 0, sizeof(preview_jpg));

    status = process_markers(scratch, (long)length, &preview_jpg);
    if (status < 0 || (status & (DQT | SOF0)) != (DQT | SOF0) ||
        preview_jpg.table_error != 0)
        return false;

    if (!(status & DHT))
        default_huff_tbl(&preview_jpg);
    build_lut(&preview_jpg);

    ds = preview_choose_ds(exif);
    raw_width = preview_jpg.x_size / ds;
    raw_height = preview_jpg.y_size / ds;
    if (raw_width <= 0 || raw_height <= 0)
        return false;

    if (!preview_alloc_planes(scratch, scratch_size, length, ds,
                              planes, &rgb))
        return false;

    jpeg_decode_set_mcu_row_callback(NULL, NULL);
    jpeg_decode_set_mcu_row_reuse(false);
    if (jpeg_decode(&preview_jpg, planes, ds, NULL) != 0)
        return false;

    if (!yuv_bitmap_part_to_buffer(
            planes,
            preview_jpg.blocks > 1 ? preview_jpg.subsample_x[1] : 0,
            preview_jpg.blocks > 1 ? preview_jpg.subsample_y[1] : 0,
            0, 0, preview_jpg.x_phys / ds,
            raw_width, raw_height, rgb, raw_width))
        return false;

    shown_width = exif->orientation >= 5 ? raw_height : raw_width;
    shown_height = exif->orientation >= 5 ? raw_width : raw_height;

    vp = *(rb->screens[SCREEN_MAIN]->current_viewport);
    if (vp == NULL || vp->buffer == NULL || vp->buffer->fb_ptr == NULL ||
        vp->x != 0 || vp->y != 0 ||
        vp->width != LCD_WIDTH || vp->height != LCD_HEIGHT ||
        vp->buffer->stride != LCD_WIDTH ||
        shown_width > LCD_WIDTH || shown_height > LCD_HEIGHT)
        return false;

    x0 = (LCD_WIDTH - shown_width) / 2;
    y0 = (LCD_HEIGHT - shown_height) / 2;

    rb->lcd_clear_display();
    for (sy = 0; sy < raw_height; sy++)
    {
        int sx;
        for (sx = 0; sx < raw_width; sx++)
        {
            int dx, dy;
            preview_map(exif->orientation, sx, sy,
                        raw_width, raw_height, &dx, &dy);
            vp->buffer->fb_ptr[(y0 + dy) * LCD_WIDTH + x0 + dx] =
                rgb[sy * raw_width + sx];
        }
    }

    rb->lcd_update();
    return true;
}

#else

static bool preview_main_large_enough(const unsigned char *src, size_t size)
{
    (void)src;
    (void)size;
    return false;
}

static bool jpeg_exif_try_preview(const char *filename, int offset,
                                  unsigned char *scratch,
                                  size_t scratch_size,
                                  struct jpeg_exif *exif)
{
    (void)filename;
    (void)offset;
    (void)scratch;
    (void)scratch_size;
    (void)exif;
    return false;
}

#endif

void jpeg_exif_reset(struct jpeg_exif *exif)
{
    jpeg_exif_reset_metadata(exif);
}

bool jpeg_exif_read(const char *filename, int offset, int filesize,
                    unsigned char *scratch, size_t scratch_size,
                    struct jpeg_exif *exif)
{
    bool status = jpeg_exif_read_metadata(
        filename, offset, filesize, scratch, scratch_size, exif);

    if (status && exif->thumbnail_length != 0 &&
        preview_main_large_enough(scratch, scratch_size))
        jpeg_exif_try_preview(filename, offset, scratch,
                              scratch_size, exif);

    return status;
}
