/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockbox driver for iPod LCDs
 *
 * Based on code from the ipodlinux project - http://ipodlinux.org/
 * Adapted for Rockbox in November 2005
 *
 * Original file: linux/arch/armnommu/mach-ipod/fb.c
 *
 * Copyright (c) 2003-2005 Bernard Leach (leachbj@bouncycastle.org)
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
#include "config.h"
#include "cpu.h"
#include "lcd.h"
#include "kernel.h"
#include "system.h"
#include "hwcompat.h"
#include "backlight-target.h"
#include <limits.h>
#ifdef HAVE_PP5020_PERF
#include "pp5020-perf.h"
#endif

/* LCD command codes for HD66789R */
#define LCD_CNTL_RAM_ADDR_SET           0x21
#define LCD_CNTL_WRITE_TO_GRAM          0x22
#define LCD_CNTL_HORIZ_RAM_ADDR_POS     0x44
#define LCD_CNTL_VERT_RAM_ADDR_POS      0x45

/*** globals ***/
int lcd_type = 1; /* 0,2 = "old" Color/Photo; 1,3 = similar to HD66789R */

#define LCD_WAIT_TIMEOUT_USEC 20000

enum lcd_failure_phase {
    LCD_FAILURE_BUSY = 1,
    LCD_FAILURE_BLOCK,
    LCD_FAILURE_TXOK,
    LCD_FAILURE_FIFO1,
    LCD_FAILURE_FIFO2,
};

static bool lcd_failure_pending;

static inline unsigned long lcd_deadline(void)
{
    return USEC_TIMER + LCD_WAIT_TIMEOUT_USEC;
}

static void lcd_record_timeout(enum lcd_failure_phase phase)
{
    LCD2_BLOCK_CONFIG = 0;
    lcd_failure_pending = true;
#ifdef HAVE_PP5020_PERF
    pp5020_perf_record_lcd_timeout(phase);
#else
    (void)phase;
#endif
}

static inline bool lcd_wait_write(unsigned long deadline)
{
    while (LCD2_PORT & LCD2_BUSY_MASK)
        if (!TIME_BEFORE(USEC_TIMER, deadline))
        {
            lcd_record_timeout(LCD_FAILURE_BUSY);
            return false;
        }

    return true;
}

static inline bool lcd_wait_block(unsigned long mask,
                                  unsigned long deadline,
                                  enum lcd_failure_phase phase)
{
    while (!(LCD2_BLOCK_CTRL & mask))
        if (!TIME_BEFORE(USEC_TIMER, deadline))
        {
            lcd_record_timeout(phase);
            return false;
        }

    return true;
}

static bool lcd_cmd_data(unsigned cmd, unsigned data)
{
    unsigned long deadline = lcd_deadline();

    if ((lcd_type&1) == 0) {  /* 16 bit transfers */
        if (!lcd_wait_write(deadline))
            return false;
        LCD2_PORT = LCD2_CMD_MASK | cmd;
        if (!lcd_wait_write(deadline))
            return false;
        LCD2_PORT = LCD2_CMD_MASK | data;
    } else {
        if (!lcd_wait_write(deadline))
            return false;
        LCD2_PORT = LCD2_CMD_MASK;
        LCD2_PORT = LCD2_CMD_MASK | cmd;
        if (!lcd_wait_write(deadline))
            return false;
        LCD2_PORT = LCD2_DATA_MASK | (data >> 8);
        LCD2_PORT = LCD2_DATA_MASK | (data & 0xff);
    }

    return true;
}

/*** hardware configuration ***/

#ifdef HAVE_LCD_CONTRAST
void lcd_set_contrast(int val)
{
  /* TODO: Implement lcd_set_contrast() */
  (void)val;
}
#endif

#ifdef HAVE_LCD_INVERT
void lcd_set_invert_display(bool yesno)
{
#ifdef IPOD_NANO    /* this has only been tested on the ipod nano */
    lcd_cmd_data(0x07, 0x73 | (yesno ? 0 : (1<<2)));
#endif
}
#endif

