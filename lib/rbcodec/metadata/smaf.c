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
#include <inttypes.h>
#include <stdio.h>
#include "platform.h"

#include "string-extra.h"
#include "metadata.h"
#include "metadata_common.h"
#include "metadata_parsers.h"
#include "rbunicode.h"
#include "logf.h"

static const int basebits[4] = { 4, 8, 12, 16 };

static const int frequency[5] = { 4000, 8000, 11025, 22050, 44100 };

static const int support_codepages[5] = {
    SJIS, ISO_8859_1, -1, GB_2312, BIG_5,
};

/* extra codepage */
#define UCS2  (NUM_CODEPAGES + 1)
#define SMAF_TEXT_LIMIT 256

/* support id3 tag */
#define TAG_TITLE    (('S'<<8)|'T')
#define TAG_ARTIST   (('A'<<8)|'N')
#define TAG_COMPOSER (('S'<<8)|'W')

/* convert functions */
#define CONVERT_SMAF_CHANNELS(c) (((c) >> 7) + 1)


static inline int convert_smaf_audio_basebit(unsigned int basebit)
{
    if (basebit > 3)
        return 0;
    return basebits[basebit];
}

static inline int convert_smaf_audio_frequency(unsigned int freq)
{
    if (freq > 4)
        return 0;
    return frequency[freq];
}

static int convert_smaf_codetype(unsigned int codetype)
{
    if (codetype < 5)
        return support_codepages[codetype];
    else if (codetype == 0x20 || codetype == 0x24) /* In Rockbox, UCS2 and UTF-16 are same. */
        return UCS2;
    else if (codetype == 0x23)
        return UTF_8;
    else if (codetype == 0xff)
        return ISO_8859_1;
    return -1;
}

static void set_length(struct mp3entry *id3, unsigned int ch, unsigned int basebit,
                       unsigned int numbytes)
{
    int bitspersample = convert_smaf_audio_basebit(basebit);

    if (bitspersample != 0 && id3->frequency != 0)
    {
        /* Calculate track length [ms] and bitrate [kbit/s] */
        id3->length  = (uint64_t)numbytes * 8000LL
                       / (bitspersample * CONVERT_SMAF_CHANNELS(ch) * id3->frequency);
        id3->bitrate = bitspersample * id3->frequency / 1000;
    }

    /* output contents/wave data/id3 info (for debug) */
    DEBUGF("contents info ----\n");
    DEBUGF("  TITLE:         %s\n", (id3->title)? id3->title : "(NULL)");
    DEBUGF("  ARTIST:        %s\n", (id3->artist)? id3->artist : "(NULL)");
    DEBUGF("  COMPOSER:      %s\n", (id3->composer)? id3->composer : "(NULL)");
    DEBUGF("wave data info ----\n");
    DEBUGF("  channels:      %u\n", CONVERT_SMAF_CHANNELS(ch));
    DEBUGF("  bitspersample: %d\n", bitspersample);
    DEBUGF("  numbytes;      %u\n", numbytes);
    DEBUGF("id3 info ----\n");
    DEBUGF("  frquency:      %u\n", (unsigned int)id3->frequency);
    DEBUGF("  bitrate:       %d\n", id3->bitrate);
    DEBUGF("  length:        %u\n", (unsigned int)id3->length);
}

/* contents parse functions */

/* Note: 
 *  1) When the codepage is UTF-8 or UCS2, contents data do not start BOM.
 *  2) The byte order of contents data is big endian.
 */

static void decode2utf8(const unsigned char *src, unsigned char **dst,
                        int srcsize, int *dstsize, int codepage)
{
    unsigned char tmpbuf[SMAF_TEXT_LIMIT * 3 + 1];
    unsigned char *p;
    int utf8size;

    if (!dst || !*dst || !dstsize || *dstsize <= 0 || srcsize < 0)
        return;
    srcsize = MIN(srcsize, SMAF_TEXT_LIMIT);
    if (codepage == UCS2)
        srcsize &= ~1;

    if (codepage < NUM_CODEPAGES)
        p = iso_decode(src, tmpbuf, codepage, srcsize);
    else /* codepage == UCS2 */
        p = utf16BEdecode(src, tmpbuf, srcsize);

    *p = '\0';

    strlcpy(*dst, tmpbuf, *dstsize);
    utf8size = (p - tmpbuf) + 1;
    if (utf8size > *dstsize)
    {
        DEBUGF("metadata warning: data length: %d > contents store buffer size: %d\n",
                    utf8size, *dstsize);
        utf8size = *dstsize;
    }
    *dst     += utf8size;
    *dstsize -= utf8size;
}

