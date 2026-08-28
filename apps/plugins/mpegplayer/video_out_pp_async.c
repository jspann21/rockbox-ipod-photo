/***************************************************************************
 * PortalPlayer CPU-render overlap for MPEGPlayer.
 *
 * The PortalPlayer video thread already runs libmpeg2 on the COP.  For the
 * hardware-validated iPod Color 220x176 YUV420 path, hand completed frames to
 * a CPU worker so LCD conversion/transfer overlaps decoding of the next frame.
 *
 * Other dimensions, chroma layouts, targets, and worker-allocation failures
 * retain the original synchronous renderer.
 ****************************************************************************/

#include "plugin.h"
#include "mpegplayer.h"

#define vo_draw_frame vo_draw_frame_legacy
#define vo_init       vo_init_legacy
#define vo_setup      vo_setup_legacy
#define vo_cleanup    vo_cleanup_legacy
#include "video_out_rockbox.c"
#undef vo_draw_frame
#undef vo_init
#undef vo_setup
#undef vo_cleanup

#if defined(IPOD_COLOR) && NUM_CORES > 1 && defined(HAVE_SEMAPHORE_OBJECTS)

#define PP_RENDER_SLOTS 2
#define PP_RENDER_STRIDE ((LCD_WIDTH + 15) & ~15)
#define PP_RENDER_Y_BYTES ((size_t)PP_RENDER_STRIDE * LCD_HEIGHT)
#define PP_RENDER_UV_BYTES \
    ((size_t)(PP_RENDER_STRIDE / 2) * (LCD_HEIGHT / 2))
#define PP_RENDER_SLOT_BYTES \
    (PP_RENDER_Y_BYTES + 2 * PP_RENDER_UV_BYTES)
#define PP_RENDER_STACK 3072

struct pp_render_control
{
    struct semaphore free_slots;
    struct semaphore ready_slots;
    volatile bool stop;
    bool worker_active;
    bool eligible;
    unsigned int thread_id;
    unsigned int produce;
    unsigned int consume;
    unsigned char *slots;
};

static struct pp_render_control pp_render SHAREDBSS_ATTR;
static uint32_t pp_render_stack[
    PP_RENDER_STACK / sizeof(uint32_t)] CACHEALIGN_ATTR;

static unsigned char *pp_render_slot(unsigned int index)
{
    return pp_render.slots +
           (index & (PP_RENDER_SLOTS - 1)) * PP_RENDER_SLOT_BYTES;
}

static void pp_render_slot_planes(unsigned int index, uint8_t *planes[3])
{
    unsigned char *slot = pp_render_slot(index);

    planes[0] = slot;
    planes[1] = slot + PP_RENDER_Y_BYTES;
    planes[2] = planes[1] + PP_RENDER_UV_BYTES;
}

static void pp_render_worker(void)
{
    while (true)
    {
        uint8_t *planes[3];

        rb->semaphore_wait(&pp_render.ready_slots, TIMEOUT_BLOCK);

        if (pp_render.stop && pp_render.consume == pp_render.produce)
            break;

        /* The COP committed the completed staging slot before publishing it. */
        rb->commit_discard_dcache();
        pp_render_slot_planes(pp_render.consume, planes);

        vo_draw_frame_legacy(planes);
        pp_render.consume++;

        membarrier();
        rb->semaphore_release(&pp_render.free_slots);
    }

    rb->thread_exit();
}

static bool pp_render_worker_start(void)
{
    size_t bytes = PP_RENDER_SLOTS * PP_RENDER_SLOT_BYTES;

    /* Keep staging in libmpeg2's dedicated video arena.  bufalloc advances
       the reset boundary, so these slots survive mpeg2_mem_reset() and cannot
       collide with CPU-side MPEG audio/disk allocations. */
    pp_render.slots = mpeg2_bufalloc(bytes, MPEG2_ALLOC_YUV);
    if (pp_render.slots == NULL)
        return false;

    rb->semaphore_init(&pp_render.free_slots,
                       PP_RENDER_SLOTS, PP_RENDER_SLOTS);
    rb->semaphore_init(&pp_render.ready_slots,
                       PP_RENDER_SLOTS, 0);

    pp_render.stop = false;
    pp_render.produce = 0;
    pp_render.consume = 0;

    pp_render.thread_id = rb->create_thread(
        pp_render_worker,
        pp_render_stack, sizeof(pp_render_stack),
        CREATE_THREAD_FROZEN, "mpgrender"
        IF_PRIO(, PRIORITY_PLAYBACK) IF_COP(, CPU));

    if (pp_render.thread_id == 0)
        return false;

    /* Publish control state and the cached CPU stack before the CPU starts. */
    rb->commit_dcache();
    rb->thread_thaw(pp_render.thread_id);
    pp_render.worker_active = true;
    return true;
}

