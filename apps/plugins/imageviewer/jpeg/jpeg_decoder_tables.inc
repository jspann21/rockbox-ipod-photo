/* Build one combined quant/DC/AC slot per frame component. */
void build_lut(struct jpeg *p_jpeg)
{
    int i, j;

    p_jpeg->table_error = 0;

    /* Keep the legacy geometry/sampling setup in one place.  Its temporary
       table-0/1 derivation is overwritten below. */
    jpeg_legacy_build_lut(p_jpeg);

    if (p_jpeg->dht_dc_present == 0 && p_jpeg->dht_ac_present == 0)
    {
        /* default_huff_tbl() was used by the caller */
        p_jpeg->dht_dc_present = 0x03;
        p_jpeg->dht_ac_present = 0x03;
    }

    if (p_jpeg->components < 1 ||
        p_jpeg->components > JPEG_MAX_COMPONENTS ||
        p_jpeg->x_mbl <= 0 || p_jpeg->y_mbl <= 0 ||
        p_jpeg->p_entropy_data == NULL)
    {
        p_jpeg->table_error = -12;
        return;
    }

    if ((p_jpeg->components == 1 &&
         (p_jpeg->frameheader[0].horizontal_sampling != 1 ||
          p_jpeg->frameheader[0].vertical_sampling != 1)) ||
        (p_jpeg->components == 3 &&
         (p_jpeg->frameheader[1].horizontal_sampling != 1 ||
          p_jpeg->frameheader[1].vertical_sampling != 1 ||
          p_jpeg->frameheader[2].horizontal_sampling != 1 ||
          p_jpeg->frameheader[2].vertical_sampling != 1)))
    {
        p_jpeg->table_error = -3;
        return;
    }

    for (i = 0; i < p_jpeg->components; i++)
    {
        int scan_index = -1;
        int q, d, a;

        for (j = 0; j < p_jpeg->components; j++)
        {
            if (p_jpeg->scanheader[j].ID == p_jpeg->frameheader[i].ID)
            {
                scan_index = j;
                break;
            }
        }

        if (scan_index < 0)
        {
            p_jpeg->table_error = -12;
            return;
        }

        q = p_jpeg->frameheader[i].quanttable_select;
        d = p_jpeg->scanheader[scan_index].DC_select;
        a = p_jpeg->scanheader[scan_index].AC_select;

        if (q >= JPEG_MAX_TABLES || d >= JPEG_MAX_TABLES ||
            a >= JPEG_MAX_TABLES ||
            !(p_jpeg->dqt_present & (1u << q)) ||
            !(p_jpeg->dht_dc_present & (1u << d)) ||
            !(p_jpeg->dht_ac_present & (1u << a)))
        {
            p_jpeg->table_error = -13;
            return;
        }

        p_jpeg->component_quant[i] = q;
        p_jpeg->component_dc[i] = d;
        p_jpeg->component_ac[i] = a;

        fix_huff_tbl(p_jpeg->hufftable[d].huffmancodes_dc,
                     &p_jpeg->dc_derived_tbls[i]);
        fix_huff_tbl(p_jpeg->hufftable[a].huffmancodes_ac,
                     &p_jpeg->ac_derived_tbls[i]);
        for (j = 0; j < QUANT_TABLE_LENGTH; j++)
            p_jpeg->qt_idct[i][zag[j]] = p_jpeg->quanttable[q][j];
    }

    /* Entropy blocks follow SOS order, not necessarily SOF order.  Rebuild
       block membership while retaining the legacy output geometry. */
    {
        int block = 0;

        for (i = 0; i < p_jpeg->components; i++)
        {
            int ci = -1;
            int count;

            for (j = 0; j < p_jpeg->components; j++)
                if (p_jpeg->scanheader[i].ID == p_jpeg->frameheader[j].ID)
                {
                    ci = j;
                    break;
                }

            if (ci < 0)
            {
                p_jpeg->table_error = -12;
                return;
            }

            count = p_jpeg->frameheader[ci].horizontal_sampling *
                    p_jpeg->frameheader[ci].vertical_sampling;
            for (j = 0; j < count; j++)
            {
                if (block >= (int)ARRAYLEN(p_jpeg->mcu_membership))
                {
                    p_jpeg->table_error = -3;
                    return;
                }
                p_jpeg->mcu_membership[block] = ci;
                p_jpeg->mcu_block_pos[block] = j;
                p_jpeg->tab_membership[block] = ci;
                block++;
            }
        }

        if (block != p_jpeg->blocks)
        {
            p_jpeg->table_error = -3;
            return;
        }
    }
}

