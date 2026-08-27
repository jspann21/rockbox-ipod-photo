static int jpegp_div_up(int value, int divisor)
{
    return (value + divisor - 1) / divisor;
}

static bool jpegp_fast_set_dim(struct JPEGD *j, int unit_size,
                               int *total_du)
{
    size_t total = 0;
    int i;

    j->Hmax = 0;
    j->Vmax = 0;

    for (i = 0; i < j->Nf; i++)
    {
        struct COMP *component = j->Components + i;
        int sampling;
        int previous;

        component->Ci = GETC();
        sampling = GETC();
        component->Hi = sampling >> 4;
        component->Vi = sampling & 15;
        component->Qi = GETC();

        if (component->Hi < 1 || component->Hi > 4 ||
            component->Vi < 1 || component->Vi > 4 ||
            component->Qi < 0 || component->Qi > 3)
            return false;

        for (previous = 0; previous < i; previous++)
            if (j->Components[previous].Ci == component->Ci)
                return false;

        if (component->Hi > j->Hmax)
            j->Hmax = component->Hi;
        if (component->Vi > j->Vmax)
            j->Vmax = component->Vi;
    }

    if (j->Hmax <= 0 || j->Vmax <= 0 ||
        j->X <= 0 || j->Y <= 0)
        return false;

    j->mcu_width = jpegp_div_up(j->X, j->Hmax * unit_size);
    j->mcu_height = jpegp_div_up(j->Y, j->Vmax * unit_size);

    if (j->mcu_width <= 0 || j->mcu_height <= 0 ||
        j->mcu_width > INT_MAX / j->mcu_height)
        return false;

    j->mcu_total = j->mcu_width * j->mcu_height;

    for (i = 0; i < j->Nf; i++)
    {
        struct COMP *component = j->Components + i;
        int sample_width = jpegp_div_up(j->X * component->Hi, j->Hmax);
        int sample_height = jpegp_div_up(j->Y * component->Vi, j->Vmax);
        size_t component_total;

        component->du_w = jpegp_div_up(sample_width, unit_size);
        component->du_h = jpegp_div_up(sample_height, unit_size);
        if (component->du_w <= 0 || component->du_h <= 0 ||
            component->du_w > INT_MAX / component->du_h)
            return false;
        component->du_size = component->du_w * component->du_h;

        if (j->mcu_width > INT_MAX / component->Hi)
            return false;
        component->du_width = j->mcu_width * component->Hi;

        component_total = (size_t)component->du_width *
                          j->mcu_height * component->Vi;
        if (component_total > INT_MAX || total > INT_MAX - component_total)
            return false;

        component->du_total = (int)component_total;
        total += component_total;
    }

    *total_du = (int)total;
    return true;
}

static int jpegp_component_index(struct JPEGD *j, struct COMP *component)
{
    return (int)(component - j->Components);
}

static void jpegp_fast_dc_succ(struct JPEGD *j, struct COMP *sc,
                               TCOEF *coef)
{
    (void)sc;
    *coef |= jpegp_fast_get_bit(j) << j->Al;
}

static void jpegp_fast_dc_decode(struct JPEGD *j, struct COMP *sc,
                                 TCOEF *coef)
{
    int ci = jpegp_component_index(j, sc);
    int size = sc->DCS[jpegp_fast_huff_index(
        j, sc->DCB, jpegp_fast.dc_look[ci])];

    if (jpegp_fast.stream_error || size < 0 || size > 16)
    {
        jpegp_fast.stream_error = true;
        return;
    }

    if (size)
        sc->DC += jpegp_fast_read_diff(j, size);

    *coef = sc->DC;
}

static void jpegp_fast_ac_decode(struct JPEGD *j, struct COMP *sc,
                                 TCOEF *coef)
{
    int ci = jpegp_component_index(j, sc);
    int k = j->Ss;

    if (sc->EOBRUN == 0)
    {
        for (;; k++)
        {
            int symbol = sc->ACS[jpegp_fast_huff_index(
                j, sc->ACB, jpegp_fast.ac_look[ci])];
            int run = symbol >> 4;
            int value = symbol & 15;

            if (jpegp_fast.stream_error || value > 10)
            {
                jpegp_fast.stream_error = true;
                return;
            }

            if (value)
                value = jpegp_fast_read_diff(j, value);
            else if (run < 15)
            {
                if (run)
                    sc->EOBRUN =
                        jpegp_fast_get_bits(j, run) + (1 << run) - 1;
                return;
            }

            k += run;
            if (k < j->Ss || k > j->Se || k > 63)
            {
                jpegp_fast.stream_error = true;
                return;
            }
            coef[k] = value;
            if (k == j->Se)
                return;
        }
    }
    else
    {
        sc->EOBRUN--;
    }
}

static int jpegp_fast_ac_refine(struct JPEGD *j, TCOEF *coef)
{
    if (*coef && jpegp_fast_get_bit(j))
        *coef += *coef > 0 ? j->Al2 : -j->Al2;

    return *coef;
}

static void jpegp_fast_ac_succ(struct JPEGD *j, struct COMP *sc,
                               TCOEF *coef)
{
    int ci = jpegp_component_index(j, sc);
    int k = j->Ss;

    if (sc->EOBRUN == 0)
    {
        for (;; k++)
        {
            int symbol = sc->ACS[jpegp_fast_huff_index(
                j, sc->ACB, jpegp_fast.ac_look[ci])];
            int run = symbol >> 4;
            int value = symbol & 15;

            if (jpegp_fast.stream_error || (value != 0 && value != 1))
            {
                jpegp_fast.stream_error = true;
                return;
            }

            if (value)
                value = jpegp_fast_get_bit(j) ? j->Al2 : -j->Al2;
            else if (run < 15)
            {
                if (run)
                    sc->EOBRUN =
                        jpegp_fast_get_bits(j, run) + (1 << run) - 1;
                break;
            }

            for (;; k++)
            {
                if (k > j->Se || k > 63)
                {
                    jpegp_fast.stream_error = true;
                    return;
                }
                if (!jpegp_fast_ac_refine(j, coef + k) && !run--)
                    break;
            }

            coef[k] = value;
            if (k == j->Se)
                return;
        }
    }
    else
    {
        sc->EOBRUN--;
    }

    for (; k <= j->Se && k <= 63; k++)
        jpegp_fast_ac_refine(j, coef + k);
}

static void jpegp_fast_sequential(struct JPEGD *j, struct COMP *sc,
                                  TCOEF *coef)
{
    int ci = jpegp_component_index(j, sc);
    int size;
    int k;

    jpegp_fast_dc_decode(j, sc, coef);

    for (k = 1;
         (size = sc->ACS[jpegp_fast_huff_index(
              j, sc->ACB, jpegp_fast.ac_look[ci])]) != 0;
         k++)
    {
        int magnitude = size & 15;

        k += size >> 4;
        if (size == 0xf0)
            continue;
        if (magnitude < 1 || magnitude > 10 || k < 1 || k > 63)
        {
            jpegp_fast.stream_error = true;
            return;
        }

        coef[k] = jpegp_fast_read_diff(j, magnitude);
        if (k == 63)
            return;
    }
}

static void jpegp_fast_reset_decoder(struct JPEGD *j)
{
    int component;

    for (component = 0; component < j->Ns; component++)
        j->ScanComponents[component]->DC = 0;

    jpegp_fast.bit_buffer = 0;
    jpegp_fast.bits = 0;
    jpegp_fast.stream_error = false;
    j->ScanBit = 0;
}
