#define JPEG_DECODE_COP_ERROR   (-14)

#if defined(IPOD_COLOR) && defined(CPU_ARM) && \
    NUM_CORES > 1 && defined(HAVE_SEMAPHORE_OBJECTS)

#define JPEG_COP_JOB_SLOTS        2
#define JPEG_COP_BATCH_BLOCKS    42
#define JPEG_COP_STACK_SIZE    4096
#define JPEG_COP_MIN_PROBE_AC     4
#define JPEG_COP_WAIT_TICKS      HZ

#if (JPEG_COP_JOB_SLOTS & (JPEG_COP_JOB_SLOTS - 1)) != 0
#error JPEG_COP_JOB_SLOTS must be a power of two
#endif

struct jpeg_cop_block_job
{
    int16_t block[64];
    unsigned char *dst;
    const int *quant;
    int stride;
    bool dc_only;
};

struct jpeg_cop_job
{
    int count;
    int downscale;
    bool end_of_row;
    struct jpeg_cop_block_job block[JPEG_COP_BATCH_BLOCKS];
};

struct jpeg_cop_control
{
    struct semaphore ready;
    struct semaphore done;
    struct semaphore started;
    volatile bool stop;
    bool prepared;
    bool active;
    int downscale;
    unsigned int thread_id;
    unsigned int produce;
    unsigned int inflight;
    unsigned int fill_count;
    struct jpeg_cop_stats stats;
};

static struct jpeg_cop_control jpeg_cop SHAREDBSS_ATTR;

/*
 * Jobs are exchanged through uncached SDRAM. This avoids one whole-cache
 * commit/discard pair for every 42-block batch. The COP keeps its output
 * strip cached and commits it once at the final job of each MCU row.
 */
static struct jpeg_cop_job jpeg_cop_jobs[JPEG_COP_JOB_SLOTS]
    SHAREDBSS_ATTR CACHEALIGN_ATTR;

/*
 * Only the COP uses the worker stack. Keep it cached for the IDCT workspace.
 * The thread is created frozen, then the CPU commits the initialized stack
 * before thawing it.
 */
static uint32_t jpeg_cop_stack[JPEG_COP_STACK_SIZE / sizeof(uint32_t)]
    CACHEALIGN_ATTR;

static uint32_t jpeg_cop_now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (uint32_t)*rb->current_tick * (1000000u / HZ);
#endif
}

static int jpeg_cop_coeff_count(int downscale)
{
    switch (downscale)
    {
    case 1:
        return 64;
    case 2:
        return 28;
    case 4:
        return 10;
    default:
        return 1;
    }
}

static void jpeg_cop_run_block(struct jpeg_cop_block_job *block,
                               int downscale)
{
    int expanded[64];
    int count;
    int i;

    if (block->dc_only)
    {
        expanded[0] = block->block[0];
        idct_dc_fill(block->dst, expanded, (int *)block->quant,
                     block->stride, 8 / downscale);
        return;
    }

    count = jpeg_cop_coeff_count(downscale);
    for (i = 0; i < count; i++)
        expanded[i] = block->block[i];

    switch (downscale)
    {
    case 1:
        idct8x8_arm(block->dst, expanded, (int *)block->quant,
                    block->stride);
        break;
    case 2:
        idct4x4_arm(block->dst, expanded, (int *)block->quant,
                    block->stride);
        break;
    case 4:
        idct2x2_arm(block->dst, expanded, (int *)block->quant,
                    block->stride);
        break;
    default:
        idct1x1(block->dst, expanded, (int *)block->quant,
                block->stride);
        break;
    }
}

