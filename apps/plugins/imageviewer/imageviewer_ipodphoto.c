/***************************************************************************
 * iPod Color/Photo integration wrapper for Image Viewer.
 *
 * Ordinary Image Viewer sessions use the real plugin API unchanged. When
 * launched on Apple's Photo Database, present it as one multi-frame source,
 * map photo navigation onto Image Viewer's NEXT_FRAME path, and keep the disk
 * awake while the library is open so native F1013 reads remain responsive.
 ****************************************************************************/
#include "plugin.h"

#define IPOD_PHOTO_DATABASE "/Photos/Photo Database"

static const struct plugin_api *imgviewer_rb;

#define rb imgviewer_rb
#define plugin_start imageviewer_core_start
#include "imageviewer.c"
#undef plugin_start
#undef rb

static const struct plugin_api *real_rb;
static struct plugin_api photo_api;
static struct tree_context photo_tree;
static struct entry photo_entry;
static char photo_database_name[] = "Photo Database";
static bool photo_session;

static bool is_photo_database(const void *parameter)
{
    const char *path = parameter;
    const char *name;

    if (path == NULL)
        return false;

    if (!real_rb->strcmp(path, IPOD_PHOTO_DATABASE))
        return true;

    name = real_rb->strrchr(path, '/');
    return name != NULL && !real_rb->strcmp(name + 1, photo_database_name);
}

static struct tree_context *photo_tree_get_context(void)
{
    if (photo_session)
        return &photo_tree;

    return real_rb->tree_get_context();
}

static struct entry *photo_tree_get_entries(struct tree_context *tree)
{
    if (photo_session && tree == &photo_tree)
        return &photo_entry;

    return real_rb->tree_get_entries(tree);
}

static long photo_map_button(long button)
{
    long base;

    if (!photo_session || (button & (BUTTON_REL | BUTTON_REPEAT)))
        return button;

    base = button & ~(BUTTON_REPEAT | BUTTON_REL);

    if (base == BUTTON_RIGHT || base == BUTTON_SCROLL_FWD ||
        base == (BUTTON_SELECT | BUTTON_RIGHT))
    {
        /* The Photo Database decoder reads this one-shot direction before
           Image Viewer recenters the next frame. Clear queued wheel/button
           events so one deliberate action advances exactly one photo. */
        image_info.x = 1;
        real_rb->button_clear_queue();
        return BUTTON_NONE;
    }

    if (base == BUTTON_LEFT || base == BUTTON_SCROLL_BACK ||
        base == (BUTTON_SELECT | BUTTON_LEFT))
    {
        image_info.x = -1;
        real_rb->button_clear_queue();
        return BUTTON_NONE;
    }

    return button;
}

static long photo_button_get(bool block)
{
    return photo_map_button(real_rb->button_get(block));
}

static long photo_button_get_w_tmo(int ticks)
{
    return photo_map_button(real_rb->button_get_w_tmo(ticks));
}

static void photo_storage_spindown(int seconds)
{
    (void)seconds;
    /* F1013 changes are fast once the disk is awake. Keep it awake only while
       Photos is active; plugin_start restores the user's normal setting. */
    real_rb->storage_spindown(0);
}

enum plugin_status plugin_start(const void *parameter)
{
    enum plugin_status status;

    real_rb = rb;
    imgviewer_rb = real_rb;
    photo_session = is_photo_database(parameter);

    if (photo_session)
    {
        real_rb->memset(&photo_tree, 0, sizeof(photo_tree));
        real_rb->memset(&photo_entry, 0, sizeof(photo_entry));
        photo_tree.filesindir = 1;
        photo_tree.dirlength = 1;
        photo_entry.name = photo_database_name;

        photo_api = *real_rb;
        photo_api.tree_get_context = photo_tree_get_context;
        photo_api.tree_get_entries = photo_tree_get_entries;
        photo_api.button_get = photo_button_get;
        photo_api.button_get_w_tmo = photo_button_get_w_tmo;
        photo_api.storage_spindown = photo_storage_spindown;
        imgviewer_rb = &photo_api;

        real_rb->storage_spindown(0);
    }

    status = imageviewer_core_start(parameter);

    if (photo_session)
        real_rb->storage_spindown(real_rb->global_settings->disk_spindown);

    imgviewer_rb = real_rb;
    photo_session = false;
    return status;
}
