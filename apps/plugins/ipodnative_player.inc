#define IPVF_AUDIO_PREBUFFER_FRAMES (2u * 44100u)

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
        if (scan_position > info->file_size ||
            (off_t)record_bytes > info->file_size - scan_position ||
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
#endif
    unsigned long frame;
    unsigned long prebuffer_end_frame = 1;
    long playback_start_tick;
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

    if (!ipodnative_audio_init(info.temporal_xor))
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

    stats.start_tick = (unsigned long)*rb->current_tick;
    playback_start_tick = (long)stats.start_tick;

    current_sectors = info.first_record_sectors;
    for (frame = 0; frame < info.frame_count; frame++)
    {
        unsigned char *decoded_record;
        unsigned char *record;
        const unsigned char *audio_payload;
        struct ipvf_record_info record_info;
        uint32_t record_bytes;
        uint32_t target, now, late;
        uint32_t operation_started, operation_us;
#ifdef IPVF_NATIVE_PIPELINE
        off_t frame_offset = file_position;
#endif
        int button;

#ifdef IPVF_NATIVE_PIPELINE
        operation_started = USEC_TIMER;
        decoded_record = render_acquire();
        operation_us = USEC_TIMER - operation_started;
        add_qualification_timing(&stats.render_slot_wait_us,
                                 &stats.max_render_slot_wait_us,
                                 operation_us);
        record = UNCACHED_ADDR(read_buffer);
        if (decoded_record == NULL)
        {
            note_qualification_error(&stats, IPVF_ERROR_RENDER_ACQUIRE,
                                     frame);
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
            note_qualification_error(&stats, IPVF_ERROR_RECORD_CHAIN,
                                     frame);
            failed = true;
            break;
        }
        record_bytes = current_sectors * IPVF_RECORD_SECTOR_SIZE;
        if (file_position > info.file_size ||
            (off_t)record_bytes > info.file_size - file_position)
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            note_qualification_error(&stats, IPVF_ERROR_RECORD_BOUNDS,
                                     frame);
            failed = true;
            break;
        }
        operation_started = USEC_TIMER;
        if (!read_exact(fd, record, record_bytes))
        {
            operation_us = USEC_TIMER - operation_started;
            add_qualification_timing(&stats.read_us,
                                     &stats.max_read_us, operation_us);
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            note_qualification_error(&stats, IPVF_ERROR_READ, frame);
            failed = true;
            break;
        }
        operation_us = USEC_TIMER - operation_started;
        add_qualification_timing(&stats.read_us,
                                 &stats.max_read_us, operation_us);
        if (!parse_record(record, current_sectors, frame, &info,
                          &record_info))
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            note_qualification_error(&stats, IPVF_ERROR_PARSE, frame);
            failed = true;
            break;
        }
        file_position += record_bytes;
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

        audio_payload = record + IPVF_FRAME_HEADER_SIZE +
                        record_info.stored_video_bytes;
        if (frame == 0 || frame >= prebuffer_end_frame)
        {
            bool audio_ok;

            operation_started = USEC_TIMER;
            audio_ok = audio_write_ima_block(
                audio_payload, record_info.audio_bytes,
                record_info.audio_frames);
            operation_us = USEC_TIMER - operation_started;
            add_qualification_timing(&stats.audio_feed_us,
                                     &stats.max_audio_feed_us,
                                     operation_us);
            if (!audio_ok)
            {
#ifdef IPVF_NATIVE_PIPELINE
                render_abandon();
#endif
                note_qualification_error(&stats, IPVF_ERROR_AUDIO_FEED,
                                         frame);
                failed = true;
                break;
            }
        }
        operation_started = USEC_TIMER;
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
            operation_us = USEC_TIMER - operation_started;
            add_qualification_timing(&stats.video_decode_us,
                                     &stats.max_video_decode_us,
                                     operation_us);
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            note_qualification_error(&stats, IPVF_ERROR_VIDEO_DECODE,
                                     frame);
            failed = true;
            break;
        }
        operation_us = USEC_TIMER - operation_started;
        add_qualification_timing(&stats.video_decode_us,
                                 &stats.max_video_decode_us,
                                 operation_us);
        if (frame + 1 == info.frame_count)
            audio_mark_eof();

        if (frame != 0)
        {
            uint32_t late_us;

            target = audio_frame_boundary(&info, frame);
            now = audio_clock();
            late = now > target ? now - target : 0;
            late_us = (uint32_t)
                ((uint64_t)late * 1000000u / info.audio_sample_rate);
            if (late_us > 500)
            {
                stats.late_frames++;
                if (late_us > stats.max_late_us)
                    stats.max_late_us = late_us;
            }
            else if (now < target && !audio_wait_until(target))
            {
#ifdef IPVF_NATIVE_PIPELINE
                render_abandon();
#endif
                note_qualification_error(&stats, IPVF_ERROR_AUDIO_CLOCK,
                                         frame);
                failed = true;
                break;
            }
        }

