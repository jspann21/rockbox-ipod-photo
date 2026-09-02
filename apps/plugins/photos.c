/***************************************************************************
 * Native synced-photo launcher for iPod Color/Photo.
 ****************************************************************************/
#include "plugin.h"

#define IPOD_PHOTO_DATABASE "/Photos/Photo Database"
#define IPOD_PHOTO_DATABASE_ALT "/iPod_Control/Photos/Photo Database"

enum plugin_status plugin_start(const void *parameter)
{
    const char *database = IPOD_PHOTO_DATABASE;

    (void)parameter;

    if (!rb->file_exists(database))
        database = IPOD_PHOTO_DATABASE_ALT;

    if (!rb->file_exists(database))
    {
        rb->splash(HZ * 2, "No synced photos");
        return PLUGIN_OK;
    }

    return rb->plugin_open(VIEWERS_DIR "/imageviewer.rock",
                           database);
}