static void pp_render_drain(void)
{
    int i;

    if (!pp_render.worker_active)
        return;

    /* Owning every free slot proves that every published frame has finished. */
    for (i = 0; i < PP_RENDER_SLOTS; i++)
        rb->semaphore_wait(&pp_render.free_slots, TIMEOUT_BLOCK);

    for (i = 0; i < PP_RENDER_SLOTS; i++)
        rb->semaphore_release(&pp_render.free_slots);
}

static void pp_render_worker_finish(void)
{
    if (!pp_render.worker_active)
        return;

    pp_render_drain();

    pp_render.stop = true;
    membarrier();
    rb->semaphore_release(&pp_render.ready_slots);
    rb->thread_wait(pp_render.thread_id);
    rb->commit_discard_dcache();

    pp_render.worker_active = false;
    pp_render.thread_id = 0;
    pp_render.eligible = false;
}

static bool pp_render_sequence_eligible(const mpeg2_sequence_t *sequence)
{
    return sequence->picture_width == LCD_WIDTH &&
           sequence->picture_height == LCD_HEIGHT &&
           sequence->width == PP_RENDER_STRIDE &&
           sequence->height == LCD_HEIGHT &&
           sequence->display_width == LCD_WIDTH &&
           sequence->display_height == LCD_HEIGHT &&
           sequence->chroma_width == PP_RENDER_STRIDE / 2 &&
           sequence->chroma_height == LCD_HEIGHT / 2;
}

static void pp_render_queue_frame(uint8_t * const *buf)
{
    uint8_t *dst[3];

    rb->semaphore_wait(&pp_render.free_slots, TIMEOUT_BLOCK);
    pp_render_slot_planes(pp_render.produce, dst);

    /* libmpeg2 pads a 220-pixel picture to a 224-pixel decoder stride.
       Preserve the full padded planes so the target YUV blitter receives the
       exact layout it expects while drawing only the 220 visible pixels. */
    rb->memcpy(dst[0], buf[0], PP_RENDER_Y_BYTES);
    rb->memcpy(dst[1], buf[1], PP_RENDER_UV_BYTES);
    rb->memcpy(dst[2], buf[2], PP_RENDER_UV_BYTES);

    /* Publish the complete slot to the CPU before waking it. */
    rb->commit_dcache();

    pp_render.produce++;
    membarrier();
    rb->semaphore_release(&pp_render.ready_slots);
}

bool vo_init(void)
{
    rb->memset(&pp_render, 0, sizeof(pp_render));
    return vo_init_legacy();
}

void vo_setup(const mpeg2_sequence_t *sequence)
{
    bool eligible;

    /* Sequence changes can mutate state read by the CPU renderer.  Drain any
       outstanding frame first so no worker observes a half-updated vo state. */
    pp_render_drain();
    vo_setup_legacy(sequence);

    eligible = pp_render_sequence_eligible(sequence);
    if (eligible && !pp_render.worker_active)
        eligible = pp_render_worker_start();

    pp_render.eligible = eligible && pp_render.worker_active;
}

void vo_draw_frame(uint8_t * const *buf)
{
    if (buf != NULL && pp_render.eligible && vo_is_visible())
    {
        pp_render_queue_frame(buf);
        return;
    }

    vo_draw_frame_legacy(buf);
}

void vo_cleanup(void)
{
    pp_render_worker_finish();
    vo_cleanup_legacy();
}

#else

bool vo_init(void)
{
    return vo_init_legacy();
}

void vo_setup(const mpeg2_sequence_t *sequence)
{
    vo_setup_legacy(sequence);
}

void vo_draw_frame(uint8_t * const *buf)
{
    vo_draw_frame_legacy(buf);
}

void vo_cleanup(void)
{
    vo_cleanup_legacy();
}

#endif
