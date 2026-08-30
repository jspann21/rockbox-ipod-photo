static enum plugin_status play_file(const char *filename)
{
    struct ipvf_info info;
    struct ipvf_stats stats;
    size_t buf_size;
    uintptr_t buffer_end;
    uintptr_t required_end;
    unsigned char *buf;
    unsigned char *record_buffer;
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
#ifdef IPVF_NATIVE_PIPELINE
    record_buffer = (unsigned char *)
        (((uintptr_t)buf + IPVF_RECORD_SECTOR_SIZE - 1) &
         ~(uintptr_t)(IPVF_RECORD_SECTOR_SIZE - 1));
    required_end = (uintptr_t)record_buffer +
                   IPVF_RENDER_SLOTS * IPVF_RENDER_SLOT_STRIDE;
#else
    record_buffer = (unsigned char *)
        (((uintptr_t)buf + IPVF_RECORD_SECTOR_SIZE - 1) &
         ~(uintptr_t)(IPVF_RECORD_SECTOR_SIZE - 1));
    required_end = (uintptr_t)record_buffer + IPVF_RECORD_MAX_BYTES;
#endif
    if (buffer_end < (uintptr_t)buf ||
        (uintptr_t)record_buffer < (uintptr_t)buf ||
        required_end < (uintptr_t)record_buffer ||
        required_end > buffer_end)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF buffer too small");
        return PLUGIN_ERROR;
    }

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

    if (!ipodnative_audio_init())
    {
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF audio unavailable");
        return PLUGIN_ERROR;
    }

    rb->lcd_set_backdrop(NULL);
    rb->lcd_clear_display();
    rb->lcd_update();
#ifdef IPVF_NATIVE_PIPELINE
    if (!render_start(record_buffer))
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

    current_sectors = info.first_record_sectors;
    for (frame = 0; frame < info.frame_count; frame++)
    {
        unsigned char *record;
        const unsigned char *audio_payload;
        unsigned int type, rect_count;
        uint32_t payload_size, audio_frames, next_sectors;
        uint32_t record_bytes;
        uint32_t target, now, late;
#ifdef IPVF_NATIVE_PIPELINE
        off_t frame_offset = file_position;
#endif
        int button;

#ifdef IPVF_NATIVE_PIPELINE
        record = render_acquire();
        if (record == NULL)
        {
            failed = true;
            break;
        }
#else
        record = record_buffer;
#endif

        if (current_sectors == 0 ||
            current_sectors > IPVF_RECORD_MAX_SECTORS)
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            failed = true;
            break;
        }
        record_bytes = current_sectors * IPVF_RECORD_SECTOR_SIZE;
        if (file_position > info.file_size ||
            (off_t)record_bytes > info.file_size - file_position ||
            rb->read(fd, record, record_bytes) != (ssize_t)record_bytes ||
            !parse_record(record, current_sectors, frame, &info,
                          &type, &rect_count, &payload_size,
                          &audio_frames, &next_sectors,
#ifdef IPVF_NATIVE_PIPELINE
                          true
#else
                          false
#endif
                          ))
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            failed = true;
            break;
        }
        file_position += record_bytes;

        audio_payload = record + IPVF_FRAME_HEADER_SIZE + payload_size;
        if (!audio_write_frames(audio_payload, audio_frames))
        {
#ifdef IPVF_NATIVE_PIPELINE
            render_abandon();
#endif
            failed = true;
            break;
        }
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
                failed = true;
                break;
            }
        }

#ifdef IPVF_NATIVE_PIPELINE
        if (!render_queue(type, rect_count, payload_size))
        {
            failed = true;
            break;
        }
        if (type == IPVF_TYPE_KEY)
        {
            last_key_offset = frame_offset;
            last_key_sectors = current_sectors;
            last_key_frame = frame;
        }
        if (frame == 0 &&
            (!render_wait_presented(1) || !audio_start()))
        {
            failed = true;
            break;
        }
#else
        if (type == IPVF_TYPE_KEY)
        {
            rb->memcpy(fb, record + IPVF_FRAME_HEADER_SIZE,
                       IPVF_FRAME_BYTES);
            rb->lcd_update();
        }
        else if (type == IPVF_TYPE_RECTS &&
                 !apply_rects(record + IPVF_FRAME_HEADER_SIZE,
                              payload_size, rect_count, fb, true))
        {
            failed = true;
            break;
        }
        if (frame == 0 && !audio_start())
        {
            failed = true;
            break;
        }
#endif

        current_sectors = next_sectors;
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
        failed = true;

    if (stopped || usb_connected || failed ||
        stats.frames != info.frame_count)
        audio_stop_playback();

#ifdef IPVF_NATIVE_PIPELINE
    if (!render_finish())
        failed = true;
#endif

    if (!stopped && !usb_connected && !failed &&
        stats.frames == info.frame_count &&
        !audio_wait_until(info.audio_sample_frames))
        failed = true;

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
                                   stats.frames, fb,
                                   stats.frames == info.frame_count))
            failed = true;
#ifdef HAVE_IPOD_CRASH_RECORD
        rb->crash_record_ipvf_update(0, CRASH_RECORD_IPVF_IDLE, 0, 0);
#endif
    }
#endif

    stats.audio_underruns = audio.underruns;
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
