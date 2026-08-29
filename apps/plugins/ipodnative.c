/***************************************************************************
 * iPod Photo native video and audio viewer (.ipvf)
 *
 * IPVF stores 220x176 RGB565SWAPPED frames as sector-aligned key, rectangle,
 * or repeat records, followed by the matching 44.1 kHz stereo PCM slice. On
 * PP5020 the CPU performs one aligned read into an uncached staging slot while
 * the COP sends the previous record through the LCD driver.
 ****************************************************************************/
#include "plugin.h"
#include "cpu.h"
#include <lib/helper.h>

#if LCD_WIDTH != 220 || LCD_HEIGHT != 176 || LCD_DEPTH != 16 || \
    LCD_PIXELFORMAT != RGB565SWAPPED
#error IPVF requires the iPod Photo/Color RGB565SWAPPED framebuffer
#endif

#ifndef USEC_TIMER
#define USEC_TIMER \
    ((uint32_t)((uint64_t)*rb->current_tick * 1000000u / HZ))
#endif

#define IPVF_MAGIC                  "IPVF"
#define IPVF_VERSION                1u
#define IPVF_HEADER_SIZE            64u
#define IPVF_DATA_OFFSET            512u
#define IPVF_RECORD_SECTOR_SIZE     512u
#define IPVF_RECORD_MAX_SECTORS     256u
#define IPVF_RECORD_MAX_BYTES \
    (IPVF_RECORD_MAX_SECTORS * IPVF_RECORD_SECTOR_SIZE)
#define IPVF_FRAME_HEADER_SIZE      8u
#define IPVF_RECT_HEADER_SIZE       8u
#define IPVF_FLAG_RGB565BE          0x00000001u
#define IPVF_FLAG_SECTOR_RECORDS    0x00000002u
#define IPVF_FLAG_PCM_S16LE         0x00000004u
#define IPVF_FLAGS \
    (IPVF_FLAG_RGB565BE | IPVF_FLAG_SECTOR_RECORDS | \
     IPVF_FLAG_PCM_S16LE)
#define IPVF_TYPE_KEY               0u
#define IPVF_TYPE_RECTS             1u
#define IPVF_TYPE_REPEAT            2u
#define IPVF_MAX_FPS                240u
#define IPVF_FRAME_BYTES \
    ((size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data))
#define IPVF_MAX_PAYLOAD            (IPVF_FRAME_BYTES + 4096u)
#define IPVF_AUDIO_FORMAT_PCM_S16LE  1u
#define IPVF_AUDIO_CHANNELS          2u
#define IPVF_AUDIO_BITS_PER_SAMPLE   16u
#define IPVF_AUDIO_SAMPLE_RATE       44100u
#define IPVF_AUDIO_FRAME_BYTES       4u
#define IPVF_MIN_FPS                 4u
#define IPVF_AUDIO_RING_MIN_BYTES    (256u * 1024u)
#define IPVF_AUDIO_RING_MAX_BYTES    (1024u * 1024u)
#define IPVF_AUDIO_DMA_MAX_BYTES     (16u * 1024u)
#define IPVF_AUDIO_DMA_MAX_FRAMES \
    (IPVF_AUDIO_DMA_MAX_BYTES / IPVF_AUDIO_FRAME_BYTES)
#define IPVF_AUDIO_CHANNEL           PCM_MIXER_CHAN_PLAYBACK

#if defined(IPOD_COLOR) && NUM_CORES > 1 && \
    defined(HAVE_SEMAPHORE_OBJECTS) && !defined(SIMULATOR)
#define IPVF_NATIVE_PIPELINE
#define IPVF_RENDER_SLOTS       3u
#define IPVF_RENDER_SLOT_STRIDE 0x20000u
#define IPVF_RENDER_STACK       3072u
#endif

typedef char ipvf_fb_data_must_be_16_bits[
    sizeof(fb_data) == 2 ? 1 : -1];
typedef char ipvf_pcm_frame_must_be_4_bytes[
    IPVF_AUDIO_FRAME_BYTES == 4 ? 1 : -1];

struct ipvf_info
{
    unsigned int fps_num;
    unsigned int fps_den;
    unsigned long frame_count;
    uint32_t first_record_sectors;
    off_t file_size;
    uint32_t audio_sample_rate;
    uint32_t audio_sample_frames;
};

struct ipvf_stats
{
    unsigned long frames;
    unsigned long late_frames;
    unsigned long max_late_us;
    unsigned long audio_underruns;
};

struct ipvf_audio_state
{
    unsigned char *buffer;
    uint32_t capacity_frames;
    uint32_t frame_mask;
    volatile uint32_t written_frames;
    volatile uint32_t completed_frames;
    volatile uint32_t current_frames;
    volatile unsigned long underruns;
    volatile bool source_eof;
    unsigned int old_frequency;
    bool buffer_acquired;
    bool configured;
    bool started;
};

static struct ipvf_audio_state audio;

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

static uint32_t audio_frame_boundary(const struct ipvf_info *info,
                                     unsigned long frame)
{
    return (uint32_t)(((uint64_t)frame * info->audio_sample_rate *
                       info->fps_den + info->fps_num / 2) /
                      info->fps_num);
}

static uint32_t record_audio_frames(const struct ipvf_info *info,
                                    unsigned long frame)
{
    uint32_t start = audio_frame_boundary(info, frame);
    uint32_t end = audio_frame_boundary(info, frame + 1);

    return end - start;
}

