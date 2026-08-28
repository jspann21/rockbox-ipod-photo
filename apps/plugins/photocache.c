/***************************************************************************
 * Native iPod Photo cache browser for iPod Color/Photo.
 *
 * Apple pre-renders synced photos into F1013 .ithmb frames.  On iPod Photo,
 * format 1013 is a full-screen 220x176 RGB565 image stored big-endian after a
 * 90-degree counter-clockwise storage rotation.  The iPod Color framebuffer
 * is RGB565-swapped, so the on-disk pixel bytes already match framebuffer
 * bytes after the storage rotation is undone.
 *
 * This plugin reads format-1013 records directly from "Photo Database" and
 * therefore bypasses JPEG entropy decode, IDCT, scaling and color conversion.
 ****************************************************************************/

#include "plugin.h"

#if defined(IPOD_COLOR) && defined(HAVE_LCD_COLOR) && LCD_DEPTH == 16

#define PHOTO_DB_REAL "/iPod_Control/Photos/Photo Database"
#define PHOTO_THUMBS_REAL "/iPod_Control/Photos/Thumbs"

#define PHOTO_DB_TEST ROCKBOX_DIR "/ithmb17/Photo Database"
#define PHOTO_THUMBS_TEST ROCKBOX_DIR "/ithmb17/Thumbs"
#define PHOTO_LOG_ENABLE ROCKBOX_DIR "/ithmb17.enabled"
#define PHOTO_LOG_PATH ROCKBOX_DIR "/ithmb17.csv"

#define F1013_FORMAT_ID 1013u
#define F1013_WIDTH LCD_WIDTH
#define F1013_HEIGHT LCD_HEIGHT
#define F1013_STORED_WIDTH LCD_HEIGHT
#define F1013_FRAME_BYTES ((size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data))

#define DB_SCAN_BLOCK 4096
#define DB_RECORD_SCAN 1024

struct photo_cache_entry
{
    uint32_t offset;
    uint32_t image_size;
    uint16_t file_index;
    int16_t width;
    int16_t height;
    int16_t horizontal_padding;
    int16_t vertical_padding;
};

struct photo_cache
{
    struct photo_cache_entry *entries;
    unsigned int count;
    unsigned char *raw;
    fb_data *frame;
    size_t work_size;
    const char *db_path;
    const char *thumb_dir;
    const char *source_name;
    bool log_enabled;
};

static unsigned char db_scan[DB_SCAN_BLOCK];
static unsigned char db_record[DB_RECORD_SCAN];

static uint16_t get_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t get_le16s(const unsigned char *p)
{
    return (int16_t)get_le16(p);
}

static uint32_t get_le32(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (uint32_t)*rb->current_tick * (1000000u / HZ);
#endif
}

static bool path_exists(const char *path)
{
    int fd = rb->open(path, O_RDONLY);

    if (fd < 0)
        return false;

    rb->close(fd);
    return true;
}

static bool read_at(int fd, off_t offset, void *buffer, size_t bytes)
{
    unsigned char *dst = buffer;

    if (rb->lseek(fd, offset, SEEK_SET) < 0)
        return false;

    while (bytes != 0)
    {
        ssize_t got = rb->read(fd, dst, bytes);

        if (got <= 0)
            return false;

        dst += got;
        bytes -= (size_t)got;
    }

    return true;
}

static int find_f1013_file_index(const unsigned char *record, size_t bytes)
{
    static const char prefix[] = "F1013_";
    const size_t prefix_len = sizeof(prefix) - 1;
    size_t i;

    if (bytes < prefix_len + 2)
        return 0;

    for (i = 0; i + prefix_len < bytes; i++)
    {
        unsigned int value = 0;
        size_t j;

        if (rb->memcmp(record + i, prefix, prefix_len) != 0)
            continue;

        j = i + prefix_len;
        if (j >= bytes || record[j] < '0' || record[j] > '9')
            continue;

        while (j < bytes && record[j] >= '0' && record[j] <= '9')
        {
            if (value > 9999)
                return 0;
            value = value * 10 + (unsigned int)(record[j] - '0');
            j++;
        }

        if (value != 0)
            return (int)value;
    }

    return 0;
}

static bool parse_mhni_at(int fd, off_t file_size, off_t offset,
                          struct photo_cache_entry *entry)
{
    unsigned char header[36];
    uint32_t header_len;
    uint32_t total_len;
    uint32_t format_id;
    uint32_t image_size;
    size_t record_bytes;
    int file_index;

    if (offset < 0 || offset > file_size - (off_t)sizeof(header))
        return false;

    if (!read_at(fd, offset, header, sizeof(header)) ||
        rb->memcmp(header, "mhni", 4) != 0)
        return false;

    header_len = get_le32(header + 4);
    total_len = get_le32(header + 8);
    format_id = get_le32(header + 16);
    image_size = get_le32(header + 24);

    if (header_len < sizeof(header) ||
        total_len < header_len ||
        (uint64_t)total_len > (uint64_t)(file_size - offset) ||
        format_id != F1013_FORMAT_ID ||
        image_size != F1013_FRAME_BYTES)
        return false;

