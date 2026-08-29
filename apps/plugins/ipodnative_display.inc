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
        return false;

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
        return false;
    }

    render_active = true;
    return true;
}

static unsigned char *render_acquire(void)
{
    volatile struct ipvf_render_control *control = UNCACHED_ADDR(&render);
    unsigned int produce;

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
    membarrier();
    control->produce = produce + 1;
    membarrier();
    rb->semaphore_release(&render.ready_slots);
    return true;
}

static bool render_wait_presented(unsigned int count)
{
    volatile struct ipvf_render_control *control = UNCACHED_ADDR(&render);
    long deadline = *rb->current_tick + HZ * 3;

    while (control->consume < count && !control->failed &&
           TIME_BEFORE(*rb->current_tick, deadline))
        rb->yield();
    return control->consume >= count && !control->failed;
}

static bool render_finish(void)
{
    volatile struct ipvf_render_control *control = UNCACHED_ADDR(&render);
    long deadline;
    unsigned int i;
    bool passed;

    if (!render_active)
        return false;

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
        uint32_t payload_size, audio_frames, next_sectors;
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
            !parse_record(record, current_sectors, frame, info,
                          &type, &rect_count, &payload_size,
                          &audio_frames, &next_sectors, true) ||
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

