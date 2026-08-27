/***************************************************************************
 * Motion-JPEG AVI player for iPod Color/Photo.
 *
 * First bounded phase: video-only AVI/MJPG playback.  Frames are decoded with
 * the same baseline JPEG decoder used by Image Viewer and converted through
 * the validated full-range RGB565 path.  Audio streams, seeking and indexing
 * are deliberately deferred until frame throughput is qualified on A1099.
 ****************************************************************************/

#include "plugin.h"
#include "../imageviewer/jpeg/jpeg_decoder.h"
#include "../imageviewer/jpeg/yuv2rgb.h"

/* Reuse the validated JPEG parser/decoder and RGB565 converter in this plugin. */
#include "../imageviewer/jpeg/jpeg_decoder_accel.c"
#include "../imageviewer/jpeg/yuv2rgb_accel.c"

#define AVI_FOURCC(a,b,c,d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define AVI_RIFF AVI_FOURCC('R','I','F','F')
#define AVI_AVI  AVI_FOURCC('A','V','I',' ')
#define AVI_LIST AVI_FOURCC('L','I','S','T')
#define AVI_HDRL AVI_FOURCC('h','d','r','l')
#define AVI_STRL AVI_FOURCC('s','t','r','l')
#define AVI_ODML AVI_FOURCC('o','d','m','l')
#define AVI_MOVI AVI_FOURCC('m','o','v','i')
#define AVI_AVIH AVI_FOURCC('a','v','i','h')
#define AVI_STRH AVI_FOURCC('s','t','r','h')
#define AVI_VIDS AVI_FOURCC('v','i','d','s')
#define AVI_MJPG AVI_FOURCC('M','J','P','G')
#define AVI_mjpg AVI_FOURCC('m','j','p','g')
#define AVI_JPEG AVI_FOURCC('J','P','E','G')
#define AVI_jpeg AVI_FOURCC('j','p','e','g')
#define AVI_REC  AVI_FOURCC('r','e','c',' ')

#define MJPEG_CURSOR_DEPTH 6
#define MJPEG_LOG_PATH ROCKBOX_DIR "/mjpeg15.csv"
#define MJPEG_LOG_ENABLE ROCKBOX_DIR "/mjpeg15.enabled"

#define MJPEG_QUIT  BUTTON_MENU
#define MJPEG_PAUSE (BUTTON_PLAY | BUTTON_REL)

struct avi_info
{
    off_t file_size;
    off_t movi_start;
    off_t movi_end;
    uint32_t usec_per_frame;
    uint32_t total_frames;
    uint32_t width;
    uint32_t height;
    bool has_mjpeg_video;
};

struct avi_cursor
{
    off_t pos;
    off_t end;
    off_t parent_end[MJPEG_CURSOR_DEPTH];
    off_t resume[MJPEG_CURSOR_DEPTH];
    int depth;
};

struct mjpeg_stats
{
    uint32_t frames;
    uint32_t errors;
    uint32_t late_frames;
    uint32_t read_us;
    uint32_t decode_us;
    uint32_t render_us;
    uint32_t max_process_us;
    uint32_t first_crc;
    uint32_t last_crc;
    uint32_t play_us;
};

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool read_at(int fd, off_t offset, void *buffer, size_t bytes)
{
    unsigned char *dst = buffer;

    if (rb->lseek(fd, offset, SEEK_SET) < 0)
        return false;

    while (bytes > 0)
    {
        ssize_t got = rb->read(fd, dst, bytes);
        if (got <= 0)
            return false;
        dst += got;
        bytes -= got;
    }

    return true;
}

static bool avi_is_mjpg(uint32_t fourcc)
{
    return fourcc == AVI_MJPG || fourcc == AVI_mjpg ||
           fourcc == AVI_JPEG || fourcc == AVI_jpeg;
}

static bool avi_parse_range(int fd, off_t start, off_t end,
                            struct avi_info *avi, int depth)
{
    off_t pos = start;

