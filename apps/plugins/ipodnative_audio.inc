static uint32_t floor_power_of_two(size_t value)
{
    uint32_t power = 1;

    if (value > IPVF_AUDIO_RING_MAX_BYTES)
        value = IPVF_AUDIO_RING_MAX_BYTES;
    while (power <= IPVF_AUDIO_RING_MAX_BYTES / 2 &&
           (size_t)(power << 1) <= value)
        power <<= 1;
    return power;
}

static uint32_t audio_used_frames(void)
{
    return audio.written_frames - audio.completed_frames;
}

static void audio_get_more(const void **start, size_t *size)
{
    uint32_t completed = audio.completed_frames + audio.current_frames;
    uint32_t written;
    uint32_t available;
    uint32_t offset;
    uint32_t frames;

    audio.completed_frames = completed;
    audio.current_frames = 0;
    membarrier();

    written = audio.written_frames;
    available = written - completed;
    if (audio.expect_playback && !audio.source_eof)
    {
        audio.buffer_callbacks++;
        if (available <= audio.capacity_frames &&
            available < audio.low_water_frames)
            audio.low_water_frames = available;
    }
    if (available != 0 && available <= audio.capacity_frames)
    {
        offset = completed & audio.frame_mask;
        frames = audio.capacity_frames - offset;
        if (frames > available)
            frames = available;
        if (frames > IPVF_AUDIO_DMA_MAX_FRAMES)
            frames = IPVF_AUDIO_DMA_MAX_FRAMES;

        audio.current_frames = frames;
        membarrier();
        *start = audio.buffer +
                 (size_t)offset * IPVF_AUDIO_FRAME_BYTES;
        *size = (size_t)frames * IPVF_AUDIO_FRAME_BYTES;
        return;
    }

    if (audio.expect_playback && !audio.source_eof)
        audio.underruns++;
    *start = NULL;
    *size = 0;
}

static const struct mixer_play_cbs audio_mixer_cbs =
{
    .get_more = audio_get_more,
};

static bool audio_restart(void)
{
    if (!audio.configured || !audio.started)
        return false;
    if (rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) != CHANNEL_STOPPED)
        return true;
    if (audio_used_frames() == 0)
        return false;

    audio.expect_playback = true;
    membarrier();
    rb->mixer_channel_set_amplitude(IPVF_AUDIO_CHANNEL, MIX_AMP_UNITY);
    rb->mixer_channel_play_data(IPVF_AUDIO_CHANNEL, &audio_mixer_cbs,
                                NULL, 0);
    return rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) != CHANNEL_STOPPED;
}

static bool audio_needs_rebuffer(void)
{
    return audio.configured && audio.started && !audio.source_eof &&
           rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) == CHANNEL_STOPPED;
}

static bool audio_write_ima_block(const unsigned char *source,
                                  uint32_t source_bytes,
                                  uint32_t frames)
{
    struct ipvf_ima_state state;
    long deadline = *rb->current_tick + HZ * 3;
    uint32_t last_completed = audio.completed_frames;
    uint32_t decoded = 0;
    bool silence = source_bytes == 0;
    bool mono = source_bytes == record_mono_audio_bytes(frames);

    if (!silence && mono &&
        !ipvf_ima_parse_mono_header(source, source_bytes, frames, &state))
        return false;
    if (!silence && !mono &&
        !ipvf_ima_parse_header(source, source_bytes, frames, &state))
        return false;

    while (decoded < frames)
    {
        uint32_t written = audio.written_frames;
        uint32_t completed = audio.completed_frames;
        uint32_t used = written - completed;
        uint32_t free_frames;
        uint32_t offset;
        uint32_t count;
        uint32_t i;
        int16_t *destination;

        if (used > audio.capacity_frames)
            return false;
        free_frames = audio.capacity_frames - used;
        if (free_frames == 0)
        {
            if (audio.started &&
                rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) ==
                    CHANNEL_STOPPED)
                audio_restart();
            rb->yield();
            if (audio.completed_frames != last_completed)
            {
                last_completed = audio.completed_frames;
                deadline = *rb->current_tick + HZ * 3;
            }
            else if (!TIME_BEFORE(*rb->current_tick, deadline))
            {
                return false;
            }
            continue;
        }

        offset = written & audio.frame_mask;
        count = audio.capacity_frames - offset;
        if (count > free_frames)
            count = free_frames;
        if (count > frames - decoded)
            count = frames - decoded;

        destination = (int16_t *)(audio.buffer +
                      (size_t)offset * IPVF_AUDIO_FRAME_BYTES);
        for (i = 0; i < count; i++, decoded++)
        {
            int16_t left;
            int16_t right;

            if (silence)
            {
                left = 0;
                right = 0;
            }
            else if (decoded == 0)
            {
                left = (int16_t)state.left_predictor;
                right = (int16_t)state.right_predictor;
            }
            else if (mono)
            {
                unsigned int packed =
                    source[IPVF_IMA_MONO_HEADER_BYTES + (decoded - 1u) / 2u];
                unsigned int code = (decoded - 1u) & 1u ?
                                    packed >> 4 : packed & 0x0fu;

                left = ipvf_ima_decode_nibble(
                    code, &state.left_predictor, &state.left_index);
                right = left;
            }
            else
            {
                unsigned int codes = source[IPVF_IMA_HEADER_BYTES +
                                            decoded - 1u];

                left = ipvf_ima_decode_nibble(
                    codes & 0x0fu, &state.left_predictor,
                    &state.left_index);
                right = ipvf_ima_decode_nibble(
                    codes >> 4, &state.right_predictor,
                    &state.right_index);
            }
            if (audio.seek_ramp_frames != 0)
            {
                uint32_t gain = IPVF_AUDIO_SEEK_RAMP_FRAMES -
                                audio.seek_ramp_frames + 1u;

                left = (int16_t)((int32_t)left * (int32_t)gain /
                                 (int32_t)IPVF_AUDIO_SEEK_RAMP_FRAMES);
                right = (int16_t)((int32_t)right * (int32_t)gain /
                                  (int32_t)IPVF_AUDIO_SEEK_RAMP_FRAMES);
                audio.seek_ramp_frames--;
            }
            destination[2u * i] = left;
            destination[2u * i + 1u] = right;
        }
        membarrier();
        audio.written_frames = written + count;
    }

    return true;
}

