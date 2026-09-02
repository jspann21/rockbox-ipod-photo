/***************************************************************************
 * Native iPod Photo Database decoder for Image Viewer.
 *
 * iPod Photo format 1013 is a screen-sized 220x176 RGB565 image stored in
 * F1013_N.ithmb as a 176x220 raster rotated 90 degrees counter-clockwise.
 * Its big-endian RGB565 bytes already match the iPod Color RGB565SWAPPED
 * framebuffer representation, so display requires only a read and rotation.
 ****************************************************************************/
#include "plugin.h"
#include "../imageviewer.h"
#include <limits.h>

#define F1013_FORMAT_ID      1013u
#define F1013_WIDTH          LCD_WIDTH
#define F1013_HEIGHT         LCD_HEIGHT
#define F1013_STORED_WIDTH   LCD_HEIGHT
#define F1013_FRAME_BYTES    ((size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data))

#define DB_SCAN_BLOCK 4096
#define DB_RECORD_SCAN 1024

struct photo_entry
{
    uint32_t offset;
    uint16_t file_index;
    uint16_t reserved;
};

struct photo_database
{
    struct photo_entry *entries;
    unsigned int count;
    unsigned int current;
    unsigned char *raw;
    fb_data *frame;
    bool first_get;
    bool frame_valid;
    char thumb_dir[MAX_PATH];
};

static struct photo_database photos;
static unsigned char db_scan[DB_SCAN_BLOCK];
static unsigned char db_record[DB_RECORD_SCAN];

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

static int parse_f1013_index_ascii(const unsigned char *text, size_t bytes)
{
    static const char prefix[] = "F1013_";
    const size_t prefix_len = sizeof(prefix) - 1;
    size_t i;

    for (i = 0; i + prefix_len < bytes; i++)
    {
        unsigned int value = 0;
        size_t j;

        if (rb->memcmp(text + i, prefix, prefix_len) != 0)
            continue;

        j = i + prefix_len;
        while (j < bytes && text[j] >= '0' && text[j] <= '9')
        {
            unsigned int digit = (unsigned int)(text[j] - '0');
            if (value > (UINT16_MAX - digit) / 10)
                return 0;
            value = value * 10 + digit;
            j++;
        }

        if (j != i + prefix_len && value != 0)
            return (int)value;
    }

    return 0;
}

static int parse_f1013_index_utf16le(const unsigned char *text, size_t bytes)
{
    static const char prefix[] = "F1013_";
    const size_t prefix_len = sizeof(prefix) - 1;
    size_t i;

    for (i = 0; i + prefix_len * 2 < bytes; i++)
    {
        unsigned int value = 0;
        size_t j, k;

        for (k = 0; k < prefix_len; k++)
        {
            if (text[i + k * 2] != (unsigned char)prefix[k] ||
                text[i + k * 2 + 1] != 0)
                break;
        }
        if (k != prefix_len)
            continue;

        j = i + prefix_len * 2;
        while (j + 1 < bytes && text[j] >= '0' && text[j] <= '9' &&
               text[j + 1] == 0)
        {
            unsigned int digit = (unsigned int)(text[j] - '0');
            if (value > (UINT16_MAX - digit) / 10)
                return 0;
            value = value * 10 + digit;
            j += 2;
        }

        if (j != i + prefix_len * 2 && value != 0)
            return (int)value;
    }

    return 0;
}

static int find_f1013_file_index(const unsigned char *record, size_t bytes)
{
    size_t i;

    /* Standard little-endian iPods use a type-3 mhod with UTF-16LE.
       Accept byte strings too for databases produced by other sync tools. */
    for (i = 0; i + 36 <= bytes; i++)
    {
        uint32_t total_len;
        uint32_t string_len;
        const unsigned char *text;
        int index;

        if (record[i] != 'm' || rb->memcmp(record + i, "mhod", 4) != 0)
            continue;

        total_len = get_le32(record + i + 8);
        if (total_len < 36 || total_len > bytes - i ||
            get_le16(record + i + 12) != 3)
            continue;

        string_len = get_le32(record + i + 24);
        if (string_len > total_len - 36)
            continue;

        text = record + i + 36;
        if (record[i + 28] == 2)
            index = parse_f1013_index_utf16le(text, string_len);
        else
            index = parse_f1013_index_ascii(text, string_len);

        if (index != 0)
            return index;
    }

    /* Compatibility with older/minimal Photo Database writers. */
    {
        int index = parse_f1013_index_ascii(record, bytes);
        if (index != 0)
            return index;
    }

    return parse_f1013_index_utf16le(record, bytes);
}

static bool parse_mhni_at(int fd, off_t file_size, off_t offset,
                          struct photo_entry *entry)
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

    if (header_len < sizeof(header) || total_len < header_len ||
        (uint64_t)total_len > (uint64_t)(file_size - offset) ||
        format_id != F1013_FORMAT_ID || image_size != F1013_FRAME_BYTES)
        return false;

    record_bytes = MIN((size_t)total_len, sizeof(db_record));
    if (!read_at(fd, offset, db_record, record_bytes))
        return false;

    file_index = find_f1013_file_index(db_record, record_bytes);
    if (file_index <= 0 || file_index > UINT16_MAX)
        return false;

    entry->offset = get_le32(header + 20);
    entry->file_index = (uint16_t)file_index;
    entry->reserved = 0;
    return true;
}

