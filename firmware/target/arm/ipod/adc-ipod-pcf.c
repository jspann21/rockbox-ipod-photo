/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
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
#include "cpu.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "string.h"
#include "adc.h"
#include "pcf50605.h"
#include "i2c-pp.h"

struct adc_struct {
    long timeout;
    void (*conversion)(unsigned short *data);
    short channelnum;
    unsigned short data;
    long sample_tick;
#ifdef IPOD_COLOR
    unsigned short consecutive_failures;
    unsigned long total_failures;
#endif
};

static struct adc_struct adcdata[NUM_ADC_CHANNELS] IDATA_ATTR;

static long adc_cache_interval(const struct adc_struct *adc)
{
#ifdef IPOD_COLOR
    /* battery_read_info() has callers outside the power thread, so use a real
     * one-second deadline here rather than assuming HZ/2 polling is the only
     * ADC consumer. Accessory detection keeps its original 400 ms cadence. */
    if (adc == &adcdata[ADC_BATTERY])
        return HZ;
#else
    (void)adc;
#endif
    return HZ * 2 / 5;
}

static unsigned short _adc_read(struct adc_struct *adc)
{
    bool expired = adc->timeout == 0;
#ifdef IPOD_COLOR
    if (adc == &adcdata[ADC_BATTERY])
        expired = expired || !TIME_BEFORE(current_tick, adc->timeout);
    else
#endif
        expired = expired || TIME_AFTER(current_tick, adc->timeout);

    if (expired) {
        unsigned char data[2] = {0};
        unsigned short value;

        i2c_lock();

        adc->timeout = current_tick + adc_cache_interval(adc);

        /* ADCC1, 10 bit, start */
        if (pcf50605_write(0x2f, (adc->channelnum << 1) | 0x1) < 0)
        {
#ifdef IPOD_COLOR
            if (adc->consecutive_failures != 0xffff)
                adc->consecutive_failures++;
            adc->total_failures++;
#endif
            i2c_unlock();
            return adc->data;
        }
        if (pcf50605_read_multiple(0x30, data, 2) != 0)
        {
#ifdef IPOD_COLOR
            if (adc->consecutive_failures != 0xffff)
                adc->consecutive_failures++;
            adc->total_failures++;
#endif
            i2c_unlock();
            return adc->data;
        }
        value   = data[0];
        value <<= 2;
        value  |= data[1] & 0x3;

        if (adc->conversion) {
            adc->conversion(&value);
        }
        adc->data = value;
        adc->sample_tick = current_tick;
#ifdef IPOD_COLOR
        adc->consecutive_failures = 0;
#endif

        i2c_unlock();
        return value;
    } else
    {
        return adc->data;
    }
}

/* Force an ADC scan _now_ */
unsigned short adc_scan(int channel) {
    struct adc_struct *adc = &adcdata[channel];
    adc->timeout = 0;
    return _adc_read(adc);
}

/* Retrieve the ADC value, only does a scan periodically */
unsigned short adc_read(int channel) {
    return _adc_read(&adcdata[channel]);
}

#ifdef IPOD_COLOR
void adc_get_channel_status(int channel, struct adc_channel_status *status)
{
    struct adc_struct *adc = &adcdata[channel];

    if (status == NULL)
        return;

    i2c_lock();
    status->sample_tick = adc->sample_tick;
    status->consecutive_failures = adc->consecutive_failures;
    status->total_failures = adc->total_failures;
    i2c_unlock();
}
#endif

void adc_init(void)
{
    struct adc_struct *adc_battery = &adcdata[ADC_BATTERY];
    adc_battery->channelnum = 0x2; /* ADCVIN1, resistive divider */
    adc_battery->timeout = 0;
    adcdata[ADC_ACCESSORY].channelnum = 4;
    adcdata[ADC_ACCESSORY].timeout = 0;
#if defined(IPOD_VIDEO) || defined(IPOD_NANO)
    adcdata[ADC_4066_ISTAT].channelnum = 7;
    adcdata[ADC_4066_ISTAT].timeout = 0;
#endif
    _adc_read(adc_battery);
}
