/***************************************************************************
 * Minimal EXIF metadata reader for the JPEG image viewer.
 ****************************************************************************/
#ifndef JPEG_EXIF_H
#define JPEG_EXIF_H

#include "plugin.h"

struct jpeg_exif
{
    unsigned short orientation;
    unsigned long thumbnail_offset;
    unsigned long thumbnail_length;
};

void jpeg_exif_reset(struct jpeg_exif *exif);
bool jpeg_exif_read(const char *filename, int offset, int filesize,
                    unsigned char *scratch, size_t scratch_size,
                    struct jpeg_exif *exif);

#endif
