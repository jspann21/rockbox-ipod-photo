/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Dave Chapman
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

#ifdef HAVE_ALBUMART

bool parse_flac_album_art(unsigned char *buf, int bytes_read, enum mp3_aa_type *type, int *picframe_pos)
{
    *type = AA_TYPE_UNKNOWN;
    *picframe_pos = 0;

    /* Fixed fields occupy 32 bytes, including the picture data length.
     * Bound unsigned lengths before adding them to a buffer offset. */
    if (bytes_read < 32)
        return false;

    uint32_t mime_length = get_long_be(buf + 4);
    if (mime_length > (uint32_t)bytes_read - 32)
        return false;

    unsigned int pos = 8 + mime_length;
    uint32_t description_length = get_long_be(buf + pos);
    pos += 4;
    if (description_length > (uint32_t)bytes_read - pos - 20)
        return false;

    /* Skip description, width, height, depth and palette count. The caller
     * may now safely read the four-byte picture length at this offset. */
    *picframe_pos = pos + description_length + 16;

    /* MIME strings are length-delimited, not NUL-terminated. */
    if ((mime_length == 10 && memcmp(buf + 8, "image/jpeg", 10) == 0) ||
        (mime_length == 9 && memcmp(buf + 8, "image/jpg", 9) == 0))
        *type = AA_TYPE_JPG;
    else if (mime_length == 9 && memcmp(buf + 8, "image/png", 9) == 0)
        *type = AA_TYPE_PNG;

    return true;
}

#endif /* HAVE_ALBUMART */

bool get_flac_metadata(int fd, struct mp3entry* id3)
{
    /* A simple parser to read vital metadata from a FLAC file - length,
     * frequency, bitrate etc. This code should either be moved to a
     * seperate file, or discarded in favour of the libFLAC code.
     * The FLAC stream specification can be found at
     * http://flac.sourceforge.net/format.html#stream
     */

    /* Use the trackname part of the id3 structure as a temporary buffer */
    unsigned char* buf = (unsigned char *)id3->path;
    bool last_metadata = false;
    bool rc = false;
    off_t file_size = filesize(fd);

    if (file_size < 0)
        return false;

    if (!skip_id3v2(fd, id3) || (read(fd, buf, 4) < 4))
    {
        return rc;
    }

    if (memcmp(buf, "fLaC", 4) != 0)
    {
        return rc;
    }

    while (!last_metadata)
    {
        unsigned long i;
        int type;

        if (read(fd, buf, 4) != 4)
        {
            return false;
        }

        last_metadata = buf[0] & 0x80;
        type = buf[0] & 0x7f;
        /* The length of the block */
        i = (buf[1] << 16) | (buf[2] << 8) | buf[3];

        off_t block_start = lseek(fd, 0, SEEK_CUR);
        if (block_start < 0 || block_start > file_size ||
            i > (uint64_t)(file_size - block_start) ||
            (!rc && type != 0))
            return false;

        if (type == 0)       /* 0 is the STREAMINFO block */
        {
            if (rc || i != 34 || read(fd, buf, i) != (int)i)
            {
                return false;
            }

            id3->vbr = true;   /* All FLAC files are VBR */
            id3->filesize = file_size;
            id3->frequency = (buf[10] << 12) | (buf[11] << 4)
                | ((buf[12] & 0xf0) >> 4);
            if (id3->frequency == 0)
                return false;

            uint64_t totalsamples = ((uint64_t)(buf[13] & 0x0f) << 32) |
                                    get_long_be(&buf[14]);

            if(totalsamples > 0)
            {
                /* Calculate track length (in ms) and estimate the bitrate (in kbit/s) */
                uint64_t length = totalsamples * 1000 / id3->frequency;
                if (length > ULONG_MAX)
                    return false;
                /* A valid sub-millisecond stream must not divide by zero. */
                id3->length = MAX(length, 1);
                id3->bitrate = MIN((uint64_t)id3->filesize * 8 / id3->length,
                                  UINT_MAX);
            }
            else
            {
                id3->length = 0;
                id3->bitrate = 0;
            }
            rc = true;  /* Got vital metadata */
        }
        else if (type == 4)  /* 4 is the VORBIS_COMMENT block */
        {
            /* The next i bytes of the file contain the VORBIS COMMENTS. */
            if (i == 0 || read_vorbis_tags(fd, id3, i) == 0)
            {
                return false;
            }
        }
#ifdef HAVE_ALBUMART
        else if (type == 6) /* 6 is the PICTURE block */
        {
            if(!id3->has_embedded_albumart) /* only use the first PICTURE */
            {
                unsigned int buf_size = MIN(sizeof(id3->path), i);

                int bytes_read = read(fd, buf, buf_size);
                if (bytes_read != (int)buf_size)
                    return false;

                int picframe_pos;
                enum mp3_aa_type art_type;
                if (parse_flac_album_art(buf, bytes_read, &art_type, &picframe_pos) &&
                    art_type != AA_TYPE_UNKNOWN)
                {
                    uint32_t art_size = get_long_be(buf + picframe_pos);
                    if (art_size > 0 && art_size <= i - picframe_pos - 4)
                    {
                        id3->has_embedded_albumart = true;
                        id3->albumart.type = art_type;
                        id3->albumart.size = art_size;
                        id3->albumart.pos = block_start + picframe_pos + 4;
                    }
                }
            }
        }
#endif

        /* Always resume at the validated block boundary, including when an
         * optional picture header exceeds the bounded scratch buffer. */
        if (lseek(fd, block_start + i, SEEK_SET) < 0)
        {
            return false;
        }
    }

    return rc;
}
