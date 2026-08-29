        case 0xdb: /* DQT */
            if (jpeg_segment(&p_src, p_end, &seg_end) < 0)
                return -9;
            ret |= DQT;
            while (p_src < seg_end)
            {
                int i;
                unsigned int spec;
                int precision, table_id;

                spec = *p_src++;
                precision = spec >> 4;
                table_id = spec & 0x0f;
                if (precision != 0 || table_id >= JPEG_MAX_TABLES ||
                    seg_end - p_src < QUANT_TABLE_LENGTH)
                    return -8;

                for (i = 0; i < QUANT_TABLE_LENGTH; i++)
                    p_jpeg->quanttable[table_id][i] = *p_src++;
                p_jpeg->dqt_present |= 1u << table_id;
            }
            break;

        case 0xdd: /* DRI */
            if (jpeg_segment(&p_src, p_end, &seg_end) < 0 ||
                seg_end - p_src != 2)
                return -9;
            p_jpeg->restart_interval = ((int)p_src[0] << 8) | p_src[1];
            p_src = seg_end;
            break;

        case 0xdc: case 0xde: case 0xdf:
        case 0xe0: case 0xe1: case 0xe2: case 0xe3:
        case 0xe4: case 0xe5: case 0xe6: case 0xe7:
        case 0xe8: case 0xe9: case 0xea: case 0xeb:
        case 0xec: case 0xed: case 0xee: case 0xef:
        case 0xfe:
            if (jpeg_segment(&p_src, p_end, &seg_end) < 0)
                return -9;
            p_src = seg_end;
            break;

        default:
            return -9;
        }
    }

    return ret;
}
