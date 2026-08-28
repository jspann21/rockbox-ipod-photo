/***************************************************************************
 * PP5020 / iPod Color LCD2 RGB write-path qualification.
 *
 * This is a temporary roadmap #14 hardware benchmark.  It compares the
 * stock lcd_update() path against a hand-written ARM feeder which preserves
 * the exact LCD2_BLOCK_TXOK handshake before every 32-bit FIFO write.  It
 * does not attempt undocumented DMA or unchecked FIFO bursts.
 ****************************************************************************/
#include "plugin.h"
#include "cpu.h"
#include "hwcompat.h"

PLUGIN_HEADER

#if defined(IPOD_COLOR) && defined(HAVE_LCD_COLOR) && LCD_DEPTH == 16 && \
    (CONFIG_CPU == PP5020) && !defined(SIMULATOR)

#define LCD14_LOG_PATH ROCKBOX_DIR "/lcd14.csv"
#define LCD14_FRAMES 32
#define LCD14_WARMUP 4
#define LCD14_POLL_LIMIT 1000000u
#define LCD14_WORDS ((LCD_WIDTH * LCD_HEIGHT) / 2)

static fb_data pattern[LCD_WIDTH * LCD_HEIGHT] __attribute__((aligned(4)));

static uint32_t now_us(void)
{
    return USEC_TIMER;
}

static inline bool wait_write(void)
{
    unsigned int count = LCD14_POLL_LIMIT;

    while (LCD2_PORT & LCD2_BUSY_MASK)
        if (--count == 0)
            return false;

    return true;
}

static inline bool wait_block(unsigned long mask)
{
    unsigned int count = LCD14_POLL_LIMIT;

    while (!(LCD2_BLOCK_CTRL & mask))
        if (--count == 0)
            return false;

    return true;
}

static bool cmd_data(int lcd_type, unsigned cmd, unsigned data)
{
    if ((lcd_type & 1) == 0)
    {
        if (!wait_write())
            return false;
        LCD2_PORT = LCD2_CMD_MASK | cmd;
        if (!wait_write())
            return false;
        LCD2_PORT = LCD2_CMD_MASK | data;
    }
    else
    {
        if (!wait_write())
            return false;
        LCD2_PORT = LCD2_CMD_MASK;
        LCD2_PORT = LCD2_CMD_MASK | cmd;
        if (!wait_write())
            return false;
        LCD2_PORT = LCD2_DATA_MASK | (data >> 8);
        LCD2_PORT = LCD2_DATA_MASK | (data & 0xff);
    }

    return true;
}

static int detect_lcd_type(void)
{
    if (IPOD_HW_REVISION == 0x60000)
        return 0;

    return (GPIOA_INPUT_VAL & 0x2) | ((GPIOA_INPUT_VAL & 0x10) >> 4);
}

static bool setup_fullscreen(int lcd_type)
{
    const int y0 = 0;
    const int y1 = LCD_HEIGHT - 1;
    int x1 = LCD_WIDTH - 1;
    int x0 = 0;

    if ((lcd_type & 1) == 0)
    {
        if (!cmd_data(lcd_type, 0x12, y0) ||
            !cmd_data(lcd_type, 0x13, x1) ||
            !cmd_data(lcd_type, 0x15, y1) ||
            !cmd_data(lcd_type, 0x16, x0))
            return false;
    }
    else
    {
        if (!cmd_data(lcd_type, 0x44, (y1 << 8) | y0) ||
            !cmd_data(lcd_type, 0x45, (x1 << 8) | x0))
            return false;

        x0 = x1;
        if (!cmd_data(lcd_type, 0x21, (x0 << 8) | y0))
            return false;

        if (!wait_write())
            return false;
        LCD2_PORT = LCD2_CMD_MASK;
        LCD2_PORT = LCD2_CMD_MASK | 0x22;
    }

    return true;
}

/* Fast candidate: four source words are loaded at a time, but every single
 * LCD write is still preceded by the established TXOK check.  The slow wait
 * path remains bounded exactly as in the C driver. */
