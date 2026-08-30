/***************************************************************************
 * Shared telemetry types for measured battery models.
 ***************************************************************************/

#ifndef BATTERY_MODEL_H
#define BATTERY_MODEL_H

enum battery_model_state
{
    BATTERY_MODEL_NORMAL = 0,
    BATTERY_MODEL_LOW_PENDING,
    BATTERY_MODEL_LOW_CONFIRMED,
    BATTERY_MODEL_SHUTDOWN_PENDING,
};

enum battery_model_load_flags
{
    BATTERY_MODEL_LOAD_ATA_POWERED = 0x01,
    BATTERY_MODEL_LOAD_ATA_ACTIVE  = 0x02,
    BATTERY_MODEL_LOAD_CPU_BOOST   = 0x04,
    BATTERY_MODEL_LOAD_BACKLIGHT   = 0x08,
    BATTERY_MODEL_LOAD_AUDIO       = 0x10,
};

enum battery_model_source_flags
{
    BATTERY_MODEL_SOURCE_MAIN     = 0x01,
    BATTERY_MODEL_SOURCE_USB      = 0x02,
    BATTERY_MODEL_SOURCE_CHARGING = 0x04,
};

struct battery_model_sample
{
    unsigned long tick;
    unsigned short raw_mv;
    unsigned short median_mv;
    unsigned short filtered_mv;
    unsigned short model_mv;
    short sag_mv;
    unsigned short learned_sag_mv;
    unsigned char source_flags;
    unsigned char load_flags;
    unsigned char brightness;
    unsigned char cpu_mhz;
    unsigned char pcf_lowbat;
    signed char percent;
    unsigned char state;
};

struct battery_model_debug
{
    struct battery_model_sample sample;
    unsigned short median_mv;
    unsigned short learned_sag_mv;
    unsigned short disksafe_mv;
    unsigned short shutoff_mv;
    unsigned char trace_count;
    unsigned char pcf_id;
    unsigned char pcf_lowbat_reg;
    unsigned char pcf_lowbat_boot;
    unsigned char pcf_lowbat_now;
};

#endif /* BATTERY_MODEL_H */
