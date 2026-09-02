/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Heikki Hannikainen, Uwe Freese
 * Revisions copyright (C) 2005 by Gerald Van Baren
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"
#include "adc.h"
#include "powermgmt.h"
#include "pcf5060x.h"
#include "pcf50605.h"
#include "audiohw.h"
#include "logf.h"
#ifdef HAVE_BATTERY_MEASURED_MODEL
#include "audio.h"
#include "backlight.h"
#include "kernel.h"
#include "power.h"
#include "storage.h"
#include "system.h"
#endif

unsigned short battery_level_disksafe =
#if   defined(IPOD_NANO)
    3330;
#elif defined(IPOD_VIDEO)
    3500;
#elif defined(IPOD_COLOR)
    /* Keep writes above the measured 0% point. Shutdown has its own lower,
     * debounced threshold. */
    3450;
#elif defined(IPOD_3G)
    3700;
#else
    /* FIXME: calibrate value for other iPods */
    3300;
#endif

unsigned short battery_level_shutoff =
#if   defined(IPOD_NANO)
    3230;
#elif defined(IPOD_VIDEO)
    3300;
#elif defined(IPOD_COLOR)
    3300;
#elif defined(IPOD_3G)
    3500;
#else
    /* FIXME: calibrate value for other iPods */
    3000;
#endif

/* voltages (millivolt) of 0%, 10%, ... 100% when charging disabled */
unsigned short percent_to_volt_discharge[11] =
{
#if   defined(IPOD_NANO)
    /* measured values */
    3230, 3620, 3700, 3730, 3750, 3780, 3830, 3890, 3950, 4030, 4160
#elif defined(IPOD_VIDEO)
    /* iPod Video 30GB Li-Ion 400mAh */
    3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180
#elif defined(IPOD_COLOR)
    /* iPod Photo 30GB, see FS#9072 */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#elif defined(IPOD_3G)
    /* iPod 3G 40GB, first approach based upon measurements */
    3720, 3740, 3760, 3780, 3830, 3870, 3910, 3970, 4020, 4060, 4090
#else
    /* FIXME: calibrate value for other iPods */
    /* Table is "provisional" from IPOD_COLOR */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#endif
};

#if CONFIG_CHARGING
/* voltages (millivolt) of 0%, 10%, ... 100% when charging enabled */
unsigned short percent_to_volt_charge[11] =
{
#if   defined(IPOD_NANO)
    /* measured values */
    3230, 3620, 3700, 3730, 3750, 3780, 3830, 3890, 3950, 4030, 4160
#elif defined(IPOD_VIDEO)
    /* iPOD Video 30GB Li-Ion 400mAh */
    3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180
#elif defined(IPOD_COLOR)
    /* No hardware-qualified A1099 charging curve is available yet. Keep the
     * established Photo fallback rather than borrowing another iPod's pack
     * profile; battery_levels.cfg can supply a measured replacement. */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#elif defined(IPOD_3G)
    /* iPod 3G 40GB, first approach based upon measurements */
    3720, 3740, 3760, 3780, 3830, 3870, 3910, 3970, 4020, 4060, 4090
#else
    /* FIXME: calibrate value for other iPods */
    /* Table is "provisional" from IPOD_COLOR */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#endif
};
#endif /* CONFIG_CHARGING */

#define BATTERY_SCALE_FACTOR 6000
/* full-scale ADC readout (2^10) in millivolt */

/* Returns battery voltage from ADC [millivolts] */
int _battery_voltage(void)
{
    return (adc_read(ADC_UNREG_POWER) * BATTERY_SCALE_FACTOR) >> 10;
}

#ifdef HAVE_BATTERY_MEASURED_MODEL
/*
 * Voltage-only fuel model for the iPod Photo (A1099).
 *
 * The PCF50605 does not expose battery current on this target. Rather than
 * pretending to coulomb-count, this model normalizes short, repeatable ATA
 * and CPU load steps to the last observed baseline. All arithmetic is fixed
 * point (Q8 millivolts) for the ARMv4T target.
 */