    record_bytes = MIN((size_t)total_len, sizeof(db_record));
    if (!read_at(fd, offset, db_record, record_bytes))
        return false;

    file_index = find_f1013_file_index(db_record, record_bytes);
    if (file_index <= 0 || file_index > UINT16_MAX)
        return false;

    if (entry != NULL)
    {
        entry->offset = get_le32(header + 20);
        entry->image_size = image_size;
        entry->file_index = (uint16_t)file_index;
        entry->vertical_padding = get_le16s(header + 28);
        entry->horizontal_padding = get_le16s(header + 30);
        entry->height = get_le16s(header + 32);
        entry->width = get_le16s(header + 34);
    }

    return true;
}

static unsigned int scan_photo_database(const char *path,
                                        struct photo_cache_entry *entries,
                                        unsigned int capacity)
{
    off_t file_size;
    off_t base = 0;
    off_t last_match = -1;
    unsigned int count = 0;
    int fd;

    fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    file_size = rb->filesize(fd);
    if (file_size < 12)
        goto out;

    {
        unsigned char magic[4];

        if (!read_at(fd, 0, magic, sizeof(magic)) ||
            rb->memcmp(magic, "mhfd", 4) != 0)
            goto out;
    }

    while (base < file_size)
    {
        size_t want = (size_t)MIN((off_t)sizeof(db_scan), file_size - base);
        size_t i;

        if (want < 4 || !read_at(fd, base, db_scan, want))
            break;

        for (i = 0; i + 4 <= want; i++)
        {
            off_t candidate;

            if (db_scan[i] != 'm' ||
                rb->memcmp(db_scan + i, "mhni", 4) != 0)
                continue;

            candidate = base + (off_t)i;
            if (candidate == last_match)
                continue;

            last_match = candidate;

            if (parse_mhni_at(fd, file_size, candidate,
                              entries != NULL && count < capacity ?
                              &entries[count] : NULL))
            {
                if (entries == NULL || count < capacity)
                    count++;
                else
                    goto out;
            }
        }

        if (want <= 3)
            break;

        /* Three-byte overlap catches a signature crossing a block boundary. */
        base += (off_t)want - 3;
    }

out:
    rb->close(fd);
    return count;
}

static void select_photo_source(struct photo_cache *cache)
{
    if (path_exists(PHOTO_DB_TEST))
    {
        cache->db_path = PHOTO_DB_TEST;
        cache->thumb_dir = PHOTO_THUMBS_TEST;
        cache->source_name = "test";
    }
    else
    {
        cache->db_path = PHOTO_DB_REAL;
        cache->thumb_dir = PHOTO_THUMBS_REAL;
        cache->source_name = "synced";
    }

    cache->log_enabled = path_exists(PHOTO_LOG_ENABLE);
}

/* F1013 is a 176x220 raster representing the 220x176 screen rotated 90
 * degrees counter-clockwise.  Undo that with a clockwise rotation.
 *
 * Because raw big-endian RGB565 bytes equal RGB565SWAPPED framebuffer bytes
 * on iPod Color, assigning the raw 16-bit word preserves the correct memory
 * byte order without any color conversion.
 */
static void rotate_f1013(const unsigned char *raw, fb_data *frame)
{
    const fb_data *src = (const fb_data *)raw;
    int x;

    for (x = 0; x < F1013_WIDTH; x++)
    {
        const fb_data *s =
            src + (F1013_WIDTH - 1 - x) * F1013_STORED_WIDTH;
        fb_data *d = frame + x;
        int y;

        for (y = 0; y < F1013_HEIGHT; y++)
        {
            *d = s[y];
            d += F1013_WIDTH;
        }
    }
}

