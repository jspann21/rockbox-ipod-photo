/***************************************************************************
 * PP5020 low-overhead performance counters.
 *
 * Counters live in uncached RAM and are updated per core. This avoids cache
 * maintenance in the DMA and interrupt paths. The platform corelock keeps
 * debug snapshots from accepting torn 64-bit values.
 ****************************************************************************/

#include "config.h"
#include "system.h"
#include "corelock.h"
#include "pp5020-perf.h"
#include <limits.h>
#include <string.h>

static struct pp5020_perf_stats perf_core[NUM_CORES] NOCACHEBSS_ATTR;
static struct pp5020_perf_stats perf_device NOCACHEBSS_ATTR;
/* A forced single-core debug build needs only the local interrupt mask. */
#ifdef HAVE_CORELOCK_OBJECT
static struct corelock perf_lock SHAREDBSS_ATTR;
#endif

static inline struct pp5020_perf_stats *local_stats(void)
{
    return &perf_core[CURRENT_CORE];
}

static inline int begin_update(void)
{
    int oldlevel = disable_interrupt_save(IRQ_FIQ_STATUS);
    membarrier();
#ifdef HAVE_CORELOCK_OBJECT
    corelock_lock(&perf_lock);
    membarrier();
#endif
    return oldlevel;
}

static inline void end_update(int oldlevel)
{
    membarrier();
#ifdef HAVE_CORELOCK_OBJECT
    corelock_unlock(&perf_lock);
    membarrier();
#endif
    restore_interrupt(oldlevel);
}

static void snapshot_core(struct pp5020_perf_stats *stats, int core)
{
    int oldlevel = disable_interrupt_save(IRQ_FIQ_STATUS);
    membarrier();
#ifdef HAVE_CORELOCK_OBJECT
    corelock_lock(&perf_lock);
    membarrier();
#endif
    *stats = perf_core[core];
    membarrier();
#ifdef HAVE_CORELOCK_OBJECT
    corelock_unlock(&perf_lock);
    membarrier();
#endif
    restore_interrupt(oldlevel);
}

static inline void add_u64_sat(uint64_t *value, uint64_t add)
{
    if (UINT64_MAX - *value < add)
        *value = UINT64_MAX;
    else
        *value += add;
}

static inline void inc_u64_sat(uint64_t *value)
{
    add_u64_sat(value, 1);
}

static inline void record_timing(uint64_t *calls, uint64_t *total,
                                 uint32_t *maximum, uint32_t elapsed)
{
    inc_u64_sat(calls);
    add_u64_sat(total, elapsed);
    if (elapsed > *maximum)
        *maximum = elapsed;
}

void pp5020_perf_init(void)
{
#ifdef HAVE_CORELOCK_OBJECT
    corelock_init(&perf_lock);
#endif
}

void pp5020_perf_reset(void)
{
    int oldlevel = begin_update();
    memset(perf_core, 0, sizeof(perf_core));
    end_update(oldlevel);
}

