enum JPEGENUM JPEGDecode(struct JPEGD *j)
{
    enum JPEGENUM status;
    uint32_t started = jpegp_now_us();

    memset(&jpegp_stats, 0, sizeof(jpegp_stats));

    if (!jpegp_fast_enabled)
    {
        status = JPEGDecode_legacy(j);
    }
    else
    {
        int start_position = TELL();

        jpegp_stats.fast_used = true;
        status = jpegp_decode_fast(j);

        if (status == JPEGP_FAST_FALLBACK)
        {
            jpegp_stats.fallback = true;
            POS(start_position);
            clear_mem_pool();
            memset(j, 0, sizeof(*j));
            status = JPEGDecode_legacy(j);
        }
    }

    jpegp_stats.decode_us = jpegp_now_us() - started;
    return status;
}