__attribute__((naked, noinline))
static int write_words_arm(const uint32_t *src __attribute__((unused)),
                           unsigned int words __attribute__((unused)))
{
    __asm__ volatile(
        "stmfd sp!, {r4-r10, lr}\n"
        "ldr r2, =0x70008a20\n"
        "add r3, r2, #0xe0\n"
        "ldr r4, =1000000\n"
        "cmp r1, #4\n"
        "blt .Llcd14_tail\n"
        ".Llcd14_loop4:\n"
        "ldmia r0!, {r5-r8}\n"

        "ldr r9, [r2]\n"
        "tst r9, #0x01000000\n"
        "bne .Llcd14_s1\n"
        "mov r10, r4\n"
        ".Llcd14_w1:\n"
        "ldr r9, [r2]\n"
        "tst r9, #0x01000000\n"
        "bne .Llcd14_s1\n"
        "subs r10, r10, #1\n"
        "bne .Llcd14_w1\n"
        "b .Llcd14_fail\n"
        ".Llcd14_s1:\n"
        "str r5, [r3]\n"

        "ldr r9, [r2]\n"
        "tst r9, #0x01000000\n"
        "bne .Llcd14_s2\n"
        "mov r10, r4\n"
        ".Llcd14_w2:\n"
        "ldr r9, [r2]\n"
        "tst r9, #0x01000000\n"
        "bne .Llcd14_s2\n"
        "subs r10, r10, #1\n"
        "bne .Llcd14_w2\n"
        "b .Llcd14_fail\n"
        ".Llcd14_s2:\n"
        "str r6, [r3]\n"

        "ldr r9, [r2]\n"
        "tst r9, #0x01000000\n"
        "bne .Llcd14_s3\n"
        "mov r10, r4\n"
        ".Llcd14_w3:\n"
        "ldr r9, [r2]\n"
        "tst r9, #0x01000000\n"
        "bne .Llcd14_s3\n"
        "subs r10, r10, #1\n"
        "bne .Llcd14_w3\n"
        "b .Llcd14_fail\n"
        ".Llcd14_s3:\n"
        "str r7, [r3]\n"

        "ldr r9, [r2]\n"
        "tst r9, #0x01000000\n"
        "bne .Llcd14_s4\n"
        "mov r10, r4\n"
        ".Llcd14_w4:\n"
        "ldr r9, [r2]\n"
        "tst r9, #0x01000000\n"
        "bne .Llcd14_s4\n"
        "subs r10, r10, #1\n"
        "bne .Llcd14_w4\n"
        "b .Llcd14_fail\n"
        ".Llcd14_s4:\n"
        "str r8, [r3]\n"

        "subs r1, r1, #4\n"
        "cmp r1, #4\n"
        "bge .Llcd14_loop4\n"

        ".Llcd14_tail:\n"
        "cmp r1, #0\n"
        "beq .Llcd14_ok\n"
        ".Llcd14_tail_loop:\n"
        "ldr r5, [r0], #4\n"
        "ldr r9, [r2]\n"
        "tst r9, #0x01000000\n"
        "bne .Llcd14_tail_store\n"
        "mov r10, r4\n"
        ".Llcd14_tail_wait:\n"
        "ldr r9, [r2]\n"
        "tst r9, #0x01000000\n"
        "bne .Llcd14_tail_store\n"
        "subs r10, r10, #1\n"
        "bne .Llcd14_tail_wait\n"
        "b .Llcd14_fail\n"
        ".Llcd14_tail_store:\n"
        "str r5, [r3]\n"
        "subs r1, r1, #1\n"
        "bne .Llcd14_tail_loop\n"

        ".Llcd14_ok:\n"
        "mov r0, #1\n"
        "ldmfd sp!, {r4-r10, pc}\n"
        ".Llcd14_fail:\n"
        "mov r0, #0\n"
        "ldmfd sp!, {r4-r10, pc}\n"
    );
}

/* One-frame observational probe. initial_misses is the number of words for
 * which TXOK was not already asserted on the first status read; polls counts
 * all TXOK register reads. It never writes unless TXOK is asserted. */