#ifdef HAVE_LCD_FLIP
/* turn the display upside down (call lcd_update() afterwards) */
void lcd_set_flip(bool yesno)
{
  /* TODO: Implement lcd_set_flip() */
  (void)yesno;
}
#endif

/* LCD init */
void lcd_init_device(void)
{
    bool ok = true;

#if CONFIG_LCD == LCD_IPODCOLOR
    if (IPOD_HW_REVISION == 0x60000) {
        lcd_type = 0;
    } else {
        lcd_type = (GPIOA_INPUT_VAL & 0x2) | ((GPIOA_INPUT_VAL & 0x10) >> 4);
    }
    if ((lcd_type&1) == 0) {
        ok = lcd_cmd_data(0xef, 0x0) &&
             lcd_cmd_data(0x01, 0x0) &&
             lcd_cmd_data(0x80, 0x1) &&
             lcd_cmd_data(0x10, 0xc) &&
             lcd_cmd_data(0x18, 0x6) &&
             lcd_cmd_data(0x7e, 0x4) &&
             lcd_cmd_data(0x7e, 0x5) &&
             lcd_cmd_data(0x7f, 0x1);
    }
#elif CONFIG_LCD == LCD_IPODNANO
    /* iPodLinux doesn't appear have any LCD init code for the Nano */
#endif
    if (!ok)
        lcd_failure_pending = true;
}

/* A failed transfer may leave the controller window or FIFO state stale.
 * Recover only at the next display call, then redraw the canonical
 * framebuffer before allowing caller-owned/direct pixels onto the panel. */
static bool lcd_recover_if_needed(bool redraw)
{
    if (!lcd_failure_pending)
        return true;

    lcd_failure_pending = false;
#ifdef HAVE_PP5020_PERF
    pp5020_perf_record_lcd_reinitialization();
#endif
    lcd_init_device();
    if (lcd_failure_pending)
        return false;

    if (redraw)
        lcd_update();

    return !lcd_failure_pending;
}

#ifdef HAVE_LCD_SHUTDOWN
void lcd_shutdown(void) {
    /* Immediately switch off the backlight to avoid flashing. */
#if defined(IPOD_NANO)
    _backlight_hw_enable(false);
#elif defined(IPOD_COLOR)
    backlight_hw_off();
#endif

    if ((lcd_type&1) == 0) {
        /* lcd_type 0 and 2 */
        lcd_cmd_data(0x00EF, 0x0000);
        lcd_cmd_data(0x0080, 0x0000); udelay(1000);
        lcd_cmd_data(0x0001, 0x0001);
    } else if (lcd_type == 1) {
        /* lcd_type 1 */
        lcd_cmd_data(0x0007, 0x0236); sleep( 40*HZ/1000);
        lcd_cmd_data(0x0007, 0x0226); sleep( 40*HZ/1000);
        lcd_cmd_data(0x0007, 0x0204);
        lcd_cmd_data(0x0010, 0x7574); sleep(200*HZ/1000);
        lcd_cmd_data(0x0010, 0x7504); sleep( 50*HZ/1000);
        lcd_cmd_data(0x0010, 0x0501);
    } else {
        /* lcd_type 3 */
        lcd_cmd_data(0x0007, 0x4016); sleep( 20*HZ/1000);
        lcd_cmd_data(0x0059, 0x0011); sleep( 20*HZ/1000);
        lcd_cmd_data(0x0059, 0x0003); sleep( 20*HZ/1000);
        lcd_cmd_data(0x0059, 0x0002); sleep( 20*HZ/1000);
        lcd_cmd_data(0x0010, 0x6360); sleep(200*HZ/1000);
        lcd_cmd_data(0x0010, 0x6300); sleep( 50*HZ/1000);
        lcd_cmd_data(0x0010, 0x0300);
        lcd_cmd_data(0x0059, 0x0000);
        lcd_cmd_data(0x0007, 0x4004);
        lcd_cmd_data(0x0010, 0x0301);
    }
}
#endif