static void log_frame(const struct photo_cache *cache,
                      unsigned int index,
                      const struct photo_cache_entry *entry,
                      uint32_t read_us, uint32_t rotate_us,
                      uint32_t lcd_us, uint32_t total_us,
                      uint32_t crc)
{
    int fd;

    if (!cache->log_enabled)
        return;

    fd = rb->open(PHOTO_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    if (rb->filesize(fd) == 0)
    {
        rb->fdprintf(fd,
            "source,index,count,file_index,offset,image_size,width,height,"
            "hpad,vpad,read_us,rotate_us,lcd_us,total_us,crc\n");
    }

    rb->fdprintf(fd,
        "%s,%u,%u,%u,%lu,%lu,%d,%d,%d,%d,%lu,%lu,%lu,%lu,%08lx\n",
        cache->source_name,
        index,
        cache->count,
        (unsigned int)entry->file_index,
        (unsigned long)entry->offset,
        (unsigned long)entry->image_size,
        entry->width,
        entry->height,
        entry->horizontal_padding,
        entry->vertical_padding,
        (unsigned long)read_us,
        (unsigned long)rotate_us,
        (unsigned long)lcd_us,
        (unsigned long)total_us,
        (unsigned long)crc);

    rb->close(fd);
}

static bool show_photo(struct photo_cache *cache, unsigned int index)
{
    const struct photo_cache_entry *entry = &cache->entries[index];
    char path[MAX_PATH];
    uint32_t total_started;
    uint32_t started;
    uint32_t read_us;
    uint32_t rotate_us;
    uint32_t lcd_us;
    uint32_t crc;
    off_t file_size;
    int fd;

    rb->snprintf(path, sizeof(path), "%s/F1013_%u.ithmb",
                 cache->thumb_dir, (unsigned int)entry->file_index);

    total_started = now_us();

    fd = rb->open(path, O_RDONLY);
    if (fd < 0)
    {
        rb->splashf(HZ, "Missing F1013_%u.ithmb",
                    (unsigned int)entry->file_index);
        return false;
    }

    file_size = rb->filesize(fd);
    if ((uint64_t)entry->offset + F1013_FRAME_BYTES >
        (uint64_t)file_size)
    {
        rb->close(fd);
        rb->splash(HZ, "Bad .ithmb offset");
        return false;
    }

    started = now_us();
    if (!read_at(fd, (off_t)entry->offset,
                 cache->raw, F1013_FRAME_BYTES))
    {
        rb->close(fd);
        rb->splash(HZ, "Read failed");
        return false;
    }
    rb->close(fd);
    read_us = now_us() - started;

    started = now_us();
    rotate_f1013(cache->raw, cache->frame);
    rotate_us = now_us() - started;

    crc = rb->crc_32(cache->frame, F1013_FRAME_BYTES, 0xffffffff);

    started = now_us();
    rb->lcd_bitmap(cache->frame, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    rb->lcd_update();
    lcd_us = now_us() - started;

    log_frame(cache, index, entry, read_us, rotate_us, lcd_us,
              now_us() - total_started, crc);

    return true;
}

static int load_photo_index(struct photo_cache *cache)
{
    size_t buffer_size;
    unsigned char *buffer;
    unsigned int count;
    unsigned int capacity;
    size_t fixed_bytes = 2 * F1013_FRAME_BYTES;

    buffer = rb->plugin_get_buffer(&buffer_size);
    if (buffer == NULL || buffer_size <= fixed_bytes)
        return -1;

    cache->raw = buffer;
    cache->frame = (fb_data *)(buffer + F1013_FRAME_BYTES);
    cache->work_size = buffer_size;

    count = scan_photo_database(cache->db_path, NULL, 0);
    if (count == 0)
        return 0;

    capacity = (unsigned int)((buffer_size - fixed_bytes) /
                              sizeof(*cache->entries));
    if (count > capacity)
        return -1;

    cache->entries =
        (struct photo_cache_entry *)(buffer + fixed_bytes);
    cache->count = scan_photo_database(cache->db_path,
                                       cache->entries, capacity);

    if (cache->count != count)
        return -1;

    return (int)cache->count;
}

static enum plugin_status browse_cache(struct photo_cache *cache)
{
    unsigned int current = 0;

    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "Native Photo Cache");
    rb->lcd_putsf(0, 1, "%u photos (%s)",
                  cache->count, cache->source_name);
    rb->lcd_puts(0, 3, "Menu: exit");
    rb->lcd_puts(0, 4, "Wheel/L/R: browse");
    rb->lcd_update();
    rb->sleep(HZ / 2);

    if (!show_photo(cache, current))
        return PLUGIN_ERROR;

    while (true)
    {
        int button = rb->button_get(true);

        if (button & BUTTON_MENU)
            break;

        if ((button & BUTTON_RIGHT) || (button & BUTTON_SCROLL_FWD))
        {
            current++;
            if (current >= cache->count)
                current = 0;
            show_photo(cache, current);
            rb->button_clear_queue();
            continue;
        }

        if ((button & BUTTON_LEFT) || (button & BUTTON_SCROLL_BACK))
        {
            if (current == 0)
                current = cache->count - 1;
            else
                current--;
            show_photo(cache, current);
            rb->button_clear_queue();
            continue;
        }

        if (button != BUTTON_NONE &&
            rb->default_event_handler(button) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
    }

    return PLUGIN_OK;
}

enum plugin_status plugin_start(const void *parameter)
{
    struct photo_cache cache;
    int photos;

    (void)parameter;

    rb->memset(&cache, 0, sizeof(cache));
    select_photo_source(&cache);

    if (!path_exists(cache.db_path))
    {
        rb->splash(HZ * 2, "No Photo Database");
        return PLUGIN_ERROR;
    }

    photos = load_photo_index(&cache);
    if (photos < 0)
    {
        rb->splash(HZ * 2, "Photo cache out of memory");
        return PLUGIN_ERROR;
    }
    if (photos == 0)
    {
        rb->splash(HZ * 2, "No F1013 photos found");
        return PLUGIN_ERROR;
    }

    return browse_cache(&cache);
}

#else

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    rb->splash(HZ * 2, "iPod Photo only");
    return PLUGIN_ERROR;
}

#endif
