/***************************************************************************
 * PP5020 low-overhead performance counters.
 ****************************************************************************/

#ifndef PP5020_PERF_H
#define PP5020_PERF_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef HAVE_PP5020_PERF

struct pp5020_perf_stats
{
    char ata_model[41];
    bool ata_is_ssd;
    int ata_dma_mode;
    bool lba48;
    bool flush_supported;
    bool sleep_supported;

    uint64_t cache_clean_calls;
    uint64_t cache_clean_total_us;
    uint32_t cache_clean_max_us;
    uint64_t cache_discard_calls;
    uint64_t cache_discard_total_us;
    uint32_t cache_discard_max_us;

    uint64_t dma_requests;
    uint64_t dma_bytes;
    uint64_t dma_total_us;
    uint32_t dma_max_us;
    uint64_t dma_busy_poll_us;
    uint64_t dma_timeouts;
    uint64_t pio_fallbacks;
    uint64_t dma_missing_irqs;
    uint64_t dma_late_irqs;
    uint64_t dma_spurious_irqs;

    uint64_t storage_event_wakeups;
    uint64_t storage_deadline_wakeups;

    uint64_t pcm_track_changes;
    uint64_t pcm_notify_total_us;
    uint32_t pcm_notify_max_us;
    uint64_t pcm_underruns;
    uint64_t pcm_deferred_notifications;
    uint64_t pcm_duplicate_notifications;
    uint64_t pcm_missed_transitions;

    uint64_t lcd_updates;
    uint64_t lcd_requested_pixels;
    uint64_t lcd_transmitted_pixels;
    uint64_t lcd_total_us;
    uint32_t lcd_max_us;
    uint64_t lcd_busy_timeouts;
    uint64_t lcd_block_timeouts;
    uint64_t lcd_txok_timeouts;
    uint64_t lcd_fifo1_timeouts;
    uint64_t lcd_fifo2_timeouts;
    uint64_t lcd_abandoned_rectangles;
    uint64_t lcd_reinitializations;
    uint32_t lcd_failure_streak;
    uint32_t lcd_max_failure_streak;
};

void pp5020_perf_init(void);
void pp5020_perf_reset(void);
void pp5020_perf_get(struct pp5020_perf_stats *stats);

void pp5020_perf_set_ata_info(const uint16_t *identify, bool is_ssd,
                              int dma_mode, bool lba48,
                              bool flush_supported, bool sleep_supported);
void pp5020_perf_record_cache_clean(uint32_t elapsed_us);
void pp5020_perf_record_cache_discard(uint32_t elapsed_us);
void pp5020_perf_record_dma_request(uint32_t bytes);
void pp5020_perf_record_dma_complete(uint32_t elapsed_us,
                                     uint32_t busy_poll_us, bool success);
void pp5020_perf_record_pio_fallback(void);
void pp5020_perf_record_dma_missing_irq(void);
void pp5020_perf_record_dma_late_irq(void);
void pp5020_perf_record_dma_spurious_irq(void);
void pp5020_perf_record_storage_wakeup(bool deadline);
void pp5020_perf_record_pcm_notify(uint32_t latency_us);
void pp5020_perf_record_pcm_underrun(void);
void pp5020_perf_record_pcm_deferred(void);
void pp5020_perf_record_pcm_duplicate(void);
void pp5020_perf_record_pcm_missed(void);
void pp5020_perf_record_lcd_update(uint32_t requested_pixels,
                                   uint32_t transmitted_pixels,
                                   uint32_t elapsed_us, bool success);
void pp5020_perf_record_lcd_timeout(int phase);
void pp5020_perf_record_lcd_reinitialization(void);

#endif /* HAVE_PP5020_PERF */
#endif /* PP5020_PERF_H */
