#ifdef HAVE_LCD_COLOR
int jpeg_decode(struct jpeg *p_jpeg, unsigned char *p_pixel[3],
                int downscale, void (*pf_progress)(int current, int total))
{
    return jpeg_decode_accel(p_jpeg, p_pixel, downscale,
                             jpeg_mcu_row_cb != NULL ? NULL : pf_progress);
}
#else
int jpeg_decode(struct jpeg *p_jpeg, unsigned char *p_pixel[1],
                int downscale, void (*pf_progress)(int current, int total))
{
    return jpeg_decode_accel(p_jpeg, p_pixel, downscale, pf_progress);
}
#endif
