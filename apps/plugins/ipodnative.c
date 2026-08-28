/***************************************************************************
 * iPod Photo native video viewer (.ipvf)
 *
 * IPVF v1 stores 220x176 framebuffer-native RGB565SWAPPED frames. Frames are
 * either complete keyframes, repeats, or one/more raw rectangular patches.
 * The pixel bytes are RGB565 big-endian on disk, which is the byte layout of
 * Rockbox's RGB565SWAPPED framebuffer on iPod Color/Photo.
 ****************************************************************************/
#include "plugin.h"
#include <lib/helper.h>

#define IPVF_MAGIC              "IPVF"
#define IPVF_VERSION            1
#define IPVF_HEADER_SIZE        64
#define IPVF_FRAME_HEADER_SIZE   8
#define IPVF_RECT_HEADER_SIZE    8
#define IPVF_FLAG_RGB565BE       0x00000001u
#define IPVF_TYPE_KEY            0
#define IPVF_TYPE_RECTS          1
#define IPVF_TYPE_REPEAT         2
#define IPVF_FRAME_BYTES ((size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data))
#define IPVF_MAX_PAYLOAD (IPVF_FRAME_BYTES + 4096)
#define IPVF_LOG ROCKBOX_DIR "/ipvf17.csv"

struct ipvf_info
{
    unsigned int fps_num;
    unsigned int fps_den;
    unsigned long frame_count;
};

struct ipvf_stats
{
    unsigned long frames;
    unsigned long keyframes;
    unsigned long delta_frames;
    unsigned long repeat_frames;
    unsigned long late_frames;
    unsigned long max_late_us;
    unsigned long long payload_bytes;
    unsigned long long read_us;
    unsigned long long apply_us;
    unsigned long long lcd_us;
    unsigned long long wait_us;
};

static uint16_t get_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool read_exact(int fd, void *buffer, size_t bytes)
{
    unsigned char *p = buffer;

    while (bytes != 0)
    {
        ssize_t n = rb->read(fd, p, bytes);
        if (n <= 0)
            return false;
        p += n;
        bytes -= (size_t)n;
    }

    return true;
}

static bool read_header(int fd, struct ipvf_info *info)
{
    unsigned char h[IPVF_HEADER_SIZE];
    uint32_t flags;
    uint32_t data_offset;

    if (!read_exact(fd, h, sizeof(h)) || rb->memcmp(h, IPVF_MAGIC, 4))
        return false;
    if (get_le16(h + 4) != IPVF_VERSION ||
        get_le16(h + 6) < IPVF_HEADER_SIZE ||
        get_le16(h + 8) != LCD_WIDTH || get_le16(h + 10) != LCD_HEIGHT)
        return false;

    info->fps_num = get_le16(h + 12);
    info->fps_den = get_le16(h + 14);
    info->frame_count = get_le32(h + 16);
    flags = get_le32(h + 20);
    data_offset = get_le32(h + 24);

    if (info->fps_num == 0 || info->fps_den == 0 || info->frame_count == 0 ||
        !(flags & IPVF_FLAG_RGB565BE) || data_offset < IPVF_HEADER_SIZE)
        return false;

    if (rb->lseek(fd, data_offset, SEEK_SET) < 0)
        return false;

    return true;
}

