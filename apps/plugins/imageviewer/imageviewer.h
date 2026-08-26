/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * user intereface of image viewer.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef _IMAGE_VIEWER_H
#define _IMAGE_VIEWER_H

#include "plugin.h"

#if LCD_DEPTH < 8
#define USEGSLIB
#include <lib/grey.h>
#else
#include <lib/xlcd.h>
#endif

#include <lib/mylcd.h>

#if defined(USEGSLIB) && defined(IMGDEC)
#undef mylcd_ub_
#undef myxlcd_ub_
#define mylcd_ub_(fn)       iv->fn
#define myxlcd_ub_(fn)      iv->fn
#endif

#define MIN_MEM 120000

enum {
    PLUGIN_OTHER = 0x200,
    PLUGIN_ABORT,
    PLUGIN_OUTOFMEM,
    PLUGIN_JPEG_PROGRESSIVE,

    ZOOM_IN,
    ZOOM_OUT,
    NEXT_FRAME,
};

#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && defined(HAVE_DISK_STORAGE)
#define DISK_SPINDOWN
#endif
#if PLUGIN_BUFFER_SIZE >= MIN_MEM
#define USE_PLUG_BUF
#endif

struct imgview_settings
{
#ifdef HAVE_LCD_COLOR
    int jpeg_colour_mode;
    int jpeg_dither_mode;
#endif
    int ss_timeout;
    bool hide_info;
};

struct image_info {
    int x_size, y_size;
    int width, height;
    int x, y;
    int frames_count;
    int delay;
    void *data;
};

struct imgdec_api {
    const struct imgview_settings *settings;
    bool slideshow_enabled;
    bool running_slideshow;

    /* One-shot handoff for a decoder that has already updated the panel. */
    bool skip_next_update;

#ifdef DISK_SPINDOWN
    bool immediate_ata_off;
#endif
#ifdef USE_PLUG_BUF
    bool plug_buf;
#endif

    void (*cb_progress)(int current, int total);

#ifdef USEGSLIB
    void (*gray_bitmap_part)(const unsigned char *src, int src_x, int src_y,
                              int stride, int x, int y, int width, int height);
#endif
};

struct image_decoder {
    const bool unscaled_avail;
    int (*img_mem)(int ds);
    int (*load_image)(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size, int offset, int filesize);
    int (*get_image)(struct image_info *info, int frame, int ds);
    void (*draw_image_rect)(struct image_info *info,
                            int x, int y, int width, int height);
};

#define IMGDEC_API_VERSION 2

struct imgdec_header {
    struct lc_header lc_hdr;
    const struct image_decoder *decoder;
    const struct plugin_api **api;
    unsigned short plugin_api_version;
    size_t plugin_api_size;
    const struct imgdec_api **img_api;
    size_t img_api_size;
};

#ifdef IMGDEC
extern const struct imgdec_api *iv;
extern const struct image_decoder image_decoder;

static inline void imgdec_skip_next_lcd_update(void)
{
    ((struct imgdec_api *)(uintptr_t)iv)->skip_next_update = true;
}

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
#define IMGDEC_HEADER \
        const struct plugin_api *rb DATA_ATTR; \
        const struct imgdec_api *iv DATA_ATTR; \
        const struct imgdec_header __header \
        __attribute__ ((section (".header")))= { \
        { PLUGIN_MAGIC, TARGET_ID, IMGDEC_API_VERSION, \
          plugin_start_addr, plugin_end_addr, }, &image_decoder, \
          &rb, PLUGIN_API_VERSION, sizeof(struct plugin_api), \
          &iv, sizeof(struct imgdec_api) };
#else
#define IMGDEC_HEADER \
        const struct plugin_api *rb DATA_ATTR; \
        const struct imgdec_api *iv DATA_ATTR; \
        const struct imgdec_header __header \
        __attribute__((visibility("default"))) = { \
        { PLUGIN_MAGIC, TARGET_ID, IMGDEC_API_VERSION, NULL, NULL }, \
          &image_decoder, &rb, PLUGIN_API_VERSION, sizeof(struct plugin_api), \
          &iv, sizeof(struct imgdec_api), };
#endif
#endif

#if !defined(IMGDEC) && LCD_DEPTH >= 4
#undef mylcd_ub_update
#define mylcd_ub_update() do { \
    if (iv_api.skip_next_update) \
        iv_api.skip_next_update = false; \
    else \
        rb->lcd_update(); \
} while (0)
#endif

#endif /* _IMAGE_VIEWER_H */
