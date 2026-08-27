/***************************************************************************
 * Progressive JPEG Huffman acceleration.
 *
 * The original decoder remains available as JPEGDecode_legacy. The fast path
 * keeps the existing marker parser, coefficient layout and scan traversal
 * contract, but uses a 32-bit bit reservoir and an 8-bit canonical Huffman
 * lookahead table for ordinary 8-bit Huffman DCT JPEGs.
 ****************************************************************************/

#include "jpeg81_accel.h"
#include "GETC.h"
#include "rb_glue.h"
#include <limits.h>

#define JPEGDecode JPEGDecode_legacy
#include "jpeg81.c"
#undef JPEGDecode

#include "jpeg81_fast_core.inc"
#include "jpeg81_fast_decode.inc"
#include "jpeg81_fast_parser.inc"
#include "jpeg81_fast_dispatch.inc"
