/***************************************************************************
 * PP5020 low-overhead performance counters.
 *
 * Counters live in uncached RAM and are updated per core. This avoids cache
 * maintenance and inter-core locks in the DMA and interrupt paths. A snapshot
 * can be very slightly stale if the other core updates while it is copied.
 ****************************************************************************/

#include "config.h"
#include "system.h"
#include "pp5020-perf.h"
#include <limits.h>

static struct pp5020_perf_stats perf_core[NUM_CORES] NOCACHEBSS_ATTR;
static struct pp5020_perf_stats perf_device NOCACHEBSS_ATTR;

static inline struct pp5020_perf_stats *local_stats(void)
{
    return &perf_core[CURRENT_CORE];
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

void pp5020_perf_get(struct pp5020_perf_stats *stats)
{
    int core;
    int oldlevel = disable_interrupt_save(IRQ_FIQ_STATUS);

    *stats = perf_device;
    for (core = 0; core < NUM_CORES; core++)
    {
        const struct pp5020_perf_stats *s = &perf_core[core];
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
#undef ADD_FIELD
        if (s->cache_clean_max_us > stats->cache_clean_max_us)
            stats->cache_clean_max_us = s->cache_clean_max_us;
        if (s->cache_discard_max_us > stats->cache_discard_max_us)
            stats->cache_discard_max_us = s->cache_discard_max_us;
        if (s->dma_max_us > stats->dma_max_us)
            stats->dma_max_us = s->dma_max_us;
    }

    restore_interrupt(oldlevel);
}

void pp5020_perf_set_ata_info(const uint16_t *identify, bool is_ssd,
                              int dma_mode, bool lba48,
                              bool flush_supported, bool sleep_supported)
{
    int i;
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
}

void pp5020_perf_record_cache_clean(uint32_t elapsed_us)
{
    struct pp5020_perf_stats *s = local_stats();
    record_timing(&s->cache_clean_calls, &s->cache_clean_total_us,
                  &s->cache_clean_max_us, elapsed_us);
}

void pp5020_perf_record_cache_discard(uint32_t elapsed_us)
{
    struct pp5020_perf_stats *s = local_stats();
    record_timing(&s->cache_discard_calls, &s->cache_discard_total_us,
                  &s->cache_discard_max_us, elapsed_us);
}

void pp5020_perf_record_dma_request(uint32_t bytes)
{
    struct pp5020_perf_stats *s = local_stats();
    inc_u64_sat(&s->dma_requests);
    add_u64_sat(&s->dma_bytes, bytes);
}

void pp5020_perf_record_dma_complete(uint32_t elapsed_us,
                                     uint32_t busy_poll_us, bool success)
{
    struct pp5020_perf_stats *s = local_stats();
    add_u64_sat(&s->dma_total_us, elapsed_us);
    add_u64_sat(&s->dma_busy_poll_us, busy_poll_us);
    if (elapsed_us > s->dma_max_us)
        s->dma_max_us = elapsed_us;
    if (!success)
        inc_u64_sat(&s->dma_timeouts);
}

#define DEFINE_COUNTER(name, field) \
    void name(void) { inc_u64_sat(&local_stats()->field); }

DEFINE_COUNTER(pp5020_perf_record_pio_fallback, pio_fallbacks)
DEFINE_COUNTER(pp5020_perf_record_dma_missing_irq, dma_missing_irqs)
DEFINE_COUNTER(pp5020_perf_record_dma_late_irq, dma_late_irqs)
DEFINE_COUNTER(pp5020_perf_record_dma_spurious_irq, dma_spurious_irqs)

void pp5020_perf_record_storage_wakeup(bool deadline)
{
    struct pp5020_perf_stats *s = local_stats();
    inc_u64_sat(deadline ? &s->storage_deadline_wakeups
                         : &s->storage_event_wakeups);
}
