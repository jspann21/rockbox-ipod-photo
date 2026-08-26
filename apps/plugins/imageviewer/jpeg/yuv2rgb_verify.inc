/* Compare the fast buffer with the exact legacy no-dither pixel function.
   This runs only when jpegbench.enabled exists. */
bool yuv_bitmap_verify_rgb565(unsigned char *src[3], int csub_x, int csub_y,
                              int src_x, int src_y, int stride,
                              int width, int height,
                              const fb_data *fast, int fast_stride,
                              uint32_t *reference_crc,
                              uint32_t *mismatch_count)
{
    struct rgb_pixel px;
    struct rgb_pixel *saved_pixel = pixel;
    fb_data reference_row[LCD_WIDTH];
    uint32_t crc = 0xffffffff;
    uint32_t mismatches = 0;
    int row;

    if (width <= 0 || width > LCD_WIDTH || height <= 0 || fast == NULL)
        return false;
    if (csub_y != 0 &&
        ((csub_x != 1 && csub_x != 2) ||
         (csub_y != 1 && csub_y != 2)))
        return false;

    pixel = &px;
    for (row = 0; row < height; row++)
    {
        const unsigned char *ysrc = src[0] +
            (src_y + row) * stride + src_x;
        const fb_data *fast_row = fast + row * fast_stride;
        int col;

        for (col = 0; col < width; col++)
        {
            int u = 0;
            int v = 0;
            int yy = YFAC * ysrc[col];

            if (csub_y != 0)
            {
                int cstride = stride / csub_x;
                int ci = (src_x + col) / csub_x;
                int crow = (src_y + row) / csub_y;
                u = src[1][crow * cstride + ci] - 128;
                v = src[2][crow * cstride + ci] - 128;
            }

            px.r = yy + RVFAC * v;
            px.g = yy + GUFAC * u + GVFAC * v;
            px.b = yy + BUFAC * u;
            reference_row[col] = pixel_to_lcd_colour();
            if (reference_row[col] != fast_row[col])
                mismatches++;
        }

        crc = rb->crc_32(reference_row,
                         width * sizeof(reference_row[0]), crc);
    }

    pixel = saved_pixel;
    if (reference_crc != NULL)
        *reference_crc = crc;
    if (mismatch_count != NULL)
        *mismatch_count = mismatches;
    return true;
}

