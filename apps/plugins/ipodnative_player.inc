#define IPVF_AUDIO_PREBUFFER_FRAMES (2u * 44100u)

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
static void write_qualification_log(const char *filename,
                                    const struct ipvf_info *info,
                                    const struct ipvf_stats *stats,
                                    const char *status)
{
    const char *name = rb->strrchr(filename, '/');
    bool new_file;
    int log_fd;

    if (!rb->file_exists(IPVF_QUALIFICATION_MARKER))
        return;
    new_file = !rb->file_exists(IPVF_QUALIFICATION_LOG);
    log_fd = rb->open(IPVF_QUALIFICATION_LOG,
                      O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (log_fd < 0)
        return;
    if (name != NULL)
        name++;
    else
        name = filename;
    if (new_file)
    {
        rb->fdprintf(log_fd,
            "file\tbuild\tdecoder_rev\tstatus\tstart_tick\thz\t"
            "elapsed_ticks\t"
            "frames\ttotal_frames\tfps_num\tfps_den\tfile_bytes\t"
            "late_gt_500us\tmax_late_us\t"
            "audio_gaps\ttype_key\ttype_rect\ttype_repeat\t"
            "type_key_lz4\ttype_rect_lz4\ttype_xor_lz4\t"
            "read_us\tmax_read_us\tvideo_decode_us\t"
            "max_video_decode_us\tlz4_decode_us\tmax_lz4_decode_us\t"
            "temporal_check_us\tmax_temporal_check_us\t"
            "temporal_reconstruct_us\tmax_temporal_reconstruct_us\t"
            "video_copy_us\tmax_video_copy_us\taudio_feed_us\t"
            "max_audio_feed_us\tprebuffer_us\trender_us\t"
            "max_render_us\trender_slot_wait_us\t"
            "max_render_slot_wait_us\trender_calls\trectangle_calls\t"
            "render_failures\t"
            "record_bytes\tstored_video_bytes\tdecoded_video_bytes\t"
            "audio_bytes\taudio_frames\tpadding_bytes\t"
            "max_record_sectors\tfinal_crc\terror_count\t"
            "first_error\tfirst_error_frame\trow_complete\n");
    }
    rb->fdprintf(log_fd,
        "%s\t%s\t%s\t%s\t%lu\t%d\t%lu\t%lu\t%lu\t%u\t%u\t%ld\t"
        "%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t"
        "%llu\t%lu\t%llu\t%lu\t%llu\t%lu\t%llu\t%lu\t%llu\t%lu\t"
        "%llu\t%lu\t%llu\t%lu\t%llu\t%llu\t%lu\t"
        "%llu\t%lu\t%lu\t%lu\t%lu\t%llu\t%llu\t%llu\t%llu\t%llu\t"
        "%llu\t%lu\t%08lx\t%lu\t%lu\t%lu\t1\n",
        name, rb->rbversion, IPVF_DECODER_REV, status,
        stats->start_tick, HZ,
        stats->elapsed_ticks, stats->frames, info->frame_count,
        info->fps_num, info->fps_den, (long)info->file_size,
        stats->late_frames, stats->max_late_us,
        stats->audio_underruns,
        stats->type_counts[IPVF_TYPE_KEY],
        stats->type_counts[IPVF_TYPE_RECTS],
        stats->type_counts[IPVF_TYPE_REPEAT],
        stats->type_counts[IPVF_TYPE_KEY_LZ4],
        stats->type_counts[IPVF_TYPE_RECTS_LZ4],
        stats->type_counts[IPVF_TYPE_XOR_LZ4],
        (unsigned long long)stats->read_us, stats->max_read_us,
        (unsigned long long)stats->video_decode_us,
        stats->max_video_decode_us,
        (unsigned long long)stats->lz4_decode_us,
        stats->max_lz4_decode_us,
        (unsigned long long)stats->temporal_check_us,
        stats->max_temporal_check_us,
        (unsigned long long)stats->temporal_reconstruct_us,
        stats->max_temporal_reconstruct_us,
        (unsigned long long)stats->video_copy_us,
        stats->max_video_copy_us,
        (unsigned long long)stats->audio_feed_us,
        stats->max_audio_feed_us,
        (unsigned long long)stats->prebuffer_us,
        (unsigned long long)stats->render_us, stats->max_render_us,
        (unsigned long long)stats->render_slot_wait_us,
        stats->max_render_slot_wait_us,
        stats->render_calls, stats->rectangle_calls,
        stats->render_failures,
        (unsigned long long)stats->record_bytes,
        (unsigned long long)stats->stored_video_bytes,
        (unsigned long long)stats->decoded_video_bytes,
        (unsigned long long)stats->audio_bytes,
        (unsigned long long)stats->audio_frames,
        (unsigned long long)stats->padding_bytes,
        stats->max_record_sectors, (unsigned long)stats->final_crc,
        stats->error_count, stats->first_error,
        stats->first_error_frame);
    rb->close(log_fd);
}

static void note_qualification_error(struct ipvf_stats *stats,
                                     enum ipvf_error_code code,
                                     unsigned long frame)
{
    stats->error_count++;
    if (stats->first_error == IPVF_ERROR_NONE)
    {
        stats->first_error = code;
        stats->first_error_frame = frame;
    }
}
#define IPVF_NOTE_QUALIFICATION_ERROR(stats, code, frame) \
    note_qualification_error((stats), (code), (frame))
#else
#define IPVF_NOTE_QUALIFICATION_ERROR(stats, code, frame) ((void)0)
#endif

#define IPVF_BUTTON_DRAIN_LIMIT 16u
#define IPVF_PENDING_SEEK_LIMIT 1024

static void add_pending_seek(int *steps, int direction)
{
    if (direction > 0)
    {
        if (*steps < IPVF_PENDING_SEEK_LIMIT)
            (*steps)++;
    }
    else if (direction < 0 && *steps > -IPVF_PENDING_SEEK_LIMIT)
    {
        (*steps)--;
    }
}

static void adjust_playback_volume(int delta)
{
    int volume;
    int minimum;
    int maximum;

    if (delta == 0)
        return;

    volume = rb->global_status->volume + delta;
    minimum = rb->sound_min(SOUND_VOLUME);
    maximum = rb->sound_max(SOUND_VOLUME);

    if (volume < minimum)
        volume = minimum;
    else if (volume > maximum)
        volume = maximum;

    if (volume != rb->global_status->volume)
        rb->sound_set(SOUND_VOLUME, volume);
}

enum ipvf_pause_result
{
    IPVF_PAUSE_RESUMED = 0,
    IPVF_PAUSE_STOPPED,
    IPVF_PAUSE_USB,
    IPVF_PAUSE_SEEK_BACK,
    IPVF_PAUSE_SEEK_FORWARD,
    IPVF_PAUSE_FAILED,
};

static enum ipvf_pause_result pause_playback(unsigned long presented_frames)
{
    int volume_delta = 0;

#ifdef IPVF_NATIVE_PIPELINE
    if (!render_wait_presented(presented_frames))
        return IPVF_PAUSE_FAILED;
#else
    (void)presented_frames;
#endif
    if (!audio_pause_playback())
        return IPVF_PAUSE_FAILED;

    rb->button_clear_queue();
    while (true)
    {
        long button = rb->button_get_w_tmo(HZ / 4);
        long base_button;

        if (button == BUTTON_NONE)
            continue;
        base_button = button & ~(BUTTON_REL | BUTTON_REPEAT);
        if (base_button == BUTTON_SCROLL_FWD)
            volume_delta++;
        else if (base_button == BUTTON_SCROLL_BACK)
            volume_delta--;
        else if (base_button == BUTTON_PLAY &&
                 (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
        {
            adjust_playback_volume(volume_delta);
            rb->button_clear_queue();
            return audio_resume_playback() ?
                   IPVF_PAUSE_RESUMED : IPVF_PAUSE_FAILED;
        }
        else if (base_button == BUTTON_MENU &&
                 (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
        {
            adjust_playback_volume(volume_delta);
            return IPVF_PAUSE_STOPPED;
        }
        else if (base_button == BUTTON_LEFT &&
                 (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
        {
            adjust_playback_volume(volume_delta);
            return IPVF_PAUSE_SEEK_BACK;
        }
        else if (base_button == BUTTON_RIGHT &&
                 (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
        {
            adjust_playback_volume(volume_delta);
            return IPVF_PAUSE_SEEK_FORWARD;
        }
        else if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
        {
            adjust_playback_volume(volume_delta);
            return IPVF_PAUSE_USB;
        }

        if (volume_delta >= 4 || volume_delta <= -4)
        {
            adjust_playback_volume(volume_delta);
            volume_delta = 0;
        }
        rb->reset_poweroff_timer();
    }
}

/* Fill the ring with future audio while frame zero is being held on screen. */
static bool audio_prebuffer_records(int fd, const struct ipvf_info *info,
                                    unsigned char *read_buffer,
                                    unsigned long first_frame,
                                    off_t first_position,
                                    uint32_t first_sectors,
                                    unsigned long *end_frame)
{
    unsigned long scan_frame = first_frame;
    off_t scan_position = first_position;
    uint32_t scan_sectors = first_sectors;
    uint32_t target_frames = IPVF_AUDIO_PREBUFFER_FRAMES;

    if (info == NULL || read_buffer == NULL || end_frame == NULL ||
        audio.started)
        return false;
    if (audio.capacity_frames <= IPVF_AUDIO_DMA_MAX_FRAMES)
        return false;
    if (target_frames >
        audio.capacity_frames - IPVF_AUDIO_DMA_MAX_FRAMES)
        target_frames =
            audio.capacity_frames - IPVF_AUDIO_DMA_MAX_FRAMES;

    while (scan_frame < info->frame_count &&
           audio_used_frames() < target_frames)
    {
        struct ipvf_record_info record_info;
        const unsigned char *audio_payload;
        uint32_t record_bytes;
        uint32_t used;

        if (scan_sectors == 0 ||
            scan_sectors > IPVF_RECORD_MAX_SECTORS)
            return false;
        record_bytes = scan_sectors * IPVF_RECORD_SECTOR_SIZE;
        if (scan_position > info->media_end_offset ||
            (off_t)record_bytes > info->media_end_offset - scan_position ||
            !read_exact(fd, read_buffer, record_bytes) ||
            !parse_record(read_buffer, scan_sectors, scan_frame, info,
                          &record_info))
            return false;

        used = audio_used_frames();
        if (used > audio.capacity_frames)
            return false;
        /* audio is stopped, so audio_write_ima_block cannot wait for space. */
        if (record_info.audio_frames > audio.capacity_frames - used)
            break;

        audio_payload = read_buffer + IPVF_FRAME_HEADER_SIZE +
                        record_info.stored_video_bytes;
        if (!audio_write_ima_block(audio_payload, record_info.audio_bytes,
                                   record_info.audio_frames))
            return false;

        scan_position += record_bytes;
        scan_sectors = record_info.next_sectors;
        scan_frame++;
    }

    *end_frame = scan_frame;
    return true;
}

static enum plugin_status play_file(const char *filename)
{
    struct ipvf_info info;
    struct ipvf_stats stats;
    size_t buf_size;
    uintptr_t buffer_end;
    uintptr_t decoded_address;
    uintptr_t read_address;
    uintptr_t required_end;
    unsigned char *buf;
    unsigned char *decoded_buffer;
    unsigned char *read_buffer;
    struct viewport *vp;
    fb_data *fb;
    uint32_t current_sectors;
    off_t file_position = IPVF_DATA_OFFSET;
#ifdef IPVF_NATIVE_PIPELINE
    off_t last_key_offset = -1;
    uint32_t last_key_sectors = 0;
    unsigned long last_key_frame = 0;
    off_t seek_key_offset = -1;
    uint32_t seek_key_sectors = 0;
    unsigned long seek_key_frame = 0;
#endif
    unsigned long frame;
    unsigned long prebuffer_end_frame = 1;
    unsigned long render_frames = 0;
    unsigned long seek_base_frame = 0;
    uint32_t seek_base_flags = 0;
    unsigned long seek_target = 0;
    int pending_seek_steps = 0;
    bool seeking = false;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    long playback_start_tick;
#endif
    int fd;
    bool failed = false;
    bool stopped = false;
    bool usb_connected = false;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    bool boosted = false;
#endif
    int old_spindown = rb->global_settings->disk_spindown;

    rb->memset(&info, 0, sizeof(info));
    rb->memset(&stats, 0, sizeof(stats));

    fd = rb->open(filename, O_RDONLY);
    if (fd < 0 || !read_header(fd, &info))
    {
        if (fd >= 0)
            rb->close(fd);
        rb->splash(HZ * 2, "Unsupported IPVF");
        return PLUGIN_ERROR;
    }

    buf = rb->plugin_get_buffer(&buf_size);
    if (buf == NULL)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF buffer unavailable");
        return PLUGIN_ERROR;
    }
    buffer_end = (uintptr_t)buf + buf_size;
    decoded_address = ((uintptr_t)buf + IPVF_RECORD_SECTOR_SIZE - 1) &
                      ~(uintptr_t)(IPVF_RECORD_SECTOR_SIZE - 1);
#ifdef IPVF_NATIVE_PIPELINE
    read_address = decoded_address +
                   IPVF_RENDER_SLOTS * IPVF_RENDER_SLOT_STRIDE;
#else
    read_address = decoded_address + IPVF_DECODED_SLOT_STRIDE;
#endif
    required_end = read_address + IPVF_RECORD_MAX_BYTES;
    if (buffer_end < (uintptr_t)buf ||
        decoded_address < (uintptr_t)buf ||
        read_address < decoded_address || required_end < read_address ||
        required_end > buffer_end)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF buffer too small");
        return PLUGIN_ERROR;
    }
    decoded_buffer = (unsigned char *)decoded_address;
    read_buffer = (unsigned char *)read_address;

    rb->lcd_set_viewport(NULL);
    if (rb->screens[SCREEN_MAIN] == NULL ||
        rb->screens[SCREEN_MAIN]->current_viewport == NULL)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "Unsupported framebuffer");
        return PLUGIN_ERROR;
    }
    vp = *(rb->screens[SCREEN_MAIN]->current_viewport);
    if (vp == NULL || vp->buffer == NULL ||
        vp->buffer->stride != LCD_WIDTH)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "Unsupported framebuffer");
        return PLUGIN_ERROR;
    }
    fb = vp->buffer->fb_ptr;

    /* Indexed seeking reconstructs from a keyframe for every video mode, so
     * retain a full reference frame even when temporal XOR is absent. */
    if (!ipodnative_audio_init(true))
    {
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF audio unavailable");
        return PLUGIN_ERROR;
    }

    rb->lcd_set_backdrop(NULL);
    rb->lcd_clear_display();
    rb->lcd_update();
#ifdef IPVF_NATIVE_PIPELINE
    if (!render_start(decoded_buffer))
    {
        audio_shutdown();
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF pipeline unavailable");
        return PLUGIN_ERROR;
    }
#endif
    rb->button_clear_queue();
    backlight_ignore_timeout();
    rb->storage_spindown(0);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
    boosted = true;
#endif

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    stats.start_tick = (unsigned long)*rb->current_tick;
    playback_start_tick = (long)stats.start_tick;
#endif

    current_sectors = info.first_record_sectors;
    frame = 0;
    while (frame < info.frame_count)
    {
        unsigned char *decoded_record;
        unsigned char *record;
        const unsigned char *audio_payload;
        struct ipvf_record_info record_info;
        uint32_t record_bytes;
        uint32_t target, now, late;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        uint32_t operation_started, operation_us;
#endif
#ifdef IPVF_NATIVE_PIPELINE
        off_t frame_offset = file_position;
#endif
        long button;

#ifdef IPVF_NATIVE_PIPELINE
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        operation_started = USEC_TIMER;
#endif
        decoded_record = render_acquire();
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        operation_us = USEC_TIMER - operation_started;
        add_qualification_timing(&stats.render_slot_wait_us,
                                 &stats.max_render_slot_wait_us,
                                 operation_us);
#endif
        record = UNCACHED_ADDR(read_buffer);
        if (decoded_record == NULL)
        {
            IPVF_NOTE_QUALIFICATION_ERROR(&stats,
                                          IPVF_ERROR_RENDER_ACQUIRE, frame);
            failed = true;
            break;
        }
#else
        decoded_record = decoded_buffer;
        record = read_buffer;
#endif

        if (current_sectors == 0 ||
            current_sectors > IPVF_RECORD_MAX_SECTORS)
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            IPVF_NOTE_QUALIFICATION_ERROR(&stats, IPVF_ERROR_RECORD_CHAIN,
                                          frame);
            failed = true;
            break;
        }
        record_bytes = current_sectors * IPVF_RECORD_SECTOR_SIZE;
        if (file_position > info.media_end_offset ||
            (off_t)record_bytes > info.media_end_offset - file_position)
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            IPVF_NOTE_QUALIFICATION_ERROR(&stats, IPVF_ERROR_RECORD_BOUNDS,
                                          frame);
            failed = true;
            break;
        }
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        operation_started = USEC_TIMER;
#endif
        if (!read_exact(fd, record, record_bytes))
        {
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_us = USEC_TIMER - operation_started;
            add_qualification_timing(&stats.read_us,
                                     &stats.max_read_us, operation_us);
#endif
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            IPVF_NOTE_QUALIFICATION_ERROR(&stats, IPVF_ERROR_READ, frame);
            failed = true;
            break;
        }
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        operation_us = USEC_TIMER - operation_started;
        add_qualification_timing(&stats.read_us,
                                 &stats.max_read_us, operation_us);
#endif
        if (!parse_record(record, current_sectors, frame, &info,
                          &record_info))
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            IPVF_NOTE_QUALIFICATION_ERROR(&stats, IPVF_ERROR_PARSE, frame);
            failed = true;
            break;
        }
        if (seeking && frame == seek_base_frame &&
            (!record_info.keyframe ||
             ((seek_base_flags & IPVF_INDEX_FLAG_KEY_LZ4) != 0) !=
                 (record_info.source_type == IPVF_TYPE_KEY_LZ4)))
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            IPVF_NOTE_QUALIFICATION_ERROR(&stats, IPVF_ERROR_PARSE, frame);
            failed = true;
            break;
        }
        file_position += record_bytes;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        stats.record_bytes += record_bytes;
        stats.stored_video_bytes += record_info.stored_video_bytes;
        stats.decoded_video_bytes += record_info.decoded_video_bytes;
        stats.audio_bytes += record_info.audio_bytes;
        stats.audio_frames += record_info.audio_frames;
        stats.padding_bytes += record_bytes - IPVF_FRAME_HEADER_SIZE -
                               record_info.stored_video_bytes -
                               record_info.audio_bytes;
        stats.rectangle_calls += record_info.rect_count;
        stats.type_counts[record_info.source_type]++;
        if (current_sectors > stats.max_record_sectors)
            stats.max_record_sectors = current_sectors;
#endif

        audio_payload = record + IPVF_FRAME_HEADER_SIZE +
                        record_info.stored_video_bytes;
        if ((!seeking || frame >= seek_target) &&
            (!audio.started || frame >= prebuffer_end_frame))
        {
            bool audio_ok;

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_started = USEC_TIMER;
#endif
            audio_ok = audio_write_ima_block(
                audio_payload, record_info.audio_bytes,
                record_info.audio_frames);
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_us = USEC_TIMER - operation_started;
            add_qualification_timing(&stats.audio_feed_us,
                                     &stats.max_audio_feed_us,
                                     operation_us);
#endif
            if (!audio_ok)
            {
#ifdef IPVF_NATIVE_PIPELINE
                render_abandon();
#endif
                IPVF_NOTE_QUALIFICATION_ERROR(&stats,
                                              IPVF_ERROR_AUDIO_FEED, frame);
                failed = true;
                break;
            }
        }
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        operation_started = USEC_TIMER;
#endif
        if (!decode_record_video(record, &record_info, decoded_record,
                                 audio.video_scratch,
                                 audio.video_reference,
#ifdef IPVF_NATIVE_PIPELINE
                                 true,
#else
                                 false,
#endif
                                 &stats))
        {
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_us = USEC_TIMER - operation_started;
            add_qualification_timing(&stats.video_decode_us,
                                     &stats.max_video_decode_us,
                                     operation_us);
#endif
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            IPVF_NOTE_QUALIFICATION_ERROR(&stats,
                                          IPVF_ERROR_VIDEO_DECODE, frame);
            failed = true;
            break;
        }
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        operation_us = USEC_TIMER - operation_started;
        add_qualification_timing(&stats.video_decode_us,
                                 &stats.max_video_decode_us,
                                 operation_us);
#endif
#ifdef IPVF_NATIVE_PIPELINE
        if (record_info.keyframe && !seeking)
        {
            last_key_offset = frame_offset;
            last_key_sectors = current_sectors;
            last_key_frame = frame;
        }
#endif
        if (seeking && frame < seek_target)
        {
            if (((frame - seek_base_frame) & 7u) == 0)
            {
                unsigned int seek_events;

                for (seek_events = 0;
                     seek_events < IPVF_BUTTON_DRAIN_LIMIT;
                     seek_events++)
                {
                    long seek_button = rb->button_get(false);
                    long base_button;

                    if (seek_button == BUTTON_NONE)
                        break;
                    base_button =
                        seek_button & ~(BUTTON_REL | BUTTON_REPEAT);
                    if (base_button == BUTTON_LEFT &&
                        (seek_button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
                        add_pending_seek(&pending_seek_steps, -1);
                    else if (base_button == BUTTON_RIGHT &&
                             (seek_button &
                              (BUTTON_REL | BUTTON_REPEAT)) == 0)
                        add_pending_seek(&pending_seek_steps, 1);
                    else if (base_button == BUTTON_MENU &&
                             (seek_button &
                              (BUTTON_REL | BUTTON_REPEAT)) == 0)
                    {
                        stopped = true;
                        break;
                    }
                    else if (base_button != BUTTON_LEFT &&
                             base_button != BUTTON_RIGHT &&
                             base_button != BUTTON_SCROLL_FWD &&
                             base_button != BUTTON_SCROLL_BACK &&
                             base_button != BUTTON_PLAY &&
                             rb->default_event_handler(seek_button) ==
                             SYS_USB_CONNECTED)
                    {
                        usb_connected = true;
                        break;
                    }
                }
            }
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            if (stopped || usb_connected)
                break;
            current_sectors = record_info.next_sectors;
            frame++;
            continue;
        }
        if (seeking)
        {
            /* Rectangle and repeat records assume the previous LCD contents.
             * A seek reconstructs off-screen, then promotes the target to one
             * full-frame render so the display has no dependency on its old
             * position. */
            rb->memcpy(decoded_record + IPVF_FRAME_HEADER_SIZE,
                       audio.video_reference, IPVF_FRAME_BYTES);
            record_info.type = IPVF_TYPE_KEY;
            record_info.rect_count = 0;
            record_info.decoded_video_bytes = IPVF_FRAME_BYTES;
        }
        if (frame + 1 == info.frame_count)
            audio_mark_eof();

        if (frame != 0)
        {
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            uint32_t late_us;
#endif

            target = audio_frame_boundary(&info, frame);
            now = audio_clock();
            late = now > target ? now - target : 0;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            late_us = (uint32_t)
                ((uint64_t)late * 1000000u / info.audio_sample_rate);
            if (late_us > 500)
#else
            if (late >= IPVF_LATE_THRESHOLD_FRAMES)
#endif
            {
                stats.late_frames++;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
                if (late_us > stats.max_late_us)
                    stats.max_late_us = late_us;
#endif
            }
            else if (now < target && !audio_wait_until(target))
            {
#ifdef IPVF_NATIVE_PIPELINE
                render_abandon();
#endif
                IPVF_NOTE_QUALIFICATION_ERROR(&stats,
                                              IPVF_ERROR_AUDIO_CLOCK, frame);
                failed = true;
                break;
            }
        }

#ifdef IPVF_NATIVE_PIPELINE
        if (!render_queue(record_info.type, record_info.rect_count,
                          record_info.decoded_video_bytes))
        {
            IPVF_NOTE_QUALIFICATION_ERROR(&stats,
                                          IPVF_ERROR_RENDER_QUEUE, frame);
            failed = true;
            break;
        }
        render_frames++;
        if (!audio.started && !render_wait_presented(render_frames))
        {
            IPVF_NOTE_QUALIFICATION_ERROR(&stats,
                                          IPVF_ERROR_RENDER_PRESENT, frame);
            failed = true;
            break;
        }
#else
        if (record_info.type == IPVF_TYPE_KEY)
        {
            rb->memcpy(fb, decoded_record + IPVF_FRAME_HEADER_SIZE,
                       IPVF_FRAME_BYTES);
            rb->lcd_update();
        }
        else if (record_info.type == IPVF_TYPE_RECTS &&
                 !apply_rects(decoded_record + IPVF_FRAME_HEADER_SIZE,
                              record_info.decoded_video_bytes,
                              record_info.rect_count, fb, true))
        {
            IPVF_NOTE_QUALIFICATION_ERROR(&stats,
                                          IPVF_ERROR_DISPLAY_APPLY, frame);
            failed = true;
            break;
        }
#endif

        if (!audio.started)
        {
            off_t frame1_position = file_position;
            uint32_t frame1_sectors = record_info.next_sectors;
            bool prebuffer_ok;

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_started = USEC_TIMER;
#endif
            prebuffer_ok = audio_prebuffer_records(
                fd, &info, record, frame + 1, frame1_position,
                frame1_sectors,
                &prebuffer_end_frame);
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            stats.prebuffer_us += USEC_TIMER - operation_started;
#endif
            if (!prebuffer_ok ||
                (frame + 1 < info.frame_count &&
                 rb->lseek(fd, frame1_position, SEEK_SET) !=
                 frame1_position) ||
                !audio_start())
            {
                IPVF_NOTE_QUALIFICATION_ERROR(&stats,
                                              IPVF_ERROR_PREBUFFER, frame);
                failed = true;
                break;
            }
#ifdef IPVF_NATIVE_PIPELINE
            if (seeking)
            {
                last_key_offset = seek_key_offset;
                last_key_sectors = seek_key_sectors;
                last_key_frame = seek_key_frame;
            }
#endif
            seeking = false;
        }

        current_sectors = record_info.next_sectors;
        stats.frames = frame + 1;
        rb->reset_poweroff_timer();

        {
            unsigned int button_events;
            int volume_delta = 0;
            int seek_steps = pending_seek_steps;
            bool pause_requested = false;

            pending_seek_steps = 0;

            /* Drain a bounded burst so fast wheel movement cannot accumulate
             * behind video frames. Apply volume once after aggregation. */
            for (button_events = 0;
                 button_events < IPVF_BUTTON_DRAIN_LIMIT;
                 button_events++)
            {
                long base_button;

                button = rb->button_get(false);
                if (button == BUTTON_NONE)
                    break;

                base_button = button & ~(BUTTON_REL | BUTTON_REPEAT);
                if (base_button == BUTTON_SCROLL_FWD)
                    volume_delta++;
                else if (base_button == BUTTON_SCROLL_BACK)
                    volume_delta--;
                else if (base_button == BUTTON_PLAY &&
                         (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
                    pause_requested = true;
                else if (base_button == BUTTON_LEFT &&
                         (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
                    add_pending_seek(&seek_steps, -1);
                else if (base_button == BUTTON_RIGHT &&
                         (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
                    add_pending_seek(&seek_steps, 1);
                else if (base_button == BUTTON_MENU &&
                         (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
                {
                    stopped = true;
                    break;
                }
                else if (rb->default_event_handler(button) ==
                         SYS_USB_CONNECTED)
                {
                    usb_connected = true;
                    break;
                }
            }

            adjust_playback_volume(volume_delta);
            if (pause_requested && !stopped && !usb_connected)
            {
                enum ipvf_pause_result pause_result =
                    pause_playback(render_frames);

                if (pause_result == IPVF_PAUSE_STOPPED)
                    stopped = true;
                else if (pause_result == IPVF_PAUSE_USB)
                    usb_connected = true;
                else if (pause_result == IPVF_PAUSE_SEEK_BACK)
                {
                    add_pending_seek(&seek_steps, -1);
                    pause_requested = false;
                }
                else if (pause_result == IPVF_PAUSE_SEEK_FORWARD)
                {
                    add_pending_seek(&seek_steps, 1);
                    pause_requested = false;
                }
                else if (pause_result == IPVF_PAUSE_FAILED)
                    failed = true;
            }
            if (seek_steps != 0 && !pause_requested &&
                !stopped && !usb_connected && !failed)
            {
                struct ipvf_index_entry entry;
                unsigned long step =
                    ((unsigned long)info.fps_num * 10u +
                     info.fps_den - 1u) / info.fps_den;
                unsigned long target_frame;
                int64_t requested_frame;

                if (step == 0)
                    step = 1;
                requested_frame = (int64_t)frame +
                                  (int64_t)seek_steps * step;
                if (requested_frame <= 0)
                    target_frame = 0;
                else if ((uint64_t)requested_frame >= info.frame_count)
                    target_frame = info.frame_count - 1;
                else
                    target_frame = (unsigned long)requested_frame;

                if (target_frame == frame)
                {
                    /* The request was clamped at a file boundary. */
                }
                else if (!find_seek_entry(fd, &info, target_frame, &entry) ||
                    !audio_reset_to(audio_frame_boundary(&info,
                                                         target_frame))
#ifdef IPVF_NATIVE_PIPELINE
                    || !render_finish(&stats)
                    || !render_start(decoded_buffer)
#endif
                    || rb->lseek(fd, entry.offset, SEEK_SET) != entry.offset)
                {
                    failed = true;
                }
                else
                {
                    file_position = entry.offset;
                    current_sectors = entry.sectors;
                    frame = entry.frame;
                    seek_base_frame = entry.frame;
                    seek_base_flags = entry.flags;
                    seek_target = target_frame;
                    prebuffer_end_frame = target_frame + 1;
                    render_frames = 0;
                    seeking = true;
#ifdef IPVF_NATIVE_PIPELINE
                    seek_key_offset = entry.offset;
                    seek_key_sectors = entry.sectors;
                    seek_key_frame = entry.frame;
#endif
                    continue;
                }
            }
            if (stopped || usb_connected || failed)
                break;
        }
        frame++;
    }

    if (!stopped && !usb_connected && stats.frames == info.frame_count &&
        (current_sectors != 0 || file_position != info.media_end_offset))
    {
        IPVF_NOTE_QUALIFICATION_ERROR(&stats, IPVF_ERROR_EOF_CHAIN,
                                      stats.frames);
        failed = true;
    }

    if (stopped || usb_connected || failed ||
        stats.frames != info.frame_count)
        audio_stop_playback();

#ifdef IPVF_NATIVE_PIPELINE
    if (!render_finish(&stats))
    {
        IPVF_NOTE_QUALIFICATION_ERROR(&stats, IPVF_ERROR_RENDER_FINISH,
                                      stats.frames);
        failed = true;
    }
#endif

    if (!stopped && !usb_connected && !failed &&
        stats.frames == info.frame_count &&
        !audio_wait_until(info.audio_sample_frames))
    {
        IPVF_NOTE_QUALIFICATION_ERROR(&stats, IPVF_ERROR_AUDIO_FINISH,
                                      stats.frames);
        failed = true;
    }

    if (!stopped && !usb_connected && !failed &&
        stats.frames == info.frame_count)
        audio_drain_output();

    if (failed)
        audio_stop_playback();

#ifdef IPVF_NATIVE_PIPELINE
    if (!usb_connected && !failed && stats.frames != 0 &&
        (stopped || stats.frames == info.frame_count))
    {
#ifdef HAVE_IPOD_CRASH_RECORD
        rb->crash_record_ipvf_update(1, CRASH_RECORD_IPVF_RECONCILING,
                                     stats.frames, 0);
#endif
        if (!reconcile_framebuffer(fd, &info, last_key_offset,
                                   last_key_sectors, last_key_frame,
                                   stats.frames, read_buffer,
                                   audio.video_scratch, fb,
                                   stats.frames == info.frame_count))
        {
            IPVF_NOTE_QUALIFICATION_ERROR(&stats, IPVF_ERROR_RECONCILE,
                                          stats.frames);
            failed = true;
        }
#ifdef HAVE_IPOD_CRASH_RECORD
        rb->crash_record_ipvf_update(0, CRASH_RECORD_IPVF_IDLE, 0, 0);
#endif
    }
#endif

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    if (!usb_connected && !failed && stats.frames != 0)
        stats.final_crc = rb->crc_32(fb, IPVF_FRAME_BYTES, 0xffffffffu);
#endif

    stats.audio_underruns = audio.underruns;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    stats.elapsed_ticks = (unsigned long)
        (*rb->current_tick - playback_start_tick);
#endif
    audio_shutdown();
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (boosted)
        rb->cpu_boost(false);
#endif
    rb->storage_spindown(old_spindown);
    backlight_use_settings();
    rb->close(fd);

    if (usb_connected)
        return PLUGIN_USB_CONNECTED;

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    write_qualification_log(
        filename, &info, &stats,
        failed ? "failed" :
        (stats.frames == info.frame_count ? "complete" : "stopped"));
#endif

    if (failed)
        rb->splash(HZ * 2, "IPVF playback failed");

    return (!failed &&
            (stats.frames == info.frame_count || stopped)) ?
           PLUGIN_OK : PLUGIN_ERROR;
}

enum plugin_status plugin_start(const void *parameter)
{
    if (parameter == NULL)
    {
        rb->splash(HZ * 2, "Open an .ipvf file");
        return PLUGIN_OK;
    }

    return play_file(parameter);
}
