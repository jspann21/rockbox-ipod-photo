/***************************************************************************
 * Low-risk JPEG decoder extensions for the iPod Photo workstream.
 *
 * The original decoder remains in jpeg_decoder.c. It is included with its
 * public entry points renamed so the accelerated parser and block loop can
 * reuse the proven bitstream, Huffman and IDCT primitives. The implementation
 * is split into review-sized includes; all code is compiled as this single
 * translation unit.
 ****************************************************************************/

#include "plugin.h"
#include "jpeg_accel.h"

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
#include "jpeg_decoder_decode_setup.inc"
#include "jpeg_decoder_decode_loop.inc"
#include "jpeg_decoder_dispatch.inc"
