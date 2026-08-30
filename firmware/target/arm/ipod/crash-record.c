#include "config.h"

#if defined(HAVE_IPOD_CRASH_RECORD)

#include <stddef.h>
#include <string.h>
#include "crash-record.h"
#include "crc32.h"
#include "system.h"
#include "kernel.h"

/* This section is outside the startup BSS clear range.  Use its uncached
 * alias so a fatal exception does not depend on a cache writeback. */
static struct crash_record retained_record
    __attribute__((section(".crash_record_noinit"), used,
                   aligned(CACHEALIGN_SIZE)));

static struct {
    volatile uint32_t active;
    volatile uint32_t phase;
    volatile uint32_t frame;
    volatile uint32_t slot;
    volatile uint32_t heartbeat_tick;
} ipvf_status SHAREDBSS_ATTR;

static volatile struct crash_record *record_ptr(void)
{
    return UNCACHED_ADDR(&retained_record);
}

static uint32_t read_cpsr(void)
{
    uint32_t value;
    asm volatile ("mrs %0, cpsr" : "=r"(value));
    return value;
}

static uint32_t read_spsr(void)
{
    uint32_t value;
    asm volatile ("mrs %0, spsr" : "=r"(value));
    return value;
}

static void copy_record(struct crash_record *dst,
                        volatile const struct crash_record *src)
{
    volatile const uint32_t *s = (volatile const uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;
    unsigned i;

    for (i = 0; i < sizeof(*dst) / sizeof(uint32_t); i++)
        d[i] = s[i];
}

static uint32_t record_crc(const struct crash_record *record)
{
    struct crash_record copy = *record;

    copy.crc = 0;
    return crc_32((const unsigned char *)&copy + offsetof(struct crash_record,
                                                           version),
                  sizeof(copy) - offsetof(struct crash_record, version),
                  0xffffffffu);
}

static void commit_record(const struct crash_record *next)
{
    volatile struct crash_record *dst = record_ptr();
    struct crash_record copy = *next;
    unsigned i;

    /* Invalidate first, then publish the body, CRC, and magic in that order.
     * A reset during the copy therefore cannot leave a valid old header over
     * a partially updated record. */
    dst->magic = 0;
    dst->crc = 0;
    for (i = 1; i < sizeof(copy) / sizeof(uint32_t); i++) {
        if (i != offsetof(struct crash_record, crc) / sizeof(uint32_t))
            ((volatile uint32_t *)dst)[i] = ((const uint32_t *)&copy)[i];
    }
    membarrier();
    dst->crc = copy.crc;
    membarrier();
    dst->magic = CRASH_RECORD_MAGIC;
    membarrier();
}

static void fill_common(struct crash_record *record, uint32_t kind,
                        uint32_t pc, uint32_t lr, uint32_t sp,
                        bool exception_context)
{
    uint32_t valid = CRASH_RECORD_VALID_CORE | CRASH_RECORD_VALID_PC |
                     CRASH_RECORD_VALID_SP | CRASH_RECORD_VALID_CPSR |
                     CRASH_RECORD_VALID_TIMESTAMP;

    memset(record, 0, sizeof(*record));
    record->version = CRASH_RECORD_VERSION;
    record->size = sizeof(*record);
    record->kind = kind;
    record->core = current_core();
    record->processor_id = processor_id();
    record->pc = pc;
    record->sp = sp;
    record->cpsr = read_cpsr();
    record->timestamp = (uint32_t)current_tick;
    if (lr != 0) {
        record->lr = lr;
        valid |= CRASH_RECORD_VALID_LR;
    }
    if (exception_context) {
        record->spsr = read_spsr();
        valid |= CRASH_RECORD_VALID_SPSR;
    }
    record->valid = valid;
    record->ipvf_active = ipvf_status.active;
    record->ipvf_phase = ipvf_status.phase;
    record->ipvf_frame = ipvf_status.frame;
    record->ipvf_slot = ipvf_status.slot;
    record->ipvf_heartbeat_age = record->ipvf_active ?
        record->timestamp - ipvf_status.heartbeat_tick : 0;
    if (record->ipvf_active || record->ipvf_phase || record->ipvf_frame ||
        record->ipvf_slot || record->ipvf_heartbeat_age)
        record->valid |= CRASH_RECORD_VALID_IPVF;
}

void crash_record_panic(uint32_t pc, uint32_t sp, const char *message)
{
    struct crash_record next;
    unsigned i;

    fill_common(&next, CRASH_RECORD_PANIC, pc, 0, sp, false);
    if (message != NULL)
        for (i = 0; i + 1 < sizeof(next.panic) && message[i] != '\0'; i++)
            next.panic[i] = message[i];
    next.crc = record_crc(&next);
    commit_record(&next);
}

void crash_record_exception(uint32_t pc, uint32_t kind,
                            uint32_t sp, uint32_t lr, bool has_spsr)
{
    struct crash_record next;

    fill_common(&next, kind, pc, lr, sp, has_spsr);
    next.crc = record_crc(&next);
    commit_record(&next);
}

void crash_record_ipvf_update(uint32_t active, uint32_t phase,
                              uint32_t frame, uint32_t slot)
{
    ipvf_status.frame = frame;
    ipvf_status.slot = slot;
    ipvf_status.heartbeat_tick = (uint32_t)current_tick;
    ipvf_status.phase = phase;
    membarrier();
    ipvf_status.active = active;
    membarrier();
}

bool crash_record_get(struct crash_record *record)
{
    struct crash_record copy;

    if (record == NULL)
        return false;
    copy_record(&copy, record_ptr());
    if (copy.magic != CRASH_RECORD_MAGIC ||
        copy.version != CRASH_RECORD_VERSION ||
        copy.size != sizeof(copy))
        return false;
    if (copy.crc != record_crc(&copy))
        return false;
    *record = copy;
    return true;
}

void crash_record_clear(void)
{
    volatile uint32_t *dst = (volatile uint32_t *)record_ptr();
    unsigned i;

    for (i = 0; i < sizeof(retained_record) / sizeof(uint32_t); i++)
        dst[i] = 0;
}

#endif /* HAVE_IPOD_CRASH_RECORD */