static void jpeg_cop_worker(void)
{
    unsigned int consume = 0;

    rb->semaphore_release(&jpeg_cop.started);

    while (true)
    {
        struct jpeg_cop_job *job;
        int i;

        rb->semaphore_wait(&jpeg_cop.ready, TIMEOUT_BLOCK);

        if (jpeg_cop.stop)
            break;

        job = &jpeg_cop_jobs[consume & (JPEG_COP_JOB_SLOTS - 1)];
        for (i = 0; i < job->count; i++)
            jpeg_cop_run_block(&job->block[i], job->downscale);

        /*
         * Both 42-block jobs of the exact-screen row fit in the COP data
         * cache together with the 5,376-byte output strip. Publish output
         * only after the second job, cutting output cache commits in half.
         */
        if (job->end_of_row)
        {
            rb->commit_dcache();
            jpeg_cop.stats.row_commits++;
        }

        membarrier();
        consume++;
        rb->semaphore_release(&jpeg_cop.done);
    }

    rb->thread_exit();
}

static bool jpeg_cop_wait_one(void)
{
    uint32_t started = jpeg_cop_now_us();

    if (rb->semaphore_wait(&jpeg_cop.done, JPEG_COP_WAIT_TICKS) !=
        OBJ_WAIT_SUCCEEDED)
    {
        jpeg_cop.stats.failed = true;
        return false;
    }

    jpeg_cop.stats.wait_us += jpeg_cop_now_us() - started;
    jpeg_cop.inflight--;
    return true;
}

static bool jpeg_cop_publish(bool end_of_row)
{
    struct jpeg_cop_job *job;

    if (jpeg_cop.fill_count == 0)
        return true;

    job = &jpeg_cop_jobs[
        jpeg_cop.produce & (JPEG_COP_JOB_SLOTS - 1)];
    job->count = jpeg_cop.fill_count;
    job->downscale = jpeg_cop.downscale;
    job->end_of_row = end_of_row;

    membarrier();
    rb->semaphore_release(&jpeg_cop.ready);

    jpeg_cop.produce++;
    jpeg_cop.inflight++;
    jpeg_cop.fill_count = 0;
    jpeg_cop.stats.jobs++;
    return true;
}

static bool jpeg_cop_submit(unsigned char *dst, int *block,
                            const int *quant, int stride, bool dc_only,
                            bool end_of_row)
{
    struct jpeg_cop_job *job;
    struct jpeg_cop_block_job *entry;
    int count;
    int i;

    if (!jpeg_cop.active || jpeg_cop.stats.failed)
        return false;

    if (jpeg_cop.fill_count == 0 &&
        jpeg_cop.inflight == JPEG_COP_JOB_SLOTS &&
        !jpeg_cop_wait_one())
        return false;

    job = &jpeg_cop_jobs[
        jpeg_cop.produce & (JPEG_COP_JOB_SLOTS - 1)];
    entry = &job->block[jpeg_cop.fill_count];

    count = jpeg_cop_coeff_count(jpeg_cop.downscale);
    rb->memset(entry->block, 0, sizeof(entry->block));
    for (i = 0; i < count; i++)
    {
        int value = block[i];

        if (value < -32768 || value > 32767)
        {
            jpeg_cop.stats.failed = true;
            return false;
        }

        entry->block[i] = (int16_t)value;
    }

    entry->dst = dst;
    entry->quant = quant;
    entry->stride = stride;
    entry->dc_only = dc_only;

    jpeg_cop.fill_count++;
    jpeg_cop.stats.blocks++;
    if (dc_only)
        jpeg_cop.stats.dc_blocks++;

    if (jpeg_cop.fill_count == JPEG_COP_BATCH_BLOCKS || end_of_row)
        return jpeg_cop_publish(end_of_row);

    return true;
}

static bool jpeg_cop_drain(void)
{
    if (!jpeg_cop.active)
        return true;

    if (!jpeg_cop_publish(true))
        return false;

    while (jpeg_cop.inflight > 0)
        if (!jpeg_cop_wait_one())
            return false;

    /*
     * The COP committed the completed row once. Discard the CPU's stale
     * reusable-strip lines before conversion/LCD output or strip reuse.
     */
    rb->commit_discard_dcache();
    return true;
}

