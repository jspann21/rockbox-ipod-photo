#if defined(CPU_ARM)
/*
 * Rockbox already carries a classic-ARM JPEG IDCT used by the core bitmap
 * loader. Its vertical passes are independent of output pixel size, so they
 * can be reused here while the horizontal pass writes the imageviewer's
 * contiguous Y/Cb/Cr planes exactly like the legacy decoder.
 */
extern void jpeg_idct2v(int16_t *ws, int16_t *end);
extern void jpeg_idct4v(int16_t *ws, int16_t *end);
extern void jpeg_idct8v(int16_t *ws, int16_t *end);

static void idct2x2_arm(unsigned char *dst, int *block, int *quant,
                        int stride)
{
    int16_t ws[16] __attribute__((aligned(8)));
    int row;

    MEMSET(ws, 0, sizeof(ws));
    ws[0] = (int16_t)DEQUANTIZE(block[0], quant[0]);
    ws[1] = (int16_t)DEQUANTIZE(block[1], quant[1]);
    ws[8] = (int16_t)DEQUANTIZE(block[8], quant[8]);
    ws[9] = (int16_t)DEQUANTIZE(block[9], quant[9]);

    jpeg_idct2v(ws, ws + 2);

    for (row = 0; row < 2; row++)
    {
        int16_t *w = ws + row * 8;
        unsigned char *out = dst + row * stride;

        out[0] = range_limit((int)DESCALE((long)w[0] + w[1], 3));
        out[1] = range_limit((int)DESCALE((long)w[0] - w[1], 3));
    }
}

static void idct4x4_arm(unsigned char *dst, int *block, int *quant,
                        int stride)
{
    int16_t ws[32] __attribute__((aligned(8)));
    int row, col;

    MEMSET(ws, 0, sizeof(ws));
    for (row = 0; row < 4; row++)
        for (col = 0; col < 4; col++)
        {
            int index = row * 8 + col;
            ws[index] = (int16_t)DEQUANTIZE(block[index], quant[index]);
        }

    jpeg_idct4v(ws, ws + 4);

    for (row = 0; row < 4; row++)
    {
        int16_t *w = ws + row * 8;
        unsigned char *out = dst + row * stride;
        int tmp0, tmp2, tmp10, tmp12;
        int z1, z2, z3;

        tmp0 = w[0];
        tmp2 = w[2];

        tmp10 = (tmp0 + tmp2) << CONST_BITS;
        tmp12 = (tmp0 - tmp2) << CONST_BITS;

        z2 = w[1];
        z3 = w[3];

        z1 = MULTIPLY16(z2 + z3, FIX_0_541196100);
        tmp0 = z1 + MULTIPLY16(z3, -FIX_1_847759065);
        tmp2 = z1 + MULTIPLY16(z2, FIX_0_765366865);

        out[0] = range_limit((int)DESCALE(tmp10 + tmp2,
                    CONST_BITS + PASS1_BITS + 3));
        out[3] = range_limit((int)DESCALE(tmp10 - tmp2,
                    CONST_BITS + PASS1_BITS + 3));
        out[1] = range_limit((int)DESCALE(tmp12 + tmp0,
                    CONST_BITS + PASS1_BITS + 3));
        out[2] = range_limit((int)DESCALE(tmp12 - tmp0,
                    CONST_BITS + PASS1_BITS + 3));
    }
}

static void idct8x8_arm(unsigned char *dst, int *block, int *quant,
                        int stride)
{
    int16_t ws[128] __attribute__((aligned(8)));
    int row, col;

    /*
     * jpeg_idct8v() is the transposing ARM variant. Feed each natural-order
     * column as one input row; it writes the vertical-pass workspace to the
     * second half in natural row order.
     */
    for (row = 0; row < 8; row++)
        for (col = 0; col < 8; col++)
        {
            int source = row * 8 + col;
            ws[col * 8 + row] =
                (int16_t)DEQUANTIZE(block[source], quant[source]);
        }

    jpeg_idct8v(ws, ws + 64);

    for (row = 0; row < 8; row++)
    {
        int16_t *w = ws + 64 + row * 8;
        unsigned char *out = dst + row * stride;
        long tmp0, tmp1, tmp2, tmp3;
        long tmp10, tmp11, tmp12, tmp13;
        long z1, z2, z3, z4, z5;

#ifndef NO_ZERO_ROW_TEST
        if ((w[1] | w[2] | w[3] | w[4] |
             w[5] | w[6] | w[7]) == 0)
        {
            unsigned char dc = range_limit((int)DESCALE((long)w[0],
                                                        PASS1_BITS + 3));
            MEMSET(out, dc, 8);
            continue;
        }
#endif

        z2 = w[2];
        z3 = w[6];

        z1 = MULTIPLY16(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY16(z3, -FIX_1_847759065);
        tmp3 = z1 + MULTIPLY16(z2, FIX_0_765366865);

        tmp0 = ((long)w[0] + w[4]) << CONST_BITS;
        tmp1 = ((long)w[0] - w[4]) << CONST_BITS;

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        tmp0 = w[7];
        tmp1 = w[5];
        tmp2 = w[3];
        tmp3 = w[1];

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY16(z3 + z4, FIX_1_175875602);

        tmp0 = MULTIPLY16(tmp0, FIX_0_298631336);
        tmp1 = MULTIPLY16(tmp1, FIX_2_053119869);
        tmp2 = MULTIPLY16(tmp2, FIX_3_072711026);
        tmp3 = MULTIPLY16(tmp3, FIX_1_501321110);
        z1 = MULTIPLY16(z1, -FIX_0_899976223);
        z2 = MULTIPLY16(z2, -FIX_2_562915447);
        z3 = MULTIPLY16(z3, -FIX_1_961570560);
        z4 = MULTIPLY16(z4, -FIX_0_390180644);

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        out[0] = range_limit((int)DESCALE(tmp10 + tmp3,
                    CONST_BITS + PASS1_BITS + 3));
        out[7] = range_limit((int)DESCALE(tmp10 - tmp3,
                    CONST_BITS + PASS1_BITS + 3));
        out[1] = range_limit((int)DESCALE(tmp11 + tmp2,
                    CONST_BITS + PASS1_BITS + 3));
        out[6] = range_limit((int)DESCALE(tmp11 - tmp2,
                    CONST_BITS + PASS1_BITS + 3));
        out[2] = range_limit((int)DESCALE(tmp12 + tmp1,
                    CONST_BITS + PASS1_BITS + 3));
        out[5] = range_limit((int)DESCALE(tmp12 - tmp1,
                    CONST_BITS + PASS1_BITS + 3));
        out[3] = range_limit((int)DESCALE(tmp13 + tmp0,
                    CONST_BITS + PASS1_BITS + 3));
        out[4] = range_limit((int)DESCALE(tmp13 - tmp0,
                    CONST_BITS + PASS1_BITS + 3));
    }
}
#endif /* CPU_ARM */
