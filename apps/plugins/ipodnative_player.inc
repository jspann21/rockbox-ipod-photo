#define IPVF_AUDIO_PREBUFFER_FRAMES (4u * 44100u)
#define IPVF_AUDIO_SEEK_PREBUFFER_FRAMES (1u * 44100u)

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
            "type_motion_lz4\t"
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
        "%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t"
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
        stats->type_counts[IPVF_TYPE_MOTION_LZ4],
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

#endif

static void note_playback_error(struct ipvf_stats *stats,
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
    note_playback_error((stats), (code), (frame))

/* Keep lifecycle evidence out of the playback hot path. One compact row is
 * appended only after audio, rendering, storage policy, and the movie fd have
 * all been restored. Rotation bounds persistent storage without exposing
 * diagnostic counters in the UI. */
static void write_run_journal(const struct ipvf_info *info,
                              const struct ipvf_stats *stats,
                              const char *status,
                              uint32_t active_frame,
                              unsigned long started_tick)
{
    bool new_file;
    bool rotate_file = false;
    char header[sizeof(IPVF_RUN_JOURNAL_HEADER)];
    int journal_fd;
    off_t journal_size;

    if (info == NULL || stats == NULL || status == NULL || stats->frames == 0)
        return;

    journal_size = 0;
    journal_fd = rb->open(IPVF_RUN_JOURNAL, O_RDONLY);
    if (journal_fd >= 0)
    {
        journal_size = rb->filesize(journal_fd);
        if (journal_size > 0)
        {
            ssize_t header_bytes = rb->read(
                journal_fd, header, sizeof(IPVF_RUN_JOURNAL_HEADER) - 1u);
            rotate_file =
                header_bytes !=
                    (ssize_t)(sizeof(IPVF_RUN_JOURNAL_HEADER) - 1u) ||
                rb->memcmp(header, IPVF_RUN_JOURNAL_HEADER,
                           sizeof(IPVF_RUN_JOURNAL_HEADER) - 1u) != 0;
        }
        rb->close(journal_fd);
    }
    if (rotate_file ||
        journal_size >= (off_t)IPVF_RUN_JOURNAL_MAX_BYTES)
    {
        rb->remove(IPVF_RUN_JOURNAL_PREVIOUS);
        if (rb->rename(IPVF_RUN_JOURNAL,
                       IPVF_RUN_JOURNAL_PREVIOUS) < 0)
            return;
    }

    new_file = !rb->file_exists(IPVF_RUN_JOURNAL);
    journal_fd = rb->open(IPVF_RUN_JOURNAL,
                          O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (journal_fd < 0)
        return;
    if (new_file)
        rb->fdprintf(journal_fd, "%s", IPVF_RUN_JOURNAL_HEADER);
    rb->fdprintf(journal_fd,
        "%lu\t%lu\t%d\t%s\t%s\t%u\t%u\t%lu\t%lu\t%lu\t%lu\t"
        "%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t"
        "%lu\t%lu\t%lu\n",
        started_tick, (unsigned long)(*rb->current_tick - started_tick),
        HZ, rb->rbversion, status, info->fps_num, info->fps_den,
        stats->frames, info->frame_count, (unsigned long)active_frame,
        stats->late_frames, stats->audio_underruns,
        stats->audio_rebuffers,
        stats->first_audio_rebuffer_frame,
        stats->last_audio_rebuffer_frame,
        stats->audio_capacity_frames,
        stats->audio_low_water_frames,
        stats->audio_buffer_callbacks,
        stats->startup_ticks,
        stats->seek_count, stats->seek_reconstructed_frames,
        stats->max_seek_ticks,
        stats->first_error, stats->first_error_frame);
    rb->close(journal_fd);
}

#define IPVF_BUTTON_DRAIN_LIMIT 16u
#define IPVF_PENDING_SEEK_LIMIT 1024
#define IPVF_RESUME_SLOT_BYTES 36u
#define IPVF_RESUME_PATH_BYTES 64u
#define IPVF_RESUME_INTERVAL_SECONDS 30u

struct ipvf_resume_candidate
{
    uint32_t frame;
    uint32_t sequence;
    uint32_t state;
    int slot;
    bool valid;
};

enum ipvf_resume_state
{
    IPVF_RESUME_ACTIVE = 1,
    IPVF_RESUME_DISMISSED,
    IPVF_RESUME_COMPLETE,
};

static uint32_t resume_writer_sequence;
static int resume_writer_slot;
static bool resume_writer_ready;

static void resume_put_le32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void resume_slot_path(char *path, size_t path_size,
                             const struct ipvf_info *info, int slot)
{
    rb->snprintf(path, path_size, PLUGIN_DATA_DIR "/ipvf-%08lx.%c.resume",
                 (unsigned long)info->media_id, slot == 0 ? 'a' : 'b');
}

static bool read_resume_slot(const struct ipvf_info *info, int slot,
                             struct ipvf_resume_candidate *candidate)
{
    unsigned char data[IPVF_RESUME_SLOT_BYTES];
    char path[IPVF_RESUME_PATH_BYTES];
    int fd;

    candidate->valid = false;
    resume_slot_path(path, sizeof(path), info, slot);
    fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return false;
    if (rb->filesize(fd) != (off_t)sizeof(data) ||
        rb->read(fd, data, sizeof(data)) != (ssize_t)sizeof(data))
    {
        rb->close(fd);
        return false;
    }
    rb->close(fd);

    if (rb->memcmp(data, "IPRS", 4) != 0 ||
        rb->crc_32(data, sizeof(data) - 4u, 0xffffffffu) !=
            get_le32(data + sizeof(data) - 4u) ||
        get_le32(data + 4) != info->media_id ||
        get_le32(data + 8) != info->frame_count ||
        get_le32(data + 12) != info->audio_sample_frames ||
        get_le32(data + 16) != (uint32_t)info->media_end_offset ||
        get_le32(data + 28) < IPVF_RESUME_ACTIVE ||
        get_le32(data + 28) > IPVF_RESUME_COMPLETE ||
        (get_le32(data + 28) == IPVF_RESUME_ACTIVE &&
         (get_le32(data + 20) == 0 ||
          get_le32(data + 20) >= info->frame_count)) ||
        (get_le32(data + 28) != IPVF_RESUME_ACTIVE &&
         get_le32(data + 20) != 0))
        return false;

    candidate->frame = get_le32(data + 20);
    candidate->sequence = get_le32(data + 24);
    candidate->state = get_le32(data + 28);
    candidate->slot = slot;
    candidate->valid = true;
    return true;
}

static bool load_resume(const struct ipvf_info *info,
                        struct ipvf_resume_candidate *latest)
{
    struct ipvf_resume_candidate a;
    struct ipvf_resume_candidate b;

    read_resume_slot(info, 0, &a);
    read_resume_slot(info, 1, &b);
    if (!a.valid && !b.valid)
    {
        latest->valid = false;
        return false;
    }
    if (!a.valid)
        *latest = b;
    else if (!b.valid)
        *latest = a;
    else if ((int32_t)(b.sequence - a.sequence) > 0)
        *latest = b;
    else
        *latest = a;
    return true;
}

static bool prepare_resume_slot(const struct ipvf_info *info, int slot)
{
    unsigned char empty[IPVF_RESUME_SLOT_BYTES];
    char path[IPVF_RESUME_PATH_BYTES];
    int fd;
    bool written;

    resume_slot_path(path, sizeof(path), info, slot);
    fd = rb->open(path, O_RDWR);
    if (fd >= 0)
    {
        bool ready = rb->filesize(fd) == (off_t)sizeof(empty);
        rb->close(fd);
        if (ready)
            return true;
    }

    rb->memset(empty, 0, sizeof(empty));
    fd = rb->open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    written = rb->write(fd, empty, sizeof(empty)) ==
              (ssize_t)sizeof(empty);
    if (rb->close(fd) < 0)
        written = false;
    if (!written)
        rb->remove(path);
    return written;
}

static bool prepare_resume_slots(const struct ipvf_info *info)
{
    return prepare_resume_slot(info, 0) && prepare_resume_slot(info, 1);
}

static void initialize_resume_writer(
    const struct ipvf_resume_candidate *latest)
{
    if (latest != NULL && latest->valid)
    {
        resume_writer_sequence = latest->sequence;
        resume_writer_slot = latest->slot == 0 ? 1 : 0;
    }
    else
    {
        resume_writer_sequence = 0;
        resume_writer_slot = 0;
    }
    resume_writer_ready = true;
}

static bool save_resume(const struct ipvf_info *info, uint32_t frame,
                        enum ipvf_resume_state state)
{
    unsigned char data[IPVF_RESUME_SLOT_BYTES];
    char path[IPVF_RESUME_PATH_BYTES];
    uint32_t sequence;
    int slot;
    int fd;
    bool written;

    if (!resume_writer_ready ||
        (state == IPVF_RESUME_ACTIVE &&
         (frame == 0 || frame >= info->frame_count)) ||
        (state != IPVF_RESUME_ACTIVE && frame != 0))
        return false;
    sequence = resume_writer_sequence + 1u;
    slot = resume_writer_slot;

    rb->memset(data, 0, sizeof(data));
    rb->memcpy(data, "IPRS", 4);
    resume_put_le32(data + 4, info->media_id);
    resume_put_le32(data + 8, info->frame_count);
    resume_put_le32(data + 12, info->audio_sample_frames);
    resume_put_le32(data + 16, (uint32_t)info->media_end_offset);
    resume_put_le32(data + 20, frame);
    resume_put_le32(data + 24, sequence);
    resume_put_le32(data + 28, state);
    resume_put_le32(data + 32,
                    rb->crc_32(data, sizeof(data) - 4u, 0xffffffffu));

    resume_slot_path(path, sizeof(path), info, slot);
    fd = rb->open(path, O_RDWR);
    if (fd < 0)
        return false;
    written = rb->filesize(fd) == (off_t)sizeof(data) &&
              rb->lseek(fd, 0, SEEK_SET) == 0 &&
              rb->write(fd, data, sizeof(data)) == (ssize_t)sizeof(data);
    if (rb->close(fd) < 0)
        written = false;
    if (written)
    {
        resume_writer_sequence = sequence;
        resume_writer_slot = slot == 0 ? 1 : 0;
    }
    return written;
}

struct ipvf_system_cleanup
{
    const struct ipvf_info *info;
    struct ipvf_stats *stats;
    int *fd;
    uint32_t *active_frame;
    bool done;
    bool failed;
};

static void ipvf_cleanup_for_system(void *parameter)
{
    struct ipvf_system_cleanup *cleanup = parameter;

    if (cleanup->done)
        return;

    audio_stop_playback();
#ifdef IPVF_NATIVE_PIPELINE
    if (render_active && !render_finish(cleanup->stats))
        cleanup->failed = true;
#endif
    if (*cleanup->active_frame > 0 &&
        *cleanup->active_frame < cleanup->info->frame_count &&
        !save_resume(cleanup->info, *cleanup->active_frame,
                     IPVF_RESUME_ACTIVE))
        cleanup->failed = true;
    audio_shutdown();
    if (*cleanup->fd >= 0)
    {
        if (rb->close(*cleanup->fd) < 0)
            cleanup->failed = true;
        *cleanup->fd = -1;
    }
    cleanup->done = true;
}

static bool handle_system_event(long event,
                                struct ipvf_system_cleanup *cleanup)
{
    long handled = rb->default_event_handler_ex(
        event, ipvf_cleanup_for_system, cleanup);

    return handled == SYS_USB_CONNECTED || cleanup->done;
}

static bool checkpoint_presented(const struct ipvf_info *info,
                                 uint32_t frame, uint32_t *active_frame)
{
    if (frame == 0 || frame >= info->frame_count)
        return false;
    *active_frame = frame;
    return save_resume(info, frame, IPVF_RESUME_ACTIVE);
}

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

enum ipvf_details_result
{
    IPVF_DETAILS_RESUMED = 0,
    IPVF_DETAILS_STOPPED,
    IPVF_DETAILS_USB,
    IPVF_DETAILS_FAILED,
};

static enum ipvf_details_result show_media_details(
    const struct ipvf_info *info, fb_data *fb,
    unsigned long presented_frames, uint32_t presented_frame,
    uint32_t *active_frame, struct ipvf_system_cleanup *cleanup)
{
    uint32_t duration_seconds;

#ifdef IPVF_NATIVE_PIPELINE
    if (!render_wait_presented(presented_frames))
        return IPVF_DETAILS_FAILED;
#else
    (void)presented_frames;
#endif
    if (!audio_pause_playback())
        return IPVF_DETAILS_FAILED;
    checkpoint_presented(info, presented_frame, active_frame);

    duration_seconds = info->audio_sample_frames / info->audio_sample_rate;
    rb->lcd_set_viewport(NULL);
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_clear_display();
    rb->lcd_puts_scroll(0, 0, info->title[0] ? info->title : "IPVF video");
    if (info->artist[0])
        rb->lcd_puts_scroll(0, 1, info->artist);
    if (info->album[0])
        rb->lcd_puts_scroll(0, 2, info->album);
    rb->lcd_putsf(0, 4, "%lu:%02lu  %u/%u fps",
                  (unsigned long)(duration_seconds / 60u),
                  (unsigned long)(duration_seconds % 60u),
                  info->fps_num, info->fps_den);
    rb->lcd_putsf(0, 5, "%ld KiB",
                  (long)(info->file_size / 1024));
    rb->lcd_puts(0, 7, "Select/Play: back");
    rb->lcd_puts(0, 8, "Menu: exit");
    rb->lcd_update();

    while (true)
    {
        long button = rb->button_get_w_tmo(HZ / 4);
        long base_button;

        if (button == BUTTON_NONE)
            continue;
        base_button = button & ~(BUTTON_REL | BUTTON_REPEAT);
        if ((base_button == BUTTON_SELECT || base_button == BUTTON_PLAY) &&
            (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
        {
            rb->lcd_scroll_stop();
            rb->memcpy(fb, audio.video_reference, IPVF_FRAME_BYTES);
            rb->lcd_update();
            rb->button_clear_queue();
            return audio_resume_playback() ?
                   IPVF_DETAILS_RESUMED : IPVF_DETAILS_FAILED;
        }
        if (base_button == BUTTON_MENU &&
            (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
        {
            rb->lcd_scroll_stop();
            return IPVF_DETAILS_STOPPED;
        }
        if (handle_system_event(button, cleanup))
        {
            rb->lcd_scroll_stop();
            return IPVF_DETAILS_USB;
        }
        rb->reset_poweroff_timer();
    }
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

static enum ipvf_pause_result pause_playback(
    const struct ipvf_info *info, unsigned long presented_frames,
    uint32_t presented_frame, uint32_t *active_frame,
    struct ipvf_system_cleanup *cleanup)
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
    checkpoint_presented(info, presented_frame, active_frame);

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
        else if (handle_system_event(button, cleanup))
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
                                    uint32_t target_frames,
                                    unsigned long *end_frame)
{
    unsigned long scan_frame = first_frame;
    off_t scan_position = first_position;
    uint32_t scan_sectors = first_sectors;
    if (info == NULL || read_buffer == NULL || end_frame == NULL ||
        target_frames == 0 || audio.started)
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
    struct ipvf_index_entry startup_seek_entry;
    struct ipvf_resume_candidate saved_resume;
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
    unsigned long render_epoch_start_frame = 0;
    unsigned long seek_base_frame = 0;
    uint32_t seek_base_flags = 0;
    unsigned long seek_target = 0;
    unsigned long resume_frame = 0;
    unsigned long resume_checkpoint_interval = 1;
    unsigned long next_resume_checkpoint = 1;
    unsigned long next_resume_attempt = 1;
    unsigned long seek_started_tick = 0;
    uint32_t active_frame = 0;
    int pending_seek_steps = 0;
    bool pending_details = false;
    bool startup_resume = false;
    bool initial_audio_started = false;
    bool seeking = false;
    bool seek_timing_active = false;
    long playback_start_tick;
    int fd;
    struct ipvf_system_cleanup system_cleanup;
    bool failed = false;
    bool stopped = false;
    bool usb_connected = false;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    bool boosted = false;
#endif
    int old_spindown = rb->global_settings->disk_spindown;

    rb->memset(&info, 0, sizeof(info));
    rb->memset(&stats, 0, sizeof(stats));
    rb->memset(&saved_resume, 0, sizeof(saved_resume));
    rb->memset(&system_cleanup, 0, sizeof(system_cleanup));
    system_cleanup.info = &info;
    system_cleanup.stats = &stats;
    system_cleanup.fd = &fd;
    system_cleanup.active_frame = &active_frame;

    fd = rb->open(filename, O_RDONLY);
    if (fd < 0 || !read_header(fd, &info))
    {
        if (fd >= 0)
            rb->close(fd);
        rb->splash(HZ * 2, "Unsupported IPVF");
        return PLUGIN_ERROR;
    }

    if (!prepare_resume_slots(&info))
    {
        rb->close(fd);
        rb->splash(HZ * 2, "IPVF resume unavailable");
        return PLUGIN_ERROR;
    }
    if (!load_resume(&info, &saved_resume))
        saved_resume.valid = false;
    initialize_resume_writer(saved_resume.valid ? &saved_resume : NULL);

    if (saved_resume.valid && saved_resume.state == IPVF_RESUME_ACTIVE)
    {
        const char *lines[] = { "Resume video?" };
        const struct text_message prompt = { lines, 1 };
        enum yesno_res answer =
            rb->gui_syncyesno_run(&prompt, NULL, NULL);

        if (answer == YESNO_USB)
        {
            rb->close(fd);
            return PLUGIN_USB_CONNECTED;
        }
        if (answer == YESNO_YES &&
            find_seek_entry(fd, &info, saved_resume.frame,
                            &startup_seek_entry))
        {
            resume_frame = saved_resume.frame;
            startup_resume = true;
        }
        else
        {
            save_resume(&info, 0, IPVF_RESUME_DISMISSED);
            if (rb->lseek(fd, IPVF_DATA_OFFSET, SEEK_SET) !=
                (off_t)IPVF_DATA_OFFSET)
            {
                rb->close(fd);
                return PLUGIN_ERROR;
            }
        }
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

    playback_start_tick = *rb->current_tick;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    stats.start_tick = (unsigned long)playback_start_tick;
#endif

    current_sectors = info.first_record_sectors;
    frame = 0;
    resume_checkpoint_interval =
        ((unsigned long)info.fps_num * IPVF_RESUME_INTERVAL_SECONDS +
         info.fps_den - 1u) / info.fps_den;
    if (resume_checkpoint_interval == 0)
        resume_checkpoint_interval = 1;
    next_resume_checkpoint = resume_checkpoint_interval;
    next_resume_attempt = next_resume_checkpoint;
    if (startup_resume)
    {
        if (!audio_reset_to(audio_frame_boundary(&info, resume_frame)) ||
            rb->lseek(fd, startup_seek_entry.offset, SEEK_SET) !=
                startup_seek_entry.offset)
        {
            save_resume(&info, 0, IPVF_RESUME_DISMISSED);
            startup_resume = false;
            resume_frame = 0;
            audio_reset_to(0);
            rb->lseek(fd, IPVF_DATA_OFFSET, SEEK_SET);
        }
        else
        {
            file_position = startup_seek_entry.offset;
            current_sectors = startup_seek_entry.sectors;
            frame = startup_seek_entry.frame;
            seek_base_frame = startup_seek_entry.frame;
            seek_base_flags = startup_seek_entry.flags;
            seek_target = resume_frame;
            prebuffer_end_frame = resume_frame + 1u;
            seeking = true;
#ifdef IPVF_NATIVE_PIPELINE
            seek_key_offset = startup_seek_entry.offset;
            seek_key_sectors = startup_seek_entry.sectors;
            seek_key_frame = startup_seek_entry.frame;
#endif
            next_resume_checkpoint = resume_frame +
                                     resume_checkpoint_interval;
            next_resume_attempt = next_resume_checkpoint;
            render_epoch_start_frame = resume_frame;
        }
    }
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
        bool render_slot_acquired = false;
#endif
        long button;

#ifdef IPVF_NATIVE_PIPELINE
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
        operation_started = USEC_TIMER;
#endif
        if (seeking && frame < seek_target)
            decoded_record = decoded_buffer;
        else
        {
            decoded_record = render_acquire();
            render_slot_acquired = decoded_record != NULL;
        }
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
            if (render_slot_acquired)
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
            if (render_slot_acquired)
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
            if (render_slot_acquired)
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
            if (render_slot_acquired)
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
            if (render_slot_acquired)
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
                if (render_slot_acquired)
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
                                 !seeking || frame >= seek_target,
                                 &stats))
        {
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_us = USEC_TIMER - operation_started;
            add_qualification_timing(&stats.video_decode_us,
                                     &stats.max_video_decode_us,
                                     operation_us);
#endif
#ifdef IPVF_NATIVE_PIPELINE
            if (render_slot_acquired)
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

        /* Rebuild the full decoded-audio cushion after a true mixer
         * starvation. The current video is already decoded, so the shared
         * record buffer can safely scan future records and then seek back. */
        if (!seeking && audio_needs_rebuffer())
        {
            off_t next_position = file_position;
            uint32_t next_sectors = record_info.next_sectors;
            bool rebuffer_ok = audio_reset_to(
                audio_frame_boundary(&info, frame));

            if (rebuffer_ok)
            {
                rebuffer_ok = audio_write_ima_block(
                    audio_payload, record_info.audio_bytes,
                    record_info.audio_frames);
            }
            if (rebuffer_ok)
            {
                rebuffer_ok = audio_prebuffer_records(
                    fd, &info, record, frame + 1, next_position,
                    next_sectors, IPVF_AUDIO_PREBUFFER_FRAMES,
                    &prebuffer_end_frame);
            }
            if (rebuffer_ok && frame + 1 < info.frame_count)
            {
                rebuffer_ok = rb->lseek(fd, next_position, SEEK_SET) ==
                              next_position;
            }
            if (rebuffer_ok)
                rebuffer_ok = audio_start();
            if (!rebuffer_ok)
            {
#ifdef IPVF_NATIVE_PIPELINE
                if (render_slot_acquired)
                    render_abandon();
#endif
                IPVF_NOTE_QUALIFICATION_ERROR(
                    &stats, IPVF_ERROR_PREBUFFER, frame);
                failed = true;
                break;
            }
            if (stats.audio_rebuffers == 0)
                stats.first_audio_rebuffer_frame = frame;
            stats.last_audio_rebuffer_frame = frame;
            stats.audio_rebuffers++;
        }
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
            stats.seek_reconstructed_frames++;
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
                    else if (base_button == BUTTON_SELECT &&
                             (seek_button &
                              (BUTTON_REL | BUTTON_REPEAT)) == 0)
                        pending_details = true;
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
                              base_button != BUTTON_SELECT &&
                              handle_system_event(seek_button,
                                                  &system_cleanup))
                    {
                        usb_connected = true;
                        break;
                    }
                }
            }
#ifdef IPVF_NATIVE_PIPELINE
            if (render_slot_acquired)
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
                if (render_slot_acquired)
                    render_abandon();
#endif
                IPVF_NOTE_QUALIFICATION_ERROR(&stats,
                                              IPVF_ERROR_AUDIO_CLOCK, frame);
                failed = true;
                break;
            }
        }

#ifdef IPVF_NATIVE_PIPELINE
        if (!render_slot_acquired ||
            !render_queue(record_info.type, record_info.rect_count,
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
            bool completed_seek = seeking;

            active_frame = (uint32_t)(frame + 1u);

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
            operation_started = USEC_TIMER;
#endif
            prebuffer_ok = audio_prebuffer_records(
                fd, &info, record, frame + 1, frame1_position,
                frame1_sectors,
                completed_seek ? IPVF_AUDIO_SEEK_PREBUFFER_FRAMES :
                                 IPVF_AUDIO_PREBUFFER_FRAMES,
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
            if (!initial_audio_started)
            {
                stats.startup_ticks = (unsigned long)
                    (*rb->current_tick - playback_start_tick);
                initial_audio_started = true;
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
            if (completed_seek && active_frame < info.frame_count)
                save_resume(&info, active_frame, IPVF_RESUME_ACTIVE);
            if (completed_seek && seek_timing_active)
            {
                unsigned long seek_ticks =
                    (unsigned long)(*rb->current_tick - seek_started_tick);

                if (seek_ticks > stats.max_seek_ticks)
                    stats.max_seek_ticks = seek_ticks;
                seek_timing_active = false;
            }
        }

        current_sectors = record_info.next_sectors;
        stats.frames = frame + 1;
#ifdef IPVF_NATIVE_PIPELINE
        {
            unsigned int presented_count = render_presented_count();
            unsigned long presented = render_epoch_start_frame +
                                      presented_count;
            if (presented_count != 0 &&
                presented > active_frame && presented <= stats.frames)
                active_frame = (uint32_t)presented;
        }
#else
        active_frame = (uint32_t)stats.frames;
#endif
        rb->reset_poweroff_timer();
        if (!seeking && stats.frames >= next_resume_checkpoint &&
            stats.frames >= next_resume_attempt &&
            stats.frames < info.frame_count)
        {
            bool presented = true;
#ifdef IPVF_NATIVE_PIPELINE
            presented = render_wait_presented(render_frames);
#endif
            if (!presented)
            {
                IPVF_NOTE_QUALIFICATION_ERROR(&stats,
                                              IPVF_ERROR_RENDER_PRESENT,
                                              stats.frames);
                failed = true;
            }
            else if (checkpoint_presented(&info, (uint32_t)stats.frames,
                                          &active_frame))
            {
                next_resume_checkpoint = stats.frames +
                                         resume_checkpoint_interval;
                next_resume_attempt = next_resume_checkpoint;
            }
            else
            {
                unsigned long retry_frames =
                    ((unsigned long)info.fps_num + info.fps_den - 1u) /
                    info.fps_den;
                next_resume_attempt = stats.frames +
                                      (retry_frames ? retry_frames : 1u);
            }
        }

        {
            unsigned int button_events;
            int volume_delta = 0;
            int seek_steps = pending_seek_steps;
            bool pause_requested = false;
            bool details_requested = pending_details;

            pending_seek_steps = 0;
            pending_details = false;

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
                else if (base_button == BUTTON_SELECT &&
                         (button & (BUTTON_REL | BUTTON_REPEAT)) == 0)
                    details_requested = true;
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
                else if (handle_system_event(button, &system_cleanup))
                {
                    usb_connected = true;
                    break;
                }
            }

            adjust_playback_volume(volume_delta);
            if (details_requested && !stopped && !usb_connected)
            {
                enum ipvf_details_result details_result =
                    show_media_details(&info, fb, render_frames,
                                       (uint32_t)stats.frames,
                                       &active_frame, &system_cleanup);

                pause_requested = false;
                if (details_result == IPVF_DETAILS_STOPPED)
                    stopped = true;
                else if (details_result == IPVF_DETAILS_USB)
                    usb_connected = true;
                else if (details_result == IPVF_DETAILS_FAILED)
                {
                    IPVF_NOTE_QUALIFICATION_ERROR(
                        &stats, IPVF_ERROR_DETAILS, stats.frames);
                    failed = true;
                }
            }
            if (pause_requested && !stopped && !usb_connected)
            {
                enum ipvf_pause_result pause_result =
                    pause_playback(&info, render_frames,
                                   (uint32_t)stats.frames,
                                   &active_frame, &system_cleanup);

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
                {
                    IPVF_NOTE_QUALIFICATION_ERROR(
                        &stats, IPVF_ERROR_PAUSE, stats.frames);
                    failed = true;
                }
            }
            if (seek_steps != 0 && !pause_requested &&
                !stopped && !usb_connected && !failed)
            {
                struct ipvf_index_entry entry;
                unsigned long step =
                    ((unsigned long)info.fps_num * 10u +
                     info.fps_den - 1u) / info.fps_den;
                unsigned long target_frame;
                unsigned long request_started_tick;
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
                request_started_tick = (unsigned long)*rb->current_tick;

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
                    IPVF_NOTE_QUALIFICATION_ERROR(
                        &stats, IPVF_ERROR_SEEK, stats.frames);
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
                    next_resume_checkpoint = target_frame +
                                             resume_checkpoint_interval;
                    next_resume_attempt = next_resume_checkpoint;
                    render_frames = 0;
                    render_epoch_start_frame = target_frame;
                    seeking = true;
                    stats.seek_count++;
                    seek_started_tick = request_started_tick;
                    seek_timing_active = true;
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

    if (system_cleanup.done)
    {
        usb_connected = true;
        failed = failed || system_cleanup.failed;
        goto runtime_restoration;
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
    else if (!seeking && stats.frames != 0)
    {
        active_frame = (uint32_t)stats.frames;
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
    stats.audio_capacity_frames = audio.capacity_frames;
    stats.audio_low_water_frames = audio.low_water_frames;
    stats.audio_buffer_callbacks = audio.buffer_callbacks;
#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    stats.elapsed_ticks = (unsigned long)
        (*rb->current_tick - playback_start_tick);
#endif

    if (!failed && stats.frames == info.frame_count)
        save_resume(&info, 0, IPVF_RESUME_COMPLETE);
    else if (active_frame != 0 && active_frame < info.frame_count)
        save_resume(&info, active_frame, IPVF_RESUME_ACTIVE);

    audio_shutdown();
runtime_restoration:
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (boosted)
        rb->cpu_boost(false);
#endif
    rb->storage_spindown(old_spindown);
    backlight_use_settings();
    if (fd >= 0)
        rb->close(fd);

    if (usb_connected)
        return PLUGIN_USB_CONNECTED;

#if IPVF_ENABLE_QUALIFICATION_TELEMETRY
    write_qualification_log(
        filename, &info, &stats,
        failed ? "failed" :
        (stats.frames == info.frame_count ? "complete" : "stopped"));
#endif

    write_run_journal(
        &info, &stats,
        failed ? "failed" :
        (stats.frames == info.frame_count ? "complete" : "stopped"),
        active_frame, (unsigned long)playback_start_tick);

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