#define MODEL_FP_SHIFT             8
#define MODEL_RAW_SAMPLES          5
#define MODEL_TRACE_LEN          128
#define MODEL_TERMINAL_DIV         4 /* about 4 s at the effective ADC rate */
#define MODEL_BASELINE_DIV        16 /* about 16 s at the effective ADC rate */
#define MODEL_EXTERNAL_DIV         8
#define MODEL_SAG_DIV              4
#define MODEL_SAG_SETTLE_TICKS    (2 * HZ)
#define MODEL_SAG_MAX_MV         250
#define MODEL_SAG_HEADROOM_MV     12
#define MODEL_LOW_CONFIRM_TICKS  (10 * HZ)
#define MODEL_LOW_HYSTERESIS_MV   75
#define MODEL_OFF_CONFIRM_TICKS   (8 * HZ)
#define MODEL_OFF_HYSTERESIS_MV   80
#define MODEL_HARD_FLOOR_MV     3200
#define MODEL_HARD_FLOOR_TICKS   (2 * HZ)
#define MODEL_ADC_STALE_TICKS    (4 * HZ)
#define MODEL_ADC_FAULT_TICKS   (10 * HZ)
#define MODEL_ADC_FAILURE_LIMIT         3
#define MODEL_PCF_STATUS_TICKS    (5 * HZ)
#define MODEL_TRACE_TICKS         (1 * HZ)

struct ipodphoto_battery_model
{
    unsigned short raw[MODEL_RAW_SAMPLES];
    unsigned char raw_pos;
    unsigned char raw_count;
    unsigned short last_raw_mv;
    unsigned short median_mv;
    int terminal_q8;
    int model_q8;
    int sag_q8;
    int pre_transient_mv;
    long adc_sample_tick;
    bool adc_stale;
    bool adc_fault;
    bool transient;
    bool sag_sampled;
    long transient_since;
    enum battery_model_state state;
    long low_since;
    long shutdown_since;
    long hard_floor_since;
    bool hard_floor_pending;
    bool force_shutdown;
    signed char reported_percent;
    unsigned char pcf_id;
    unsigned char pcf_lowbat_reg;
    unsigned char pcf_lowbat_boot;
    unsigned char pcf_lowbat_now;
    long pcf_lowbat_tick;
    long trace_tick;
    bool telemetry_enabled;
    struct mutex mutex;
    struct battery_model_sample trace[MODEL_TRACE_LEN];
    unsigned char trace_head;
    unsigned char trace_count;
};

static struct ipodphoto_battery_model battery_model;

static int q8_to_mv(int value)
{
    return (value + (1 << (MODEL_FP_SHIFT - 1))) >> MODEL_FP_SHIFT;
}

static bool model_elapsed(long since, long duration)
{
    return !TIME_BEFORE(current_tick, since + duration);
}

static unsigned long model_tick_age(long tick)
{
    return (unsigned long)current_tick - (unsigned long)tick;
}

static unsigned char model_source_flags(void)
{
    unsigned char flags = 0;

    if (power_thread_inputs & POWER_INPUT_MAIN)
        flags |= BATTERY_MODEL_SOURCE_MAIN;
    if (power_thread_inputs & POWER_INPUT_USB)
        flags |= BATTERY_MODEL_SOURCE_USB;
    if (charging_state())
        flags |= BATTERY_MODEL_SOURCE_CHARGING;

    return flags;
}

static unsigned char model_load_flags(void)
{
    unsigned char flags = 0;

    if (ide_powered())
        flags |= BATTERY_MODEL_LOAD_ATA_POWERED;
    if (storage_disk_is_active())
        flags |= BATTERY_MODEL_LOAD_ATA_ACTIVE;
    if (cpu_frequency > CPUFREQ_NORMAL)
        flags |= BATTERY_MODEL_LOAD_CPU_BOOST;
    if (is_backlight_on(true))
        flags |= BATTERY_MODEL_LOAD_BACKLIGHT;
    if ((audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)) ==
        AUDIO_STATUS_PLAY)
        flags |= BATTERY_MODEL_LOAD_AUDIO;

    return flags;
}

