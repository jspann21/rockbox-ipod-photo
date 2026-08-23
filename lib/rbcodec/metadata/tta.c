/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2010 Yoshihisa Uchida
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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include "platform.h"

#include "metadata.h"
#include "metadata_common.h"
#include "metadata_parsers.h"
#include "logf.h"

#define TTA1_SIGN    0x31415454

#define TTA_HEADER_ID              0
#define TTA_HEADER_AUDIO_FORMAT    (TTA_HEADER_ID              + sizeof(unsigned int))
#define TTA_HEADER_NUM_CHANNELS    (TTA_HEADER_AUDIO_FORMAT    + sizeof(unsigned short))
#define TTA_HEADER_BITS_PER_SAMPLE (TTA_HEADER_NUM_CHANNELS    + sizeof(unsigned short))
#define TTA_HEADER_SAMPLE_RATE     (TTA_HEADER_BITS_PER_SAMPLE + sizeof(unsigned short))
#define TTA_HEADER_DATA_LENGTH     (TTA_HEADER_SAMPLE_RATE     + sizeof(unsigned int))
#define TTA_HEADER_CRC32           (TTA_HEADER_DATA_LENGTH     + sizeof(unsigned int))
#define TTA_HEADER_SIZE            (TTA_HEADER_CRC32           + sizeof(unsigned int))

#define TTA_HEADER_GETTER_ID(x)              get_long_le(x)
#define TTA_HEADER_GETTER_AUDIO_FORMAT(x)    get_short_le(x)
#define TTA_HEADER_GETTER_NUM_CHANNELS(x)    get_short_le(x)
#define TTA_HEADER_GETTER_BITS_PER_SAMPLE(x) get_short_le(x)
#define TTA_HEADER_GETTER_SAMPLE_RATE(x)     get_long_le(x)
#define TTA_HEADER_GETTER_DATA_LENGTH(x)     get_long_le(x)
#define TTA_HEADER_GETTER_CRC32(x)           get_long_le(x)

#define GET_HEADER(x, tag) TTA_HEADER_GETTER_ ## tag((x) + TTA_HEADER_ ## tag)

/* Keep metadata acceptance in sync with the bundled decoder. */
static bool tta_supported_frequency(uint32_t frequency)
{
    switch (frequency)
    {
        case 16000:
        case 22050:
        case 24000:
        case 32000:
        case 44100:
        case 48000:
        case 64000:
        case 88200:
        case 96000:
            return true;
        default:
            return false;
    }
}

static void read_id3_tags(int fd, struct mp3entry* id3)
{
    id3->title    = NULL;
    id3->filesize = filesize(fd);
    id3->id3v2len = getid3v2len(fd);
    id3->vbr      = false;   /* All TTA files are CBR */

    /* first get id3v2 tags. if no id3v2 tags ware found, get id3v1 tags */
    if (id3->id3v2len)
    {
        setid3v2title(fd, id3);
        id3->first_frame_offset = id3->id3v2len;
        return;
    }
    setid3v1title(fd, id3);
}

bool get_tta_metadata(int fd, struct mp3entry* id3)
{
    unsigned char ttahdr[TTA_HEADER_SIZE];
    uint64_t datasize;
    uint64_t origsize;
    unsigned int audio_format;
    uint32_t data_length;
    unsigned int channels;
    unsigned int frequency;
    unsigned int bps;
    off_t file_size;

    if (lseek(fd, 0, SEEK_SET) < 0)
        return false;

    /* read id3 tags */
    read_id3_tags(fd, id3);
    if (lseek(fd, id3->first_frame_offset, SEEK_SET) < 0)
        return false;

    /* read TTA header */
    if (read(fd, ttahdr, TTA_HEADER_SIZE) != TTA_HEADER_SIZE)
        return false;

    /* check for TTA3 signature */
    if ((GET_HEADER(ttahdr, ID)) != TTA1_SIGN)
        return false;

    /* skip check CRC */

    audio_format = GET_HEADER(ttahdr, AUDIO_FORMAT);
    channels  = GET_HEADER(ttahdr, NUM_CHANNELS);
    frequency = GET_HEADER(ttahdr, SAMPLE_RATE);
    data_length = GET_HEADER(ttahdr, DATA_LENGTH);
    bps = GET_HEADER(ttahdr, BITS_PER_SAMPLE);

    if (audio_format != 1 || channels == 0 || channels > 2 ||
        frequency == 0 || !tta_supported_frequency(frequency) ||
        data_length == 0 || bps == 0 || bps > 24)
    {
        return false;
    }

    file_size = filesize(fd);
    if (file_size < 0 ||
        id3->first_frame_offset > (uint64_t)file_size ||
        TTA_HEADER_SIZE > (uint64_t)file_size - id3->first_frame_offset)
    {
        return false;
    }

    id3->frequency = frequency;
    uint64_t length = ((uint64_t)data_length * 1000) / frequency;
    if (length == 0 || length > ULONG_MAX)
        return false;
    id3->length = (unsigned long)length;

    datasize = (uint64_t)file_size - id3->first_frame_offset;

    uint64_t bytes_per_sample = (bps + 7) / 8;
    if (data_length > UINT64_MAX / bytes_per_sample)
        return false;
    origsize = (uint64_t)data_length * bytes_per_sample;
    if (origsize > UINT64_MAX / channels)
        return false;
    origsize *= channels;
    if (origsize == 0 || origsize > UINT64_MAX / 1000)
        return false;

    uint64_t numerator = datasize;
    if (numerator > UINT64_MAX / id3->frequency)
        return false;
    numerator *= id3->frequency;
    if (numerator > UINT64_MAX / channels)
        return false;
    numerator *= channels;
    if (numerator > UINT64_MAX / bps)
        return false;
    numerator *= bps;

    uint64_t bitrate = numerator / (origsize * 1000);
    if (bitrate > UINT_MAX)
        return false;
    id3->bitrate = (unsigned int)bitrate;

    /* output header info (for debug) */
    DEBUGF("TTA header info ----\n");
    DEBUGF("id:        %x\n",  (unsigned int)(GET_HEADER(ttahdr, ID)));
    DEBUGF("channels:  %u\n",  channels);
    DEBUGF("frequency: %lu\n", id3->frequency);
    DEBUGF("length:    %lu\n", id3->length);
    DEBUGF("bitrate:   %u\n",  id3->bitrate);
    DEBUGF("bits per sample: %u\n", bps);
    DEBUGF("compressed size: %llu\n", (unsigned long long)datasize);
    DEBUGF("original size:   %llu\n", (unsigned long long)origsize);
    DEBUGF("id3----\n");
    DEBUGF("artist:  %s\n",  id3->artist);
    DEBUGF("title:   %s\n",  id3->title);
    DEBUGF("genre:   %s\n",  id3->genre_string);

    return true;
}
