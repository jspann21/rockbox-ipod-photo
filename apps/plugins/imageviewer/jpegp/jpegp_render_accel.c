/***************************************************************************
 * Progressive JPEG 4:2:0 rendering acceleration.
 *
 * The coefficient decoder and full-resolution IDCT remain unchanged. This
 * wrapper specializes the expensive RGB rendering phase for the common 4:2:0
 * layout at 1:1 and 1:2 output scales. Other layouts and scales retain the
 * complete legacy renderer.
 ****************************************************************************/

#include "plugin.h"
#include "../imageviewer.h"
#include "idct_accel.h"

/* rbunicode.h uses COMP as a character-class flag, while the progressive
 * decoder uses it as a structure tag. */
#ifdef COMP
#undef COMP
#endif

struct jpegp_legacy_decoder
{
    const bool unscaled_avail;
    int (*img_mem)(int ds);
    int (*load_image)(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize);
    int (*get_image)(struct image_info *info, int frame, int ds);
    void (*draw_image_rect)(struct image_info *info,
                            int x, int y, int width, int height);
};

#undef IMGDEC_HEADER
#define IMGDEC_HEADER
#define draw_image_rect jpegp_legacy_draw_image_rect
#define load_image      jpegp_legacy_load_image
#define get_image       jpegp_legacy_get_image
#define image_decoder   jpegp_legacy_decoder
#include "jpegp.c"
#undef draw_image_rect
#undef load_image
#undef get_image
#undef image_decoder
#undef IMGDEC_HEADER

static inline TCOEF *jpegp_component_row(const struct COMP *component,
                                         int sample_y)
{
    return component->du[(sample_y >> 3) * component->du_width] +
           ((sample_y & 7) << 3);
}

static inline int jpegp_component_sample(const TCOEF *row, int sample_x)
{
    return row[((sample_x >> 3) << 6) + (sample_x & 7)];
}

static inline fb_data jpegp_pack_rgb565(int luma, int cb, int cr)
{
    int yy = (luma << 5) + 16;
    int u = cb - 128;
    int v = cr - 128;
    int b = CLIP[(yy + 57 * u) >> 5];
    int g = CLIP[(yy - 11 * u - 23 * v) >> 5];
    int r = CLIP[(yy + 45 * v) >> 5];

    return FB_RGBPACK(r, g, b);
}

static bool jpegp_fast420_eligible(const struct JPEGD *j, int ds)
{
    return (ds == 1 || ds == 2) &&
           j->Nf == 3 && j->Hmax == 2 && j->Vmax == 2 &&
           j->Components[0].Hi == 2 &&
           j->Components[0].Vi == 2 &&
           j->Components[1].Hi == 1 &&
           j->Components[1].Vi == 1 &&
           j->Components[2].Hi == 1 &&
           j->Components[2].Vi == 1;
}

static void jpegp_render420_1x(const struct JPEGD *j,
                               fb_data *dst,
                               int width, int height)
{
    const struct COMP *ycomp = &j->Components[0];
    const struct COMP *ucomp = &j->Components[1];
    const struct COMP *vcomp = &j->Components[2];
    int y;

    for (y = 0; y < height; y++)
    {
        const TCOEF *yrow = jpegp_component_row(ycomp, y);
        const TCOEF *urow = jpegp_component_row(ucomp, y >> 1);
        const TCOEF *vrow = jpegp_component_row(vcomp, y >> 1);
        int x;

        for (x = 0; x + 1 < width; x += 2)
        {
            int chroma_x = x >> 1;
            int cb = jpegp_component_sample(urow, chroma_x);
            int cr = jpegp_component_sample(vrow, chroma_x);

            *dst++ = jpegp_pack_rgb565(
                jpegp_component_sample(yrow, x), cb, cr);
            *dst++ = jpegp_pack_rgb565(
                jpegp_component_sample(yrow, x + 1), cb, cr);
        }

        if (x < width)
        {
            int chroma_x = x >> 1;
            *dst++ = jpegp_pack_rgb565(
                jpegp_component_sample(yrow, x),
                jpegp_component_sample(urow, chroma_x),
                jpegp_component_sample(vrow, chroma_x));
        }
    }
}

