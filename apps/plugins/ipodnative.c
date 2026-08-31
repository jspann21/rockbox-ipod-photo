/***************************************************************************
 * iPod Photo native video and audio viewer (.ipvf)
 *
 * IPVF stores 220x176 RGB565SWAPPED frames as sector-aligned raw, independently
 * LZ4-compressed, or repeat records, followed by matching 44.1 kHz stereo IMA
 * ADPCM. On PP5020 the CPU reads and decodes the next record while the COP sends
 * the previous decoded record through the LCD driver.
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

/* Qualification instrumentation is deliberately opt-in. Define this as 1
 * for a dedicated measurement build; normal playback leaves it at zero. */
#ifndef IPVF_ENABLE_QUALIFICATION_TELEMETRY
#define IPVF_ENABLE_QUALIFICATION_TELEMETRY 0
#endif

#define IPVF_MAGIC                  "IPVF"
#define IPVF_VERSION                1u
#define IPVF_HEADER_SIZE            64u
#define IPVF_DATA_OFFSET            512u
#define IPVF_RECORD_SECTOR_SIZE     512u
#define IPVF_RECORD_MAX_SECTORS     192u
#define IPVF_RECORD_MAX_BYTES \
    (IPVF_RECORD_MAX_SECTORS * IPVF_RECORD_SECTOR_SIZE)
#define IPVF_FRAME_HEADER_SIZE      12u
#define IPVF_RECT_HEADER_SIZE       8u
#define IPVF_FLAG_RGB565BE          0x00000001u
#define IPVF_FLAG_SECTOR_RECORDS    0x00000002u
#define IPVF_FLAG_IMA_ADPCM         0x00000008u
#define IPVF_FLAG_TEMPORAL_XOR      0x00000010u
#define IPVF_FLAGS \
    (IPVF_FLAG_RGB565BE | IPVF_FLAG_SECTOR_RECORDS | \
     IPVF_FLAG_IMA_ADPCM)
#define IPVF_TYPE_KEY               0u
#define IPVF_TYPE_RECTS             1u
#define IPVF_TYPE_REPEAT            2u
#define IPVF_TYPE_KEY_LZ4           3u
#define IPVF_TYPE_RECTS_LZ4         4u
#define IPVF_TYPE_XOR_LZ4           5u
#define IPVF_TYPE_COUNT              6u
#define IPVF_MAX_FPS                240u
#define IPVF_FRAME_BYTES \
    ((size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data))
#define IPVF_MAX_PAYLOAD            (IPVF_FRAME_BYTES + 4096u)
#define IPVF_AUDIO_FORMAT_IMA_ADPCM  2u
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
#define IPVF_DECODED_SLOT_STRIDE      0x20000u
#define IPVF_LATE_THRESHOLD_FRAMES \
    ((IPVF_AUDIO_SAMPLE_RATE * 501u + 999999u) / 1000000u)

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
#define IPVF_QUALIFICATION_LOG \
    ROCKBOX_DIR "/ipvf-qualification-v5.tsv"
#define IPVF_QUALIFICATION_MARKER \
    ROCKBOX_DIR "/ipvf-qualification.enable"
#define IPVF_DECODER_REV             "xor-payloadcrc-4"
#endif

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
enum ipvf_error_code
{
    IPVF_ERROR_NONE = 0,
    IPVF_ERROR_RENDER_ACQUIRE,
    IPVF_ERROR_RECORD_CHAIN,
    IPVF_ERROR_RECORD_BOUNDS,
    IPVF_ERROR_READ,
    IPVF_ERROR_PARSE,
    IPVF_ERROR_AUDIO_FEED,
    IPVF_ERROR_VIDEO_DECODE,
    IPVF_ERROR_AUDIO_CLOCK,
    IPVF_ERROR_RENDER_QUEUE,
    IPVF_ERROR_RENDER_PRESENT,
    IPVF_ERROR_DISPLAY_APPLY,
    IPVF_ERROR_PREBUFFER,
    IPVF_ERROR_EOF_CHAIN,
    IPVF_ERROR_RENDER_FINISH,
    IPVF_ERROR_AUDIO_FINISH,
    IPVF_ERROR_RECONCILE,
};
#endif

#if defined(IPOD_COLOR) && NUM_CORES > 1 && \
    defined(HAVE_SEMAPHORE_OBJECTS) && !defined(SIMULATOR)
#define IPVF_NATIVE_PIPELINE
#define IPVF_RENDER_SLOTS       3u
#define IPVF_RENDER_SLOT_STRIDE IPVF_DECODED_SLOT_STRIDE
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
    bool temporal_xor;
};