__attribute__((naked, noinline))
static int write_words_probe(const uint32_t *src __attribute__((unused)),
                             unsigned int words __attribute__((unused)),
                             uint32_t *initial_misses __attribute__((unused)),
                             uint32_t *polls __attribute__((unused)))
{
    __asm__ volatile(
        "stmfd sp!, {r4-r10, lr}\n"
        "ldr r4, =0x70008a20\n"
        "add r5, r4, #0xe0\n"
        "mov r6, #0\n"
        "mov r7, #0\n"
        "ldr r8, =1000000\n"
        ".Llcd14_probe_loop:\n"
        "ldr r9, [r0], #4\n"
        "ldr r10, [r4]\n"
        "add r7, r7, #1\n"
        "tst r10, #0x01000000\n"
        "bne .Llcd14_probe_store\n"
        "add r6, r6, #1\n"
        "mov r12, r8\n"
        ".Llcd14_probe_wait:\n"
        "ldr r10, [r4]\n"
        "add r7, r7, #1\n"
        "tst r10, #0x01000000\n"
        "bne .Llcd14_probe_store\n"
        "subs r12, r12, #1\n"
        "bne .Llcd14_probe_wait\n"
        "str r6, [r2]\n"
        "str r7, [r3]\n"
        "mov r0, #0\n"
        "ldmfd sp!, {r4-r10, pc}\n"
        ".Llcd14_probe_store:\n"
        "str r9, [r5]\n"
        "subs r1, r1, #1\n"
        "bne .Llcd14_probe_loop\n"
        "str r6, [r2]\n"
        "str r7, [r3]\n"
        "mov r0, #1\n"
        "ldmfd sp!, {r4-r10, pc}\n"
    );
}

static bool direct_update(int lcd_type, const fb_data *src, bool probe,
                          uint32_t *misses, uint32_t *polls)
{
    int y = 0;
    int height = LCD_HEIGHT;

    if (!setup_fullscreen(lcd_type))
        return false;

    while (height > 0)
    {
        int h = height;
        int bytes = LCD_WIDTH * h * 2;
        unsigned int words;
        int ok;

        if (bytes > 0x10000)
        {
            h = ((0x10000 / 2) / LCD_WIDTH) & ~1;
            bytes = LCD_WIDTH * h * 2;
        }

        LCD2_BLOCK_CTRL = 0x10000080;
        LCD2_BLOCK_CONFIG = 0xc0010000 | (bytes - 1);
        LCD2_BLOCK_CTRL = 0x34000000;

        words = (unsigned int)(LCD_WIDTH * h / 2);
        if (probe)
        {
            uint32_t block_misses = 0;
            uint32_t block_polls = 0;
            ok = write_words_probe((const uint32_t *)(src + y * LCD_WIDTH),
                                   words, &block_misses, &block_polls);
            *misses += block_misses;
            *polls += block_polls;
        }
        else
        {
            ok = write_words_arm((const uint32_t *)(src + y * LCD_WIDTH),
                                 words);
        }

        if (!ok || !wait_block(LCD2_BLOCK_READY))
        {
            LCD2_BLOCK_CONFIG = 0;
            return false;
        }

        LCD2_BLOCK_CONFIG = 0;
        y += h;
        height -= h;
    }

    return true;
}

static void make_pattern(void)
{
    static const fb_data bars[] = {
        FB_RGBPACK(255, 255, 255), FB_RGBPACK(255, 0, 0),
        FB_RGBPACK(0, 255, 0),     FB_RGBPACK(0, 0, 255),
        FB_RGBPACK(255, 255, 0),   FB_RGBPACK(0, 255, 255),
        FB_RGBPACK(255, 0, 255),   FB_RGBPACK(0, 0, 0),
    };
    int y;

    for (y = 0; y < LCD_HEIGHT; y++)
    {
        int x;
        for (x = 0; x < LCD_WIDTH; x++)
        {
            fb_data p = bars[(x * (int)ARRAYLEN(bars)) / LCD_WIDTH];
            if (x == y || x == LCD_WIDTH - 1 - y ||
                x == 0 || y == 0 || x == LCD_WIDTH - 1 || y == LCD_HEIGHT - 1)
                p = FB_RGBPACK(255, 255, 255);
            pattern[y * LCD_WIDTH + x] = p;
        }
    }
}

static uint32_t bench_reference(unsigned int frames)
{
    uint32_t start = now_us();
    unsigned int i;

    for (i = 0; i < frames; i++)
        rb->lcd_update();

    return now_us() - start;
}

static uint32_t bench_candidate(int lcd_type, unsigned int frames, bool *ok)
{
    uint32_t start = now_us();
    unsigned int i;

    *ok = true;
    for (i = 0; i < frames; i++)
    {
        uint32_t dummy_misses = 0, dummy_polls = 0;
        if (!direct_update(lcd_type, pattern, false,
                           &dummy_misses, &dummy_polls))
        {
            *ok = false;
            break;
        }
    }

    return now_us() - start;
}

