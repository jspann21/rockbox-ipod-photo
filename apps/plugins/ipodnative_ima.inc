/*
 * Per-record stereo IMA ADPCM helpers.
 *
 * A record starts with one four-byte little-endian header for each channel:
 * signed 16-bit predictor, eight-bit step index, and a reserved zero byte.
 * The first predictors are the first stereo PCM frame.  Each byte after the
 * header contains the next left sample in its low nibble and the next right
 * sample in its high nibble.
 */

#define IPVF_IMA_HEADER_BYTES 8u
#define IPVF_IMA_MAX_INDEX     88

static const int32_t ipvf_ima_step_table[89] =
{
       7,     8,     9,    10,    11,    12,    13,    14,
      16,    17,    19,    21,    23,    25,    28,    31,
      34,    37,    41,    45,    50,    55,    60,    66,
      73,    80,    88,    97,   107,   118,   130,   143,
     157,   173,   190,   209,   230,   253,   279,   307,
     337,   371,   408,   449,   494,   544,   598,   658,
     724,   796,   876,   963,  1060,  1166,  1282,  1411,
    1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
   15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
   32767
};

static const int8_t ipvf_ima_index_table[16] =
{
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

struct ipvf_ima_state
{
    /* Keep arithmetic wider than PCM output to avoid signed overflow. */
    int32_t left_predictor;
    int32_t right_predictor;
    unsigned char left_index;
    unsigned char right_index;
};

static int32_t ipvf_ima_clamp_sample(int32_t sample)
{
    if (sample > 32767)
        return 32767;
    if (sample < -32768)
        return -32768;
    return sample;
}

static int32_t ipvf_ima_read_s16le(const unsigned char *p)
{
    uint32_t value = (uint32_t)p[0] | ((uint32_t)p[1] << 8);

    /* Avoid an implementation-defined uint16_t-to-int16_t conversion. */
    return value < 0x8000u ? (int32_t)value : (int32_t)value - 0x10000;
}

/* Parse and validate a complete block, initializing the frame-zero state. */
static bool ipvf_ima_parse_header(const unsigned char *block, size_t bytes,
                                  uint32_t frames,
                                  struct ipvf_ima_state *state)
{
    uint64_t expected;

    if (block == NULL || state == NULL || frames == 0)
        return false;

    expected = (uint64_t)IPVF_IMA_HEADER_BYTES + (uint64_t)(frames - 1u);
    if ((uint64_t)bytes != expected || block[3] != 0 || block[7] != 0 ||
        block[2] > IPVF_IMA_MAX_INDEX || block[6] > IPVF_IMA_MAX_INDEX)
        return false;

    state->left_predictor = ipvf_ima_read_s16le(block);
    state->right_predictor = ipvf_ima_read_s16le(block + 4);
    state->left_index = block[2];
    state->right_index = block[6];
    return true;
}

/* Decode one channel's nibble and return the resulting signed PCM sample. */
static int16_t ipvf_ima_decode_nibble(unsigned int nibble,
                                      int32_t *predictor,
                                      unsigned char *index)
{
    int32_t sample;
    int32_t step;
    int32_t difference;
    int32_t next_index;

    nibble &= 0x0fu;
    if (*index > IPVF_IMA_MAX_INDEX)
        *index = IPVF_IMA_MAX_INDEX;
    *predictor = ipvf_ima_clamp_sample(*predictor);

    step = ipvf_ima_step_table[*index];
    difference = step >> 3;
    if (nibble & 1u)
        difference += step >> 2;
    if (nibble & 2u)
        difference += step >> 1;
    if (nibble & 4u)
        difference += step;

    sample = *predictor;
    if (nibble & 8u)
        sample -= difference;
    else
        sample += difference;
    sample = ipvf_ima_clamp_sample(sample);

    next_index = (int32_t)*index + ipvf_ima_index_table[nibble];
    if (next_index < 0)
        next_index = 0;
    else if (next_index > IPVF_IMA_MAX_INDEX)
        next_index = IPVF_IMA_MAX_INDEX;

    *predictor = sample;
    *index = (unsigned char)next_index;
    return (int16_t)sample;
}
