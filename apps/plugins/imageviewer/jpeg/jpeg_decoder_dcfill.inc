static void idct_dc_fill(unsigned char *dst, int *block, int *quant,
                         int stride, int block_size)
{
    long dc = (long)block[0] * quant[0];
    unsigned char value;
    int row;

    /* The 2x2/4x4/8x8 transforms round their final divide by eight;
       the existing 1x1 transform deliberately truncates it. */
    if (block_size == 1)
        value = range_limit((int)(dc >> 3));
    else
        value = range_limit((int)DESCALE(dc, 3));

    for (row = 0; row < block_size; row++)
    {
        MEMSET(dst, value, block_size);
        dst += stride;
    }
}