/* Helper function to set up drawing region and start drawing */
static bool lcd_setup_drawing_region(int x, int y, int width, int height)
{
    int y0, x0, y1, x1;
    
    /* calculate the drawing region */
#if CONFIG_LCD == LCD_IPODNANO
    y0 = x;                         /* start horiz */
    y1 = (x + width) - 1;           /* max horiz */
    x0 = y;                         /* start vert */
    x1 = (y + height) - 1;          /* max vert */
#elif CONFIG_LCD == LCD_IPODCOLOR
    y0 = y;                         /* start vert */
    y1 = (y + height) - 1;          /* end vert */
    x1 = (LCD_WIDTH - 1) - x;       /* end horiz */
    x0 = (x1 - width) + 1;          /* start horiz */
#endif

    /* setup the drawing region */
    if ((lcd_type&1) == 0) {
        /* x0 and x1 need to be swapped until 
         * proper direction setup is added */
        if (!lcd_cmd_data(0x12, y0) || /* start vert */
            !lcd_cmd_data(0x13, x1) || /* start horiz */
            !lcd_cmd_data(0x15, y1) || /* end vert */
            !lcd_cmd_data(0x16, x0))   /* end horiz */
            return false;
    } else {
        /* max horiz << 8 | start horiz */
        if (!lcd_cmd_data(LCD_CNTL_HORIZ_RAM_ADDR_POS, (y1 << 8) | y0))
            return false;
        /* max vert << 8 | start vert */
        if (!lcd_cmd_data(LCD_CNTL_VERT_RAM_ADDR_POS, (x1 << 8) | x0))
            return false;

        /* start vert = max vert */
#if CONFIG_LCD == LCD_IPODCOLOR
        x0 = x1;
#endif

        /* position cursor (set AD0-AD15) */
        /* start vert << 8 | start horiz */
        if (!lcd_cmd_data(LCD_CNTL_RAM_ADDR_SET, ((x0 << 8) | y0)))
            return false;

        /* start drawing */
        if (!lcd_wait_write(lcd_deadline()))
            return false;
        LCD2_PORT = LCD2_CMD_MASK;
        LCD2_PORT = (LCD2_CMD_MASK|LCD_CNTL_WRITE_TO_GRAM);
    }

    return true;
}

/* Line write helper function for lcd_yuv_blit. Writes two lines of yuv420. */
extern int lcd_write_yuv420_lines(unsigned char const * const src[3],
                                  const unsigned int lcd_baseadress,
                                  int width, int stride,
                                  unsigned long deadline,
                                  volatile unsigned long *usec_timer);