    if (depth > 6)
        return false;

    while (pos + 8 <= end)
    {
        unsigned char header[64];
        uint32_t id;
        uint32_t size;
        off_t data;
        off_t chunk_end;
        off_t next;

        if (!read_at(fd, pos, header, 8))
            return false;

        id = le32(header);
        size = le32(header + 4);
        data = pos + 8;

        if (data > end ||
            (uint64_t)size > (uint64_t)(end - data))
            return false;

        chunk_end = data + size;
        next = chunk_end + (size & 1u);
        if (next < chunk_end || next > end + 1)
            return false;

        if (id == AVI_LIST)
        {
            uint32_t type;

            if (size < 4 || !read_at(fd, data, header, 4))
                return false;

            type = le32(header);
            if (type == AVI_MOVI)
            {
                avi->movi_start = data + 4;
                avi->movi_end = chunk_end;
            }
            else if ((type == AVI_HDRL || type == AVI_STRL ||
                      type == AVI_ODML) &&
                     !avi_parse_range(fd, data + 4, chunk_end,
                                      avi, depth + 1))
            {
                return false;
            }
        }
        else if (id == AVI_AVIH && size >= 40)
        {
            if (!read_at(fd, data, header, 40))
                return false;

            avi->usec_per_frame = le32(header);
            avi->total_frames = le32(header + 16);
            avi->width = le32(header + 32);
            avi->height = le32(header + 36);
        }
        else if (id == AVI_STRH && size >= 40)
        {
            if (!read_at(fd, data, header, 40))
                return false;

            if (le32(header) == AVI_VIDS && avi_is_mjpg(le32(header + 4)))
            {
                uint32_t scale = le32(header + 20);
                uint32_t rate = le32(header + 24);

                avi->has_mjpeg_video = true;
                if (avi->usec_per_frame == 0 && scale != 0 && rate != 0)
                    avi->usec_per_frame =
                        (uint32_t)(((uint64_t)scale * 1000000u + rate / 2) /
                                   rate);
            }
        }

        pos = next;
    }

    return true;
}

static bool avi_open(int fd, struct avi_info *avi)
{
    unsigned char header[12];
    uint32_t riff_size;
    off_t end;

    rb->memset(avi, 0, sizeof(*avi));
    avi->file_size = rb->filesize(fd);
    if (avi->file_size < 12 || !read_at(fd, 0, header, sizeof(header)))
        return false;

    if (le32(header) != AVI_RIFF || le32(header + 8) != AVI_AVI)
        return false;

    riff_size = le32(header + 4);
    end = (off_t)riff_size + 8;
    if (end > avi->file_size || end < 12)
        end = avi->file_size;

    if (!avi_parse_range(fd, 12, end, avi, 0))
        return false;

    if (!avi->has_mjpeg_video || avi->movi_start <= 0 ||
        avi->movi_end <= avi->movi_start)
        return false;

    if (avi->usec_per_frame == 0)
        avi->usec_per_frame = 100000; /* conservative 10 fps fallback */

    return true;
}

static void avi_cursor_init(struct avi_cursor *cursor,
                            const struct avi_info *avi)
{
    rb->memset(cursor, 0, sizeof(*cursor));
    cursor->pos = avi->movi_start;
    cursor->end = avi->movi_end;
}

static bool avi_chunk_is_video(const unsigned char id[4])
{
    bool stream_digits =
        ((id[0] >= '0' && id[0] <= '9') || (id[0] >= 'A' && id[0] <= 'F')) &&
        ((id[1] >= '0' && id[1] <= '9') || (id[1] >= 'A' && id[1] <= 'F'));

    return stream_digits && id[2] == 'd' && (id[3] == 'c' || id[3] == 'b');
}

