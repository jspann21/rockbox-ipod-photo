/***************************************************************************
 * Minimal EXIF/TIFF parser.
 *
 * Reads only metadata needed by imageviewer: Orientation and the optional
 * embedded JPEG thumbnail locator. Unknown/malformed EXIF is ignored and the
 * image remains orientation 1.
 ****************************************************************************/

#include "jpeg_exif.h"

#define JPEG_EXIF_SCAN_LIMIT (64 * 1024)
#define TIFF_TAG_ORIENTATION  0x0112
#define TIFF_TAG_JPEG_OFFSET  0x0201
#define TIFF_TAG_JPEG_LENGTH  0x0202

static unsigned short exif_u16(const unsigned char *p, bool little)
{
    if (little)
        return (unsigned short)(p[0] | (p[1] << 8));
    return (unsigned short)((p[0] << 8) | p[1]);
}

static unsigned long exif_u32(const unsigned char *p, bool little)
{
    if (little)
        return (unsigned long)p[0] |
               ((unsigned long)p[1] << 8) |
               ((unsigned long)p[2] << 16) |
               ((unsigned long)p[3] << 24);

    return ((unsigned long)p[0] << 24) |
           ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) |
           (unsigned long)p[3];
}

static bool exif_range(size_t size, unsigned long offset, size_t bytes)
{
    return offset <= size && bytes <= size - offset;
}

static bool exif_ifd(const unsigned char *tiff, size_t tiff_size,
                     unsigned long offset, bool little,
                     struct jpeg_exif *exif, bool thumbnail_ifd,
                     unsigned long tiff_file_offset)
{
    unsigned short count;
    unsigned int i;

    if (!exif_range(tiff_size, offset, 2))
        return false;

    count = exif_u16(tiff + offset, little);
    if (count > 128)
        return false;

    offset += 2;
    if (!exif_range(tiff_size, offset, (size_t)count * 12 + 4))
        return false;

    for (i = 0; i < count; i++)
    {
        const unsigned char *entry = tiff + offset + i * 12;
        unsigned short tag = exif_u16(entry, little);
        unsigned short type = exif_u16(entry + 2, little);
        unsigned long values = exif_u32(entry + 4, little);

        if (!thumbnail_ifd && tag == TIFF_TAG_ORIENTATION &&
            type == 3 && values == 1)
        {
            unsigned short orientation = exif_u16(entry + 8, little);
            if (orientation >= 1 && orientation <= 8)
                exif->orientation = orientation;
        }
        else if (thumbnail_ifd && tag == TIFF_TAG_JPEG_OFFSET &&
                 type == 4 && values == 1)
        {
            unsigned long relative = exif_u32(entry + 8, little);
            if (relative <= 0xffffffffUL - tiff_file_offset)
                exif->thumbnail_offset = tiff_file_offset + relative;
        }
        else if (thumbnail_ifd && tag == TIFF_TAG_JPEG_LENGTH &&
                 type == 4 && values == 1)
        {
            exif->thumbnail_length = exif_u32(entry + 8, little);
        }
    }

    return true;
}

static void exif_parse_app1(const unsigned char *payload, size_t payload_size,
                            unsigned long payload_file_offset,
                            struct jpeg_exif *exif)
{
    const unsigned char *tiff;
    size_t tiff_size;
    bool little;
    unsigned long ifd0;
    unsigned short count;
    unsigned long next_pos;
    unsigned long ifd1;

    if (payload_size < 14 ||
        rb->memcmp(payload, "Exif\0\0", 6) != 0)
        return;

    tiff = payload + 6;
    tiff_size = payload_size - 6;

    if (tiff[0] == 'I' && tiff[1] == 'I')
        little = true;
    else if (tiff[0] == 'M' && tiff[1] == 'M')
        little = false;
    else
        return;

    if (exif_u16(tiff + 2, little) != 42)
        return;

