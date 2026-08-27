/***************************************************************************
 * JPEG decoder extensions for the iPod Photo acceleration path.
 *
 * The original decoder remains in jpeg_decoder.c. It is included with its
 * public entry points renamed so the enhanced parser and block loop can reuse
 * the existing bitstream, Huffman, and IDCT primitives.
 ****************************************************************************/

#include "plugin.h"
#include "jpeg_decoder.h"

#ifdef HAVE_LCD_COLOR
static jpeg_mcu_row_callback jpeg_mcu_row_cb;
static void *jpeg_mcu_row_cb_data;
static bool jpeg_mcu_row_reuse;

void jpeg_decode_set_mcu_row_callback(jpeg_mcu_row_callback callback,
                                      void *user)
{
    jpeg_mcu_row_cb = callback;
    jpeg_mcu_row_cb_data = user;
}

void jpeg_decode_set_mcu_row_reuse(bool reuse)
{
    jpeg_mcu_row_reuse = reuse;
}
#endif

#define process_markers jpeg_legacy_process_markers
#define build_lut       jpeg_legacy_build_lut
#define jpeg_decode     jpeg_legacy_decode
#include "jpeg_decoder.c"
#undef process_markers
#undef build_lut
#undef jpeg_decode

#include "jpeg_decoder_marker_sof.inc"
#include "jpeg_decoder_marker_tables.inc"
#include "jpeg_decoder_marker_tail.inc"
#include "jpeg_decoder_tables.inc"
#include "jpeg_decoder_dcfill.inc"
#if defined(IPOD_COLOR) && defined(CPU_ARM)
#include "jpeg_decoder_idct_arm.inc"
#endif
#ifdef HAVE_LCD_COLOR
#include "jpeg_decoder_cop.inc"
#endif
#include "jpeg_decoder_decode_setup.inc"
#include "jpeg_decoder_decode_loop.inc"
#include "jpeg_decoder_dispatch.inc"
