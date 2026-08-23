/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 Dave Chapman
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

bool get_monkeys_metadata(int fd, struct mp3entry* id3)
{
    /* Use the trackname part of the id3 structure as a temporary buffer */
    unsigned char* buf = (unsigned char *)id3->path;
    unsigned char* header;
    bool rc = false;
    uint32_t descriptorlength;
    uint64_t totalsamples;
    uint32_t blocksperframe, finalframeblocks, totalframes;
    int fileversion;
    ssize_t bytes_read;

    if (lseek(fd, 0, SEEK_SET) < 0)
        return false;

    if (read(fd, buf, 4) < 4)
    {
        return rc;
    }
    
    if (memcmp(buf, "MAC ", 4) != 0) 
    {
        return rc;
    }

    bytes_read = read(fd, buf + 4, MAX_PATH - 4);
    if (bytes_read < 28)
    {
        return false;
    }

    fileversion = get_short_le(buf+4);
    if (fileversion < 3970)
    {
        /* Not supported */
        return false;
    }

    if (fileversion >= 3980)
    {
        descriptorlength = get_long_le(buf+8);

        /* The descriptor is followed by the fixed-size frame header. */
        if (descriptorlength < 52 ||
            descriptorlength > sizeof(id3->path) - 24 ||
            bytes_read < (ssize_t)(descriptorlength + 20))
        {
            return false;
        }

        header = buf + descriptorlength;

        blocksperframe = get_long_le(header+4);
        finalframeblocks = get_long_le(header+8);
        totalframes = get_long_le(header+12);
        id3->frequency = get_long_le(header+20);
    } 
    else 
    {
        /* v3.95 and later files all have a fixed framesize */
        blocksperframe = 73728 * 4;

        finalframeblocks = get_long_le(buf+28);
        totalframes = get_long_le(buf+24);
        id3->frequency = get_long_le(buf+12);
    }

    if (id3->frequency == 0 || blocksperframe == 0 || totalframes == 0 ||
        finalframeblocks == 0 || finalframeblocks > blocksperframe)
    {
        return false;
    }

    off_t file_size = filesize(fd);
    if (file_size < 0)
        return false;

    id3->vbr = true;   /* All APE files are VBR */
    id3->filesize = (unsigned long)file_size;

    totalsamples = finalframeblocks;
    if (totalframes > 1)
        totalsamples += (uint64_t)blocksperframe * (totalframes-1);

    if (totalsamples > UINT64_MAX / 1000)
        return false;

    uint64_t length = (totalsamples * 1000) / id3->frequency;
    if (length == 0 || length > ULONG_MAX)
        return false;

    uint64_t bitrate = ((uint64_t)id3->filesize * 8) / length;
    if (bitrate > UINT_MAX)
        return false;

    id3->length = (unsigned long)length;
    id3->bitrate = (unsigned int)bitrate;

    read_ape_tags(fd, id3);
    return true;
}
