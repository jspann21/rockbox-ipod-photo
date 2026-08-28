/***************************************************************************
 * PortalPlayer MPEG video timing wrapper.
 *
 * Keeps the existing video thread intact and profiles mpeg2_parse() so the
 * CPU-render experiment can distinguish decoder work from display work.
 ****************************************************************************/

#include "plugin.h"
#include "mpegplayer.h"
#include "mpeg16_stats.h"

struct mpeg16_parse_profile mpeg16_parse_profile SHAREDBSS_ATTR;

static uint32_t mpeg16_now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (uint32_t)*rb->current_tick * (1000000u / HZ);
#endif
}

static mpeg2_state_t mpeg16_mpeg2_parse(mpeg2dec_t *mpeg2dec)
{
    uint32_t started = mpeg16_now_us();
    mpeg2_state_t state = mpeg2_parse(mpeg2dec);

    mpeg16_parse_profile.parse_us += mpeg16_now_us() - started;
    mpeg16_parse_profile.parse_calls++;
    return state;
}

#define mpeg2_parse mpeg16_mpeg2_parse
#include "video_thread.c"
#undef mpeg2_parse