static unsigned short model_median(unsigned short raw_mv)
{
    unsigned short sorted[MODEL_RAW_SAMPLES];
    unsigned int count;

    battery_model.raw[battery_model.raw_pos++] = raw_mv;
    if (battery_model.raw_pos == MODEL_RAW_SAMPLES)
        battery_model.raw_pos = 0;
    if (battery_model.raw_count < MODEL_RAW_SAMPLES)
        battery_model.raw_count++;

    count = battery_model.raw_count;
    for (unsigned int i = 0; i < count; i++)
    {
        unsigned int j = i;
        sorted[i] = battery_model.raw[i];
        while (j > 0 && sorted[j - 1] > sorted[j])
        {
            unsigned short swap = sorted[j - 1];
            sorted[j - 1] = sorted[j];
            sorted[j] = swap;
            j--;
        }
    }

    return sorted[count / 2];
}

static void model_update_adc_health(long sample_tick,
                                    unsigned int consecutive_failures)
{
    battery_model.adc_stale =
        model_tick_age(sample_tick) >= MODEL_ADC_STALE_TICKS;
    battery_model.adc_fault =
        consecutive_failures >= MODEL_ADC_FAILURE_LIMIT ||
        model_tick_age(sample_tick) >= MODEL_ADC_FAULT_TICKS;

    if (battery_model.adc_fault && !battery_model.force_shutdown)
        battery_model.state = BATTERY_MODEL_ADC_FAULT;
}

static void model_update_policy(int raw_mv, int terminal_mv, int model_mv,
                                unsigned char source_flags)
{
    int safety_mv = MIN((int)battery_model.median_mv,
                        MIN(terminal_mv, model_mv));
    int hard_floor_mv = MIN(raw_mv, (int)battery_model.median_mv);

    if (hard_floor_mv <= MODEL_HARD_FLOOR_MV)
    {
        if (!battery_model.hard_floor_pending)
        {
            battery_model.hard_floor_pending = true;
            battery_model.hard_floor_since = current_tick;
        }
        else if (model_elapsed(battery_model.hard_floor_since,
                               MODEL_HARD_FLOOR_TICKS))
        {
            battery_model.state = BATTERY_MODEL_SHUTDOWN_PENDING;
            battery_model.force_shutdown = true;
            return;
        }
    }
    else if (hard_floor_mv >= MODEL_HARD_FLOOR_MV +
                              MODEL_OFF_HYSTERESIS_MV)
    {
        battery_model.hard_floor_pending = false;
    }

    /* External-power detection is not proof that the source is charging or
     * strong enough to support writes. Preserve the absolute battery floor;
     * only clear ordinary low-battery policy once the floor has recovered. */
    if (source_flags & (BATTERY_MODEL_SOURCE_MAIN |
                        BATTERY_MODEL_SOURCE_USB))
    {
        if (!battery_model.hard_floor_pending)
        {
            battery_model.state = BATTERY_MODEL_NORMAL;
            battery_model.force_shutdown = false;
        }
        else if (!battery_model.force_shutdown)
        {
            battery_model.state = BATTERY_MODEL_LOW_CONFIRMED;
        }
        return;
    }

    /* Sag compensation may improve the displayed level, but never delays
     * shutdown below an uncompensated voltage. This is the software layer
     * above the PCF's independent LOWBAT/standby backstop. */
    if (safety_mv <= battery_level_shutoff)
    {
        if (battery_model.state != BATTERY_MODEL_SHUTDOWN_PENDING)
        {
            battery_model.state = BATTERY_MODEL_SHUTDOWN_PENDING;
            battery_model.shutdown_since = current_tick;
        }
        else if (model_elapsed(battery_model.shutdown_since,
                               MODEL_OFF_CONFIRM_TICKS))
        {
            battery_model.force_shutdown = true;
        }
        return;
    }

    if (battery_model.state == BATTERY_MODEL_SHUTDOWN_PENDING)
    {
        if (safety_mv < battery_level_shutoff + MODEL_OFF_HYSTERESIS_MV)
            return;

        battery_model.state = model_mv <= battery_level_disksafe +
                                          MODEL_LOW_HYSTERESIS_MV ?
                              BATTERY_MODEL_LOW_CONFIRMED :
                              BATTERY_MODEL_NORMAL;
    }

    if (model_mv <= battery_level_disksafe)
    {
        if (battery_model.state == BATTERY_MODEL_NORMAL)
        {
            battery_model.state = BATTERY_MODEL_LOW_PENDING;
            battery_model.low_since = current_tick;
        }
        else if (battery_model.state == BATTERY_MODEL_LOW_PENDING &&
                 model_elapsed(battery_model.low_since,
                               MODEL_LOW_CONFIRM_TICKS))
        {
            battery_model.state = BATTERY_MODEL_LOW_CONFIRMED;
        }
    }
    else if (model_mv >= battery_level_disksafe + MODEL_LOW_HYSTERESIS_MV)
    {
        battery_model.state = BATTERY_MODEL_NORMAL;
    }

}

