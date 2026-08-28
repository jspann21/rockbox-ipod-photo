#ifndef MPEG16_STATS_H
#define MPEG16_STATS_H

#include <stdint.h>

struct mpeg16_parse_profile
{
    uint32_t parse_us;
    uint32_t parse_calls;
};

extern struct mpeg16_parse_profile mpeg16_parse_profile;

#endif