#ifdef IPVF_NATIVE_PIPELINE
        if (!render_queue(record_info.type, record_info.rect_count,
                          record_info.decoded_video_bytes))
        {
            note_qualification_error(&stats, IPVF_ERROR_RENDER_QUEUE,
                                     frame);
            failed = true;
            break;
        }
        if (record_info.keyframe)
        {
            last_key_offset = frame_offset;
            last_key_sectors = current_sectors;
            last_key_frame = frame;
        }
        if (frame == 0 && !render_wait_presented(1))
        {
            note_qualification_error(&stats, IPVF_ERROR_RENDER_PRESENT,
                                     frame);
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
            note_qualification_error(&stats, IPVF_ERROR_DISPLAY_APPLY,
                                     frame);
            failed = true;
            break;
        }
#endif

        if (frame == 0)
        {
            off_t frame1_position = file_position;
            uint32_t frame1_sectors = record_info.next_sectors;
            bool prebuffer_ok;

            operation_started = USEC_TIMER;
            prebuffer_ok = audio_prebuffer_records(
                fd, &info, record, 1, frame1_position, frame1_sectors,
                &prebuffer_end_frame);
            stats.prebuffer_us += USEC_TIMER - operation_started;
            if (!prebuffer_ok ||
                (info.frame_count > 1 &&
                 rb->lseek(fd, frame1_position, SEEK_SET) !=
                 frame1_position) ||
                !audio_start())
            {
                note_qualification_error(&stats, IPVF_ERROR_PREBUFFER,
                                         frame);
                failed = true;
                break;
            }
        }

        current_sectors = record_info.next_sectors;
        stats.frames++;
        rb->reset_poweroff_timer();

        button = rb->button_get(false);
        if ((button & ~(BUTTON_REL | BUTTON_REPEAT)) == BUTTON_MENU)
        {
            stopped = true;
            break;
        }
        if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
        {
            usb_connected = true;
            break;
        }
    }

    if (!stopped && !usb_connected && stats.frames == info.frame_count &&
        (current_sectors != 0 || file_position != info.file_size))
    {
        note_qualification_error(&stats, IPVF_ERROR_EOF_CHAIN,
                                 stats.frames);
        failed = true;
    }

    if (stopped || usb_connected || failed ||
        stats.frames != info.frame_count)
        audio_stop_playback();

#ifdef IPVF_NATIVE_PIPELINE
    if (!render_finish(&stats))
    {
        note_qualification_error(&stats, IPVF_ERROR_RENDER_FINISH,
                                 stats.frames);
        failed = true;
    }
#endif

    if (!stopped && !usb_connected && !failed &&
        stats.frames == info.frame_count &&
        !audio_wait_until(info.audio_sample_frames))
    {
        note_qualification_error(&stats, IPVF_ERROR_AUDIO_FINISH,
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
            note_qualification_error(&stats, IPVF_ERROR_RECONCILE,
                                     stats.frames);
            failed = true;
        }
#ifdef HAVE_IPOD_CRASH_RECORD
        rb->crash_record_ipvf_update(0, CRASH_RECORD_IPVF_IDLE, 0, 0);
#endif
    }
#endif

    if (!usb_connected && !failed && stats.frames != 0)
        stats.final_crc = rb->crc_32(fb, IPVF_FRAME_BYTES, 0xffffffffu);

    stats.audio_underruns = audio.underruns;
    stats.elapsed_ticks = (unsigned long)
        (*rb->current_tick - playback_start_tick);
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

    write_qualification_log(
        filename, &info, &stats,
        failed ? "failed" :
        (stats.frames == info.frame_count ? "complete" : "stopped"));

    if (!failed && stats.frames == info.frame_count)
        rb->splashf(HZ * 2, "%lu frames, %lu late, %lu audio gaps",
                    stats.frames, stats.late_frames,
                    stats.audio_underruns);
    else if (!failed && stopped)
        rb->splashf(HZ, "Stopped at %lu/%lu",
                    stats.frames, info.frame_count);
    else
        rb->splashf(HZ * 2, "Failed at %lu/%lu",
                    stats.frames, info.frame_count);

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