/* Performance function to blit a YUV bitmap directly to the LCD */
void lcd_blit_yuv(unsigned char * const src[3],
                  int src_x, int src_y, int stride,
                  int x, int y, int width, int height)
{
    int z;
    int requested_width = width;
    int requested_height = height;
    int transmitted = 0;
    uint32_t started = USEC_TIMER;
    unsigned char const * yuv_src[3];

    /* Validate the caller's geometry before 4:2:0 rounding. Rounding is
     * accepted only when both source stride and destination still contain
     * the expanded rectangle. */
    if (src == NULL || src[0] == NULL || src[1] == NULL || src[2] == NULL ||
        stride <= 0 || (stride & 1) ||
        src_x < 0 || src_y < 0 || (src_x & 1) || (src_y & 1) ||
        width <= 0 || height <= 0 || x < 0 || y < 0 ||
        x >= LCD_WIDTH || y >= LCD_HEIGHT ||
        width > LCD_WIDTH - x || height > LCD_HEIGHT - y ||
        src_x >= stride || width > stride - src_x ||
        src_y > INT_MAX / stride)
        return;

    width  = (width  + 1) & ~1; /* ensure width is even  */
    height = (height + 1) & ~1; /* ensure height is even */
    if (width > LCD_WIDTH - x || height > LCD_HEIGHT - y ||
        width > stride - src_x)
        return;

    if (!lcd_recover_if_needed(true))
        return;

    if (!lcd_setup_drawing_region(x, y, width, height))
        goto out;

    z = stride * src_y;
    yuv_src[0] = src[0] + z + src_x;
    yuv_src[1] = src[1] + (z >> 2) + (src_x >> 1);
    yuv_src[2] = src[2] + (yuv_src[1] - src[1]);

    while (height > 0) {
        int r, h, pixels_to_write;

        pixels_to_write = (width * height) * 2;
        h = height;

        /* calculate how much we can do in one go */
        if (pixels_to_write > 0x10000) {
            h = ((0x10000/2) / width) & ~1; /* ensure h is even */
            pixels_to_write = (width * h) * 2;
        }
        
        LCD2_BLOCK_CTRL   = 0x10000080;
        LCD2_BLOCK_CONFIG = 0xc0010000 | (pixels_to_write - 1);
        LCD2_BLOCK_CTRL   = 0x34000000;
        unsigned long block_deadline = lcd_deadline();

        r = h>>1; /* lcd_write_yuv420_lines writes two lines at once */
        do {
            int result = lcd_write_yuv420_lines(yuv_src, LCD2_BASE, width,
                                                stride, block_deadline,
                                                &USEC_TIMER);
            if (result != 0) {
                lcd_record_timeout(result == 1 ? LCD_FAILURE_FIFO1 :
                                                  LCD_FAILURE_FIFO2);
                goto out;
            }
            yuv_src[0] += stride << 1;
            yuv_src[1] += stride >> 1;
            yuv_src[2] += stride >> 1;
        } while (--r > 0);
        
        /* transfer of pixels_to_write bytes finished */
        if (!lcd_wait_block(LCD2_BLOCK_READY, block_deadline,
                            LCD_FAILURE_BLOCK))
            goto out;
        LCD2_BLOCK_CONFIG = 0;
        transmitted += width * h;
        height -= h;
    }

out:
#ifdef HAVE_PP5020_PERF
    pp5020_perf_record_lcd_update(requested_width * requested_height,
                                  transmitted, USEC_TIMER - started,
                                  !lcd_failure_pending);
#endif
}

/* Helper function writes 'count' consecutive pixels from src to LCD IF */
static bool lcd_write_line(int count, const unsigned long *src,
                           unsigned long deadline)
{
    do {
        if (!lcd_wait_block(LCD2_BLOCK_TXOK, deadline, LCD_FAILURE_TXOK))
            return false;
        LCD2_BLOCK_DATA = *src++;                     /* output 2 pixels */
        count -= 2;
    } while (count > 0);

    return true;
}

