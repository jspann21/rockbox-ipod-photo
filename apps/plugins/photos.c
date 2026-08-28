/***************************************************************************
 * Native synced-photo launcher for iPod Color/Photo.
 ****************************************************************************/
#include "plugin.h"

#define IPOD_PHOTO_DATABASE "/iPod_Control/Photos/Photo Database"

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;

    if (!rb->file_exists(IPOD_PHOTO_DATABASE))
    {
        rb->splash(HZ * 2, "No synced photos");
        return PLUGIN_OK;
    }

    return rb->plugin_open(VIEWERS_DIR "/imageviewer.rock",
                           IPOD_PHOTO_DATABASE);
}
