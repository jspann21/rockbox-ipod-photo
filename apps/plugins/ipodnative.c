/***************************************************************************
 * iPod Photo native video viewer (.ipvf)
 *
 * IPVF stores 220x176 RGB565SWAPPED frames as sector-aligned key, rectangle,
 * or repeat records. On PP5020 the CPU reads directly into uncached staging
 * slots while the COP sends the previous record through the target LCD driver.
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

#define IPVF_MAGIC               "IPVF"
#define IPVF_VERSION             1
#define IPVF_HEADER_SIZE         64u
#define IPVF_DATA_OFFSET         512u
#define IPVF_RECORD_SECTOR_SIZE  512u
#define IPVF_RECORD_MAX_SECTORS  256u
#define IPVF_RECORD_MAX_BYTES \
    (IPVF_RECORD_MAX_SECTORS * IPVF_RECORD_SECTOR_SIZE)
#define IPVF_FRAME_HEADER_SIZE   8u
#define IPVF_RECT_HEADER_SIZE    8u
#define IPVF_FLAG_RGB565BE       0x00000001u
#define IPVF_FLAG_SECTOR_RECORDS 0x00000002u
#define IPVF_FLAGS \
    (IPVF_FLAG_RGB565BE | IPVF_FLAG_SECTOR_RECORDS)
#define IPVF_TYPE_KEY            0u
#define IPVF_TYPE_RECTS          1u
#define IPVF_TYPE_REPEAT         2u
#define IPVF_MAX_FPS             240u
#define IPVF_FRAME_BYTES \
    ((size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data))
#define IPVF_MAX_PAYLOAD (IPVF_FRAME_BYTES + 4096u)

#if defined(IPOD_COLOR) && NUM_CORES > 1 && \
    defined(HAVE_SEMAPHORE_OBJECTS) && !defined(SIMULATOR)
#define IPVF_NATIVE_PIPELINE
#define IPVF_RENDER_SLOTS       3u
#define IPVF_RENDER_SLOT_STRIDE 0x20000u
#define IPVF_RENDER_STACK       3072u
#endif

typedef char ipvf_fb_data_must_be_16_bits[
    sizeof(fb_data) == 2 ? 1 : -1];

struct ipvf_info
{
    unsigned int fps_num;
    unsigned int fps_den;
    unsigned long frame_count;
    uint32_t first_record_sectors;
    off_t file_size;
};

struct ipvf_stats
{
    unsigned long frames;
    unsigned long late_frames;
    unsigned long max_late_us;
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
    unsigned char h[IPVF_DATA_OFFSET];
    unsigned int i;
    uint32_t flags;
    uint32_t data_offset;

    if (!read_exact(fd, h, sizeof(h)) || rb->memcmp(h, IPVF_MAGIC, 4))
        return false;

    info->fps_num = get_le16(h + 12);
    info->fps_den = get_le16(h + 14);
    info->frame_count = get_le32(h + 16);
    flags = get_le32(h + 20);
    data_offset = get_le32(h + 24);
    info->first_record_sectors = get_le16(h + 28);
    info->file_size = rb->filesize(fd);

    if (get_le16(h + 4) != IPVF_VERSION ||
        get_le16(h + 6) != IPVF_HEADER_SIZE ||
        get_le16(h + 8) != LCD_WIDTH ||
        get_le16(h + 10) != LCD_HEIGHT ||
        info->fps_num == 0 || info->fps_den == 0 ||
        info->fps_num < info->fps_den || info->frame_count == 0 ||
        info->fps_num > IPVF_MAX_FPS * info->fps_den ||
        flags != IPVF_FLAGS || data_offset != IPVF_DATA_OFFSET ||
        info->first_record_sectors == 0 ||
        info->first_record_sectors > IPVF_RECORD_MAX_SECTORS ||
        info->file_size < (off_t)IPVF_DATA_OFFSET)
        return false;

    for (i = 30; i < sizeof(h); i++)
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
                         unsigned long frame_count,
                         unsigned int *type,
                         unsigned int *rect_count,
                         uint32_t *payload_size,
                         uint32_t *next_sectors,
                         bool native_geometry)
{
    uint32_t expected_sectors;

    *type = record[0];
    *rect_count = record[1];
    *next_sectors = get_le16(record + 2);
    *payload_size = get_le32(record + 4);

    if (*payload_size > IPVF_MAX_PAYLOAD)
        return false;

    expected_sectors = (IPVF_FRAME_HEADER_SIZE + *payload_size +
                        IPVF_RECORD_SECTOR_SIZE - 1) /
                       IPVF_RECORD_SECTOR_SIZE;
    if (current_sectors == 0 ||
        current_sectors > IPVF_RECORD_MAX_SECTORS ||
        current_sectors != expected_sectors ||
        (frame == 0 && *type != IPVF_TYPE_KEY) ||
        (frame + 1 < frame_count &&
         (*next_sectors == 0 ||
          *next_sectors > IPVF_RECORD_MAX_SECTORS)) ||
        (frame + 1 == frame_count && *next_sectors != 0))
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

static bool apply_rects(const unsigned char *payload, size_t bytes,
                        unsigned int rect_count, fb_data *fb,
                        bool update_lcd)
{
    const unsigned char *p = payload;
    const unsigned char *end = payload + bytes;
    unsigned int i;

    for (i = 0; i < rect_count; i++)
    {
        unsigned int x, y, w, h, row;
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
            (size_t)(end - p) < data_bytes)
            return false;

        for (row = 0; row < h; row++)
        {
            rb->memcpy(fb + (y + row) * LCD_WIDTH + x,
                       p + (size_t)row * w * sizeof(fb_data),
                       (size_t)w * sizeof(fb_data));
        }

        if (update_lcd)
            rb->lcd_update_rect(x, y, w, h);
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

#ifdef IPVF_NATIVE_PIPELINE
struct ipvf_render_slot
{
    unsigned char *record;
    uint32_t payload_size;
    unsigned char type;
    unsigned char rect_count;
};

struct ipvf_render_control
{
    struct semaphore free_slots;
    struct semaphore ready_slots;
    volatile bool stop;
    volatile bool heartbeat;
    volatile bool exited;
    volatile bool failed;
    volatile unsigned int produce;
    volatile unsigned int consume;
    struct ipvf_render_slot slots[IPVF_RENDER_SLOTS];
};

static struct ipvf_render_control render SHAREDBSS_ATTR;
static uint32_t render_stack[
    IPVF_RENDER_STACK / sizeof(uint32_t)] CACHEALIGN_ATTR;
static unsigned int render_thread_id;
static bool render_active;

static unsigned int render_slot_index(unsigned int sequence)
{
    return sequence % IPVF_RENDER_SLOTS;
}

static bool render_apply_slot(
    const volatile struct ipvf_render_slot *slot)
{
    const unsigned char *p =
        UNCACHED_ADDR(slot->record) + IPVF_FRAME_HEADER_SIZE;
    const unsigned char *end = p + slot->payload_size;
    unsigned int type = slot->type;
    unsigned int rect_count = slot->rect_count;
    unsigned int i;

    if (type == IPVF_TYPE_KEY)
    {
        return rect_count == 0 && slot->payload_size == IPVF_FRAME_BYTES &&
               rb->lcd_update_rect_from_buffer(
                   (const fb_data *)p, LCD_WIDTH,
                   0, 0, LCD_WIDTH, LCD_HEIGHT);
    }

    if (type == IPVF_TYPE_REPEAT)
        return rect_count == 0 && slot->payload_size == 0;

    if (type != IPVF_TYPE_RECTS || rect_count == 0)
        return false;

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
            (x & 1) != 0 || (w & 1) != 0 || ((uintptr_t)p & 3) != 0 ||
            !rb->lcd_update_rect_from_buffer(
                (const fb_data *)p, w, x, y, w, h))
            return false;

        p += data_bytes;
    }

    return p == end;
}

static void render_worker(void)
{
    volatile struct ipvf_render_control *control = UNCACHED_ADDR(&render);

    control->heartbeat = true;
#ifdef HAVE_IPOD_CRASH_RECORD
    rb->crash_record_ipvf_update(1, CRASH_RECORD_IPVF_RENDERING, 0, 0);
#endif
    membarrier();

    while (true)
    {
        unsigned int consume;
        unsigned int produce;
        int status = rb->semaphore_wait(&render.ready_slots, HZ * 3);

        consume = control->consume;
        produce = control->produce;
        if (control->stop && consume == produce)
            break;
        if (status != OBJ_WAIT_SUCCEEDED)
            continue;
        if (produce <= consume || produce - consume > IPVF_RENDER_SLOTS)
        {
            control->failed = true;
            continue;
        }

        if (!control->failed &&
            !render_apply_slot(&control->slots[render_slot_index(consume)]))
            control->failed = true;

        control->consume = consume + 1;
#ifdef HAVE_IPOD_CRASH_RECORD
        rb->crash_record_ipvf_update(1, CRASH_RECORD_IPVF_RENDERING,
                                     control->consume,
                                     render_slot_index(consume));
#endif
        membarrier();
        rb->semaphore_release(&render.free_slots);
    }

    control->exited = true;
    membarrier();
    rb->thread_exit();
}

static bool render_start(unsigned char *slot_base)
{
    volatile struct ipvf_render_control *control;
    long deadline;
    unsigned int i;

    if (((uintptr_t)slot_base & (IPVF_RECORD_SECTOR_SIZE - 1)) != 0)
        return false;

#ifdef HAVE_IPOD_CRASH_RECORD
    rb->crash_record_ipvf_update(1, CRASH_RECORD_IPVF_STARTING, 0, 0);
#endif

    rb->memset(&render, 0, sizeof(render));
    rb->semaphore_init(&render.free_slots,
                       IPVF_RENDER_SLOTS, IPVF_RENDER_SLOTS);
    rb->semaphore_init(&render.ready_slots, IPVF_RENDER_SLOTS, 0);
    for (i = 0; i < IPVF_RENDER_SLOTS; i++)
        render.slots[i].record =
            slot_base + (size_t)i * IPVF_RENDER_SLOT_STRIDE;

    render_thread_id = rb->create_thread(
        render_worker, render_stack, sizeof(render_stack),
        CREATE_THREAD_FROZEN, "ipvfrender"
        IF_PRIO(, PRIORITY_PLAYBACK) IF_COP(, COP));
    if (render_thread_id == 0)
    {
#ifdef HAVE_IPOD_CRASH_RECORD
        rb->crash_record_ipvf_update(0, CRASH_RECORD_IPVF_IDLE, 0, 0);
#endif
        return false;
    }

    rb->commit_discard_dcache();
    control = UNCACHED_ADDR(&render);
    rb->thread_thaw(render_thread_id);
    deadline = *rb->current_tick + HZ;
    while (!control->heartbeat && TIME_BEFORE(*rb->current_tick, deadline))
        rb->yield();

    if (!control->heartbeat)
    {
        control->stop = true;
        membarrier();
        rb->semaphore_release(&render.ready_slots);
        deadline = *rb->current_tick + HZ * 3;
        while (!control->exited &&
               TIME_BEFORE(*rb->current_tick, deadline))
            rb->yield();
        if (!control->exited)
            rb->panicf("IPVF render start timeout");
        rb->thread_wait(render_thread_id);
        render_thread_id = 0;
#ifdef HAVE_IPOD_CRASH_RECORD
        rb->crash_record_ipvf_update(0, CRASH_RECORD_IPVF_IDLE, 0, 0);
#endif
        return false;
    }

    render_active = true;
    return true;
}

static unsigned char *render_acquire(void)
{
    volatile struct ipvf_render_control *control = UNCACHED_ADDR(&render);
    unsigned int produce;

#ifdef HAVE_IPOD_CRASH_RECORD
    rb->crash_record_ipvf_update(1, CRASH_RECORD_IPVF_WAITING_SLOT,
                                 control->produce,
                                 render_slot_index(control->produce));
#endif

    if (rb->semaphore_wait(&render.free_slots, HZ * 3) !=
        OBJ_WAIT_SUCCEEDED)
    {
        control->failed = true;
        return NULL;
    }
    if (control->failed)
    {
        rb->semaphore_release(&render.free_slots);
        return NULL;
    }

    produce = control->produce;
    if (produce < control->consume ||
        produce - control->consume >= IPVF_RENDER_SLOTS)
    {
        control->failed = true;
        rb->semaphore_release(&render.free_slots);
        return NULL;
    }

#ifdef HAVE_IPOD_CRASH_RECORD
    rb->crash_record_ipvf_update(1, CRASH_RECORD_IPVF_READING, produce,
                                 render_slot_index(produce));
#endif
    return UNCACHED_ADDR(
        control->slots[render_slot_index(produce)].record);
}

static void render_abandon(void)
{
    rb->semaphore_release(&render.free_slots);
}

static bool render_queue(unsigned int type, unsigned int rect_count,
                         uint32_t payload_size)
{
    volatile struct ipvf_render_control *control = UNCACHED_ADDR(&render);
    unsigned int produce = control->produce;
    volatile struct ipvf_render_slot *slot =
        &control->slots[render_slot_index(produce)];

    if (control->failed)
    {
        rb->semaphore_release(&render.free_slots);
        return false;
    }

    slot->type = type;
    slot->rect_count = rect_count;
    slot->payload_size = payload_size;
#ifdef HAVE_IPOD_CRASH_RECORD
    rb->crash_record_ipvf_update(1, CRASH_RECORD_IPVF_QUEUED, produce,
                                 render_slot_index(produce));
#endif
    membarrier();
    control->produce = produce + 1;
    membarrier();
    rb->semaphore_release(&render.ready_slots);
    return true;
}

static bool render_finish(void)
{
    volatile struct ipvf_render_control *control = UNCACHED_ADDR(&render);
    long deadline;
    unsigned int i;
    bool passed;

    if (!render_active)
        return false;

#ifdef HAVE_IPOD_CRASH_RECORD
    rb->crash_record_ipvf_update(1, CRASH_RECORD_IPVF_DRAINING,
                                 control->consume,
                                 render_slot_index(control->consume));
#endif

    for (i = 0; i < IPVF_RENDER_SLOTS; i++)
    {
        if (rb->semaphore_wait(&render.free_slots, HZ * 3) !=
            OBJ_WAIT_SUCCEEDED)
        {
            control->failed = true;
            break;
        }
    }

    control->stop = true;
    membarrier();
    rb->semaphore_release(&render.ready_slots);
    deadline = *rb->current_tick + HZ * 3;
    while (!control->exited && TIME_BEFORE(*rb->current_tick, deadline))
        rb->yield();
    if (!control->exited)
        rb->panicf("IPVF render exit timeout");

    rb->thread_wait(render_thread_id);
    passed = !control->failed;
    render_thread_id = 0;
    render_active = false;
#ifdef HAVE_IPOD_CRASH_RECORD
    rb->crash_record_ipvf_update(0, CRASH_RECORD_IPVF_IDLE, 0, 0);
#endif
    rb->commit_discard_dcache();
    return passed;
}

static unsigned char *render_reconcile_buffer(void)
{
    return UNCACHED_ADDR(render.slots[0].record);
}

static bool reconcile_framebuffer(int fd, const struct ipvf_info *info,
                                  off_t key_offset,
                                  uint32_t key_record_sectors,
                                  unsigned long key_frame,
                                  unsigned long displayed_frames,
                                  fb_data *fb, bool expect_eof)
{
    unsigned char *record = render_reconcile_buffer();
    uint32_t current_sectors = key_record_sectors;
    off_t position = key_offset;
    unsigned long frame;
    bool passed = key_offset >= (off_t)IPVF_DATA_OFFSET &&
                  key_frame < displayed_frames &&
                  rb->lseek(fd, key_offset, SEEK_SET) == key_offset;

    for (frame = key_frame; passed && frame < displayed_frames; frame++)
    {
        unsigned int type, rect_count;
        uint32_t payload_size, next_sectors;
        uint32_t record_bytes;

        if (current_sectors == 0 ||
            current_sectors > IPVF_RECORD_MAX_SECTORS)
        {
            passed = false;
            break;
        }
        record_bytes = current_sectors * IPVF_RECORD_SECTOR_SIZE;
        if (position > info->file_size ||
            (off_t)record_bytes > info->file_size - position ||
            rb->read(fd, record, record_bytes) != (ssize_t)record_bytes ||
            !parse_record(record, current_sectors, frame,
                          info->frame_count, &type, &rect_count,
                          &payload_size, &next_sectors, true) ||
            (frame == key_frame && type != IPVF_TYPE_KEY))
        {
            passed = false;
            break;
        }

        if (type == IPVF_TYPE_KEY)
            rb->memcpy(fb, record + IPVF_FRAME_HEADER_SIZE,
                       IPVF_FRAME_BYTES);
        else if (type == IPVF_TYPE_RECTS &&
                 !apply_rects(record + IPVF_FRAME_HEADER_SIZE,
                              payload_size, rect_count, fb, false))
        {
            passed = false;
            break;
        }

        position += record_bytes;
        current_sectors = next_sectors;
    }

    if (passed && expect_eof &&
        (current_sectors != 0 || position != info->file_size))
        passed = false;
    if (passed)
        rb->commit_dcache();
    return passed;
}
#endif

static enum plugin_status play_file(const char *filename)
{
    struct ipvf_info info;
    struct ipvf_stats stats;
    size_t buf_size;
    uintptr_t buffer_end;
    uintptr_t required_end;
    unsigned char *buf;
    unsigned char *record_buffer;
    struct viewport *vp;
    fb_data *fb;
    uint32_t period_us;
    uint32_t start_us = 0;
    uint32_t current_sectors;
    off_t file_position = IPVF_DATA_OFFSET;
    off_t last_key_offset = -1;
    uint32_t last_key_sectors = 0;
    unsigned long last_key_frame = 0;
    unsigned long frame;
    int fd;
    bool failed = false;
    bool stopped = false;
    bool usb_connected = false;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    bool boosted = false;
#endif
    int old_spindown = rb->global_settings->disk_spindown;

    rb->memset(&info, 0, sizeof(info));
    rb->memset(&stats, 0, sizeof(stats));

    fd = rb->open(filename, O_RDONLY);
    if (fd < 0 || !read_header(fd, &info))
    {
        if (fd >= 0)
            rb->close(fd);
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
    if (buf == NULL)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF buffer unavailable");
        return PLUGIN_ERROR;
    }
    buffer_end = (uintptr_t)buf + buf_size;
#ifdef IPVF_NATIVE_PIPELINE
    record_buffer = (unsigned char *)
        (((uintptr_t)buf + IPVF_RENDER_SLOT_STRIDE - 1) &
         ~(uintptr_t)(IPVF_RENDER_SLOT_STRIDE - 1));
    required_end = (uintptr_t)record_buffer +
                   IPVF_RENDER_SLOTS * IPVF_RENDER_SLOT_STRIDE;
#else
    record_buffer = (unsigned char *)
        (((uintptr_t)buf + IPVF_RECORD_SECTOR_SIZE - 1) &
         ~(uintptr_t)(IPVF_RECORD_SECTOR_SIZE - 1));
    required_end = (uintptr_t)record_buffer + IPVF_RECORD_MAX_BYTES;
#endif
    if (buffer_end < (uintptr_t)buf ||
        (uintptr_t)record_buffer < (uintptr_t)buf ||
        required_end < (uintptr_t)record_buffer ||
        required_end > buffer_end)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF buffer too small");
        return PLUGIN_ERROR;
    }

    rb->lcd_set_viewport(NULL);
    if (rb->screens[SCREEN_MAIN] == NULL ||
        rb->screens[SCREEN_MAIN]->current_viewport == NULL)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "Unsupported framebuffer");
        return PLUGIN_ERROR;
    }
    vp = *(rb->screens[SCREEN_MAIN]->current_viewport);
    if (vp == NULL || vp->buffer == NULL ||
        vp->buffer->stride != LCD_WIDTH)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "Unsupported framebuffer");
        return PLUGIN_ERROR;
    }
    fb = vp->buffer->fb_ptr;

    rb->lcd_set_backdrop(NULL);
    rb->lcd_clear_display();
    rb->lcd_update();
#ifdef IPVF_NATIVE_PIPELINE
    if (!render_start(record_buffer))
    {
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF pipeline unavailable");
        return PLUGIN_ERROR;
    }
#endif
    rb->button_clear_queue();
    backlight_ignore_timeout();
    rb->storage_spindown(0);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
    boosted = true;
#endif

    current_sectors = info.first_record_sectors;
    for (frame = 0; frame < info.frame_count; frame++)
    {
        unsigned char *record;
        unsigned int type, rect_count;
        uint32_t payload_size, next_sectors;
        uint32_t record_bytes;
        uint32_t target, now, late;
        off_t frame_offset = file_position;
        int button;

#ifdef IPVF_NATIVE_PIPELINE
        record = render_acquire();
        if (record == NULL)
        {
            failed = true;
            break;
        }
#else
        record = record_buffer;
#endif

        if (current_sectors == 0 ||
            current_sectors > IPVF_RECORD_MAX_SECTORS)
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            failed = true;
            break;
        }
        record_bytes = current_sectors * IPVF_RECORD_SECTOR_SIZE;
        if (file_position > info.file_size ||
            (off_t)record_bytes > info.file_size - file_position ||
            rb->read(fd, record, record_bytes) != (ssize_t)record_bytes ||
            !parse_record(record, current_sectors, frame,
                          info.frame_count, &type, &rect_count,
                          &payload_size, &next_sectors,
#ifdef IPVF_NATIVE_PIPELINE
                          true
#else
                          false
#endif
                          ))
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            failed = true;
            break;
        }
        file_position += record_bytes;

        if (frame == 0)
            start_us = USEC_TIMER;
        else
        {
            target = start_us + (uint32_t)((uint64_t)frame * period_us);
            now = USEC_TIMER;
            late = (int32_t)(now - target) > 0 ? now - target : 0;
            if (late > 500)
            {
                stats.late_frames++;
                if (late > stats.max_late_us)
                    stats.max_late_us = late;
            }
            else
                wait_until(target);
        }

#ifdef IPVF_NATIVE_PIPELINE
        if (!render_queue(type, rect_count, payload_size))
        {
            failed = true;
            break;
        }
        if (type == IPVF_TYPE_KEY)
        {
            last_key_offset = frame_offset;
            last_key_sectors = current_sectors;
            last_key_frame = frame;
        }
#else
        if (type == IPVF_TYPE_KEY)
        {
            rb->memcpy(fb, record + IPVF_FRAME_HEADER_SIZE,
                       IPVF_FRAME_BYTES);
            rb->lcd_update();
        }
        else if (type == IPVF_TYPE_RECTS &&
                 !apply_rects(record + IPVF_FRAME_HEADER_SIZE,
                              payload_size, rect_count, fb, true))
        {
            failed = true;
            break;
        }
#endif

        current_sectors = next_sectors;
        stats.frames++;
        rb->reset_poweroff_timer();

        button = rb->button_get(false);
        if ((button & ~(BUTTON_REL | BUTTON_REPEAT)) == BUTTON_MENU)
        {
            stopped = true;
            break;
        }
        if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
        {
            usb_connected = true;
            break;
        }
    }

    if (!stopped && !usb_connected && stats.frames == info.frame_count &&
        (current_sectors != 0 || file_position != info.file_size))
        failed = true;

#ifdef IPVF_NATIVE_PIPELINE
    if (!render_finish())
        failed = true;
    if (!usb_connected && !failed && stats.frames != 0 &&
        (stopped || stats.frames == info.frame_count))
    {
#ifdef HAVE_IPOD_CRASH_RECORD
        rb->crash_record_ipvf_update(1, CRASH_RECORD_IPVF_RECONCILING,
                                     stats.frames, 0);
#endif
        if (!reconcile_framebuffer(fd, &info, last_key_offset,
                                   last_key_sectors, last_key_frame,
                                   stats.frames, fb,
                                   stats.frames == info.frame_count))
            failed = true;
#ifdef HAVE_IPOD_CRASH_RECORD
        rb->crash_record_ipvf_update(0, CRASH_RECORD_IPVF_IDLE, 0, 0);
#endif
    }
#endif

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (boosted)
        rb->cpu_boost(false);
#endif
    rb->storage_spindown(old_spindown);
    backlight_use_settings();
    rb->close(fd);

    if (usb_connected)
        return PLUGIN_USB_CONNECTED;

    if (!failed && stats.frames == info.frame_count)
        rb->splashf(HZ * 2, "%lu frames, %lu late",
                    stats.frames, stats.late_frames);
    else if (!failed && stopped)
        rb->splashf(HZ, "Stopped at %lu/%lu",
                    stats.frames, info.frame_count);
    else
        rb->splashf(HZ * 2, "Failed at %lu/%lu",
                    stats.frames, info.frame_count);

    return (!failed &&
            (stats.frames == info.frame_count || stopped)) ?
           PLUGIN_OK : PLUGIN_ERROR;
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