static bool read_header(int fd, struct ipvf_info *info)
{
    unsigned char h[IPVF_DATA_OFFSET];
    unsigned int i;
    uint32_t flags;
    uint32_t data_offset;
    uint64_t expected_audio_frames;

    if (!read_exact(fd, h, sizeof(h)) || rb->memcmp(h, IPVF_MAGIC, 4))
        return false;

    info->fps_num = get_le16(h + 12);
    info->fps_den = get_le16(h + 14);
    info->frame_count = get_le32(h + 16);
    flags = get_le32(h + 20);
    data_offset = get_le32(h + 24);
    info->first_record_sectors = get_le16(h + 28);
    info->audio_sample_rate = get_le32(h + 36);
    info->audio_sample_frames = get_le32(h + 40);
    info->file_size = rb->filesize(fd);

    if (get_le16(h + 4) != IPVF_VERSION ||
        get_le16(h + 6) != IPVF_HEADER_SIZE ||
        get_le16(h + 8) != LCD_WIDTH ||
        get_le16(h + 10) != LCD_HEIGHT ||
        info->fps_num == 0 || info->fps_den == 0 ||
        info->fps_num < IPVF_MIN_FPS * info->fps_den ||
        info->fps_num > IPVF_MAX_FPS * info->fps_den ||
        info->frame_count == 0 || flags != IPVF_FLAGS ||
        data_offset != IPVF_DATA_OFFSET ||
        info->first_record_sectors == 0 ||
        info->first_record_sectors > IPVF_RECORD_MAX_SECTORS ||
        get_le16(h + 30) != IPVF_AUDIO_FORMAT_PCM_S16LE ||
        get_le16(h + 32) != IPVF_AUDIO_CHANNELS ||
        get_le16(h + 34) != IPVF_AUDIO_BITS_PER_SAMPLE ||
        info->audio_sample_rate != IPVF_AUDIO_SAMPLE_RATE ||
        info->file_size < (off_t)IPVF_DATA_OFFSET)
        return false;

    expected_audio_frames =
        ((uint64_t)info->frame_count * info->audio_sample_rate *
         info->fps_den + info->fps_num / 2) / info->fps_num;
    if (expected_audio_frames == 0 ||
        expected_audio_frames > 0xffffffffu ||
        info->audio_sample_frames != (uint32_t)expected_audio_frames)
        return false;

    for (i = 44; i < sizeof(h); i++)
        if (h[i] != 0)
            return false;

    return rb->lseek(fd, IPVF_DATA_OFFSET, SEEK_SET) ==
           (off_t)IPVF_DATA_OFFSET;
}

static bool validate_rects(const unsigned char *payload, size_t bytes,
                           unsigned int rect_count, bool native_geometry)
{
    const unsigned char *p = payload;
    const unsigned char *end = payload + bytes;
    unsigned int i;

    for (i = 0; i < rect_count; i++)
    {
        unsigned int x, y, w, h;
        uint32_t data_bytes;

        if ((size_t)(end - p) < IPVF_RECT_HEADER_SIZE)
            return false;

        x = p[0];
        y = p[1];
        w = p[2];
        h = p[3];
        data_bytes = get_le32(p + 4);
        p += IPVF_RECT_HEADER_SIZE;

        if (w == 0 || h == 0 || x + w > LCD_WIDTH || y + h > LCD_HEIGHT ||
            data_bytes != (uint32_t)w * h * sizeof(fb_data) ||
            (size_t)(end - p) < data_bytes ||
            (native_geometry &&
             ((x & 1) != 0 || (w & 1) != 0 || ((uintptr_t)p & 3) != 0)))
            return false;

        p += data_bytes;
    }

    return p == end;
}

static bool parse_record(const unsigned char *record,
                         uint32_t current_sectors,
                         unsigned long frame,
                         const struct ipvf_info *info,
                         unsigned int *type,
                         unsigned int *rect_count,
                         uint32_t *payload_size,
                         uint32_t *audio_frames,
                         uint32_t *next_sectors,
                         bool native_geometry)
{
    uint64_t used_bytes;
    uint32_t expected_sectors;

    *type = record[0];
    *rect_count = record[1];
    *next_sectors = get_le16(record + 2);
    *payload_size = get_le32(record + 4);
    *audio_frames = record_audio_frames(info, frame);

    if (*payload_size > IPVF_MAX_PAYLOAD)
        return false;

    used_bytes = IPVF_FRAME_HEADER_SIZE + *payload_size +
                 (uint64_t)*audio_frames * IPVF_AUDIO_FRAME_BYTES;
    expected_sectors = (uint32_t)((used_bytes +
                                  IPVF_RECORD_SECTOR_SIZE - 1) /
                                 IPVF_RECORD_SECTOR_SIZE);
    if (expected_sectors == 0 ||
        expected_sectors > IPVF_RECORD_MAX_SECTORS ||
        current_sectors == 0 ||
        current_sectors > IPVF_RECORD_MAX_SECTORS ||
        current_sectors != expected_sectors ||
        (frame == 0 && *type != IPVF_TYPE_KEY) ||
        (frame + 1 < info->frame_count &&
         (*next_sectors == 0 ||
          *next_sectors > IPVF_RECORD_MAX_SECTORS)) ||
        (frame + 1 == info->frame_count && *next_sectors != 0))
        return false;

    if (*type == IPVF_TYPE_KEY)
        return *rect_count == 0 && *payload_size == IPVF_FRAME_BYTES;

    if (*type == IPVF_TYPE_REPEAT)
        return *rect_count == 0 && *payload_size == 0;

    if (*type != IPVF_TYPE_RECTS || *rect_count == 0)
        return false;

    return validate_rects(record + IPVF_FRAME_HEADER_SIZE,
                          *payload_size, *rect_count, native_geometry);
}

#include "ipodnative_display.inc"
#include "ipodnative_audio.inc"
#include "ipodnative_player.inc"