static void model_trace(int raw_mv, int terminal_mv, int model_mv,
                        unsigned char source_flags, unsigned char load_flags)
{
    struct battery_model_sample *sample =
        &battery_model.trace[battery_model.trace_head];
    int sag_mv = model_mv - terminal_mv;

    sample->tick = current_tick;
    sample->raw_mv = raw_mv;
    sample->median_mv = battery_model.median_mv;
    sample->filtered_mv = terminal_mv;
    sample->model_mv = model_mv;
    sample->sag_mv = MAX(sag_mv, 0);
    sample->learned_sag_mv = q8_to_mv(battery_model.sag_q8);
    sample->source_flags = source_flags;
    sample->load_flags = load_flags;
    sample->brightness = is_backlight_on(true) ? backlight_brightness : 0;
    sample->cpu_mhz = cpu_frequency / 1000000;
    sample->pcf_lowbat = battery_model.pcf_lowbat_now;
    sample->percent = battery_model.reported_percent;
    sample->state = battery_model.state;

    battery_model.trace_head++;
    if (battery_model.trace_head == MODEL_TRACE_LEN)
        battery_model.trace_head = 0;
    if (battery_model.trace_count < MODEL_TRACE_LEN)
        battery_model.trace_count++;
}

void battery_model_init(int raw_mv)
{
    int pcf_id;
    int lowbat;

    mutex_init(&battery_model.mutex);

    for (unsigned int i = 0; i < MODEL_RAW_SAMPLES; i++)
        battery_model.raw[i] = raw_mv;

    battery_model.raw_pos = 0;
    battery_model.raw_count = MODEL_RAW_SAMPLES;
    battery_model.last_raw_mv = raw_mv;
    battery_model.median_mv = raw_mv;
    battery_model.terminal_q8 = raw_mv << MODEL_FP_SHIFT;
    battery_model.model_q8 = battery_model.terminal_q8;
    battery_model.sag_q8 = 0;
    battery_model.pre_transient_mv = raw_mv;
    struct adc_channel_status adc_status;
    adc_get_channel_status(ADC_UNREG_POWER, &adc_status);
    battery_model.adc_sample_tick = adc_status.sample_tick;
    battery_model.adc_stale = false;
    battery_model.adc_fault = false;
    battery_model.transient = false;
    battery_model.sag_sampled = false;
    battery_model.state = BATTERY_MODEL_NORMAL;
    battery_model.force_shutdown = false;
    battery_model.hard_floor_pending = false;
    battery_model.reported_percent = -1;
    battery_model.trace_head = 0;
    battery_model.trace_count = 0;

    /* RetailOS 5.1.2.1 selects the low-battery status register by PMU ID:
     * ID 0x24 uses 0x36; the other supported variant uses BVMC (0x34).
     * Capture it for diagnostics only. Policy remains voltage-based until this
     * signal is qualified on physical A1099 boards. */
    pcf_id = pcf50605_read(PCF5060X_ID);
    battery_model.pcf_id = pcf_id < 0 ? 0xff : pcf_id;
    battery_model.pcf_lowbat_reg = pcf_id == 0x24 ?
                                   PCF5060X_LEDC1 : PCF5060X_BVMC;
    lowbat = pcf_id < 0 ? -1 :
             pcf50605_read(battery_model.pcf_lowbat_reg);
    battery_model.pcf_lowbat_boot = lowbat < 0 ? 0xff : lowbat;
    battery_model.pcf_lowbat_now = battery_model.pcf_lowbat_boot;
    battery_model.pcf_lowbat_tick = current_tick;
    battery_model.trace_tick = current_tick - MODEL_TRACE_TICKS;
    battery_model.telemetry_enabled = false;
}

