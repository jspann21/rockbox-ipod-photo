/***************************************************************************
 * PortalPlayer CPU-render overlap experiment for MPEGPlayer.
 *
 * Reference mode keeps the existing COP decode+render path. Accelerated mode
 * copies exact-screen YUV420 frames into two staging slots and lets a CPU
 * worker perform lcd_blit_yuv while the COP resumes MPEG parsing.
 *
 * This is intentionally gated behind .rockbox/mpeg16.enabled and the absence
 * of .rockbox/mpeg16.reference. Production behavior is unchanged otherwise.
 ****************************************************************************/

#include "plugin.h"
#include "mpegplayer.h"
#include "mpeg16_stats.h"

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

#define MPEG16_ENABLE_PATH    ROCKBOX_DIR "/mpeg16.enabled"
#define MPEG16_REFERENCE_PATH ROCKBOX_DIR "/mpeg16.reference"
#define MPEG16_LOG_PATH       ROCKBOX_DIR "/mpeg16.csv"

#define MPEG16_RENDER_SLOTS 2
#define MPEG16_STRIDE ((LCD_WIDTH + 15) & ~15)
#define MPEG16_Y_BYTES ((size_t)MPEG16_STRIDE * LCD_HEIGHT)
#define MPEG16_UV_BYTES ((size_t)(MPEG16_STRIDE / 2) * (LCD_HEIGHT / 2))
#define MPEG16_SLOT_BYTES (MPEG16_Y_BYTES + 2 * MPEG16_UV_BYTES)
#define MPEG16_RENDER_STACK 3072

struct mpeg16_render_control
{
    struct semaphore free_slots;
    struct semaphore ready_slots;
    volatile bool stop;
    bool test_enabled;
    bool reference;
    bool worker_active;
    bool eligible;
    bool selected;
    unsigned int thread_id;
    unsigned int produce;
    unsigned int consume;
    unsigned char *slots;
    uint32_t render_frames;
    uint32_t cpu_render_frames;
    uint32_t cop_render_us;
    uint32_t copy_us;
    uint32_t cpu_render_us;
    uint32_t wait_free_us;
    uint32_t started_us;
    uint32_t width;
    uint32_t height;
    uint32_t frame_period;
};

extern void *mpeg2_bufalloc(unsigned size, mpeg2_alloc_t reason);

static struct mpeg16_render_control mpeg16 SHAREDBSS_ATTR;
static uint32_t mpeg16_render_stack[
    MPEG16_RENDER_STACK / sizeof(uint32_t)] CACHEALIGN_ATTR;

static uint32_t mpeg16_now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (uint32_t)*rb->current_tick * (1000000u / HZ);
#endif
}

static bool mpeg16_path_exists(const char *path)
{
    int fd = rb->open(path, O_RDONLY);

    if (fd < 0)
        return false;

    rb->close(fd);
    return true;
}

static unsigned char *mpeg16_slot(unsigned int index)
{
    return mpeg16.slots +
           (index & (MPEG16_RENDER_SLOTS - 1)) * MPEG16_SLOT_BYTES;
}

static void mpeg16_slot_planes(unsigned int index, uint8_t *planes[3])
{
    unsigned char *slot = mpeg16_slot(index);

    planes[0] = slot;
    planes[1] = slot + MPEG16_Y_BYTES;
    planes[2] = planes[1] + MPEG16_UV_BYTES;
}

static void mpeg16_render_worker(void)
{
    while (true)
    {
        uint8_t *planes[3];
        uint32_t started;

        rb->semaphore_wait(&mpeg16.ready_slots, TIMEOUT_BLOCK);

        if (mpeg16.stop && mpeg16.consume == mpeg16.produce)
            break;

        rb->commit_discard_dcache();
        mpeg16_slot_planes(mpeg16.consume, planes);

        started = mpeg16_now_us();
        vo_draw_frame_legacy(planes);
        mpeg16.cpu_render_us += mpeg16_now_us() - started;
        mpeg16.cpu_render_frames++;
        mpeg16.consume++;

        membarrier();
        rb->semaphore_release(&mpeg16.free_slots);
    }

    rb->thread_exit();
}

static bool mpeg16_worker_start(void)
{
    size_t bytes = MPEG16_RENDER_SLOTS * MPEG16_SLOT_BYTES;

    /* Reserve staging inside libmpeg2's dedicated video arena. This avoids
       racing MPEGPlayer's top-level allocator with the CPU audio thread, and
       mpeg2_bufalloc keeps the slots alive across mpeg2_mem_reset(). */
    mpeg16.slots = mpeg2_bufalloc(bytes, MPEG2_ALLOC_YUV);
    if (mpeg16.slots == NULL)
        return false;

    rb->semaphore_init(&mpeg16.free_slots, MPEG16_RENDER_SLOTS,
                       MPEG16_RENDER_SLOTS);
    rb->semaphore_init(&mpeg16.ready_slots, MPEG16_RENDER_SLOTS, 0);

    mpeg16.stop = false;
    mpeg16.produce = 0;
    mpeg16.consume = 0;

    mpeg16.thread_id = rb->create_thread(
        mpeg16_render_worker,
        mpeg16_render_stack, sizeof(mpeg16_render_stack),
        CREATE_THREAD_FROZEN, "mpgrender"
        IF_PRIO(, PRIORITY_PLAYBACK) IF_COP(, CPU));

    if (mpeg16.thread_id == 0)
        return false;

    rb->commit_dcache();
    rb->thread_thaw(mpeg16.thread_id);
    mpeg16.worker_active = true;
    return true;
}