static bool avi_next_frame(int fd, struct avi_cursor *cursor,
                           off_t *frame_offset, uint32_t *frame_size)
{
    while (true)
    {
        unsigned char header[12];
        uint32_t id;
        uint32_t size;
        off_t data;
        off_t chunk_end;
        off_t next;

        while (cursor->depth > 0 && cursor->pos >= cursor->end)
        {
            cursor->depth--;
            cursor->pos = cursor->resume[cursor->depth];
            cursor->end = cursor->parent_end[cursor->depth];
        }

        if (cursor->pos + 8 > cursor->end)
            return false;

        if (!read_at(fd, cursor->pos, header, 8))
            return false;

        id = le32(header);
        size = le32(header + 4);
        data = cursor->pos + 8;

        if (data > cursor->end ||
            (uint64_t)size > (uint64_t)(cursor->end - data))
            return false;

        chunk_end = data + size;
        next = chunk_end + (size & 1u);

        if (id == AVI_LIST)
        {
            uint32_t type;

            if (size < 4 || !read_at(fd, data, header, 4))
                return false;
            type = le32(header);

            if ((type == AVI_REC || type == AVI_MOVI) &&
                cursor->depth < MJPEG_CURSOR_DEPTH)
            {
                cursor->parent_end[cursor->depth] = cursor->end;
                cursor->resume[cursor->depth] = next;
                cursor->depth++;
                cursor->pos = data + 4;
                cursor->end = chunk_end;
                continue;
            }

            cursor->pos = next;
            continue;
        }

        cursor->pos = next;

        if (avi_chunk_is_video(header) && size >= 4)
        {
            unsigned char soi[2];

            if (!read_at(fd, data, soi, 2))
                return false;
            if (soi[0] == 0xff && soi[1] == 0xd8)
            {
                *frame_offset = data;
                *frame_size = size;
                return true;
            }
        }
    }
}

static uint32_t now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (uint32_t)*rb->current_tick * (1000000u / HZ);
#endif
}

static int choose_ds(const struct jpeg *jpg)
{
    int ds = 1;

    while (ds < 8 &&
           (jpg->x_size / ds > LCD_WIDTH || jpg->y_size / ds > LCD_HEIGHT))
        ds <<= 1;

    if (jpg->x_size / ds > LCD_WIDTH || jpg->y_size / ds > LCD_HEIGHT)
        return 0;

    return ds;
}

