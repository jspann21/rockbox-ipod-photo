static void jpeg_stream_mcu_row(unsigned char * const row[3],
                                int y, int height, int stride, void *user)
{
    struct jpeg_strip_state *state =
        (struct jpeg_strip_state *)user;
    struct jpeg_rgb_cache *cache = &rgb_cache[1];
    unsigned char *src[3] = { row[0], row[1], row[2] };
    fb_data *dst;
    uint32_t started;

    if (!state->cache_ok || cache->pixels == NULL)
        return;

    dst = cache->pixels + y * cache->stride;
    if (!yuv_bitmap_part_to_buffer(src, 2, 2, 0, 0, stride,
                                   LCD_WIDTH, height,
                                   dst, cache->stride))
    {
        state->cache_ok = false;
        if (state->lcd_ok)
        {
            jpeg_lcd_stream_abort();
            state->lcd_ok = false;
        }
        return;
    }

    state->strips++;
    if (state->test_enabled)
        jpeg_strip_update_yuv_crc(row, height, stride);

    if (!state->lcd_ok)
        return;

    started = jpeg_now_us();
    if (!jpeg_lcd_stream_write_rgb565(dst, cache->stride, height))
    {
        state->lcd_ok = false;
        return;
    }

    state->lcd_us += jpeg_now_us() - started;
    if (state->strips == 1)
        state->first_strip_us = jpeg_now_us() - state->started_us;
}

static int jpeg_stream_decode_attempt(struct image_info *info, int ds,
                                      bool enable_cop,
                                      struct jpeg_cop_stats *cop_stats)
{
    struct jpeg_rgb_cache *cache = &rgb_cache[ds];
    int status;

    if (!jpeg_allocate_stream_buffers(info, ds))
        return PLUGIN_OUTOFMEM;

    jpeg_strip.strip_only = true;
    jpeg_strip.cache_ok = true;
    jpeg_strip.lcd_ok =
        jpeg_lcd_stream_begin(0, 0, LCD_WIDTH, LCD_HEIGHT);
    jpeg_strip.frame_pending = false;
    jpeg_strip.strips = 0;
    jpeg_strip.first_strip_us = 0;
    jpeg_strip.lcd_us = 0;
    jpeg_strip.yuv_crc = 0xffffffff;

    jpeg_decode_cop_prepare(enable_cop, ds);
    jpeg_decode_set_mcu_row_reuse(true);
    jpeg_decode_set_mcu_row_callback(jpeg_stream_mcu_row, &jpeg_strip);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
    status = jpeg_decode(&jpg, disp[ds].bitmap, ds, iv->cb_progress);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif

    jpeg_decode_set_mcu_row_callback(NULL, NULL);
    jpeg_decode_set_mcu_row_reuse(false);
    jpeg_decode_cop_finish(cop_stats);

    if (status != 0 || !jpeg_strip.cache_ok ||
        jpeg_strip.strips != jpg.y_mbl)
    {
        jpeg_lcd_stream_abort();
        return status != 0 ? PLUGIN_ERROR : PLUGIN_OUTOFMEM;
    }

    cache->valid = true;

    if (jpeg_strip.lcd_ok)
        jpeg_strip.lcd_ok = jpeg_lcd_stream_end();
    else
        jpeg_lcd_stream_abort();

    jpeg_strip.decoded = true;

    if (jpeg_strip.lcd_ok)
    {
        jpeg_strip.frame_pending = true;
        imgdec_handoff_rendered_frame();
    }

    return PLUGIN_OK;
}

static int jpeg_stream_get_image(struct image_info *info, int frame, int ds)
{
    struct jpeg_cop_stats stats;
    bool enable_cop = jpeg_strip.test_enabled &&
                      !jpeg_strip.test_reference;
    int status;

    (void)frame;
    jpeg_strip.started_us = jpeg_now_us();
    jpeg_strip.audio_playing =
        (rb->audio_status() & AUDIO_STATUS_PLAY) != 0;
    rb->memset(&stats, 0, sizeof(stats));

    status = jpeg_stream_decode_attempt(info, ds, enable_cop, &stats);
    jpeg_strip.cop = stats;

    /* A COP-path error must never strand the viewer. Abort the partial LCD
       stream, reset the image pool and perform one proven CPU-only decode. */
    if (status != PLUGIN_OK && enable_cop && stats.selected)
    {
        jpeg_strip.cop_fallback = true;
        jpeg_reset_decoded_pool();
        status = jpeg_stream_decode_attempt(info, ds, false, NULL);
    }

    jpeg_strip.total_us = jpeg_now_us() - jpeg_strip.started_us;
    if (status == PLUGIN_OK)
        jpeg_strip_log(info);
    else
        jpeg_reset_decoded_pool();

    return status;
}

static bool jpeg_materialize_planar(struct image_info *info, int ds)
{
    int status;

    if (!jpeg_strip.decoded || !jpeg_strip.strip_only || ds != 1)
        return true;

    jpeg_decode_cop_finish(NULL);
    jpeg_decode_set_mcu_row_callback(NULL, NULL);
    jpeg_decode_set_mcu_row_reuse(false);
    jpeg_lcd_stream_abort();
    jpeg_reset_decoded_pool();

    status = jpeg_legacy_get_image(info, 0, ds);
    return status == PLUGIN_OK;
}