struct ipvf_stats
{
    unsigned long frames;
    unsigned long late_frames;
    unsigned long audio_underruns;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    unsigned long max_late_us;
    unsigned long max_read_us;
    unsigned long max_video_decode_us;
    unsigned long max_lz4_decode_us;
    unsigned long max_temporal_check_us;
    unsigned long max_temporal_reconstruct_us;
    unsigned long max_video_copy_us;
    unsigned long max_audio_feed_us;
    unsigned long max_render_us;
    unsigned long max_render_slot_wait_us;
    unsigned long max_record_sectors;
    unsigned long start_tick;
    unsigned long elapsed_ticks;
    unsigned long render_calls;
    unsigned long rectangle_calls;
    unsigned long render_failures;
    unsigned long error_count;
    unsigned long first_error;
    unsigned long first_error_frame;
    unsigned long type_counts[IPVF_TYPE_COUNT];
    uint64_t read_us;
    uint64_t video_decode_us;
    uint64_t lz4_decode_us;
    uint64_t temporal_check_us;
    uint64_t temporal_reconstruct_us;
    uint64_t video_copy_us;
    uint64_t audio_feed_us;
    uint64_t prebuffer_us;
    uint64_t render_us;
    uint64_t render_slot_wait_us;
    uint64_t record_bytes;
    uint64_t stored_video_bytes;
    uint64_t decoded_video_bytes;
    uint64_t audio_bytes;
    uint64_t audio_frames;
    uint64_t padding_bytes;
    uint32_t final_crc;
#endif
};

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
static void add_qualification_timing(uint64_t *total,
                                     unsigned long *maximum,
                                     uint32_t elapsed)
{
    *total += elapsed;
    if (elapsed > *maximum)
        *maximum = elapsed;
}
#endif

struct ipvf_record_info
{
    unsigned int source_type;
    unsigned int type;
    unsigned int rect_count;
    uint32_t stored_video_bytes;
    uint32_t decoded_video_bytes;
    uint32_t audio_frames;
    uint32_t audio_bytes;
    uint32_t next_sectors;
    bool compressed;
    bool temporal;
    bool keyframe;
};