static bool alloc_frame_buffers(unsigned char *buffer, size_t buffer_size,
                                size_t compressed_size,
                                struct jpeg *jpg, int ds,
                                unsigned char *planes[3],
                                fb_data **rgb, int *width, int *height,
                                int *stride)
{
    unsigned char *cursor = buffer + ((compressed_size + 3u) & ~3u);
    unsigned char *end = buffer + buffer_size;
    int i;
    size_t size;
    uintptr_t aligned;

    *stride = jpg->x_phys / ds;
    *width = jpg->x_size / ds;
    *height = jpg->y_size / ds;

    if (*stride <= 0 || *width <= 0 || *height <= 0 || cursor > end)
        return false;

    if (jpg->blocks > 1)
    {
        for (i = 1; i < 3; i++)
        {
            int sx = jpg->subsample_x[i];
            int sy = jpg->subsample_y[i];

            if (sx <= 0 || sy <= 0)
                return false;

            size = (size_t)(jpg->x_phys / ds / sx) *
                   (jpg->y_phys / ds / sy);
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

    size = (size_t)(*stride) * (jpg->y_phys / ds);
    if (size > (size_t)(end - cursor))
        return false;
    planes[0] = cursor;
    cursor += size;

    aligned = ((uintptr_t)cursor + sizeof(fb_data) - 1) &
              ~((uintptr_t)sizeof(fb_data) - 1);
    if (aligned > (uintptr_t)end)
        return false;
    cursor = (unsigned char *)aligned;

    size = (size_t)(*width) * (*height) * sizeof(fb_data);
    if (size > (size_t)(end - cursor))
        return false;

    *rgb = (fb_data *)cursor;
    return true;
}

static bool decode_frame(unsigned char *buffer, size_t buffer_size,
                         uint32_t compressed_size,
                         fb_data **rgb_out, int *width_out, int *height_out,
                         uint32_t *decode_us)
{
    struct jpeg jpg;
    unsigned char *planes[3] = { NULL, NULL, NULL };
    fb_data *rgb;
    uint32_t started;
    int status;
    int ds;
    int width;
    int height;
    int stride;

    rb->memset(&jpg, 0, sizeof(jpg));
    started = now_us();

    status = process_markers(buffer, compressed_size, &jpg);
    if (status < 0 || (status & (DQT | SOF0)) != (DQT | SOF0) ||
        jpg.table_error != 0)
        return false;

    if (!(status & DHT))
        default_huff_tbl(&jpg);
    build_lut(&jpg);

    ds = choose_ds(&jpg);
    if (ds == 0 || !alloc_frame_buffers(buffer, buffer_size,
                                        compressed_size, &jpg, ds,
                                        planes, &rgb, &width, &height,
                                        &stride))
        return false;

    jpeg_decode_set_mcu_row_callback(NULL, NULL);
    jpeg_decode_set_mcu_row_reuse(false);
    if (jpeg_decode(&jpg, planes, ds, NULL) != 0)
        return false;

    if (!yuv_bitmap_part_to_buffer(
            planes,
            jpg.blocks > 1 ? jpg.subsample_x[1] : 0,
            jpg.blocks > 1 ? jpg.subsample_y[1] : 0,
            0, 0, stride, width, height, rgb, width))
        return false;

    *decode_us = now_us() - started;
    *rgb_out = rgb;
    *width_out = width;
    *height_out = height;
    return true;
}

static bool log_enabled(void)
{
    int fd = rb->open(MJPEG_LOG_ENABLE, O_RDONLY);
    if (fd < 0)
        return false;
    rb->close(fd);
    return true;
}

static void log_stats(const char *filename, const struct avi_info *avi,
                      const struct mjpeg_stats *stats)
{
    const char *name;
    int fd;

    if (!log_enabled())
        return;

    fd = rb->open(MJPEG_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    if (rb->filesize(fd) == 0)
        rb->fdprintf(fd,
            "file,width,height,usec_per_frame,total_frames,frames,errors,"
            "late_frames,read_us,decode_us,render_us,max_process_us,"
            "play_us,first_crc,last_crc\n");

    name = rb->strrchr(filename, '/');
    name = name != NULL ? name + 1 : filename;

    rb->fdprintf(fd,
        "%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
        "%08lx,%08lx\n",
        name,
        (unsigned long)avi->width,
        (unsigned long)avi->height,
        (unsigned long)avi->usec_per_frame,
        (unsigned long)avi->total_frames,
        (unsigned long)stats->frames,
        (unsigned long)stats->errors,
        (unsigned long)stats->late_frames,
        (unsigned long)stats->read_us,
        (unsigned long)stats->decode_us,
        (unsigned long)stats->render_us,
        (unsigned long)stats->max_process_us,
        (unsigned long)stats->play_us,
        (unsigned long)stats->first_crc,
        (unsigned long)stats->last_crc);
    rb->close(fd);
}

static int handle_button(bool *paused)
{
    int button = rb->button_get_w_tmo(0);

    if (button == BUTTON_NONE)
        return 0;
    if (button == MJPEG_QUIT)
        return -1;
    if (button == MJPEG_PAUSE)
    {
        *paused = !*paused;
        return 1;
    }

    if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
        return -1;

    return 0;
}

static bool wait_while_paused(bool *paused)
{
    while (*paused)
    {
        int button = rb->button_get_w_tmo(HZ / 10);

        if (button == MJPEG_QUIT)
            return false;
        if (button == MJPEG_PAUSE)
        {
            *paused = false;
            break;
        }
        if (button != BUTTON_NONE &&
            rb->default_event_handler(button) == SYS_USB_CONNECTED)
            return false;
    }

    return true;
}

static enum plugin_status play_avi(const char *filename)
{
    struct avi_info avi;
    struct avi_cursor cursor;
    struct mjpeg_stats stats;
    unsigned char *buffer;
    size_t buffer_size;
    uint64_t schedule_units = 0;
    long start_tick;
    uint32_t play_started;
    bool paused = false;
    bool quit = false;
    int fd;

    rb->memset(&stats, 0, sizeof(stats));

    fd = rb->open(filename, O_RDONLY);
    if (fd < 0)
    {
        rb->splash(HZ, "Cannot open AVI");
        return PLUGIN_ERROR;
    }

    if (!avi_open(fd, &avi))
    {
        rb->close(fd);
        rb->splash(HZ * 2, "Unsupported AVI/MJPG");
        return PLUGIN_ERROR;
    }

    buffer = rb->plugin_get_buffer(&buffer_size);
    if (buffer == NULL || buffer_size < 128 * 1024)
    {
        rb->close(fd);
        rb->splash(HZ, "Not enough memory");
        return PLUGIN_ERROR;
    }

    avi_cursor_init(&cursor, &avi);
    rb->lcd_clear_display();
    rb->lcd_update();

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif

    play_started = now_us();
    start_tick = *rb->current_tick;

    while (!quit)
    {
        off_t frame_offset;
        uint32_t frame_size;
        fb_data *rgb;
        uint32_t read_started;
        uint32_t decode_us;
        uint32_t render_started;
        uint32_t render_us;
        uint32_t process_us;
        uint32_t crc;
        int width;
        int height;
        int x;
        int y;
        int button_result;
        long target_tick;
        long now_tick;

        button_result = handle_button(&paused);
        if (button_result < 0)
            break;
        if (paused)
        {
            if (!wait_while_paused(&paused))
                break;
            start_tick = *rb->current_tick;
            schedule_units = 0;
        }

        if (!avi_next_frame(fd, &cursor, &frame_offset, &frame_size))
            break;

        if (frame_size == 0 || frame_size >= buffer_size)
        {
            stats.errors++;
            continue;
        }

        read_started = now_us();
        if (!read_at(fd, frame_offset, buffer, frame_size))
        {
            stats.errors++;
            break;
        }
        stats.read_us += now_us() - read_started;

        if (!decode_frame(buffer, buffer_size, frame_size,
                          &rgb, &width, &height, &decode_us))
        {
            stats.errors++;
            continue;
        }
        stats.decode_us += decode_us;

        crc = rb->crc_32(rgb,
                         (size_t)width * height * sizeof(fb_data),
                         0xffffffff);
        if (stats.frames == 0)
            stats.first_crc = crc;
        stats.last_crc = crc;

        x = (LCD_WIDTH - width) / 2;
        y = (LCD_HEIGHT - height) / 2;

        render_started = now_us();
        rb->lcd_bitmap_part(rgb, 0, 0, width,
                            x, y, width, height);
        rb->lcd_update_rect(x, y, width, height);
        render_us = now_us() - render_started;
        stats.render_us += render_us;
        stats.frames++;

        process_us = decode_us + render_us;
        if (process_us > stats.max_process_us)
            stats.max_process_us = process_us;

        schedule_units += (uint64_t)avi.usec_per_frame * HZ;
        target_tick = start_tick + (long)(schedule_units / 1000000u);
        now_tick = *rb->current_tick;

        if (now_tick < target_tick)
            rb->sleep(target_tick - now_tick);
        else if (now_tick > target_tick + 1)
            stats.late_frames++;
    }

    stats.play_us = now_us() - play_started;

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif

    rb->close(fd);
    log_stats(filename, &avi, &stats);

    if (stats.frames == 0)
        return PLUGIN_ERROR;

    return PLUGIN_OK;
}

enum plugin_status plugin_start(const void *parameter)
{
    if (parameter == NULL)
        return PLUGIN_ERROR;

    return play_avi((const char *)parameter);
}
