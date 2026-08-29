        case 0xc4: /* DHT */
            if (jpeg_segment(&p_src, p_end, &seg_end) < 0)
                return -9;
            ret |= DHT;
            while (p_src < seg_end)
            {
                int i, sum = 0;
                unsigned int spec;
                int table_class, table_id;
                int *table;
                int capacity;

                if (seg_end - p_src < 17)
                    return -9;

                spec = *p_src++;
                table_class = spec >> 4;
                table_id = spec & 0x0f;
                if (table_class > 1 || table_id >= JPEG_MAX_TABLES)
                    return -5;

                table = table_class ?
                    p_jpeg->hufftable[table_id].huffmancodes_ac :
                    p_jpeg->hufftable[table_id].huffmancodes_dc;
                capacity = table_class ? AC_LEN : DC_LEN;
                MEMSET(table, 0, capacity * sizeof(*table));

                for (i = 0; i < 16; i++)
                {
                    table[i] = *p_src++;
                    sum += table[i];
                }

                if (16 + sum > capacity || seg_end - p_src < sum)
                    return table_class ? -10 : -11;

                for (i = 0; i < sum; i++)
                    table[16 + i] = *p_src++;

                if (table_class)
                    p_jpeg->dht_ac_present |= 1u << table_id;
                else
                    p_jpeg->dht_dc_present |= 1u << table_id;
            }
            break;

        case 0xcc:
            return -6; /* arithmetic conditioning */

        case 0xda: /* SOS */
        {
            int i, components;
            int previous;

            if (jpeg_segment(&p_src, p_end, &seg_end) < 0 ||
                p_src >= seg_end)
                return -9;

            ret |= SOS;
            components = *p_src++;
            if ((components != 1 && components != 3) ||
                components != p_jpeg->components ||
                components > JPEG_MAX_COMPONENTS ||
                seg_end - p_src < components * 2 + 3)
                return -7;

            for (i = 0; i < components; i++)
            {
                int selectors;
                p_jpeg->scanheader[i].ID = *p_src++;
                selectors = *p_src++;
                p_jpeg->scanheader[i].DC_select = selectors >> 4;
                p_jpeg->scanheader[i].AC_select = selectors & 0x0f;
                for (previous = 0; previous < i; previous++)
                    if (p_jpeg->scanheader[previous].ID ==
                        p_jpeg->scanheader[i].ID)
                        return -7;
                if (p_jpeg->scanheader[i].DC_select >= JPEG_MAX_TABLES ||
                    p_jpeg->scanheader[i].AC_select >= JPEG_MAX_TABLES)
                    return -12;
            }

            /* Baseline sequential: Ss=0, Se=63, Ah/Al=0. */
            if (p_src[0] != 0 || p_src[1] != 63 || p_src[2] != 0)
                return -7;

            p_jpeg->p_entropy_data = seg_end;
            return ret;
        }