static void warmup(int lcd_type)
{
    unsigned int i;

    for (i = 0; i < LCD14_WARMUP; i++)
    {
        uint32_t dummy_misses = 0, dummy_polls = 0;
        rb->lcd_update();
        direct_update(lcd_type, pattern, false,
                      &dummy_misses, &dummy_polls);
    }
}

static void log_row(int fd, int lcd_type, int boosted, long cpu_hz,
                    const char *method, unsigned int frames,
                    uint32_t total_us, uint32_t misses,
                    uint32_t polls, bool ok)
{
    rb->fdprintf(fd, "%d,%d,%ld,%s,%u,%lu,%lu,%lu,%lu,%d\n",
                 lcd_type, boosted, cpu_hz, method, frames,
                 (unsigned long)total_us,
                 (unsigned long)(frames ? total_us / frames : 0),
                 (unsigned long)misses, (unsigned long)polls,
                 ok ? 1 : 0);
}

static enum plugin_status run_benchmark(void)
{
    uint32_t probe_misses = 0, probe_polls = 0;
    uint32_t ref_normal, arm_normal, ref_boost = 0, arm_boost = 0;
    bool probe_ok, arm_normal_ok, arm_boost_ok = true;
    long normal_hz = 0, boost_hz = 0;
    int lcd_type = detect_lcd_type();
    int fd;

    make_pattern();
    rb->lcd_bitmap(pattern, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    rb->lcd_update();

    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "LCD2 safe-path test");
    rb->lcd_puts(0, 2, "Candidate pattern next");
    rb->lcd_puts(0, 3, "Check for corruption");
    rb->lcd_update();
    rb->sleep(HZ);

    rb->lcd_bitmap(pattern, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    probe_ok = direct_update(lcd_type, pattern, true,
                             &probe_misses, &probe_polls);
    rb->sleep(2 * HZ);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    normal_hz = *rb->cpu_frequency;
#endif
    warmup(lcd_type);
    ref_normal = bench_reference(LCD14_FRAMES);
    arm_normal = bench_candidate(lcd_type, LCD14_FRAMES, &arm_normal_ok);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
    rb->sleep(HZ / 10);
    boost_hz = *rb->cpu_frequency;
    warmup(lcd_type);
    ref_boost = bench_reference(LCD14_FRAMES);
    arm_boost = bench_candidate(lcd_type, LCD14_FRAMES, &arm_boost_ok);
    rb->cpu_boost(false);
#endif

    fd = rb->open(LCD14_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        rb->fdprintf(fd,
            "lcd_type,boosted,cpu_hz,method,frames,total_us,per_frame_us,"
            "initial_misses,polls,ok\n");
        log_row(fd, lcd_type, 0, normal_hz, "probe", 1, 0,
                probe_misses, probe_polls, probe_ok);
        log_row(fd, lcd_type, 0, normal_hz, "reference", LCD14_FRAMES,
                ref_normal, 0, 0, true);
        log_row(fd, lcd_type, 0, normal_hz, "arm", LCD14_FRAMES,
                arm_normal, 0, 0, arm_normal_ok);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        log_row(fd, lcd_type, 1, boost_hz, "reference", LCD14_FRAMES,
                ref_boost, 0, 0, true);
        log_row(fd, lcd_type, 1, boost_hz, "arm", LCD14_FRAMES,
                arm_boost, 0, 0, arm_boost_ok);
#endif
        rb->close(fd);
    }

    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "LCD2 test complete");
    rb->lcd_putsf(0, 1, "ref %lu us", (unsigned long)(ref_normal / LCD14_FRAMES));
    rb->lcd_putsf(0, 2, "arm %lu us", (unsigned long)(arm_normal / LCD14_FRAMES));
    rb->lcd_putsf(0, 3, "miss %lu/%u", (unsigned long)probe_misses, LCD14_WORDS);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->lcd_putsf(0, 4, "boost %lu/%lu",
                  (unsigned long)(ref_boost / LCD14_FRAMES),
                  (unsigned long)(arm_boost / LCD14_FRAMES));
#endif
    rb->lcd_puts(0, 6, probe_ok && arm_normal_ok && arm_boost_ok ? "PASS" : "FAILED");
    rb->lcd_update();
    rb->button_get(true);

    return (probe_ok && arm_normal_ok && arm_boost_ok) ? PLUGIN_OK : PLUGIN_ERROR;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    return run_benchmark();
}

#else

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    rb->splash(HZ * 2, "iPod Color only");
    return PLUGIN_ERROR;
}

#endif