/* Update a fraction of the display. */
void lcd_update_rect(int x, int y, int width, int height)
{
    unsigned long *addr;
    int requested_width;
    int requested_height;
    int transmitted = 0;
    uint32_t started = USEC_TIMER;

    if (width <= 0 || height <= 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;
    if (x < 0)
    {
        width += x;
        x = 0;
    }
    if (y < 0)
    {
        height += y;
        y = 0;
    }
    if (width <= 0 || height <= 0)
        return;
    if (width > LCD_WIDTH - x)
        width = LCD_WIDTH - x;
    if (height > LCD_HEIGHT - y)
        height = LCD_HEIGHT - y;

    requested_width = width;
    requested_height = height;

    if (lcd_failure_pending)
    {
        if (!lcd_recover_if_needed(false))
            goto out;
        x = 0;
        y = 0;
        width = LCD_WIDTH;
        height = LCD_HEIGHT;
    }

    /* Ensure both x and width are even to be able to read 32-bit aligned 
     * data from lcd_framebuffer */
    int right = MIN(LCD_WIDTH, (x + width + 1) & ~1);
    x &= ~1;                    /* use the smaller even number */
    width = right - x;
    if (width <= 0)
        return;

    if (!lcd_setup_drawing_region(x, y, width, height))
        goto out;

    addr = (unsigned long*)FBADDR(x, y);

    while (height > 0) {
        int r, h, pixels_to_write;

        pixels_to_write = (width * height) * 2;
        h = height;

        /* calculate how much we can do in one go */
        if (pixels_to_write > 0x10000) {
            h = ((0x10000/2) / width) & ~1; /* ensure h is even */
            pixels_to_write = (width * h) * 2;
        }

        LCD2_BLOCK_CTRL   = 0x10000080;
        LCD2_BLOCK_CONFIG = 0xc0010000 | (pixels_to_write - 1);
        LCD2_BLOCK_CTRL   = 0x34000000;

        unsigned long block_deadline = lcd_deadline();
        if (LCD_WIDTH == width) {
            /* for each row and column in a single call */
            if (!lcd_write_line(h*width, addr, block_deadline))
                goto out;
            addr += LCD_WIDTH/2*h;
        } else {
            /* for each row */
            for (r = 0; r < h; r++) {
                if (!lcd_write_line(width, addr, block_deadline))
                    goto out;
                addr += LCD_WIDTH/2;
            }
        }

        /* transfer of pixels_to_write bytes finished */
        if (!lcd_wait_block(LCD2_BLOCK_READY, block_deadline,
                            LCD_FAILURE_BLOCK))
            goto out;
        LCD2_BLOCK_CONFIG = 0;
        transmitted += width * h;
        height -= h;
    }

out:
#ifdef HAVE_PP5020_PERF
    pp5020_perf_record_lcd_update(requested_width * requested_height,
                                  transmitted, USEC_TIMER - started,
                                  !lcd_failure_pending);
#endif
}

#if defined(IPOD_COLOR) && !defined(SIMULATOR)
/* Display caller-owned RGB565SWAPPED pixels synchronously. src points to the
 * first pixel of the first source row; stride is in fb_data pixels. */
bool lcd_update_rect_from_buffer(const fb_data *src, int stride,
                                 int x, int y, int width, int height)
{
    const unsigned long *addr;
    int requested_width = width;
    int requested_height = height;
    int transmitted = 0;
    uint32_t started = USEC_TIMER;

    /* Reject instead of clipping: clipping would silently change the source
     * origin, and odd geometry cannot be read as aligned 32-bit pixel pairs. */
    if (src == NULL || stride <= 0 || stride < width ||
        width <= 0 || height <= 0 ||
        x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT ||
        width > LCD_WIDTH - x || height > LCD_HEIGHT - y ||
        (x & 1) != 0 || (width & 1) != 0 || (stride & 1) != 0 ||
        ((unsigned long)src & 3) != 0)
        return false;

    if (!lcd_recover_if_needed(true))
        goto out;

    if (!lcd_setup_drawing_region(x, y, width, height))
        goto out;

    addr = (const unsigned long *)src;

    while (height > 0) {
        int r, h, pixels_to_write;

        pixels_to_write = (width * height) * 2;
        h = height;

        if (pixels_to_write > 0x10000) {
            h = ((0x10000/2) / width) & ~1;
            pixels_to_write = (width * h) * 2;
        }

        LCD2_BLOCK_CTRL   = 0x10000080;
        LCD2_BLOCK_CONFIG = 0xc0010000 | (pixels_to_write - 1);
        LCD2_BLOCK_CTRL   = 0x34000000;

        unsigned long block_deadline = lcd_deadline();
        if (stride == width) {
            if (!lcd_write_line(h * width, addr, block_deadline))
                goto out;
            addr += stride / 2 * h;
        } else {
            for (r = 0; r < h; r++) {
                if (!lcd_write_line(width, addr, block_deadline))
                    goto out;
                addr += stride / 2;
            }
        }

        if (!lcd_wait_block(LCD2_BLOCK_READY, block_deadline,
                            LCD_FAILURE_BLOCK))
            goto out;
        LCD2_BLOCK_CONFIG = 0;
        transmitted += width * h;
        height -= h;
    }

out:
#ifdef HAVE_PP5020_PERF
    pp5020_perf_record_lcd_update(requested_width * requested_height,
                                  transmitted, USEC_TIMER - started,
                                  !lcd_failure_pending);
#endif
    return !lcd_failure_pending;
}
#endif

/* Update the display.
   This must be called after all other LCD functions that change the display. */
void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}
