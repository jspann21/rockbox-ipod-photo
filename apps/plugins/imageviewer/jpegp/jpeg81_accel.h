/***************************************************************************
 * Progressive JPEG Huffman acceleration controls and statistics.
 ****************************************************************************/
#ifndef JPEGP_JPEG81_ACCEL_H
#define JPEGP_JPEG81_ACCEL_H

#include "jpeg81.h"
#include <stdbool.h>
#include <stdint.h>

struct jpegp_huffman_stats
{
    uint32_t decode_us;
    uint32_t bytes;
    uint32_t look_hits;
    uint32_t slow_hits;
    uint32_t getbits_calls;
    uint32_t marker_events;
    bool fast_used;
    bool fallback;
};

void jpegp_huffman_set_fast(bool enable);
void jpegp_huffman_get_stats(struct jpegp_huffman_stats *stats);

#endif