static unsigned int scan_photo_database(const char *path,
                                        struct photo_entry *entries,
                                        unsigned int capacity,
                                        bool *overflow)
{
    off_t file_size;
    off_t base = 0;
    off_t last_match = -1;
    unsigned int count = 0;
    int fd;

    *overflow = false;
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
            struct photo_entry entry;

            if (db_scan[i] != 'm' ||
                rb->memcmp(db_scan + i, "mhni", 4) != 0)
                continue;

            candidate = base + (off_t)i;
            if (candidate == last_match)
                continue;
            last_match = candidate;

            if (!parse_mhni_at(fd, file_size, candidate, &entry))
                continue;

            if (count >= capacity)
            {
                *overflow = true;
                goto out;
            }

            entries[count++] = entry;
        }

        if (want <= 3)
            break;
        base += (off_t)want - 3;
    }

out:
    rb->close(fd);
    return count;
}

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

static bool load_current_frame(void)
{
    const struct photo_entry *entry;
    char path[MAX_PATH];
    off_t file_size;
    int fd;

    if (photos.frame_valid)
        return true;

    entry = &photos.entries[photos.current];
    rb->snprintf(path, sizeof(path), "%s/F1013_%u.ithmb",
                 photos.thumb_dir, (unsigned int)entry->file_index);

    fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return false;

    file_size = rb->filesize(fd);
    if (file_size < 0 ||
        (sizeof(off_t) <= sizeof(entry->offset) &&
         entry->offset > (uint32_t)LONG_MAX) ||
        (uint64_t)entry->offset + F1013_FRAME_BYTES > (uint64_t)file_size ||
        !read_at(fd, (off_t)entry->offset, photos.raw, F1013_FRAME_BYTES))
    {
        rb->close(fd);
        return false;
    }
    rb->close(fd);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
    rotate_f1013(photos.raw, photos.frame);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif

    photos.frame_valid = true;
    return true;
}

static int img_mem(int ds)
{
    (void)ds;
    return 0; /* all storage required by the decoder is reserved in load_image */
}

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize)
{
    uintptr_t aligned;
    size_t skip;
    size_t available;
    size_t fixed_bytes = 2 * F1013_FRAME_BYTES;
    unsigned int capacity;
    bool overflow;
    const char *slash;
    size_t parent_len;

    (void)offset;
    (void)filesize;

    rb->memset(&photos, 0, sizeof(photos));

    /* PP5020 ATA reads require a cache-line-aligned destination for DMA.
     * Every F1013 frame is a multiple of 16 bytes, so this alignment also
     * keeps the second frame buffer and subsequent frame reads DMA-safe. */
    aligned = ((uintptr_t)buf + 15) & ~(uintptr_t)15;
    skip = aligned - (uintptr_t)buf;
    if (*buf_size <= (ssize_t)skip)
        return PLUGIN_OUTOFMEM;

    available = (size_t)*buf_size - skip;
    if (available < fixed_bytes + sizeof(struct photo_entry))
        return PLUGIN_OUTOFMEM;

    photos.raw = (unsigned char *)aligned;
    photos.frame = (fb_data *)(photos.raw + F1013_FRAME_BYTES);
    photos.entries = (struct photo_entry *)(photos.raw + fixed_bytes);
    capacity = (unsigned int)((available - fixed_bytes) /
                              sizeof(struct photo_entry));

    photos.count = scan_photo_database(filename, photos.entries,
                                       capacity, &overflow);
    if (overflow)
        return PLUGIN_OUTOFMEM;
    if (photos.count == 0)
    {
        rb->splash(HZ * 2, "No synced photos");
        return PLUGIN_ERROR;
    }

    slash = rb->strrchr(filename, '/');
    if (slash == NULL)
        return PLUGIN_ERROR;
    parent_len = (size_t)(slash - filename);
    if (parent_len + sizeof("/Thumbs") > sizeof(photos.thumb_dir))
        return PLUGIN_ERROR;
    rb->snprintf(photos.thumb_dir, sizeof(photos.thumb_dir),
                 "%.*s/Thumbs", (int)parent_len, filename);

    photos.current = 0;
    photos.first_get = true;
    photos.frame_valid = false;

    info->x_size = F1013_WIDTH;
    info->y_size = F1013_HEIGHT;
    info->frames_count = (int)photos.count;
    info->delay = 0;

    *buf_size = (ssize_t)(available - fixed_bytes -
                          photos.count * sizeof(struct photo_entry));
    return PLUGIN_OK;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    int direction;

    (void)frame;
    (void)ds;

    if (photos.first_get)
    {
        photos.first_get = false;
    }
    else
    {
        direction = info->x < 0 ? -1 : 1;
        if (direction < 0)
        {
            if (photos.current == 0)
                photos.current = photos.count - 1;
            else
                photos.current--;
        }
        else
        {
            photos.current++;
            if (photos.current >= photos.count)
                photos.current = 0;
        }
        photos.frame_valid = false;
    }

    info->x = 0;
    info->y = 0;
    info->width = F1013_WIDTH;
    info->height = F1013_HEIGHT;
    info->data = photos.frame;

    if (!load_current_frame())
    {
        rb->splash(HZ, "Photo cache read failed");
        return PLUGIN_ERROR;
    }

    return PLUGIN_OK;
}

static void draw_image_rect(struct image_info *info,
                            int x, int y, int width, int height)
{
    rb->lcd_bitmap_part(photos.frame,
                        info->x + x, info->y + y, F1013_WIDTH,
                        x, y, width, height);
}

const struct image_decoder image_decoder = {
    true,
    img_mem,
    load_image,
    get_image,
    draw_image_rect,
};

IMGDEC_HEADER