static uint32_t audio_clock(void)
{
    uint32_t completed;
    uint32_t current;
    uint32_t waiting;
    unsigned int attempts = 0;

    do
    {
        completed = audio.completed_frames;
        current = audio.current_frames;
        waiting = (uint32_t)
            (rb->mixer_channel_get_bytes_waiting(IPVF_AUDIO_CHANNEL) /
             IPVF_AUDIO_FRAME_BYTES);
    }
    while (++attempts < 8u &&
           (completed != audio.completed_frames ||
            current != audio.current_frames ||
            (waiting == 0 && current != 0 &&
             rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) ==
                 CHANNEL_PLAYING)));

    if (waiting > current)
        waiting = current;
    return completed + current - waiting;
}

static bool audio_start(void)
{
    if (!audio.configured || audio.started || audio_used_frames() == 0)
        return false;

    audio.started = true;
    audio.expect_playback = true;
    membarrier();
    rb->mixer_channel_set_amplitude(IPVF_AUDIO_CHANNEL, MIX_AMP_UNITY);
    rb->mixer_channel_play_data(IPVF_AUDIO_CHANNEL, &audio_mixer_cbs,
                                NULL, 0);
    return true;
}

static bool audio_wait_until(uint32_t target)
{
    long deadline = *rb->current_tick + HZ * 3;
    uint32_t last = audio_clock();

    while (last < target)
    {
        uint32_t now;
        uint32_t remaining = target - last;
        uint32_t remaining_us = (uint32_t)
            ((uint64_t)remaining * 1000000u / IPVF_AUDIO_SAMPLE_RATE);

        bool stopped =
            rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) == CHANNEL_STOPPED;

        if (stopped)
        {
            uint32_t used = audio_used_frames();

            if (audio.source_eof && audio.written_frames == target &&
                audio.completed_frames >= target &&
                audio.current_frames == 0)
                return true;
            if (used != 0)
                audio_restart();
        }
        if (!stopped && remaining_us > 2500u)
            wait_until(USEC_TIMER + remaining_us - 1500u);
        else
            rb->yield();
        now = audio_clock();
        if (now != last)
        {
            last = now;
            deadline = *rb->current_tick + HZ * 3;
        }
        else if (!TIME_BEFORE(*rb->current_tick, deadline))
        {
            return false;
        }
    }

    return true;
}

/* Mixer bookkeeping leads physical output by one frame; cover both buffers. */
static void audio_drain_output(void)
{
    long ticks;

    if (!audio.configured || !audio.started || !audio.source_eof)
        return;

    ticks = (long)((2u * MIX_FRAME_SAMPLES * HZ +
                    IPVF_AUDIO_SAMPLE_RATE - 1u) /
                   IPVF_AUDIO_SAMPLE_RATE);
    if (ticks < 1)
        ticks = 1;
    rb->sleep(ticks);
}

static void audio_mark_eof(void)
{
    membarrier();
    audio.source_eof = true;
    membarrier();
}

static void audio_stop_playback(void)
{
    if (!audio.configured)
        return;

    /* A mixer callback caused by an intentional stop is a control transition,
     * not a playback underrun. Disable gap accounting before stopping the
     * channel so seek, MENU, teardown, and failure cleanup stay out of the
     * steady-playback counter. */
    rb->pcm_play_lock();
    audio.expect_playback = false;
    membarrier();
    if (rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) != CHANNEL_STOPPED)
        rb->mixer_channel_stop(IPVF_AUDIO_CHANNEL);
    rb->pcm_play_unlock();
}

/* Use the mixer's native channel pause so the current DMA slice and ring
 * counters remain untouched. audio_clock() therefore freezes exactly where
 * playback paused and resumes without re-decoding or dropping samples. */
