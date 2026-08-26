static int jpeg_decode_accel(struct jpeg *p_jpeg, unsigned char **p_pixel,
                             int downscale,
                             void (*pf_progress)(int current, int total))
{
    struct bitstream bs;
    int block[64];
    int width, height;
    int skip_line[3] = {0, 0, 0};
    int skip_strip[3] = {0, 0, 0};
    int skip_mcu[3] = {0, 0, 0};
    unsigned char *p_line[3] = {p_pixel[0], NULL, NULL};
    unsigned char *p_byte[3] = {NULL, NULL, NULL};
    void (*pf_idct)(unsigned char*, int*, int*, int);
    int k_need, zero_need;
    int last_dc_val[3] = {0, 0, 0};
    int store_offs[4];
    int restart = p_jpeg->restart_interval;
    int x, y, i;

    if (p_jpeg->table_error)
        return p_jpeg->table_error;

    if (downscale == 1)
    {
        pf_idct = idct8x8;
        k_need = 64;
        zero_need = 63;
    }
    else if (downscale == 2)
    {
        pf_idct = idct4x4;
        k_need = 25;
        zero_need = 27;
    }
    else if (downscale == 4)
    {
        pf_idct = idct2x2;
        k_need = 5;
        zero_need = 9;
    }
    else if (downscale == 8)
    {
        pf_idct = idct1x1;
        k_need = 0;
        zero_need = 0;
    }
    else
        return -1;

    p_jpeg->decoded_blocks = 0;
    p_jpeg->dc_only_blocks = 0;

    bs.get_buffer = 0;
    bs.next_input_byte = p_jpeg->p_entropy_data;
    bs.bits_left = 0;
    bs.input_end = p_jpeg->p_entropy_end;

    width = p_jpeg->x_phys / downscale;
    height = p_jpeg->y_phys / downscale;

#ifdef HAVE_LCD_COLOR
    p_line[1] = p_pixel[1];
    p_line[2] = p_pixel[2];
    for (i = 0; i < 3; i++)
#else
    for (i = 0; i < 1; i++)
#endif
    {
        skip_line[i] = width / p_jpeg->subsample_x[i];
        skip_strip[i] = skip_line[i] *
            (height / p_jpeg->y_mbl) / p_jpeg->subsample_y[i];
        skip_mcu[i] = width / p_jpeg->x_mbl / p_jpeg->subsample_x[i];
    }

    store_offs[p_jpeg->store_pos[0]] = 0;
    store_offs[p_jpeg->store_pos[1]] = 8 / downscale;
    store_offs[p_jpeg->store_pos[2]] = width * 8 / downscale;
    store_offs[p_jpeg->store_pos[3]] = store_offs[1] + store_offs[2];

