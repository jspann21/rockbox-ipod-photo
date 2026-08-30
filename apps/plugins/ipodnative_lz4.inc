/*
 * Strict LZ4 block decoder for untrusted IPVF payloads.
 *
 * This is deliberately limited to the independent-block format: there is no
 * dictionary, frame header, or output-size inference here.  The caller must
 * provide the exact compressed and uncompressed byte counts.
 */

static bool ipvf_lz4_read_length(const unsigned char *source,
                                 size_t source_bytes, size_t *source_pos,
                                 unsigned int nibble, size_t *length)
{
    size_t value = nibble;

    if (nibble == 15u)
    {
        for (;;)
        {
            unsigned int extension;

            if (*source_pos >= source_bytes)
                return false;

            extension = source[(*source_pos)++];
            if (value > (size_t)-1 - extension)
                return false;
            value += extension;
            if (extension != 255u)
                break;
        }
    }

    *length = value;
    return true;
}

static bool ipvf_lz4_decode(const unsigned char *source, size_t source_bytes,
                            unsigned char *destination,
                            size_t destination_bytes)
{
    size_t source_pos = 0;
    size_t destination_pos = 0;
    bool saw_match = false;

    /* An empty block has no terminal literal sequence. */
    if (source == NULL || destination == NULL || source_bytes == 0)
        return false;

    for (;;)
    {
        unsigned int token;
        size_t literal_length;

        /* Every sequence, including the final one, has a token. */
        if (source_pos >= source_bytes)
            return false;

        token = source[source_pos++];
        if (!ipvf_lz4_read_length(source, source_bytes, &source_pos,
                                   token >> 4, &literal_length))
            return false;

        if (literal_length > source_bytes - source_pos ||
            literal_length > destination_bytes - destination_pos)
            return false;

        /* Input is an uncached record alias; bulk-copy literal runs. */
        if (literal_length != 0)
        {
            rb->memcpy(destination + destination_pos,
                       source + source_pos, literal_length);
            source_pos += literal_length;
            destination_pos += literal_length;
        }

        /* A sequence that consumes the block is the required final literal run. */
        if (source_pos == source_bytes)
            return destination_pos == destination_bytes &&
                   literal_length != 0 &&
                   (literal_length >= 5u ||
                    (!saw_match && destination_bytes < 5u));

        {
            size_t offset;
            size_t match_length;

            if (source_bytes - source_pos < 2)
                return false;

            offset = (size_t)source[source_pos] |
                     ((size_t)source[source_pos + 1] << 8);
            source_pos += 2;

            if (offset == 0 || offset > destination_pos)
                return false;

            if (!ipvf_lz4_read_length(source, source_bytes, &source_pos,
                                       token & 15u, &match_length))
                return false;
            if (match_length > (size_t)-1 - 4u)
                return false;
            match_length += 4u;

            if (match_length > destination_bytes - destination_pos)
                return false;
            if (destination_bytes < 12u ||
                destination_pos > destination_bytes - 12u)
                return false;
            saw_match = true;

            /* Forward byte copies are cheap for short matches and handle overlap. */
            if (match_length <= 16u)
            {
                size_t i;

                for (i = 0; i < match_length; i++)
                    destination[destination_pos + i] =
                        destination[destination_pos + i - offset];
                destination_pos += match_length;
                continue;
            }

            /*
             * First materialize one complete period of the match.  Its source
             * and destination are adjacent and therefore non-overlapping.
             * Every later copy doubles data already produced; limiting the
             * chunk to the amount already copied keeps those ranges disjoint.
             */
            {
                size_t copied = 0;
                size_t chunk = offset;

                if (chunk > match_length)
                    chunk = match_length;
                rb->memcpy(destination + destination_pos,
                           destination + destination_pos - offset, chunk);
                copied = chunk;

                while (copied < match_length)
                {
                    chunk = copied;
                    if (chunk > match_length - copied)
                        chunk = match_length - copied;
                    rb->memcpy(destination + destination_pos + copied,
                               destination + destination_pos, chunk);
                    copied += chunk;
                }
            }
            destination_pos += match_length;
        }
    }
}
