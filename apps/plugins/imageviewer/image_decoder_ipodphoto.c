/***************************************************************************
 * iPod Color/Photo Image Viewer decoder routing.
 *
 * Keep the stock decoder loader/type detector intact and add one target-local
 * source: Apple's Photo Database, decoded by photodb.ovl.
 ****************************************************************************/
#include "plugin.h"
#include "image_decoder.h"

#define get_image_type get_image_type_core
#define load_decoder load_decoder_core
#include "image_decoder.c"
#undef load_decoder
#undef get_image_type

static bool is_photo_database_file(const char *name)
{
    const char *base;
    char magic[4];
    int fd;
    bool match = false;

    if (name == NULL)
        return false;

    base = rb->strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    if (rb->strcmp(base, "Photo Database"))
        return false;

    fd = rb->open(name, O_RDONLY);
    if (fd < 0)
        return false;

    if (rb->read(fd, magic, sizeof(magic)) == (ssize_t)sizeof(magic) &&
        !rb->memcmp(magic, "mhfd", sizeof(magic)))
        match = true;

    rb->close(fd);
    return match;
}

enum image_type get_image_type(const char *name, bool quiet)
{
    if (is_photo_database_file(name))
        return IMAGE_PHOTODB;

    return get_image_type_core(name, quiet);
}

const struct image_decoder *load_decoder(struct loader_info *loader_info)
{
    if (loader_info->type == IMAGE_PHOTODB)
        decoder_names[IMAGE_PHOTODB] = "photodb";

    return load_decoder_core(loader_info);
}
