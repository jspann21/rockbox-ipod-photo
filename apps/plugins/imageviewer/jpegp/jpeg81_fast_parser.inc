static enum JPEGENUM jpegp_decode_fast(struct JPEGD *j)
{
    int marker;
    int i;
    bool have_frame = false;

    jpegp_fast_reset_all();

    j->jpeg_mem = 0;
    marker = NextMarker(j);
    if (marker != 0xd8)
        return JPEGENUMERR_MISSING_SOI;

    j->Ri = 0;
    j->QT[0][0] = 0;
    j->QT[1][0] = 0;
    j->QT[2][0] = 0;
    j->QT[3][0] = 0;

    for (;;)
    {
        marker = NextMarker(j);

        if (marker == 0xc4) /* DHT */
        {
            int remaining = GETWbi() - 2;

            if (remaining < 17)
                return JPEGP_FAST_FALLBACK;

            while (remaining > 0)
            {
                int symbols = 0;
                int selector;
                int table_class;
                int table_id;
                int *base;
                unsigned char *values;

                if (remaining < 17)
                    return JPEGP_FAST_FALLBACK;

                selector = GETC();
                table_class = selector >> 4;
                table_id = selector & 15;
                if (table_class > 1 || table_id > 3)
                    return JPEGP_FAST_FALLBACK;

                base = j->HTB[table_class][table_id];
                values = j->HTS[table_class][table_id];

                for (i = 0; i < 16; i++)
                {
                    int count = GETC();
                    symbols += count;
                    base[i] = symbols;
                }

                if (symbols <= 0 || symbols > 256 ||
                    remaining < 17 + symbols)
                    return JPEGP_FAST_FALLBACK;

                for (i = 0; i < symbols; i++)
                    values[i] = GETC();

                if (!jpegp_build_look(
                        base, jpegp_fast.look[table_class][table_id]))
                    return JPEGP_FAST_FALLBACK;

                jpegp_fast.dht_present[table_class] |= 1u << table_id;
                remaining -= 17 + symbols;
            }
        }
        else if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2)
        {
            int total_du;
            DU *du;
            int frame_length = GETWbi();

            j->P = GETC();
            j->Y = GETWbi();
            j->X = GETWbi();
            j->Nf = GETC();

            if (frame_length != 8 + 3 * j->Nf ||
                j->P != 8 || j->Nf < 1 || j->Nf > 4 ||
                j->Y == 0 || j->X == 0)
                return JPEGP_FAST_FALLBACK;

            j->SOF = marker;
            if (!jpegp_fast_set_dim(j, 8, &total_du))
                return JPEGP_FAST_FALLBACK;
            du = calloc((size_t)total_du, sizeof(DU));
            if (du == NULL)
                return JPEGENUMERR_MALLOC;

            j->jpeg_mem = du;
            for (i = 0; i < j->Nf; i++)
            {
                struct COMP *component = j->Components + i;
                component->du = du;
                du += component->du_total;
            }

            j->Reset_decoder = jpegp_fast_reset_decoder;
            j->Byte_in = Byte_in_huff;
            have_frame = true;
        }
        else if (marker == 0xda) /* SOS */
        {
            int scan_component;
            int dc_tables[4] = { 0, 0, 0, 0 };
            int ac_tables[4] = { 0, 0, 0, 0 };
            int scan_length;

            if (!have_frame)
                return JPEGP_FAST_FALLBACK;

            scan_length = GETWbi();
            j->Ns = GETC();
            if (j->Ns < 1 || j->Ns > 4 ||
                scan_length != 6 + 2 * j->Ns)
                return JPEGP_FAST_FALLBACK;

            for (scan_component = 0;
                 scan_component < j->Ns;
                 scan_component++)
            {
                struct COMP *sc;
                int component_id = GETC();
                int tables = GETC();
                int dc_table = tables >> 4;
                int ac_table = tables & 15;
                int ci;

                for (ci = 0;
                     ci < j->Nf &&
                     j->Components[ci].Ci != component_id;
                     ci++)
                {
                }

                if (ci == j->Nf || dc_table > 3 || ac_table > 3)
                    return JPEGENUMERR_COMPNOTFOUND;

                sc = j->Components + ci;
                dc_tables[scan_component] = dc_table;
                ac_tables[scan_component] = ac_table;
                j->ScanComponents[scan_component] = sc;
                sc->ACB = j->HTB[1][ac_table];
                sc->ACS = j->HTS[1][ac_table];
                sc->DCB = j->HTB[0][dc_table];
                sc->DCS = j->HTS[0][dc_table];
                jpegp_fast.ac_look[ci] =
                    jpegp_fast.look[1][ac_table];
                jpegp_fast.dc_look[ci] =
                    jpegp_fast.look[0][dc_table];
            }

            j->Ss = GETC();
            j->Se = GETC();
            j->Al = GETC();
            j->Ah = j->Al >> 4;
            j->Al &= 15;
            j->Al2 = 1 << j->Al;

            if (j->Ss < 0 || j->Ss > 63 ||
                j->Se < j->Ss || j->Se > 63 ||
                j->Ah > 13 || j->Al > 13)
                return JPEGP_FAST_FALLBACK;

            for (scan_component = 0;
                 scan_component < j->Ns;
                 scan_component++)
            {
                bool needs_dc = j->Ss == 0 &&
                                (j->Se != 0 || j->Ah == 0);
                bool needs_ac = j->Se != 0;

                if ((needs_dc &&
                     !(jpegp_fast.dht_present[0] &
                       (1u << dc_tables[scan_component]))) ||
                    (needs_ac &&
                     !(jpegp_fast.dht_present[1] &
                       (1u << ac_tables[scan_component]))))
                    return JPEGP_FAST_FALLBACK;
            }

            j->Reset_decoder(j);

            j->DecodeDataUnit =
                j->Ss
                    ? (j->Ah ? jpegp_fast_ac_succ
                             : jpegp_fast_ac_decode)
                    : (j->Se ? jpegp_fast_sequential
                             : (j->Ah ? jpegp_fast_dc_succ
                                      : jpegp_fast_dc_decode));

            if (j->Ns > 1)
                DecodeInterleaved_DCT(j);
            else
                DecodeSingle_DCT(j);

            if (jpegp_fast.stream_error)
                return JPEGP_FAST_FALLBACK;
        }
        else if (marker == 0xd9)
        {
            if (!have_frame)
                return JPEGP_FAST_FALLBACK;
            for (i = 0; i < j->Nf; i++)
                if (!(jpegp_fast.dqt_present &
                      (1u << j->Components[i].Qi)))
                    return JPEGENUMERR_NODQT;
            return JPEGENUM_OK;
        }
        else if (marker == 0xdb) /* DQT */
        {
            int remaining = GETWbi() - 2;

            if (remaining < 65)
                return JPEGP_FAST_FALLBACK;

            while (remaining > 0)
            {
                int selector = GETC();
                int precision = selector >> 4;
                int table_id = selector & 15;
                int table_bytes = precision ? 129 : 65;
                int *table;
                int (*get_value)(void);

                if (precision > 1 || table_id > 3 ||
                    remaining < table_bytes)
                    return JPEGP_FAST_FALLBACK;

                table = j->QT[table_id];
                get_value = precision ? GETWbi : GETC;
                if (*table)
                    return JPEGENUMERR_QTREDEF;

                for (i = 0; i < 64; i++)
                    table[i] = get_value();

                jpegp_fast.dqt_present |= 1u << table_id;
                remaining -= table_bytes;
            }
        }
        else if (marker == 0xdd) /* DRI */
        {
            if (GETWbi() != 4)
                return JPEGP_FAST_FALLBACK;
            j->Ri = GETWbi();
        }
        else if ((marker & 0xf0) == 0xe0) /* APPn */
        {
            int length = GETWbi();
            if (length < 2)
                return JPEGP_FAST_FALLBACK;
            SEEK(length - 2);
        }
        else if (marker == 0xfe) /* COM */
        {
            int length = GETWbi();
            if (length < 2)
                return JPEGP_FAST_FALLBACK;
            SEEK(length - 2);
        }
        else if (marker == 0xdc)
        {
            return JPEGENUMERR_MARKERDNL;
        }
        else
        {
            /* Arithmetic, lossless, differential and unknown extensions stay
             * on the complete legacy decoder. */
            return JPEGP_FAST_FALLBACK;
        }
    }
}