static int read_audio_track_contents(int fd, int codepage, int remaining,
                                     unsigned char **dst, int *dstsize)
{
    /* value length <= 256 bytes */
    unsigned char buf[SMAF_TEXT_LIMIT];
    unsigned char *p = buf;
    unsigned char *q = buf;

    int readsize = MIN(remaining, SMAF_TEXT_LIMIT);
    int datasize = read(fd, buf, readsize);
    if (datasize <= 0)
        return  0;

    unsigned char *end = buf + datasize;
    while (p < end)
    {
        int charsize = codepage == UCS2 ? 2 : 1;
        if ((codepage == UCS2 && end - p >= 2 && p[0] == 0 && p[1] == ',') ||
            (codepage != UCS2 && *p == ','))
        {
            p += charsize;
            break;
        }

        /* skip yen mark */
        if (codepage != UCS2)
        {
            if (*p == '\\')
            {
                p++;
                if (p >= end)
                    break;
            }
        }
        else if (end - p >= 2 && p[0] == '\0' && p[1] == '\\')
        {
            p += 2;
            if (p >= end)
                break;
        }

        charsize = codepage == UCS2 ? 2 :
                   (codepage == SJIS && *p > 0x7f &&
                    (*p <= 0xa0 || *p >= 0xe0) ? 2 : 1);
        if (end - p < charsize || q - buf > SMAF_TEXT_LIMIT - charsize)
            break;
        while (charsize-- > 0)
            *q++ = *p++;
    }

    int consumed = p - buf;
    if (lseek(fd, consumed - datasize, SEEK_CUR) < 0)
        return 0;

    if (dst != NULL)
        decode2utf8(buf, dst, q - buf, dstsize, codepage);

    return consumed;
}

static bool read_score_track_contents(int fd, int codepage, int datasize,
                                      unsigned char **dst, int *dstsize)
{
    unsigned char buf[SMAF_TEXT_LIMIT];
    int keep = MIN(datasize, SMAF_TEXT_LIMIT);
    if (read(fd, buf, keep) != keep)
        return false;
    if (datasize > keep && lseek(fd, datasize - keep, SEEK_CUR) < 0)
        return false;
    decode2utf8(buf, dst, keep, dstsize, codepage);
    return true;
}

/* traverse chunk functions */

static unsigned int search_chunk(int fd, const unsigned char *name, int nlen)
{
    unsigned char buf[8];
    unsigned int chunksize;

    while (read(fd, buf, 8) == 8)
    {
        chunksize = get_long_be(buf + 4);
        if (memcmp(buf, name, nlen) == 0)
            return chunksize;

        off_t pos = lseek(fd, 0, SEEK_CUR);
        off_t size = filesize(fd);
        if (pos < 0 || size < 0 || pos > size ||
            chunksize > (uint64_t)(size - pos) ||
            lseek(fd, chunksize, SEEK_CUR) < 0)
            break;
    }
    DEBUGF("metadata error: missing '%s' chunk\n", name);
    return 0;
}

