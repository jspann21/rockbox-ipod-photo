/***************************************************************************
 * Progressive JPEG imageviewer integration with same-build Huffman A/B test.
 ****************************************************************************/

#include "plugin.h"
#include "../imageviewer.h"
#include "jpeg81_accel.h"

struct jpegp_legacy_decoder
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
#define draw_image_rect jpegp_legacy_draw_image_rect
#define load_image      jpegp_legacy_load_image
#define get_image       jpegp_legacy_get_image
#define image_decoder   jpegp_legacy_decoder
#include "jpegp.c"
#undef draw_image_rect
#undef load_image
#undef get_image
#undef image_decoder
#undef IMGDEC_HEADER

struct jpegp_test_state
{
    bool enabled;
    bool reference;
    bool logged[9];
    char filename[MAX_PATH];
    uint32_t load_us;
};

static struct jpegp_test_state jpegp_test;

static uint32_t jpegp_now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (uint32_t)*rb->current_tick * (1000000u / HZ);
#endif
}

static bool jpegp_path_exists(const char *path)
{
    int fd = rb->open(path, O_RDONLY);

    if (fd < 0)
        return false;

    rb->close(fd);
    return true;
}

static void jpegp_test_refresh(const char *filename)
{
    rb->memset(&jpegp_test, 0, sizeof(jpegp_test));
    jpegp_test.enabled =
        jpegp_path_exists(ROCKBOX_DIR "/jpegp10.enabled");
    jpegp_test.reference = jpegp_test.enabled &&
        jpegp_path_exists(ROCKBOX_DIR "/jpegp10.reference");
    rb->strlcpy(jpegp_test.filename, filename,
                sizeof(jpegp_test.filename));

    jpegp_huffman_set_fast(!jpegp_test.reference);
}

static uint32_t jpegp_bitmap_crc(const struct image_info *info)
{
    const struct t_disp *display =
        (const struct t_disp *)info->data;
    size_t bytes;

    if (display == NULL || display->bitmap == NULL ||
        info->width <= 0 || info->height <= 0)
        return 0;

    bytes = (size_t)info->width * info->height * sizeof(fb_data);
    return rb->crc_32(display->bitmap, bytes, 0xffffffff);
}

static void jpegp_log_result(const struct image_info *info, int ds,
                             uint32_t render_us)
{
    struct jpegp_huffman_stats stats;
    const char *name;
    uint32_t crc;
    uint32_t postdecode_us;
    int fd;

    if (!jpegp_test.enabled || ds < 1 || ds > 8 ||
        jpegp_test.logged[ds])
        return;

    jpegp_huffman_get_stats(&stats);
    crc = jpegp_bitmap_crc(info);
    if (crc == 0)
        return;

    fd = rb->open(ROCKBOX_DIR "/jpegp10.csv",
                  O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    if (rb->filesize(fd) == 0)
    {
        rb->fdprintf(fd,
            "mode,file,ds,width,height,load_us,decode_us,"
            "postdecode_us,render_us,bytes,look_hits,slow_hits,"
            "getbits_calls,marker_events,fast_used,fallback,rgb_crc\n");
    }

    name = rb->strrchr(jpegp_test.filename, '/');
    name = name != NULL ? name + 1 : jpegp_test.filename;
    postdecode_us = jpegp_test.load_us > stats.decode_us
        ? jpegp_test.load_us - stats.decode_us : 0;

    rb->fdprintf(fd,
        "%s,%s,%d,%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
        "%d,%d,%08lx\n",
        jpegp_test.reference ? "reference" : "accelerated",
        name, ds, info->width, info->height,
        (unsigned long)jpegp_test.load_us,
        (unsigned long)stats.decode_us,
        (unsigned long)postdecode_us,
        (unsigned long)render_us,
        (unsigned long)stats.bytes,
        (unsigned long)stats.look_hits,
        (unsigned long)stats.slow_hits,
        (unsigned long)stats.getbits_calls,
        (unsigned long)stats.marker_events,
        stats.fast_used ? 1 : 0,
        stats.fallback ? 1 : 0,
        (unsigned long)crc);

    rb->close(fd);
    jpegp_test.logged[ds] = true;
}

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize)
{
    uint32_t started;
    int status;

    jpegp_test_refresh(filename);
    started = jpegp_now_us();
    status = jpegp_legacy_load_image(filename, info, buf, buf_size,
                                     offset, filesize);
    jpegp_test.load_us = jpegp_now_us() - started;
    return status;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    uint32_t started = jpegp_now_us();
    int status = jpegp_legacy_get_image(info, frame, ds);
    uint32_t render_us = jpegp_now_us() - started;

    if (status == PLUGIN_OK)
        jpegp_log_result(info, ds, render_us);

    return status;
}

static void draw_image_rect(struct image_info *info,
                            int x, int y, int width, int height)
{
    jpegp_legacy_draw_image_rect(info, x, y, width, height);
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
