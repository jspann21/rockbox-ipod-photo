static void jpeg_stream_mcu_row(unsigned char * const row[3],
                                int y, int height, int stride, void *user)
{
    struct jpeg_strip_state *state =
        (struct jpeg_strip_state *)user;
    struct jpeg_rgb_cache *cache = &rgb_cache[1];
    unsigned char *src[3] = { row[0], row[1], row[2] };
    fb_data *dst;

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

    if (state->lcd_ok &&
        !jpeg_lcd_stream_write_rgb565(dst, cache->stride, height))
        state->lcd_ok = false;
}

static int jpeg_stream_get_image(struct image_info *info, int frame, int ds)
{
    struct jpeg_rgb_cache *cache = &rgb_cache[ds];
    int status;

    (void)frame;
    if (!jpeg_allocate_stream_buffers(info, ds))
        return PLUGIN_OUTOFMEM;

    jpeg_strip.strip_only = true;
    jpeg_strip.cache_ok = true;
    jpeg_strip.lcd_ok =
        jpeg_lcd_stream_begin(0, 0, LCD_WIDTH, LCD_HEIGHT);
    jpeg_strip.frame_pending = false;
    jpeg_strip.strips = 0;

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

    if (status != 0 || !jpeg_strip.cache_ok ||
        jpeg_strip.strips != jpg.y_mbl)
    {
        jpeg_lcd_stream_abort();
        jpeg_reset_decoded_pool();
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

static bool jpeg_materialize_planar(struct image_info *info, int ds)
{
    int status;

    if (!jpeg_strip.decoded || !jpeg_strip.strip_only || ds != 1)
        return true;

    jpeg_decode_set_mcu_row_callback(NULL, NULL);
    jpeg_decode_set_mcu_row_reuse(false);
    jpeg_lcd_stream_abort();
    jpeg_reset_decoded_pool();

    status = jpeg_legacy_get_image(info, 0, ds);
    return status == PLUGIN_OK;
}