static bool parse_smaf_audio_track(int fd, struct mp3entry *id3, unsigned int datasize)
{
    /* temporary buffer */
    unsigned char *tmp = (unsigned char*)id3->path;
    /* contents stored buffer */
    unsigned char *buf = id3->id3v2buf;
    int bufsize = sizeof(id3->id3v2buf);
    int valsize;
    unsigned int chunksize = datasize;
    int codepage = -1;

    /* parse contents info */
    if (datasize < 5 || read(fd, tmp, 5) != 5)
        return false;
    codepage = convert_smaf_codetype(tmp[2]);

    if (codepage < 0)
    {
        DEBUGF("metadata error: smaf unsupport codetype: %d\n", tmp[2]);
        return false;
    }

    datasize -= 5;
    while ((id3->title == NULL || id3->artist == NULL || id3->composer == NULL)
           && (datasize > 0 && bufsize > 0))
    {
        if (datasize < 3 || read(fd, tmp, 3) != 3)
            return false;
        datasize -= 3;

        if (tmp[2] != ':')
        {
            DEBUGF("metadata error: illegal tag: %c%c%c\n", tmp[0], tmp[1], tmp[2]);
            return false;
        }
        switch ((tmp[0]<<8)|tmp[1])
        {
            case TAG_TITLE:
                id3->title = buf;
                valsize = read_audio_track_contents(fd, codepage, datasize,
                                                    &buf, &bufsize);
                break;
            case TAG_ARTIST:
                id3->artist = buf;
                valsize = read_audio_track_contents(fd, codepage, datasize,
                                                    &buf, &bufsize);
                break;
            case TAG_COMPOSER:
                id3->composer = buf;
                valsize = read_audio_track_contents(fd, codepage, datasize,
                                                    &buf, &bufsize);
                break;
            default:
                valsize = read_audio_track_contents(fd, codepage, datasize,
                                                    NULL, &bufsize);
                break;
        }
        if (valsize <= 0 || (unsigned int)valsize > datasize)
            return false;
        datasize -= valsize;
    }

    /* search PCM Audio Track Chunk */
    off_t content_end = 16 + (off_t)chunksize;
    if (content_end < 16 || content_end > filesize(fd) ||
        lseek(fd, content_end, SEEK_SET) != content_end)
        return false;

    chunksize = search_chunk(fd, "ATR", 3);
    if (chunksize == 0)
    {
        DEBUGF("metadata error: missing PCM Audio Track Chunk\n");
        return false;
    }

    /*
     * get format
     *  tmp
     *    +0: Format Type
     *    +1: Sequence Type
     *    +2: bit 7 0:mono/1:stereo, bit 4-6 format, bit 0-3: frequency
     *    +3: bit 4-7: base bit
     *    +4: TimeBase_D
     *    +5: TimeBase_G
     *
     * Note: If PCM Audio Track does not include Sequence Data Chunk,
     *       tmp+6 is the start position of Wave Data Chunk.
     */
    if (chunksize < 6 || read(fd, tmp, 6) != 6)
        return false;

    /* search Wave Data Chunk */
    chunksize = search_chunk(fd, "Awa", 3);
    if (chunksize == 0)
    {
        DEBUGF("metadata error: missing Wave Data Chunk\n");
        return false;
    }

    /* set track length and bitrate */
    id3->frequency = convert_smaf_audio_frequency(tmp[2] & 0x0f);
    set_length(id3, tmp[2], tmp[3] >> 4, chunksize);
    return true;
}