void pp5020_perf_get(struct pp5020_perf_stats *stats)
{
    int core;
    struct pp5020_perf_stats snapshot;
    int oldlevel = begin_update();

    *stats = perf_device;
    end_update(oldlevel);
    for (core = 0; core < NUM_CORES; core++)
    {
        snapshot_core(&snapshot, core);
        const struct pp5020_perf_stats *s = &snapshot;
#define ADD_FIELD(name) add_u64_sat(&stats->name, s->name)
        ADD_FIELD(cache_clean_calls);
        ADD_FIELD(cache_clean_total_us);
        ADD_FIELD(cache_discard_calls);
        ADD_FIELD(cache_discard_total_us);
        ADD_FIELD(dma_requests);
        ADD_FIELD(dma_bytes);
        ADD_FIELD(dma_total_us);
        ADD_FIELD(dma_busy_poll_us);
        ADD_FIELD(dma_timeouts);
        ADD_FIELD(pio_fallbacks);
        ADD_FIELD(dma_missing_irqs);
        ADD_FIELD(dma_late_irqs);
        ADD_FIELD(dma_spurious_irqs);
        ADD_FIELD(storage_event_wakeups);
        ADD_FIELD(storage_deadline_wakeups);
        ADD_FIELD(pcm_track_changes);
        ADD_FIELD(pcm_notify_total_us);
        ADD_FIELD(pcm_underruns);
        ADD_FIELD(pcm_deferred_notifications);
        ADD_FIELD(pcm_duplicate_notifications);
        ADD_FIELD(pcm_missed_transitions);
        ADD_FIELD(lcd_updates);
        ADD_FIELD(lcd_requested_pixels);
        ADD_FIELD(lcd_transmitted_pixels);
        ADD_FIELD(lcd_total_us);
        ADD_FIELD(lcd_busy_timeouts);
        ADD_FIELD(lcd_block_timeouts);
        ADD_FIELD(lcd_txok_timeouts);
        ADD_FIELD(lcd_fifo1_timeouts);
        ADD_FIELD(lcd_fifo2_timeouts);
        ADD_FIELD(lcd_abandoned_rectangles);
        ADD_FIELD(lcd_reinitializations);
#undef ADD_FIELD
        if (s->cache_clean_max_us > stats->cache_clean_max_us)
            stats->cache_clean_max_us = s->cache_clean_max_us;
        if (s->cache_discard_max_us > stats->cache_discard_max_us)
            stats->cache_discard_max_us = s->cache_discard_max_us;
        if (s->dma_max_us > stats->dma_max_us)
            stats->dma_max_us = s->dma_max_us;
        if (s->pcm_notify_max_us > stats->pcm_notify_max_us)
            stats->pcm_notify_max_us = s->pcm_notify_max_us;
        if (s->lcd_max_us > stats->lcd_max_us)
            stats->lcd_max_us = s->lcd_max_us;
        if (s->lcd_failure_streak > stats->lcd_failure_streak)
            stats->lcd_failure_streak = s->lcd_failure_streak;
        if (s->lcd_max_failure_streak > stats->lcd_max_failure_streak)
            stats->lcd_max_failure_streak = s->lcd_max_failure_streak;
    }
}

void pp5020_perf_set_ata_info(const uint16_t *identify, bool is_ssd,
                              int dma_mode, bool lba48,
                              bool flush_supported, bool sleep_supported)
{
    int i;
    int oldlevel = begin_update();
    char *model = perf_device.ata_model;

    for (i = 0; i < 20; i++)
    {
        model[i * 2] = identify[27 + i] >> 8;
        model[i * 2 + 1] = identify[27 + i] & 0xff;
    }
    model[40] = '\0';
    for (i = 39; i >= 0 && model[i] == ' '; i--)
        model[i] = '\0';

    perf_device.ata_is_ssd = is_ssd;
    perf_device.ata_dma_mode = dma_mode;
    perf_device.lba48 = lba48;
    perf_device.flush_supported = flush_supported;
    perf_device.sleep_supported = sleep_supported;
    end_update(oldlevel);
}

void pp5020_perf_record_cache_clean(uint32_t elapsed_us)
{
    int oldlevel = begin_update();
    struct pp5020_perf_stats *s = local_stats();
    record_timing(&s->cache_clean_calls, &s->cache_clean_total_us,
                  &s->cache_clean_max_us, elapsed_us);
    end_update(oldlevel);
}

void pp5020_perf_record_cache_discard(uint32_t elapsed_us)
{
    int oldlevel = begin_update();
    struct pp5020_perf_stats *s = local_stats();
    record_timing(&s->cache_discard_calls, &s->cache_discard_total_us,
                  &s->cache_discard_max_us, elapsed_us);
    end_update(oldlevel);
}

void pp5020_perf_record_dma_request(uint32_t bytes)
{
    int oldlevel = begin_update();
    struct pp5020_perf_stats *s = local_stats();
    inc_u64_sat(&s->dma_requests);
    add_u64_sat(&s->dma_bytes, bytes);
    end_update(oldlevel);
}

