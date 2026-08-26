static int jpeg_segment(unsigned char **cursor, unsigned char *end,
                        unsigned char **segment_end)
{
    unsigned char *p = *cursor;
    unsigned int length;

    if (end - p < 2)
        return -9;

    length = ((unsigned int)p[0] << 8) | p[1];
    if (length < 2 || (unsigned long)(end - p) < length)
        return -9;

    *cursor = p + 2;
    *segment_end = p + length;
    return 0;
}

/* Baseline marker parser with independent 0..3 quant/DC/AC selectors. */
int process_markers(unsigned char *p_src, long size, struct jpeg *p_jpeg)
{
    unsigned char *p_end = p_src + size;
    int ret = 0;

    p_jpeg->p_entropy_end = p_end;
    p_jpeg->p_entropy_data = NULL;
    p_jpeg->dqt_present = 0;
    p_jpeg->dht_dc_present = 0;
    p_jpeg->dht_ac_present = 0;
    p_jpeg->table_error = 0;
    p_jpeg->restart_interval = 0;

    while (p_src < p_end)
    {
        unsigned char *seg_end;
        int marker;

        while (p_src < p_end && *p_src != 0xff)
            p_src++;
        if (p_src >= p_end)
            break;

        while (p_src < p_end && *p_src == 0xff)
            p_src++;
        if (p_src >= p_end)
            return -9;

        marker = *p_src++;
        if (marker == 0x00) /* stuffed data outside the scan */
            continue;

        switch (marker)
        {
        case 0xd8: /* SOI */
        case 0xd9: /* EOI */
        case 0x01: /* TEM */
        case 0xd0: case 0xd1: case 0xd2: case 0xd3:
        case 0xd4: case 0xd5: case 0xd6: case 0xd7:
            break;

        case 0xc0: /* baseline SOF */
        {
            int i, components;
            int previous;

            if (jpeg_segment(&p_src, p_end, &seg_end) < 0 ||
                seg_end - p_src < 6)
                return -9;

            ret |= SOF0;
            if (*p_src++ != 8)
                return -1;

            p_jpeg->y_size = ((int)p_src[0] << 8) | p_src[1];
            p_src += 2;
            p_jpeg->x_size = ((int)p_src[0] << 8) | p_src[1];
            p_src += 2;
            components = *p_src++;

            if ((components != 1 && components != 3) ||
                seg_end - p_src < components * 3)
                return -2;

            if (p_jpeg->x_size <= 0 || p_jpeg->y_size <= 0)
                return -2;

            p_jpeg->components = components;
            p_jpeg->blocks = components;
            for (i = 0; i < components; i++)
            {
                int sampling;
                struct frame_component *fc = &p_jpeg->frameheader[i];

                fc->ID = *p_src++;
                sampling = *p_src++;
                fc->horizontal_sampling = sampling >> 4;
                fc->vertical_sampling = sampling & 0x0f;
                fc->quanttable_select = *p_src++;

                for (previous = 0; previous < i; previous++)
                    if (p_jpeg->frameheader[previous].ID == fc->ID)
                        return -2;

                if (fc->horizontal_sampling < 1 ||
                    fc->horizontal_sampling > 2 ||
                    fc->vertical_sampling < 1 ||
                    fc->vertical_sampling > 2)
                    return -3;
                if (fc->quanttable_select >= JPEG_MAX_TABLES)
                    return -8;
            }
            p_src = seg_end;
            break;
        }

        case 0xc1: case 0xc2: case 0xc3:
        case 0xc5: case 0xc6: case 0xc7:
        case 0xc8: case 0xc9: case 0xca: case 0xcb:
        case 0xcd: case 0xce: case 0xcf:
            return -4; /* unsupported coding process */