struct ipvf_audio_state
{
    unsigned char *video_scratch;
    unsigned char *video_reference;
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
static uint32_t temporal_crc32_table[256];
static bool temporal_crc32_table_ready;

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

/* Temporal integrity covers the compressed residual. It detects stored/read
 * corruption before LZ4 touches the reference frame while avoiding a second
 * full-frame pass. The table lives in cached plugin BSS. */
static uint32_t ipvf_temporal_payload_crc32(const unsigned char *data,
                                            size_t bytes)
{
    uint32_t crc = 0xffffffffu;
    size_t i;

    if (!temporal_crc32_table_ready)
    {
        unsigned int index;

        for (index = 0; index < 256u; index++)
        {
            uint32_t value = (uint32_t)index << 24;
            unsigned int bit;

            for (bit = 0; bit < 8u; bit++)
                value = (value << 1) ^
                        ((value & 0x80000000u) ? 0x04c11db7u : 0u);
            temporal_crc32_table[index] = value;
        }
        temporal_crc32_table_ready = true;
    }
    for (i = 0; i < bytes; i++)
        crc = temporal_crc32_table[((crc >> 24) ^ data[i]) & 0xffu] ^
              (crc << 8);
    return crc;
}

static void ipvf_xor_frame(unsigned char *frame,
                           const unsigned char *residual)
{
    size_t i;

#ifdef ROCKBOX_LITTLE_ENDIAN
    {
        uint32_t *frame_words = (uint32_t *)frame;
        const uint32_t *residual_words = (const uint32_t *)residual;

        for (i = 0; i < IPVF_FRAME_BYTES / sizeof(uint32_t); i++)
            frame_words[i] ^= residual_words[i];
    }
#else
    for (i = 0; i < IPVF_FRAME_BYTES; i++)
        frame[i] ^= residual[i];
#endif
}

#include "ipodnative_lz4.inc"
#include "ipodnative_ima.inc"

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

static uint32_t record_audio_bytes(uint32_t frames)
{
    return IPVF_IMA_HEADER_BYTES + frames - 1u;
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
    info->temporal_xor = (flags & IPVF_FLAG_TEMPORAL_XOR) != 0;
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
        info->frame_count == 0 ||
        (flags != IPVF_FLAGS &&
         flags != (IPVF_FLAGS | IPVF_FLAG_TEMPORAL_XOR)) ||
        data_offset != IPVF_DATA_OFFSET ||
        info->first_record_sectors == 0 ||
        info->first_record_sectors > IPVF_RECORD_MAX_SECTORS ||
        get_le16(h + 30) != IPVF_AUDIO_FORMAT_IMA_ADPCM ||
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

static void update_reference_rects(const unsigned char *payload,
                                   unsigned int rect_count,
                                   unsigned char *reference)
{
    const unsigned char *p = payload;
    unsigned int i;

    for (i = 0; i < rect_count; i++)
    {
        unsigned int x = p[0];
        unsigned int y = p[1];
        unsigned int w = p[2];
        unsigned int h = p[3];
        unsigned int row;

        p += IPVF_RECT_HEADER_SIZE;
        for (row = 0; row < h; row++)
        {
            rb->memcpy(reference +
                       ((size_t)(y + row) * LCD_WIDTH + x) *
                       sizeof(fb_data),
                       p + (size_t)row * w * sizeof(fb_data),
                       (size_t)w * sizeof(fb_data));
        }
        p += (size_t)w * h * sizeof(fb_data);
    }
}

static bool parse_record(const unsigned char *record,
                         uint32_t current_sectors,
                         unsigned long frame,
                         const struct ipvf_info *info,
                         struct ipvf_record_info *record_info)
{
    struct ipvf_ima_state ima_state;
    const unsigned char *audio_payload;
    uint64_t used_bytes;
    uint32_t record_bytes;
    uint32_t expected_sectors;
    size_t i;
    unsigned int source_type;

    if (record == NULL || record_info == NULL || current_sectors == 0 ||
        current_sectors > IPVF_RECORD_MAX_SECTORS)
        return false;

    source_type = record[0];
    record_info->source_type = source_type;
    record_info->rect_count = record[1];
    record_info->next_sectors = get_le16(record + 2);
    record_info->stored_video_bytes = get_le32(record + 4);
    record_info->decoded_video_bytes = get_le32(record + 8);
    record_info->audio_frames = record_audio_frames(info, frame);
    if (record_info->audio_frames == 0)
        return false;
    record_info->audio_bytes = record_audio_bytes(record_info->audio_frames);
    record_info->compressed = source_type == IPVF_TYPE_KEY_LZ4 ||
                              source_type == IPVF_TYPE_RECTS_LZ4 ||
                              source_type == IPVF_TYPE_XOR_LZ4;
    record_info->temporal = source_type == IPVF_TYPE_XOR_LZ4;
    record_info->keyframe = source_type == IPVF_TYPE_KEY ||
                            source_type == IPVF_TYPE_KEY_LZ4;

    if (record_info->stored_video_bytes > IPVF_RECORD_MAX_BYTES ||
        record_info->decoded_video_bytes > IPVF_MAX_PAYLOAD)
        return false;

    used_bytes = IPVF_FRAME_HEADER_SIZE +
                 (uint64_t)record_info->stored_video_bytes +
                 record_info->audio_bytes;
    if (used_bytes > IPVF_RECORD_MAX_BYTES)
        return false;
    expected_sectors = (uint32_t)((used_bytes +
                                  IPVF_RECORD_SECTOR_SIZE - 1) /
                                 IPVF_RECORD_SECTOR_SIZE);
    if (expected_sectors == 0 || expected_sectors > IPVF_RECORD_MAX_SECTORS ||
        current_sectors != expected_sectors ||
        (frame == 0 && source_type != IPVF_TYPE_KEY &&
         source_type != IPVF_TYPE_KEY_LZ4) ||
        (frame + 1 < info->frame_count &&
         (record_info->next_sectors == 0 ||
          record_info->next_sectors > IPVF_RECORD_MAX_SECTORS)) ||
        (frame + 1 == info->frame_count && record_info->next_sectors != 0))
        return false;

    if (source_type == IPVF_TYPE_KEY || source_type == IPVF_TYPE_KEY_LZ4)
    {
        record_info->type = IPVF_TYPE_KEY;
        if (record_info->rect_count != 0 ||
            record_info->decoded_video_bytes != IPVF_FRAME_BYTES)
            return false;
    }
    else if (source_type == IPVF_TYPE_RECTS ||
             source_type == IPVF_TYPE_RECTS_LZ4)
    {
        record_info->type = IPVF_TYPE_RECTS;
        if (record_info->rect_count == 0 ||
            record_info->decoded_video_bytes == 0)
            return false;
    }
    else if (source_type == IPVF_TYPE_REPEAT)
    {
        record_info->type = IPVF_TYPE_REPEAT;
        if (record_info->rect_count != 0 ||
            record_info->stored_video_bytes != 0 ||
            record_info->decoded_video_bytes != 0 || record_info->compressed)
            return false;
    }
    else if (source_type == IPVF_TYPE_XOR_LZ4)
    {
        /* Temporal XOR reconstructs a full renderable frame, but remains
         * dependent on the immediately preceding reconstructed frame. */
        record_info->type = IPVF_TYPE_KEY;
        if (record_info->rect_count != 0 ||
            record_info->decoded_video_bytes != IPVF_FRAME_BYTES ||
            !record_info->compressed || frame == 0 ||
            !info->temporal_xor || record_info->stored_video_bytes < 5u)
            return false;
    }
    else
    {
        return false;
    }

    if (record_info->compressed)
    {
        if (record_info->stored_video_bytes == 0 ||
            record_info->stored_video_bytes >= record_info->decoded_video_bytes)
            return false;
    }
    else if (record_info->stored_video_bytes !=
             record_info->decoded_video_bytes)
    {
        return false;
    }

    record_bytes = current_sectors * IPVF_RECORD_SECTOR_SIZE;
    for (i = (size_t)used_bytes; i < record_bytes; i++)
        if (record[i] != 0)
            return false;

    audio_payload = record + IPVF_FRAME_HEADER_SIZE +
                    record_info->stored_video_bytes;
    return ipvf_ima_parse_header(audio_payload, record_info->audio_bytes,
                                 record_info->audio_frames, &ima_state);
}

static bool decode_record_video(const unsigned char *record,
                                const struct ipvf_record_info *record_info,
                                unsigned char *decoded_record,
                                unsigned char *scratch,
                                unsigned char *reference,
                                bool native_geometry,
                                struct ipvf_stats *stats)
{
    const unsigned char *source = record + IPVF_FRAME_HEADER_SIZE;
    unsigned char *destination = decoded_record + IPVF_FRAME_HEADER_SIZE;
    unsigned char *payload = destination;
    bool valid;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    uint32_t operation_started;
    uint32_t operation_us;
#endif
    uint32_t stored_bytes = record_info->stored_video_bytes;
    uint32_t expected_crc = 0;

#if !IPVF_ENABLE_QUALIFICATION_TELEMETRY
    (void)stats;
#endif

    if (record_info->compressed)
    {
        if (record_info->temporal)
        {
            expected_crc = get_le32(source);
            source += sizeof(uint32_t);
            stored_bytes -= sizeof(uint32_t);
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_started = USEC_TIMER;
#endif
            valid = ipvf_temporal_payload_crc32(source, stored_bytes) ==
                    expected_crc;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_us = USEC_TIMER - operation_started;
            if (stats != NULL)
                add_qualification_timing(
                    &stats->temporal_check_us,
                    &stats->max_temporal_check_us, operation_us);
#endif
            if (!valid)
                return false;
        }
        payload = scratch != NULL ? scratch : destination;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        operation_started = USEC_TIMER;
#endif
        valid = ipvf_lz4_decode(source, stored_bytes, payload,
                                record_info->decoded_video_bytes);
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        operation_us = USEC_TIMER - operation_started;
        if (stats != NULL)
            add_qualification_timing(&stats->lz4_decode_us,
                                     &stats->max_lz4_decode_us,
                                     operation_us);
#endif
        if (!valid)
            return false;
        if (record_info->temporal)
        {
            if (scratch == NULL || reference == NULL)
                return false;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_started = USEC_TIMER;
#endif
            ipvf_xor_frame(reference, scratch);
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_us = USEC_TIMER - operation_started;
            if (stats != NULL)
                add_qualification_timing(
                    &stats->temporal_reconstruct_us,
                    &stats->max_temporal_reconstruct_us, operation_us);
#endif
            payload = reference;
        }
    }
    else if (record_info->stored_video_bytes != 0)
    {
        rb->memcpy(destination, source, record_info->stored_video_bytes);
    }

    if (record_info->type == IPVF_TYPE_KEY)
        valid = record_info->rect_count == 0 &&
                record_info->decoded_video_bytes == IPVF_FRAME_BYTES;
    else if (record_info->type == IPVF_TYPE_REPEAT)
        valid = record_info->rect_count == 0 &&
                record_info->decoded_video_bytes == 0;
    else if (record_info->type == IPVF_TYPE_RECTS)
        valid = validate_rects(payload, record_info->decoded_video_bytes,
                               record_info->rect_count, native_geometry);
    else
        valid = false;

    if (!valid)
        return false;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    operation_started = USEC_TIMER;
#endif
    if (record_info->temporal)
        rb->memcpy(destination, reference, IPVF_FRAME_BYTES);
    else if (record_info->compressed && scratch != NULL)
        rb->memcpy(destination, scratch,
                   record_info->decoded_video_bytes);

    if (reference != NULL)
    {
        if (record_info->type == IPVF_TYPE_KEY && !record_info->temporal)
            rb->memcpy(reference, destination, IPVF_FRAME_BYTES);
        else if (record_info->type == IPVF_TYPE_RECTS)
            update_reference_rects(destination,
                                   record_info->rect_count, reference);
    }
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    operation_us = USEC_TIMER - operation_started;
    if (stats != NULL)
        add_qualification_timing(&stats->video_copy_us,
                                 &stats->max_video_copy_us, operation_us);
#endif

    return true;
}

#include "ipodnative_display.inc"
#include "ipodnative_audio.inc"
#include "ipodnative_player.inc"
