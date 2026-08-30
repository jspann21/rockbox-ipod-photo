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

    if (!audio.source_eof)
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

    rb->mixer_channel_set_amplitude(IPVF_AUDIO_CHANNEL, MIX_AMP_UNITY);
    rb->mixer_channel_play_data(IPVF_AUDIO_CHANNEL, &audio_mixer_cbs,
                                NULL, 0);
    return true;
}

static bool audio_write_frames(const unsigned char *source, uint32_t frames)
{
    long deadline = *rb->current_tick + HZ * 3;
    uint32_t last_completed = audio.completed_frames;

    while (frames != 0)
    {
        uint32_t written = audio.written_frames;
        uint32_t completed = audio.completed_frames;
        uint32_t used = written - completed;
        uint32_t free_frames;
        uint32_t offset;
        uint32_t count;

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
        if (count > frames)
            count = frames;

        rb->memcpy(audio.buffer +
                   (size_t)offset * IPVF_AUDIO_FRAME_BYTES,
                   source, (size_t)count * IPVF_AUDIO_FRAME_BYTES);
        membarrier();
        audio.written_frames = written + count;
        source += (size_t)count * IPVF_AUDIO_FRAME_BYTES;
        frames -= count;
    }

    return true;
}

static uint32_t audio_clock(void)
{
    uint32_t completed;
    uint32_t current;
    uint32_t waiting;

    do
    {
        completed = audio.completed_frames;
        current = audio.current_frames;
        waiting = (uint32_t)
            (rb->mixer_channel_get_bytes_waiting(IPVF_AUDIO_CHANNEL) /
             IPVF_AUDIO_FRAME_BYTES);
    }
    while (completed != audio.completed_frames ||
           current != audio.current_frames ||
           (waiting == 0 && current != 0 &&
            rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) ==
                CHANNEL_PLAYING));

    if (waiting > current)
        waiting = current;
    return completed + current - waiting;
}

static bool audio_start(void)
{
    if (!audio.configured || audio.started || audio_used_frames() == 0)
        return false;

    audio.started = true;
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

        if (rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) == CHANNEL_STOPPED &&
            !audio_restart())
            return false;
        if (remaining_us > 2500u)
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

    rb->pcm_play_lock();
    if (rb->mixer_channel_status(IPVF_AUDIO_CHANNEL) != CHANNEL_STOPPED)
        rb->mixer_channel_stop(IPVF_AUDIO_CHANNEL);
    rb->pcm_play_unlock();
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

static bool ipodnative_audio_init(void)
{
    unsigned char *raw;
    uintptr_t aligned;
    size_t size;
    size_t skipped;
    uint32_t ring_bytes;

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
    ring_bytes = floor_power_of_two(size);
    if (ring_bytes < IPVF_AUDIO_RING_MIN_BYTES)
        goto fail;

    audio.buffer = (unsigned char *)aligned;
    audio.capacity_frames = ring_bytes / IPVF_AUDIO_FRAME_BYTES;
    audio.frame_mask = audio.capacity_frames - 1;
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