static bool process_rects(const unsigned char *payload, size_t bytes,
                          unsigned int rect_count, fb_data *fb,
                          struct ipvf_stats *stats, bool copy_pixels,
                          bool update_lcd)
{
    const unsigned char *p = payload;
    const unsigned char *end = payload + bytes;
    unsigned int i;

    for (i = 0; i < rect_count; i++)
    {
        unsigned int x, y, w, h;
        uint32_t data_bytes;
        unsigned int row;
        uint32_t t0;

        if ((size_t)(end - p) < IPVF_RECT_HEADER_SIZE)
            return false;

        x = p[0]; y = p[1]; w = p[2]; h = p[3];
        data_bytes = get_le32(p + 4);
        p += IPVF_RECT_HEADER_SIZE;

        if (w == 0 || h == 0 || x + w > LCD_WIDTH || y + h > LCD_HEIGHT ||
            data_bytes != (uint32_t)w * h * sizeof(fb_data) ||
            (size_t)(end - p) < data_bytes)
            return false;

        if (copy_pixels)
        {
            t0 = USEC_TIMER;
            for (row = 0; row < h; row++)
            {
                rb->memcpy(fb + (y + row) * LCD_WIDTH + x,
                           p + (size_t)row * w * sizeof(fb_data),
                           (size_t)w * sizeof(fb_data));
            }
            stats->apply_us += (uint32_t)(USEC_TIMER - t0);
        }

        if (update_lcd)
        {
            t0 = USEC_TIMER;
            rb->lcd_update_rect(x, y, w, h);
            stats->lcd_us += (uint32_t)(USEC_TIMER - t0);
        }

        p += data_bytes;
    }

    return p == end;
}

static uint32_t wait_until(uint32_t target)
{
    uint32_t start = USEC_TIMER;

    for (;;)
    {
        uint32_t now = USEC_TIMER;
        int32_t left = (int32_t)(target - now);
        if (left <= 0)
            break;
        if (left > 1500)
            rb->yield();
    }

    return USEC_TIMER - start;
}