static bool jpeg_cop_start_worker(void)
{
    rb->memset(jpeg_cop_jobs, 0, sizeof(jpeg_cop_jobs));
    rb->semaphore_init(&jpeg_cop.ready, JPEG_COP_JOB_SLOTS, 0);
    rb->semaphore_init(&jpeg_cop.done, JPEG_COP_JOB_SLOTS, 0);
    rb->semaphore_init(&jpeg_cop.started, 1, 0);

    jpeg_cop.stop = false;
    jpeg_cop.produce = 0;
    jpeg_cop.inflight = 0;
    jpeg_cop.fill_count = 0;

    jpeg_cop.thread_id = rb->create_thread(
        jpeg_cop_worker, jpeg_cop_stack, sizeof(jpeg_cop_stack),
        CREATE_THREAD_FROZEN, "jpeg-cop"
        IF_PRIO(, PRIORITY_PLAYBACK) IF_COP(, COP));

    if (jpeg_cop.thread_id == 0)
    {
        jpeg_cop.stats.start_failed = true;
        return false;
    }

    /*
     * create_thread() initialized a cached SDRAM stack on the CPU. Make it
     * visible before the COP is allowed to execute from it.
     */
    rb->commit_dcache();
    rb->thread_thaw(jpeg_cop.thread_id);

    if (rb->semaphore_wait(&jpeg_cop.started, JPEG_COP_WAIT_TICKS) !=
        OBJ_WAIT_SUCCEEDED)
    {
        jpeg_cop.stats.start_failed = true;
        jpeg_cop.stop = true;
        membarrier();
        rb->semaphore_release(&jpeg_cop.ready);
        rb->thread_wait(jpeg_cop.thread_id);
        jpeg_cop.thread_id = 0;
        return false;
    }

    jpeg_cop.active = true;
    jpeg_cop.stats.selected = true;
    return true;
}

static bool jpeg_cop_row_active(void)
{
    return jpeg_cop.active && !jpeg_cop.stats.failed;
}

static void jpeg_cop_consider_probe(int ac_blocks, int total_blocks)
{
    jpeg_cop.stats.probe_ac_blocks = ac_blocks;
    jpeg_cop.stats.probe_blocks = total_blocks;

    if (!jpeg_cop.prepared || jpeg_cop.active ||
        ac_blocks < JPEG_COP_MIN_PROBE_AC ||
        total_blocks > JPEG_COP_JOB_SLOTS * JPEG_COP_BATCH_BLOCKS)
        return;

    jpeg_cop_start_worker();
}

void jpeg_decode_cop_prepare(bool enable, int downscale)
{
    rb->memset(&jpeg_cop, 0, sizeof(jpeg_cop));
    jpeg_cop.prepared = enable && downscale >= 1 && downscale <= 4;
    jpeg_cop.downscale = downscale;
}

void jpeg_decode_cop_finish(struct jpeg_cop_stats *stats)
{
    if (jpeg_cop.active)
    {
        if (!jpeg_cop.stats.failed && !jpeg_cop_drain())
            jpeg_cop.stats.failed = true;

        jpeg_cop.stop = true;
        membarrier();
        rb->semaphore_release(&jpeg_cop.ready);
        rb->thread_wait(jpeg_cop.thread_id);
        rb->commit_discard_dcache();
        jpeg_cop.active = false;
        jpeg_cop.thread_id = 0;
    }

    if (stats != NULL)
        *stats = jpeg_cop.stats;
}

#else

static bool jpeg_cop_row_active(void)
{
    return false;
}

static bool jpeg_cop_submit(unsigned char *dst, int *block,
                            const int *quant, int stride, bool dc_only,
                            bool end_of_row)
{
    (void)dst;
    (void)block;
    (void)quant;
    (void)stride;
    (void)dc_only;
    (void)end_of_row;
    return false;
}

static bool jpeg_cop_drain(void)
{
    return true;
}

static void jpeg_cop_consider_probe(int ac_blocks, int total_blocks)
{
    (void)ac_blocks;
    (void)total_blocks;
}

void jpeg_decode_cop_prepare(bool enable, int downscale)
{
    (void)enable;
    (void)downscale;
}

void jpeg_decode_cop_finish(struct jpeg_cop_stats *stats)
{
    if (stats != NULL)
        rb->memset(stats, 0, sizeof(*stats));
}

#endif