static bool parse_smaf_score_track(int fd, struct mp3entry *id3)
{
    /* temporary buffer */
    unsigned char *tmp = (unsigned char*)id3->path;
    unsigned char *p = tmp;
    /* contents stored buffer */
    unsigned char *buf = id3->id3v2buf;
    int bufsize = sizeof(id3->id3v2buf);

    unsigned int chunksize;
    unsigned int datasize;
    int valsize;

    int codepage;

    /* parse Optional Data Chunk */
    if (read(fd, tmp, 21) != 21)
        return false;
    if (memcmp(tmp + 5, "OPDA", 4) != 0)
    {
        DEBUGF("metadata error: missing Optional Data Chunk\n");
        return false;
    }

    /* Optional Data Chunk size */
    chunksize = get_long_be(tmp + 9);
    if (chunksize < 8 || 29u + (uint64_t)chunksize > (uint64_t)id3->filesize)
        return false;

    /* parse Data Chunk */
    if (memcmp(tmp + 13, "Dch", 3) != 0)
    {
        DEBUGF("metadata error: missing Data Chunk\n");
        return false;
    }

    codepage = convert_smaf_codetype(tmp[16]);
    if (codepage < 0)
    {
        DEBUGF("metadata error: smaf unsupport codetype: %d\n", tmp[16]);
        return false;
    }

    /* Data Chunk size */
    datasize = get_long_be(tmp + 17);
    while ((id3->title == NULL || id3->artist == NULL || id3->composer == NULL)
           && (datasize > 0 && bufsize > 0))
    {
        if (datasize < 4)
            return false;
        if (read(fd, tmp, 4) != 4)
            return false;

        valsize = (tmp[2] << 8) | tmp[3];
        if ((unsigned int)valsize > datasize - 4)
            return false;
        datasize -= valsize + 4;
        switch ((tmp[0]<<8)|tmp[1])
        {
            case TAG_TITLE:
                id3->title = buf;
                if (!read_score_track_contents(fd, codepage, valsize,
                                               &buf, &bufsize))
                    return false;
                break;
            case TAG_ARTIST:
                id3->artist = buf;
                if (!read_score_track_contents(fd, codepage, valsize,
                                               &buf, &bufsize))
                    return false;
                break;
            case TAG_COMPOSER:
                id3->composer = buf;
                if (!read_score_track_contents(fd, codepage, valsize,
                                               &buf, &bufsize))
                    return false;
                break;
            default:
                if (lseek(fd, valsize, SEEK_CUR) < 0)
                    return false;
                break;
        }
    }

    /* search Score Track Chunk */
    off_t optional_end = 29 + (off_t)chunksize;
    if (optional_end < 29 || (uint64_t)optional_end > id3->filesize ||
        lseek(fd, optional_end, SEEK_SET) != optional_end)
        return false;

    if (search_chunk(fd, "MTR", 3) == 0)
    {
        DEBUGF("metadata error: missing Score Track Chunk\n");
        return false;
    }

    /*
     * search next chunk
     * usually, next chunk ('M***') found within 40 bytes.
     */
    chunksize = 40;
    int search_size = read(fd, tmp, chunksize);
    if (search_size <= 0)
        return false;

    tmp[search_size] = 'M'; /* stopper */
    while (*p != 'M')
        p++;

    chunksize = search_size - (p - tmp);
    if (chunksize == 0)
    {
        DEBUGF("metadata error: missing Score Track Stream PCM Data Chunk");
        return false;
    }

    /* search Score Track Stream PCM Data Chunk */
    if (lseek(fd, -(off_t)chunksize, SEEK_CUR) < 0)
        return false;
    if (search_chunk(fd, "Mtsp", 4) == 0)
    {
        DEBUGF("metadata error: missing Score Track Stream PCM Data Chunk\n");
        return false;
    }

    /*
     * parse Score Track Stream Wave Data Chunk
     *  tmp
     *    +4-7: chunk size (WaveType(3bytes) + wave data count)
     *    +8:   bit 7 0:mono/1:stereo, bit 4-6 format, bit 0-3: base bit
     *    +9:   frequency (MSB)
     *    +10:  frequency (LSB)
     */
    if (read(fd, tmp, 11) != 11)
        return false;
    if (memcmp(tmp, "Mwa", 3) != 0)
    {
        DEBUGF("metadata error: missing Score Track Stream Wave Data Chunk\n");
        return false;
    }

    /* set track length and bitrate */
    unsigned int wave_size = get_long_be(tmp + 4);
    if (wave_size < 3)
        return false;
    id3->frequency = (tmp[9] << 8) | tmp[10];
    set_length(id3, tmp[8], tmp[8] & 0x0f, wave_size - 3);
    return true;
}

bool get_smaf_metadata(int fd, struct mp3entry* id3)
{
    /* temporary buffer */
    unsigned char *tmp = (unsigned char *)id3->path;
    unsigned int chunksize;

    id3->title    = NULL;
    id3->artist   = NULL;
    id3->composer = NULL;

    id3->vbr      = false;   /* All SMAF files are CBR */
    off_t file_size = filesize(fd);
    if (file_size < 16)
        return false;
    id3->filesize = file_size;

    /* check File Chunk and Contents Info Chunk */
    lseek(fd, 0, SEEK_SET);

    if (read(fd, tmp, 16) != 16 || (memcmp(tmp, "MMMD", 4) != 0) || (memcmp(tmp + 8, "CNTI", 4) != 0))
    {
        DEBUGF("metadata error: no smaf format\n");
        return false;
    }

    chunksize = get_long_be(tmp + 12);
    if (chunksize > 5)
        return parse_smaf_audio_track(fd, id3, chunksize);

    return parse_smaf_score_track(fd, id3);
}
