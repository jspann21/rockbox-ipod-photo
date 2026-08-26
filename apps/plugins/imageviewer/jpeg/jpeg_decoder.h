/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
*
* JPEG image viewer
* (This is a real mess if it has to be coded in one single C file)
*
* File scrolling addition (C) 2005 Alexander Spyridakis
* Copyright (C) 2004 Jörg Hohensohn aka [IDC]Dragon
* Heavily borrowed from the IJG implementation (C) Thomas G. Lane
* Small & fast downscaling IDCT (C) 2002 by Guido Vollbeding  JPEGclub.org
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
* KIND, either express or implied.
*
****************************************************************************/

#ifndef _JPEG_JPEG_DECODER_H
#define _JPEG_JPEG_DECODER_H
#include "jpeg_common.h"

#define JPEG_MAX_COMPONENTS 3
#define JPEG_MAX_TABLES     4

struct jpeg
{
    int x_size, y_size; /* size of image (can be less than block boundary) */
    int x_phys, y_phys; /* physical size, block aligned */
    int x_mbl; /* x dimension of MBL */
    int y_mbl; /* y dimension of MBL */
    int blocks; /* blocks per MB */
    int components; /* components in the frame/scan */
    int restart_interval; /* number of MCUs between RSTm markers */
    int store_pos[4]; /* for Y block ordering */

    unsigned char* p_entropy_data;
    unsigned char* p_entropy_end;

    int quanttable[JPEG_MAX_TABLES][QUANT_TABLE_LENGTH];
    int qt_idct[JPEG_MAX_TABLES][QUANT_TABLE_LENGTH];

    struct huffman_table hufftable[JPEG_MAX_TABLES];
    struct derived_tbl dc_derived_tbls[JPEG_MAX_TABLES];
    struct derived_tbl ac_derived_tbls[JPEG_MAX_TABLES];

    struct frame_component frameheader[JPEG_MAX_COMPONENTS];
    struct scan_component scanheader[JPEG_MAX_COMPONENTS];

    unsigned char component_quant[JPEG_MAX_COMPONENTS];
    unsigned char component_dc[JPEG_MAX_COMPONENTS];
    unsigned char component_ac[JPEG_MAX_COMPONENTS];
    unsigned char dqt_present;
    unsigned char dht_dc_present;
    unsigned char dht_ac_present;
    int table_error;

    int mcu_membership[6]; /* component index per entropy block */
    int mcu_block_pos[6];  /* position within that component */
    int tab_membership[6];
    int subsample_x[3]; /* info per component */
    int subsample_y[3];
};

/* various helper functions */
void default_huff_tbl(struct jpeg* p_jpeg);
void build_lut(struct jpeg* p_jpeg);
int process_markers(unsigned char* p_src, long size, struct jpeg* p_jpeg);

/* the main decode function */
#ifdef HAVE_LCD_COLOR
typedef void (*jpeg_mcu_row_callback)(unsigned char * const row[3],
                                      int y, int height, int stride,
                                      void *user);

void jpeg_decode_set_mcu_row_callback(jpeg_mcu_row_callback callback,
                                      void *user);

int jpeg_decode(struct jpeg* p_jpeg, unsigned char* p_pixel[3],
                int downscale, void (*pf_progress)(int current, int total));
#else
int jpeg_decode(struct jpeg* p_jpeg, unsigned char* p_pixel[1], int downscale,
                void (*pf_progress)(int current, int total));
#endif

#endif /* _JPEG_JPEG_DECODER_H */