static void jpegp_render420_2x(const struct JPEGD *j,
                               fb_data *dst,
                               int width, int height)
{
    const struct COMP *ycomp = &j->Components[0];
    const struct COMP *ucomp = &j->Components[1];
    const struct COMP *vcomp = &j->Components[2];
    int out_y;

    for (out_y = 0; out_y < height; out_y++)
    {
        int src_y = out_y << 1;
        const TCOEF *yrow0 = jpegp_component_row(ycomp, src_y);
        const TCOEF *yrow1 = jpegp_component_row(ycomp, src_y + 1);
        const TCOEF *urow = jpegp_component_row(ucomp, out_y);
        const TCOEF *vrow = jpegp_component_row(vcomp, out_y);
        int out_x;

        for (out_x = 0; out_x < width; out_x++)
        {
            int src_x = out_x << 1;
            int luma =
                jpegp_component_sample(yrow0, src_x) +
                jpegp_component_sample(yrow0, src_x + 1) +
                jpegp_component_sample(yrow1, src_x) +
                jpegp_component_sample(yrow1, src_x + 1);

            luma = (luma + 2) >> 2;

            *dst++ = jpegp_pack_rgb565(
                luma,
                jpegp_component_sample(urow, out_x),
                jpegp_component_sample(vrow, out_x));
        }
    }
}

static bool jpegp_render420(struct image_info *info, int ds)
{
    struct JPEGD *j = &jpg;
    struct t_disp *display = &disp[ds];
    int mem = img_mem(ds);

    info->width = j->X / ds;
    info->height = j->Y / ds;
    info->data = display;

    if (display->bitmap != NULL)
        return true;

    display->bitmap = malloc(mem);
    if (display->bitmap == NULL)
    {
        clear_mem_pool();
        memset(&disp, 0, sizeof(disp));
        display = &disp[ds];
        info->data = display;
        display->bitmap = malloc(mem);
        if (display->bitmap == NULL)
            return false;
    }

    if (ds == 1)
        jpegp_render420_1x(j, (fb_data *)display->bitmap,
                           info->width, info->height);
    else
        jpegp_render420_2x(j, (fb_data *)display->bitmap,
                           info->width, info->height);

    return true;
}

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize)
{
    jpegp_idct_reset();
    return jpegp_legacy_load_image(filename, info, buf, buf_size,
                                   offset, filesize);
}

static int get_image(struct image_info *info, int frame, int ds)
{
    if (jpegp_fast420_eligible(&jpg, ds))
        return jpegp_render420(info, ds) ? PLUGIN_OK : PLUGIN_ERROR;

    return jpegp_legacy_get_image(info, frame, ds);
}

static void draw_image_rect(struct image_info *info,
                            int x, int y, int width, int height)
{
    jpegp_legacy_draw_image_rect(info, x, y, width, height);
}

const struct image_decoder image_decoder = {
    false,
    img_mem,
    load_image,
    get_image,
    draw_image_rect,
};

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
const struct plugin_api *rb DATA_ATTR;
struct imgdec_api *iv DATA_ATTR;
const struct imgdec_header __header
__attribute__ ((section (".header"))) = {
    { PLUGIN_MAGIC, TARGET_ID, IMGDEC_API_VERSION,
      plugin_start_addr, plugin_end_addr, }, &image_decoder,
      &rb, PLUGIN_API_VERSION, sizeof(struct plugin_api),
      &iv, sizeof(struct imgdec_api)
};
#else
const struct plugin_api *rb DATA_ATTR;
struct imgdec_api *iv DATA_ATTR;
const struct imgdec_header __header
__attribute__((visibility("default"))) = {
    { PLUGIN_MAGIC, TARGET_ID, IMGDEC_API_VERSION, NULL, NULL },
    &image_decoder, &rb, PLUGIN_API_VERSION,
    sizeof(struct plugin_api), &iv, sizeof(struct imgdec_api),
};
#endif