void pp5020_perf_record_dma_complete(uint32_t elapsed_us,
                                     uint32_t busy_poll_us, bool success)
{
    int oldlevel = begin_update();
    struct pp5020_perf_stats *s = local_stats();
    add_u64_sat(&s->dma_total_us, elapsed_us);
    add_u64_sat(&s->dma_busy_poll_us, busy_poll_us);
    if (elapsed_us > s->dma_max_us)
        s->dma_max_us = elapsed_us;
    if (!success)
        inc_u64_sat(&s->dma_timeouts);
    end_update(oldlevel);
}

#define DEFINE_COUNTER(name, field) \
    void name(void) { \
        int oldlevel = begin_update(); \
        inc_u64_sat(&local_stats()->field); \
        end_update(oldlevel); \
    }

DEFINE_COUNTER(pp5020_perf_record_pio_fallback, pio_fallbacks)
DEFINE_COUNTER(pp5020_perf_record_dma_missing_irq, dma_missing_irqs)
DEFINE_COUNTER(pp5020_perf_record_dma_late_irq, dma_late_irqs)
DEFINE_COUNTER(pp5020_perf_record_dma_spurious_irq, dma_spurious_irqs)
DEFINE_COUNTER(pp5020_perf_record_pcm_underrun, pcm_underruns)
DEFINE_COUNTER(pp5020_perf_record_pcm_deferred, pcm_deferred_notifications)
DEFINE_COUNTER(pp5020_perf_record_pcm_duplicate, pcm_duplicate_notifications)
DEFINE_COUNTER(pp5020_perf_record_pcm_missed, pcm_missed_transitions)

void pp5020_perf_record_lcd_update(uint32_t requested_pixels,
                                   uint32_t transmitted_pixels,
                                   uint32_t elapsed_us, bool success)
{
    int oldlevel = begin_update();
    struct pp5020_perf_stats *s = local_stats();
    inc_u64_sat(&s->lcd_updates);
    add_u64_sat(&s->lcd_requested_pixels, requested_pixels);
    add_u64_sat(&s->lcd_transmitted_pixels, transmitted_pixels);
    add_u64_sat(&s->lcd_total_us, elapsed_us);
    if (elapsed_us > s->lcd_max_us)
        s->lcd_max_us = elapsed_us;
    if (success)
        s->lcd_failure_streak = 0;
    else
    {
        inc_u64_sat(&s->lcd_abandoned_rectangles);
        if (s->lcd_failure_streak != UINT32_MAX)
            s->lcd_failure_streak++;
        if (s->lcd_failure_streak > s->lcd_max_failure_streak)
            s->lcd_max_failure_streak = s->lcd_failure_streak;
    }
    end_update(oldlevel);
}

void pp5020_perf_record_lcd_timeout(int phase)
{
    int oldlevel = begin_update();
    struct pp5020_perf_stats *s = local_stats();
    uint64_t *counter = NULL;

    switch (phase)
    {
        case 1: counter = &s->lcd_busy_timeouts; break;
        case 2: counter = &s->lcd_block_timeouts; break;
        case 3: counter = &s->lcd_txok_timeouts; break;
        case 4: counter = &s->lcd_fifo1_timeouts; break;
        case 5: counter = &s->lcd_fifo2_timeouts; break;
    }
    if (counter != NULL)
        inc_u64_sat(counter);
    end_update(oldlevel);
}

void pp5020_perf_record_lcd_reinitialization(void)
{
    int oldlevel = begin_update();
    inc_u64_sat(&local_stats()->lcd_reinitializations);
    end_update(oldlevel);
}

void pp5020_perf_record_storage_wakeup(bool deadline)
{
    int oldlevel = begin_update();
    struct pp5020_perf_stats *s = local_stats();
    inc_u64_sat(deadline ? &s->storage_deadline_wakeups
                         : &s->storage_event_wakeups);
    end_update(oldlevel);
}

void pp5020_perf_record_pcm_notify(uint32_t latency_us)
{
    int oldlevel = begin_update();
    struct pp5020_perf_stats *s = local_stats();
    record_timing(&s->pcm_track_changes, &s->pcm_notify_total_us,
                  &s->pcm_notify_max_us, latency_us);
    end_update(oldlevel);
}
