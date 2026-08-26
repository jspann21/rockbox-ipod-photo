void yuv_bitmap_part(unsigned char *src[3], int csub_x, int csub_y,
                     int src_x, int src_y, int stride,
                     int x, int y, int width, int height,
                     int colour_mode, int dither_mode)
{
#if LCD_STRIDEFORMAT == HORIZONTAL_STRIDE
    if (!jpeg_accel_reference_mode &&
        colour_mode == COLOURMODE_COLOUR &&
        dither_mode == DITHER_NONE &&
        x >= 0 && y >= 0 && width > 0 && height > 0 &&
        x + width <= LCD_WIDTH && y + height <= LCD_HEIGHT &&
        (csub_y == 0 ||
         ((csub_x == 1 || csub_x == 2) &&
          (csub_y == 1 || csub_y == 2))))
    {
        struct viewport *vp_main =
            *(rb->screens[SCREEN_MAIN]->current_viewport);
        fb_data *lcd_fb = vp_main->buffer->fb_ptr;

        if (jpeg_yuv_to_rgb565(src, csub_x, csub_y,
                               src_x, src_y, stride,
                               width, height, LCDADDR(x, y), LCD_WIDTH))
            return;
    }
#endif

    yuv_bitmap_part_legacy(src, csub_x, csub_y,
                           src_x, src_y, stride,
                           x, y, width, height,
                           colour_mode, dither_mode);
}