int battery_model_step(int raw_mv)
{
    struct adc_channel_status adc_status;
    long sample_tick;
    unsigned char source_flags;
    unsigned char load_flags;
    unsigned char lowbat_reg = 0;
    bool transient;
    int lowbat = -1;
    int previous_terminal_mv;
    int terminal_mv;
    int model_mv;

    adc_get_channel_status(ADC_UNREG_POWER, &adc_status);
    sample_tick = adc_status.sample_tick;
    mutex_lock(&battery_model.mutex);
    model_update_adc_health(sample_tick,
                            adc_status.consecutive_failures);

    /* adc_read() returns its cached value between real PCF conversions and
     * after I2C failures. Do not learn twice from the same conversion. */
    if (sample_tick == battery_model.adc_sample_tick)
    {
        model_mv = q8_to_mv(battery_model.model_q8);
        mutex_unlock(&battery_model.mutex);
        return model_mv;
    }

    if (battery_model.telemetry_enabled &&
        battery_model.pcf_lowbat_reg == PCF5060X_LEDC1 &&
        model_elapsed(battery_model.pcf_lowbat_tick,
                      MODEL_PCF_STATUS_TICKS))
    {
        lowbat_reg = battery_model.pcf_lowbat_reg;
        battery_model.pcf_lowbat_tick = current_tick;
    }

    /* Sample load state without holding the model lock. In particular,
     * storage_disk_is_active() may take the ATA lock, while ATA callers can
     * query battery_level_safe() and therefore take the model lock. */
    mutex_unlock(&battery_model.mutex);
    if (lowbat_reg != 0)
        lowbat = pcf50605_read(lowbat_reg);
    source_flags = model_source_flags();
    load_flags = model_load_flags();
    transient = load_flags & (BATTERY_MODEL_LOAD_ATA_ACTIVE |
                              BATTERY_MODEL_LOAD_CPU_BOOST);

    mutex_lock(&battery_model.mutex);
    if (lowbat >= 0)
        battery_model.pcf_lowbat_now = lowbat;
    if (sample_tick == battery_model.adc_sample_tick)
    {
        model_mv = q8_to_mv(battery_model.model_q8);
        mutex_unlock(&battery_model.mutex);
        return model_mv;
    }
    battery_model.adc_sample_tick = sample_tick;
    battery_model.adc_stale = false;
    battery_model.adc_fault = false;
    if (battery_model.state == BATTERY_MODEL_ADC_FAULT)
        battery_model.state = BATTERY_MODEL_NORMAL;
    battery_model.last_raw_mv = raw_mv;
    previous_terminal_mv = q8_to_mv(battery_model.terminal_q8);

    battery_model.median_mv = model_median(raw_mv);
    battery_model.terminal_q8 +=
        ((battery_model.median_mv << MODEL_FP_SHIFT) -
         battery_model.terminal_q8) / MODEL_TERMINAL_DIV;
    terminal_mv = q8_to_mv(battery_model.terminal_q8);

    if (!battery_model.transient && transient)
    {
        battery_model.pre_transient_mv = previous_terminal_mv;
        battery_model.transient_since = current_tick;
        battery_model.sag_sampled = false;
    }

    if (transient && !battery_model.sag_sampled &&
        model_elapsed(battery_model.transient_since,
                      MODEL_SAG_SETTLE_TICKS))
    {
        int observed_mv = battery_model.pre_transient_mv - terminal_mv;

        if (observed_mv > 0)
        {
            observed_mv = MIN(observed_mv, MODEL_SAG_MAX_MV);
            battery_model.sag_q8 +=
                ((observed_mv << MODEL_FP_SHIFT) - battery_model.sag_q8) /
                MODEL_SAG_DIV;
        }
        battery_model.sag_sampled = true;
    }

    if (source_flags & (BATTERY_MODEL_SOURCE_MAIN |
                        BATTERY_MODEL_SOURCE_USB))
    {
        battery_model.model_q8 +=
            (battery_model.terminal_q8 - battery_model.model_q8) /
            MODEL_EXTERNAL_DIV;
    }
    else if (transient)
    {
        int compensated_mv = terminal_mv + q8_to_mv(battery_model.sag_q8);
        int ceiling_mv = battery_model.pre_transient_mv +
                         MODEL_SAG_HEADROOM_MV;

        if (battery_model.sag_q8 == 0)
            compensated_mv = MAX(compensated_mv,
                                 battery_model.pre_transient_mv);
        compensated_mv = MIN(compensated_mv, ceiling_mv);
        compensated_mv = MAX(compensated_mv, terminal_mv);
        battery_model.model_q8 +=
            ((compensated_mv << MODEL_FP_SHIFT) - battery_model.model_q8) /
            MODEL_TERMINAL_DIV;
    }
    else
    {
        battery_model.model_q8 +=
            (battery_model.terminal_q8 - battery_model.model_q8) /
            MODEL_BASELINE_DIV;
    }

    battery_model.transient = transient;
    model_mv = q8_to_mv(battery_model.model_q8);
    model_update_policy(raw_mv, terminal_mv, model_mv, source_flags);
    if (model_elapsed(battery_model.trace_tick, MODEL_TRACE_TICKS))
    {
        battery_model.trace_tick = current_tick;
        model_trace(raw_mv, terminal_mv, model_mv, source_flags, load_flags);
    }
    mutex_unlock(&battery_model.mutex);
    return model_mv;
}

