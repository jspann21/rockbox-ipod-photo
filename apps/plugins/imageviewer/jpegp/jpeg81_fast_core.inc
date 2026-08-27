
#define JPEGP_LOOK_BITS 8
#define JPEGP_LOOK_SIZE (1 << JPEGP_LOOK_BITS)
#define JPEGP_FAST_FALLBACK ((enum JPEGENUM)-1200)

struct jpegp_fast_state
{
    uint32_t bit_buffer;
    unsigned int bits;
    uint16_t look[2][4][JPEGP_LOOK_SIZE];
    uint16_t *dc_look[4];
    uint16_t *ac_look[4];
    bool stream_error;
    unsigned char dht_present[2];
    unsigned char dqt_present;
};

static struct jpegp_fast_state jpegp_fast;
static struct jpegp_huffman_stats jpegp_stats;
static bool jpegp_fast_enabled = true;

static uint32_t jpegp_now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (uint32_t)*rb->current_tick * (1000000u / HZ);
#endif
}

void jpegp_huffman_set_fast(bool enable)
{
    jpegp_fast_enabled = enable;
}

void jpegp_huffman_get_stats(struct jpegp_huffman_stats *stats)
{
    if (stats != NULL)
        *stats = jpegp_stats;
}

static void jpegp_fast_reset_all(void)
{
    memset(&jpegp_fast, 0, sizeof(jpegp_fast));
}

static int jpegp_fast_byte(struct JPEGD *j)
{
    int value = GETC();

    (void)j;
    jpegp_stats.bytes++;

    if (value == 0xff)
    {
        int consumed = 1;
        int marker = GETC();

        jpegp_stats.bytes++;
        consumed++;

        while (marker == 0xff)
        {
            marker = GETC();
            jpegp_stats.bytes++;
            consumed++;
        }

        if (marker != 0)
        {
            /* Lookahead may legitimately ask for eight bits when only the
             * final padded bits remain. Put the marker back so NextMarker()
             * can consume it after the scan and let the slow decoder use the
             * bits already in the reservoir. */
            SEEK(-consumed);
            return -1;
        }

        /* FF00 represents one entropy byte with value FF. */
        value = 0xff;
    }

    return value & 0xff;
}

static bool jpegp_fast_fill(struct JPEGD *j, unsigned int need)
{
    while (jpegp_fast.bits < need)
    {
        int value;

        if (jpegp_fast.bits > 24)
            return false;

        value = jpegp_fast_byte(j);
        if (value < 0)
            return false;

        jpegp_fast.bit_buffer =
            (jpegp_fast.bit_buffer << 8) | (unsigned int)value;
        jpegp_fast.bits += 8;
    }

    return true;
}

static inline unsigned int jpegp_fast_peek(unsigned int count)
{
    return (jpegp_fast.bit_buffer >> (jpegp_fast.bits - count)) &
           ((1u << count) - 1u);
}

static inline void jpegp_fast_drop(unsigned int count)
{
    jpegp_fast.bits -= count;
}

static int jpegp_fast_get_bits(struct JPEGD *j, int count)
{
    unsigned int value;

    jpegp_stats.getbits_calls++;

    if (count <= 0)
        return 0;
    if (count > 16)
    {
        jpegp_fast.stream_error = true;
        return 0;
    }

    if (!jpegp_fast_fill(j, (unsigned int)count)
    )
    {
        jpegp_fast.stream_error = true;
        return 0;
    }

    value = jpegp_fast_peek((unsigned int)count);
    jpegp_fast_drop((unsigned int)count);
    return (int)value;
}

static int jpegp_fast_get_bit(struct JPEGD *j)
{
    return jpegp_fast_get_bits(j, 1);
}

static int jpegp_fast_read_diff(struct JPEGD *j, int size)
{
    int value = jpegp_fast_get_bits(j, size);

    if (value < (1 << (size - 1)))
        value -= (1 << size) - 1;

    return value << j->Al;
}

static int jpegp_fast_huff_index(struct JPEGD *j, int *base,
                                 uint16_t *look)
{
    uint16_t entry;
    int length = 1;
    int value;

    if (look != NULL && jpegp_fast_fill(j, JPEGP_LOOK_BITS))
    {
        entry = look[jpegp_fast_peek(JPEGP_LOOK_BITS)];
        if (entry != 0)
        {
            jpegp_fast_drop(entry >> 9);
            jpegp_stats.look_hits++;
            return (entry & 0x01ff) - 1;
        }
    }

    jpegp_stats.slow_hits++;
    value = jpegp_fast_get_bit(j);

    while (!jpegp_fast.stream_error && length <= 16 &&
           value >= base[length - 1])
    {
        value = 2 * value + jpegp_fast_get_bit(j) - base[length - 1];
        length++;
    }

    if (jpegp_fast.stream_error || length > 16 || value < 0 ||
        value >= base[15])
    {
        jpegp_fast.stream_error = true;
        return 0;
    }

    return value;
}

static bool jpegp_build_look(int *base, uint16_t *look)
{
    int code = 0;
    int previous = 0;
    int index = 0;
    int length;

    memset(look, 0, JPEGP_LOOK_SIZE * sizeof(*look));

    for (length = 1; length <= 16; length++)
    {
        int count = base[length - 1] - previous;
        int i;

        if (count < 0 || code + count > (1 << length))
            return false;

        for (i = 0; i < count; i++, code++, index++)
        {
            if (length <= JPEGP_LOOK_BITS)
            {
                int first = code << (JPEGP_LOOK_BITS - length);
                int copies = 1 << (JPEGP_LOOK_BITS - length);
                int k;
                uint16_t entry =
                    (uint16_t)((length << 9) | (index + 1));

                if (index >= 256 || first < 0 ||
                    first + copies > JPEGP_LOOK_SIZE)
                    return false;

                for (k = 0; k < copies; k++)
                    look[first + k] = entry;
            }
        }

        previous = base[length - 1];
        code <<= 1;
    }

    return index > 0 && index <= 256;
}