static void mpeg16_worker_finish(void)
{
    int i;

    if (!mpeg16.worker_active)
        return;

    /* Acquiring every free slot means every queued render has completed. */
    for (i = 0; i < MPEG16_RENDER_SLOTS; i++)
        rb->semaphore_wait(&mpeg16.free_slots, TIMEOUT_BLOCK);

    mpeg16.stop = true;
    membarrier();
    rb->semaphore_release(&mpeg16.ready_slots);
    rb->thread_wait(mpeg16.thread_id);
    rb->commit_discard_dcache();

    mpeg16.worker_active = false;
    mpeg16.thread_id = 0;
}

static void mpeg16_log(void)
{
    const char *mode;
    int fd;

    if (!mpeg16.test_enabled)
        return;

    fd = rb->open(MPEG16_LOG_PATH,
                  O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    if (rb->filesize(fd) == 0)
    {
        rb->fdprintf(fd,
            "mode,width,height,frame_period,async_selected,"
            "render_frames,cpu_render_frames,parse_calls,parse_us,"
            "cop_render_us,copy_us,cpu_render_us,wait_free_us,wall_us\n");
    }

    mode = mpeg16.reference ? "reference" : "accelerated";
    rb->fdprintf(fd,
        "%s,%lu,%lu,%lu,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
        mode,
        (unsigned long)mpeg16.width,
        (unsigned long)mpeg16.height,
        (unsigned long)mpeg16.frame_period,
        mpeg16.selected ? 1 : 0,
        (unsigned long)mpeg16.render_frames,
        (unsigned long)mpeg16.cpu_render_frames,
        (unsigned long)mpeg16_parse_profile.parse_calls,
        (unsigned long)mpeg16_parse_profile.parse_us,
        (unsigned long)mpeg16.cop_render_us,
        (unsigned long)mpeg16.copy_us,
        (unsigned long)mpeg16.cpu_render_us,
        (unsigned long)mpeg16.wait_free_us,
        (unsigned long)(mpeg16_now_us() - mpeg16.started_us));

    rb->close(fd);
}

static bool mpeg16_queue_frame(uint8_t * const *buf)
{
    uint8_t *dst[3];
    uint32_t wait_started;
    uint32_t copy_started;

    wait_started = mpeg16_now_us();
    rb->semaphore_wait(&mpeg16.free_slots, TIMEOUT_BLOCK);
    mpeg16.wait_free_us += mpeg16_now_us() - wait_started;

    mpeg16_slot_planes(mpeg16.produce, dst);

    /* Copy the padded libmpeg2 planes, not just 220 visible pixels. The iPod
       LCD blitter receives the decoder's 224-pixel stride and draws 220. */
    copy_started = mpeg16_now_us();
    rb->memcpy(dst[0], buf[0], MPEG16_Y_BYTES);
    rb->memcpy(dst[1], buf[1], MPEG16_UV_BYTES);
    rb->memcpy(dst[2], buf[2], MPEG16_UV_BYTES);
    rb->commit_dcache();
    mpeg16.copy_us += mpeg16_now_us() - copy_started;

    mpeg16.produce++;
    mpeg16.render_frames++;
    membarrier();
    rb->semaphore_release(&mpeg16.ready_slots);
    return true;
}

bool vo_init(void)
{
    bool result;

    rb->memset(&mpeg16, 0, sizeof(mpeg16));
    rb->memset(&mpeg16_parse_profile, 0, sizeof(mpeg16_parse_profile));

    mpeg16.test_enabled = mpeg16_path_exists(MPEG16_ENABLE_PATH);
    mpeg16.reference = mpeg16.test_enabled &&
                       mpeg16_path_exists(MPEG16_REFERENCE_PATH);
    mpeg16.started_us = mpeg16_now_us();

    result = vo_init_legacy();
    if (!result)
        return false;

    if (mpeg16.test_enabled && !mpeg16.reference)
        mpeg16_worker_start();

    return true;
}

void vo_setup(const mpeg2_sequence_t *sequence)
{
    vo_setup_legacy(sequence);

    mpeg16.width = sequence->picture_width;
    mpeg16.height = sequence->picture_height;
    mpeg16.frame_period = sequence->frame_period;

    mpeg16.eligible =
        mpeg16.worker_active &&
        sequence->picture_width == LCD_WIDTH &&
        sequence->picture_height == LCD_HEIGHT &&
        sequence->width == MPEG16_STRIDE &&
        sequence->height == LCD_HEIGHT &&
        sequence->display_width == LCD_WIDTH &&
        sequence->display_height == LCD_HEIGHT &&
        sequence->chroma_width == MPEG16_STRIDE / 2 &&
        sequence->chroma_height == LCD_HEIGHT / 2;

    if (mpeg16.eligible)
        mpeg16.selected = true;
}

void vo_draw_frame(uint8_t * const *buf)
{
    uint32_t started;

    if (buf != NULL && mpeg16.eligible)
    {
        mpeg16_queue_frame(buf);
        return;
    }

    started = mpeg16_now_us();
    vo_draw_frame_legacy(buf);
    mpeg16.cop_render_us += mpeg16_now_us() - started;
    if (buf != NULL)
        mpeg16.render_frames++;
}

void vo_cleanup(void)
{
    mpeg16_worker_finish();
    mpeg16_log();
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