void battery_model_set_reported_level(int level)
{
    mutex_lock(&battery_model.mutex);
    battery_model.reported_percent = level;

    if (battery_model.trace_count > 0)
    {
        unsigned int newest = battery_model.trace_head == 0 ?
                              MODEL_TRACE_LEN - 1 :
                              battery_model.trace_head - 1;
        battery_model.trace[newest].percent = level;
    }
    mutex_unlock(&battery_model.mutex);
}

void battery_model_set_telemetry(bool enable)
{
    mutex_lock(&battery_model.mutex);
    battery_model.telemetry_enabled = enable;
    if (enable)
    {
        battery_model.pcf_lowbat_tick = current_tick - MODEL_PCF_STATUS_TICKS;
        battery_model.trace_tick = current_tick - MODEL_TRACE_TICKS;
    }
    mutex_unlock(&battery_model.mutex);
}

bool battery_model_disk_safe(void)
{
    bool safe;
    bool external_power = power_thread_inputs & POWER_INPUT;

    mutex_lock(&battery_model.mutex);
    int safety_mv = MIN((int)battery_model.median_mv,
                        MIN(q8_to_mv(battery_model.terminal_q8),
                            q8_to_mv(battery_model.model_q8)));
    int hard_floor_mv = MIN((int)battery_model.last_raw_mv,
                            (int)battery_model.median_mv);

    if (battery_model.adc_stale || battery_model.adc_fault)
        safe = false;
    else if (external_power)
        safe = !battery_model.hard_floor_pending &&
               hard_floor_mv > MODEL_HARD_FLOOR_MV;
    else
        safe = battery_model.state == BATTERY_MODEL_NORMAL &&
               safety_mv > battery_level_disksafe;
    mutex_unlock(&battery_model.mutex);
    return safe;
}

bool battery_model_force_shutdown(void)
{
    bool force_shutdown;

    mutex_lock(&battery_model.mutex);
    force_shutdown = battery_model.force_shutdown;
    mutex_unlock(&battery_model.mutex);
    return force_shutdown;
}

bool battery_model_get_sample(unsigned int age,
                              struct battery_model_sample *sample)
{
    unsigned int head;
    unsigned int index;

    if (sample == NULL)
        return false;

    mutex_lock(&battery_model.mutex);
    if (age >= battery_model.trace_count)
    {
        mutex_unlock(&battery_model.mutex);
        return false;
    }