static bool audio_pause_playback(void)
{
    if (!audio.configured || !audio.started)
        return false;
    if (rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) != CHANNEL_PLAYING)
        return false;
    rb->mixer_channel_play_pause(IPVF_AUDIO_CHANNEL, false);
    return rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) == CHANNEL_PAUSED;
}

static bool audio_resume_playback(void)
{
    if (!audio.configured || !audio.started ||
        rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) != CHANNEL_PAUSED)
        return false;
    rb->mixer_channel_play_pause(IPVF_AUDIO_CHANNEL, true);
    return rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) == CHANNEL_PLAYING;
}

/* Rebase the ring to an absolute media sample before decoding a seek target.
 * The counters intentionally retain absolute time so existing frame/audio
 * boundary calculations continue to work after a jump. */
static bool audio_reset_to(uint32_t sample_frame)
{
    if (!audio.configured)
        return false;

    /* Stop the callback and rebase its absolute counters under one PCM lock.
     * The short ramp on the newly decoded samples suppresses a discontinuity
     * click when a seek lands at an arbitrary waveform phase. */
    rb->pcm_play_lock();
    audio.expect_playback = false;
    membarrier();
    if (rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) != CHANNEL_STOPPED)
        rb->mixer_channel_stop(IPVF_AUDIO_CHANNEL);
    audio.completed_frames = sample_frame;
    audio.current_frames = 0;
    audio.written_frames = sample_frame;
    audio.source_eof = false;
    audio.started = false;
    audio.seek_ramp_frames = IPVF_AUDIO_SEEK_RAMP_FRAMES;
    membarrier();
    rb->pcm_play_unlock();
    return true;
}

static bool audio_write_record_block(
    const unsigned char *source,
    const struct ipvf_record_info *record_info,
    struct ipvf_stats *stats)
{
    if (record_info == NULL)
        return false;
    if (!record_info->audio_valid && stats != NULL)
        stats->audio_recovery_blocks++;
    return audio_write_ima_block(
        source,
        record_info->audio_valid ? record_info->audio_bytes : 0,
        record_info->audio_frames);
}

static void audio_shutdown(void)
{
    if (audio.configured)
    {
        audio_stop_playback();
        rb->pcmbuf_fade(false, false);
        if (audio.old_frequency != 0)
            rb->mixer_set_frequency(audio.old_frequency);
    }
    if (audio.buffer_acquired)
        rb->plugin_release_audio_buffer();
    rb->memset(&audio, 0, sizeof(audio));
}

static bool ipodnative_audio_init(bool needs_video_reference)
{
    unsigned char *raw;
    uintptr_t aligned;
    size_t size;
    size_t skipped;
    uint32_t ring_bytes;
    size_t reference_bytes = needs_video_reference ? IPVF_FRAME_BYTES : 0;

    rb->memset(&audio, 0, sizeof(audio));
    raw = rb->plugin_get_audio_buffer(&size);
    audio.buffer_acquired = true;
    if (raw == NULL)
        goto fail;

    aligned = ((uintptr_t)raw + 3u) & ~(uintptr_t)3u;
    if (aligned < (uintptr_t)raw)
        goto fail;
    skipped = aligned - (uintptr_t)raw;
    if (skipped > size)
        goto fail;
    size -= skipped;
#ifdef IPVF_NATIVE_PIPELINE
    if (size < IPVF_MAX_PAYLOAD + reference_bytes +
               IPVF_AUDIO_RING_MIN_BYTES ||
        aligned > (uintptr_t)-1 - IPVF_MAX_PAYLOAD ||
        aligned + IPVF_MAX_PAYLOAD >
            (uintptr_t)-1 - reference_bytes)
        goto fail;
    audio.video_scratch = (unsigned char *)aligned;
    aligned += IPVF_MAX_PAYLOAD;
    size -= IPVF_MAX_PAYLOAD;
    if (needs_video_reference)
    {
        audio.video_reference = (unsigned char *)aligned;
        aligned += reference_bytes;
        size -= reference_bytes;
    }
#endif
    ring_bytes = floor_power_of_two(size);
    if (ring_bytes < IPVF_AUDIO_RING_MIN_BYTES)
        goto fail;

    audio.buffer = (unsigned char *)aligned;
    audio.capacity_frames = ring_bytes / IPVF_AUDIO_FRAME_BYTES;
    audio.frame_mask = audio.capacity_frames - 1;
    audio.low_water_frames = audio.capacity_frames;
    audio.old_frequency = rb->mixer_get_frequency();

    rb->pcm_play_lock();
    if (rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) != CHANNEL_STOPPED)
        rb->mixer_channel_stop(IPVF_AUDIO_CHANNEL);
    rb->pcm_play_unlock();

#if INPUT_SRC_CAPS != 0
    rb->audio_set_input_source(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);
    rb->audio_set_output_source(AUDIO_SRC_PLAYBACK);
#endif
    audio.configured = true;
    rb->mixer_set_frequency(IPVF_AUDIO_SAMPLE_RATE);
    if (rb->mixer_get_frequency() != IPVF_AUDIO_SAMPLE_RATE)
        goto fail;
    rb->pcmbuf_fade(false, true);
    return true;

fail:
    audio_shutdown();
    return false;
}