    ifd0 = exif_u32(tiff + 4, little);
    if (!exif_ifd(tiff, tiff_size, ifd0, little, exif, false,
                  payload_file_offset + 6))
        return;

    if (!exif_range(tiff_size, ifd0, 2))
        return;

    count = exif_u16(tiff + ifd0, little);
    next_pos = ifd0 + 2 + (unsigned long)count * 12;
    if (!exif_range(tiff_size, next_pos, 4))
        return;

    ifd1 = exif_u32(tiff + next_pos, little);
    if (ifd1 != 0)
        exif_ifd(tiff, tiff_size, ifd1, little, exif, true,
                 payload_file_offset + 6);

    if (exif->thumbnail_offset != 0 && exif->thumbnail_length != 0)
    {
        unsigned long relative =
            exif->thumbnail_offset - (payload_file_offset + 6);

        if (!exif_range(tiff_size, relative, exif->thumbnail_length) ||
            exif->thumbnail_length < 4 ||
            tiff[relative] != 0xff || tiff[relative + 1] != 0xd8 ||
            tiff[relative + exif->thumbnail_length - 2] != 0xff ||
            tiff[relative + exif->thumbnail_length - 1] != 0xd9)
        {
            exif->thumbnail_offset = 0;
            exif->thumbnail_length = 0;
        }
    }
    else
    {
        exif->thumbnail_offset = 0;
        exif->thumbnail_length = 0;
    }
}

void jpeg_exif_reset(struct jpeg_exif *exif)
{
    exif->orientation = 1;
    exif->thumbnail_offset = 0;
    exif->thumbnail_length = 0;
}

bool jpeg_exif_read(const char *filename, int offset, int filesize,
                    unsigned char *scratch, size_t scratch_size,
                    struct jpeg_exif *exif)
{
    int fd;
    ssize_t got;
    size_t size;
    size_t pos;

    jpeg_exif_reset(exif);

    if (scratch == NULL || scratch_size < 16)
        return false;

    fd = rb->open(filename, O_RDONLY);
    if (fd < 0)
        return false;

    if (offset > 0 && rb->lseek(fd, offset, SEEK_SET) < 0)
    {
        rb->close(fd);
        return false;
    }

    if (filesize <= 0)
    {
        off_t total = rb->filesize(fd);
        if (total <= offset)
        {
            rb->close(fd);
            return false;
        }
        filesize = (int)(total - offset);
    }

    size = (size_t)filesize;
    if (size > scratch_size)
        size = scratch_size;
    if (size > JPEG_EXIF_SCAN_LIMIT)
        size = JPEG_EXIF_SCAN_LIMIT;

    got = rb->read(fd, scratch, size);
    rb->close(fd);
    if (got < 4)
        return false;

    size = (size_t)got;
    if (scratch[0] != 0xff || scratch[1] != 0xd8)
        return false;

    pos = 2;
    while (pos + 4 <= size)
    {
        unsigned int marker;
        unsigned int length;
        size_t payload_pos;
        size_t payload_size;

        while (pos < size && scratch[pos] == 0xff)
            pos++;
        if (pos >= size)
            break;

        marker = scratch[pos++];
        if (marker == 0xda || marker == 0xd9)
            break;
        if (marker == 0x00 || marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd8))
            continue;

        if (pos + 2 > size)
            break;

        length = ((unsigned int)scratch[pos] << 8) | scratch[pos + 1];
        if (length < 2)
            break;

        payload_pos = pos + 2;
        payload_size = length - 2;
        if (payload_pos > size || payload_size > size - payload_pos)
            break;

        if (marker == 0xe1)
        {
            exif_parse_app1(scratch + payload_pos, payload_size,
                            (unsigned long)offset + payload_pos, exif);
            if (exif->orientation != 1 ||
                exif->thumbnail_length != 0)
                return true;
        }

        pos = payload_pos + payload_size;
    }

    return true;
}
