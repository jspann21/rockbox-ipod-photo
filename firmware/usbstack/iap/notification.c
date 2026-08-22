/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 by Sho Tanimoto
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
#include "playback.h"

#include "iap-usb.h"
#include "libiap/iap.h"
#include "platform.h"

#include "macros.h"

extern bool iap_initialized;

static struct IAPContext* acquire_initialized_ctx(void) {
    if(!iap_initialized) {
        return NULL;
    }

    struct IAPContext* ctx = _iap_acquire_ctx(true);
    if(!iap_initialized) {
        _iap_release_ctx();
        return NULL;
    }
    return ctx;
}

void iap_on_track_time_position(uint32_t pos_ms) {
    struct IAPContext* ctx = acquire_initialized_ctx();
    if(ctx == NULL) {
        return;
    }
    iap_notify_track_time_position(ctx, pos_ms);
    _iap_release_ctx();
}

void iap_on_track_playback_index(uint32_t index, bool track_ready) {
    struct IAPContext* ctx = acquire_initialized_ctx();
    if(ctx == NULL) {
        return;
    }

    if(track_ready) {
        /* called from from audio_finish_load_track() */
        goto notify;
    }

    /* called from audio_playlist_track_change() */
    struct Platform* plt = ctx->platform;
    if(plt->aa_slot < 0) {
        goto notify;
    }
    if(playback_current_aa_hid(plt->aa_slot) >= 0) {
        /* artwork ready, maybe preloaded track */
        goto notify;
    } else {
        /* artwork not ready, maybe after a playlist jump.
         * in this case, we will be called again from audio_finish_load_track(),
         * with track_ready == true. */
        goto exit;
    }
notify:
    iap_notify_track_playback_index(ctx, index);
exit:
    _iap_release_ctx();
}

void iap_on_tracks_count(uint32_t count) {
    struct IAPContext* ctx = acquire_initialized_ctx();
    if(ctx == NULL) {
        return;
    }
    iap_notify_tracks_count(ctx, count);
    _iap_release_ctx();
}

void iap_on_play_status(int status) {
    struct IAPContext* ctx = acquire_initialized_ctx();
    if(ctx == NULL) {
        return;
    }
    iap_notify_play_status(ctx, _iap_convert_play_status(status));
    _iap_release_ctx();
}

void iap_on_volume(int volume) {
    struct IAPContext* ctx = acquire_initialized_ctx();
    if(ctx == NULL) {
        return;
    }
    iap_notify_volume(ctx, _iap_convert_volume(volume), iap_false);
    _iap_release_ctx();
}

void iap_on_shuffle_state(bool state) {
    struct IAPContext* ctx = acquire_initialized_ctx();
    if(ctx == NULL) {
        return;
    }
    iap_notify_shuffle_state(ctx, _iap_convert_shuffle_state(state));
    _iap_release_ctx();
}

void iap_on_repeat_state(int state) {
    struct IAPContext* ctx = acquire_initialized_ctx();
    if(ctx == NULL) {
        return;
    }
    iap_notify_repeat_state(ctx, _iap_convert_repeat_state(state));
    _iap_release_ctx();
}
