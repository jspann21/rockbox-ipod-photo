    for (y = 0; y < p_jpeg->y_mbl &&
                bs.next_input_byte <= bs.input_end; y++)
    {
#ifdef HAVE_LCD_COLOR
        int row_ac_blocks = 0;
        bool cop_row = jpeg_cop_row_active();

        for (i = 0; i < 3; i++)
        {
            p_strip[i] = p_line[i];
            p_byte[i] = p_line[i];
            p_line[i] += skip_strip[i];
        }
#else
        p_byte[0] = p_line[0];
        p_line[0] += skip_strip[0];
#endif

        for (x = 0; x < p_jpeg->x_mbl; x++)
        {
            int blkn;

            for (blkn = 0; blkn < p_jpeg->blocks; blkn++)
            {
                int k = 1;
                int s, r;
                int ci = p_jpeg->mcu_membership[blkn];
                int ti = p_jpeg->tab_membership[blkn];
                bool output_block;
                bool required_ac_nonzero = false;
                struct derived_tbl *dctbl = &p_jpeg->dc_derived_tbls[ti];
                struct derived_tbl *actbl = &p_jpeg->ac_derived_tbls[ti];

#ifdef HAVE_LCD_COLOR
                output_block = true;
#else
                output_block = ci == 0;
#endif

                s = huff_decode_dc(&bs, dctbl);
                last_dc_val[ci] += s;
                if (output_block)
                {
                    block[0] = last_dc_val[ci];
                    MEMSET(block + 1, 0, zero_need * sizeof(block[0]));

                    for (; k < k_need; k++)
                    {
                        s = huff_decode_ac(&bs, actbl);
                        r = s >> 4;
                        s &= 15;
                        if (s)
                        {
                            k += r;
                            check_bit_buffer(&bs, s);
                            r = get_bits(&bs, s);
                            block[zag[k]] = HUFF_EXTEND(r, s);
                            required_ac_nonzero = true;
                        }
                        else
                        {
                            if (r != 15)
                            {
                                k = 64;
                                break;
                            }
                            k += r;
                        }
                    }
                }

                for (; k < 64; k++)
                {
                    s = huff_decode_ac(&bs, actbl);
                    r = s >> 4;
                    s &= 15;
                    if (s)
                    {
                        k += r;
                        check_bit_buffer(&bs, s);
                        drop_bits(&bs, s);
                    }
                    else
                    {
                        if (r != 15)
                            break;
                        k += r;
                    }
                }

                if (!output_block)
                    continue;

#ifdef HAVE_LCD_COLOR
                if (required_ac_nonzero)
                    row_ac_blocks++;
#endif

                {
                    unsigned char *dst;
#ifdef HAVE_LCD_COLOR
                    dst = ci == 0 ? p_byte[0] +
                        store_offs[p_jpeg->mcu_block_pos[blkn]] : p_byte[ci];
#else
                    dst = p_byte[0] + store_offs[blkn];
#endif
#ifdef HAVE_LCD_COLOR
                    if (cop_row)
                    {
                        bool end_of_row =
                            x == p_jpeg->x_mbl - 1 &&
                            blkn == p_jpeg->blocks - 1;

                        if (!jpeg_cop_submit(dst, block,
                                             p_jpeg->qt_idct[ti],
                                             skip_line[ci],
                                             !required_ac_nonzero,
                                             end_of_row))
                            return JPEG_DECODE_COP_ERROR;
                    }
                    else
#endif
                    if (!required_ac_nonzero)
                    {
                        idct_dc_fill(dst, block, p_jpeg->qt_idct[ti],
                                     skip_line[ci], 8 / downscale);
                    }
                    else
                    {
                        pf_idct(dst, block, p_jpeg->qt_idct[ti],
                                skip_line[ci]);
                    }
                }
            }

            p_byte[0] += skip_mcu[0];
#ifdef HAVE_LCD_COLOR
            p_byte[1] += skip_mcu[1];
            p_byte[2] += skip_mcu[2];
#endif

            if (p_jpeg->restart_interval && --restart == 0)
            {
                restart = p_jpeg->restart_interval;
                search_restart(&bs);
                last_dc_val[0] = last_dc_val[1] = last_dc_val[2] = 0;
            }
        }

#ifdef HAVE_LCD_COLOR
        if (cop_row && !jpeg_cop_drain())
            return JPEG_DECODE_COP_ERROR;

        if (jpeg_mcu_row_cb != NULL)
        {
            int row_y = y * strip_height;
            int row_height = MIN(strip_height, visible_height - row_y);

            if (row_height > 0)
                jpeg_mcu_row_cb(p_strip, row_y, row_height, skip_line[0],
                                jpeg_mcu_row_cb_data);
        }

        /* The first row is decoded on the CPU and acts as a content probe.
           Start the COP only when the image has enough real AC work to
           amortize thread and cache-maintenance overhead. */
        if (y == 0)
            jpeg_cop_consider_probe(row_ac_blocks,
                                    p_jpeg->x_mbl * p_jpeg->blocks);

        if (jpeg_mcu_row_reuse)
        {
            p_line[0] = p_pixel[0];
            p_line[1] = p_pixel[1];
            p_line[2] = p_pixel[2];
        }
#endif

        if (pf_progress != NULL)
            pf_progress(y, p_jpeg->y_mbl - 1);
    }

    return 0;
}