static void log_result(const char *filename, const struct ipvf_info *info,
                       const struct ipvf_stats *s, uint32_t wall_us,
                       uint32_t crc, bool complete)
{
    int fd = rb->open(IPVF_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    const char *base = rb->strrchr(filename, '/');
    if (base == NULL) base = filename; else base++;

    if (fd < 0)
        return;

    if (rb->filesize(fd) == 0)
        rb->fdprintf(fd, "file,fps_num,fps_den,frames,keyframes,deltas,repeats,"
                         "payload_bytes,read_us,apply_us,lcd_us,wait_us,"
                         "late_frames,max_late_us,wall_us,crc,complete\n");

    rb->fdprintf(fd,
        "%s,%u,%u,%lu,%lu,%lu,%lu,%llu,%llu,%llu,%llu,%llu,%lu,%lu,%lu,%08lx,%d\n",
        base, info->fps_num, info->fps_den, s->frames, s->keyframes,
        s->delta_frames, s->repeat_frames, s->payload_bytes, s->read_us,
        s->apply_us, s->lcd_us, s->wait_us, s->late_frames, s->max_late_us,
        (unsigned long)wall_us, (unsigned long)crc, complete ? 1 : 0);
    rb->close(fd);
}

static enum plugin_status play_file(const char *filename)
{
    struct ipvf_info info;
    struct ipvf_stats stats;
    size_t buf_size;
    unsigned char *buf;
    unsigned char *scratch;
    struct viewport *vp;
    fb_data *fb;
    uint32_t start_us = 0;
    uint32_t period_us;
    uint32_t wall_us = 0;
    uint32_t crc = 0;
    unsigned long frame;
    int fd;
    bool complete = false;
    bool boosted = false;
    int old_spindown = rb->global_settings->disk_spindown;

    rb->memset(&stats, 0, sizeof(stats));

    fd = rb->open(filename, O_RDONLY);
    if (fd < 0 || !read_header(fd, &info))
    {
        if (fd >= 0) rb->close(fd);
        rb->splash(HZ * 2, "Unsupported IPVF");
        return PLUGIN_ERROR;
    }

    period_us = (uint32_t)(((uint64_t)1000000 * info.fps_den +
                            info.fps_num / 2) / info.fps_num);
    if (period_us == 0)
    {
        rb->close(fd);
        return PLUGIN_ERROR;
    }

    buf = rb->plugin_get_buffer(&buf_size);
    scratch = (unsigned char *)(((uintptr_t)buf + 15) & ~(uintptr_t)15);
    if (buf_size <= (size_t)(scratch - buf) ||
        buf_size - (size_t)(scratch - buf) < IPVF_MAX_PAYLOAD)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF buffer too small");
        return PLUGIN_ERROR;
    }

    rb->lcd_set_viewport(NULL);
    vp = rb->screens[SCREEN_MAIN]->current_viewport;
    fb = vp->buffer->fb_ptr;

    rb->lcd_set_backdrop(NULL);
    rb->lcd_clear_display();
    rb->lcd_update();
    rb->button_clear_queue();
    backlight_ignore_timeout();
    rb->storage_spindown(0);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
    boosted = true;
#endif

    start_us = USEC_TIMER;

    for (frame = 0; frame < info.frame_count; frame++)
    {
        unsigned char fh[IPVF_FRAME_HEADER_SIZE];
        unsigned int type, rect_count;
        uint32_t payload_size;
        uint32_t t0, target, late;
        int button;

        t0 = USEC_TIMER;
        if (!read_exact(fd, fh, sizeof(fh)))
            break;
        type = fh[0];
        rect_count = fh[1];
        payload_size = get_le32(fh + 4);

        if (payload_size > IPVF_MAX_PAYLOAD ||
            (frame == 0 && type != IPVF_TYPE_KEY))
            break;

        if (type == IPVF_TYPE_KEY)
        {
            if (rect_count != 0 || payload_size != IPVF_FRAME_BYTES ||
                !read_exact(fd, fb, payload_size))
                break;
        }
        else if (type == IPVF_TYPE_RECTS)
        {
            if (rect_count == 0 || !read_exact(fd, scratch, payload_size))
                break;
        }
        else if (type == IPVF_TYPE_REPEAT)
        {
            if (rect_count != 0 || payload_size != 0)
                break;
        }
        else
            break;

        stats.read_us += (uint32_t)(USEC_TIMER - t0);
        stats.payload_bytes += payload_size;

        if (type == IPVF_TYPE_RECTS &&
            !process_rects(scratch, payload_size, rect_count, fb, &stats,
                           true, false))
            break;

        if (frame == 0)
        {
            start_us = USEC_TIMER;
        }
        else
        {
            target = start_us + (uint32_t)((uint64_t)frame * period_us);
            t0 = USEC_TIMER;
            late = (int32_t)(t0 - target) > 0 ? t0 - target : 0;
            if (late > 500)
            {
                stats.late_frames++;
                if (late > stats.max_late_us)
                    stats.max_late_us = late;
            }
            else
                stats.wait_us += wait_until(target);
        }

        if (type == IPVF_TYPE_KEY)
        {
            t0 = USEC_TIMER;
            rb->lcd_update();
            stats.lcd_us += (uint32_t)(USEC_TIMER - t0);
            stats.keyframes++;
        }
        else if (type == IPVF_TYPE_RECTS)
        {
            if (!process_rects(scratch, payload_size, rect_count, fb, &stats,
                               false, true))
                break;
            stats.delta_frames++;
        }
        else
        {
            stats.repeat_frames++;
        }

        stats.frames++;
        rb->reset_poweroff_timer();

        button = rb->button_get(false);
        if ((button & ~(BUTTON_REL | BUTTON_REPEAT)) == BUTTON_MENU)
            break;
    }

    wall_us = USEC_TIMER - start_us;
    complete = stats.frames == info.frame_count;
    crc = rb->crc_32(fb, IPVF_FRAME_BYTES, 0xffffffff);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (boosted)
        rb->cpu_boost(false);
#endif
    rb->storage_spindown(old_spindown);
    backlight_use_settings();
    rb->close(fd);

    log_result(filename, &info, &stats, wall_us, crc, complete);

    if (complete)
        rb->splashf(HZ * 2, "%lu frames, %lu late",
                    stats.frames, stats.late_frames);
    else
        rb->splashf(HZ * 2, "Stopped at %lu/%lu",
                    stats.frames, info.frame_count);

    return complete ? PLUGIN_OK : PLUGIN_ERROR;
}

enum plugin_status plugin_start(const void *parameter)
{
    if (parameter == NULL)
    {
        rb->splash(HZ * 2, "Open an .ipvf file");
        return PLUGIN_OK;
    }

    return play_file(parameter);
}
