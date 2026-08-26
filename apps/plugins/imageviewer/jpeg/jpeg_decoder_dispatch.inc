#ifdef HAVE_LCD_COLOR
int jpeg_decode(struct jpeg *p_jpeg, unsigned char *p_pixel[3],
                int downscale, void (*pf_progress)(int current, int total))
{
    if (jpeg_accel_reference_mode)
    {
        int rc = jpeg_legacy_decode(p_jpeg, p_pixel, downscale, pf_progress);
        p_jpeg->decoded_blocks = p_jpeg->x_mbl * p_jpeg->y_mbl *
                                 p_jpeg->blocks;
        p_jpeg->dc_only_blocks = 0;
        return rc;
    }
    return jpeg_decode_accel(p_jpeg, p_pixel, downscale, pf_progress);
}
#else
int jpeg_decode(struct jpeg *p_jpeg, unsigned char *p_pixel[1],
                int downscale, void (*pf_progress)(int current, int total))
{
    if (jpeg_accel_reference_mode)
    {
        int rc = jpeg_legacy_decode(p_jpeg, p_pixel, downscale, pf_progress);
        p_jpeg->decoded_blocks = p_jpeg->x_mbl * p_jpeg->y_mbl *
                                 p_jpeg->blocks;
        p_jpeg->dc_only_blocks = 0;
        return rc;
    }
    return jpeg_decode_accel(p_jpeg, p_pixel, downscale, pf_progress);
}
#endif