    head = battery_model.trace_head;
    index = (head + MODEL_TRACE_LEN - 1 - age) % MODEL_TRACE_LEN;
    *sample = battery_model.trace[index];
    mutex_unlock(&battery_model.mutex);
    return true;
}

unsigned int battery_model_copy_samples(unsigned long after_tick,
                                        struct battery_model_sample *samples,
                                        unsigned int max_samples)
{
    unsigned int copied = 0;

    if (samples == NULL || max_samples == 0)
        return 0;

    mutex_lock(&battery_model.mutex);
    for (unsigned int remaining = battery_model.trace_count;
         remaining > 0 && copied < max_samples; remaining--)
    {
        unsigned int index = (battery_model.trace_head + MODEL_TRACE_LEN -
                              remaining) % MODEL_TRACE_LEN;
        struct battery_model_sample *sample = &battery_model.trace[index];

        if (TIME_AFTER(sample->tick, after_tick))
            samples[copied++] = *sample;
    }
    mutex_unlock(&battery_model.mutex);
    return copied;
}

void battery_model_get_debug(struct battery_model_debug *debug)
{
    struct adc_channel_status adc_status;
    unsigned long adc_age_seconds;

    if (debug == NULL)
        return;

    adc_get_channel_status(ADC_UNREG_POWER, &adc_status);
    adc_age_seconds = model_tick_age(adc_status.sample_tick) / HZ;

    mutex_lock(&battery_model.mutex);
    if (battery_model.trace_count > 0)
    {
        unsigned int newest = battery_model.trace_head == 0 ?
                              MODEL_TRACE_LEN - 1 :
                              battery_model.trace_head - 1;
        debug->sample = battery_model.trace[newest];
    }
    else
    {
        debug->sample.tick = current_tick;
        debug->sample.raw_mv = battery_model.median_mv;
        debug->sample.median_mv = battery_model.median_mv;
        debug->sample.filtered_mv = q8_to_mv(battery_model.terminal_q8);
        debug->sample.model_mv = q8_to_mv(battery_model.model_q8);
        debug->sample.sag_mv = 0;
        debug->sample.learned_sag_mv = q8_to_mv(battery_model.sag_q8);
        debug->sample.source_flags = 0;
        debug->sample.load_flags = 0;
        debug->sample.brightness = 0;
        debug->sample.cpu_mhz = 0;
        debug->sample.pcf_lowbat = battery_model.pcf_lowbat_now;
        debug->sample.percent = battery_model.reported_percent;
        debug->sample.state = battery_model.state;
    }

    debug->median_mv = battery_model.median_mv;
    debug->learned_sag_mv = q8_to_mv(battery_model.sag_q8);
    debug->disksafe_mv = battery_level_disksafe;
    debug->shutoff_mv = battery_level_shutoff;
    debug->trace_count = battery_model.trace_count;
    debug->pcf_id = battery_model.pcf_id;
    debug->pcf_lowbat_reg = battery_model.pcf_lowbat_reg;
    debug->pcf_lowbat_boot = battery_model.pcf_lowbat_boot;
    debug->pcf_lowbat_now = battery_model.pcf_lowbat_now;
    debug->adc_age_seconds = MIN(adc_age_seconds, 0xfffful);
    debug->adc_consecutive_failures = adc_status.consecutive_failures;
    debug->adc_total_failures = adc_status.total_failures;
    debug->adc_stale = battery_model.adc_stale;
    debug->adc_fault = battery_model.adc_fault;
    mutex_unlock(&battery_model.mutex);
}
#endif /* HAVE_BATTERY_MEASURED_MODEL */

#ifdef HAVE_ACCESSORY_SUPPLY
void accessory_supply_set(bool enable)
{
    /* Set accessory power supply to 3.3V, otherwise switch it off. */
    unsigned char value = enable ? 0xf8 : 0x18;
    
    /* Accessory power is user-visible; retry transient bus failures. */
    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (pcf50605_write(PCF5060X_D2REGC1, value) >= 0)
            return;
    }
    logf("accessory supply write failed");
}
#endif

#ifdef HAVE_LINEOUT_POWEROFF
void lineout_set(bool enable)
{
    /* Call audio hardware driver implementation */
    audiohw_enable_lineout(enable);
}
#endif
