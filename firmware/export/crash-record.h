/***************************************************************************
 * Retained crash record for the iPod Color/Photo (PP5020) target.
 ****************************************************************************/
#ifndef ROCKBOX_CRASH_RECORD_H
#define ROCKBOX_CRASH_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#define CRASH_RECORD_MAGIC   0x43525348u /* "CRSH" */
#define CRASH_RECORD_VERSION 1u

enum crash_record_kind {
    CRASH_RECORD_NONE = 0,
    CRASH_RECORD_PANIC = 1,
    CRASH_RECORD_UNDEFINED = 2,
    CRASH_RECORD_PREFETCH_ABORT = 3,
    CRASH_RECORD_DATA_ABORT = 4,
    CRASH_RECORD_DIVIDE_BY_ZERO = 5,
    CRASH_RECORD_SOFTWARE = 6,
};

enum crash_record_ipvf_phase {
    CRASH_RECORD_IPVF_IDLE = 0,
    CRASH_RECORD_IPVF_STARTING = 1,
    CRASH_RECORD_IPVF_WAITING_SLOT = 2,
    CRASH_RECORD_IPVF_READING = 3,
    CRASH_RECORD_IPVF_QUEUED = 4,
    CRASH_RECORD_IPVF_RENDERING = 5,
    CRASH_RECORD_IPVF_DRAINING = 6,
    CRASH_RECORD_IPVF_RECONCILING = 7,
};

enum crash_record_valid {
    CRASH_RECORD_VALID_CORE = 1u << 0,
    CRASH_RECORD_VALID_PC = 1u << 1,
    CRASH_RECORD_VALID_LR = 1u << 2,
    CRASH_RECORD_VALID_SP = 1u << 3,
    CRASH_RECORD_VALID_CPSR = 1u << 4,
    CRASH_RECORD_VALID_SPSR = 1u << 5,
    CRASH_RECORD_VALID_TIMESTAMP = 1u << 6,
    CRASH_RECORD_VALID_IPVF = 1u << 7,
};

/* The CRC covers every byte after magic, with crc set to zero. */
struct crash_record {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t crc;
    uint32_t kind;
    uint32_t valid;
    uint32_t core;
    uint32_t processor_id;
    uint32_t pc;
    uint32_t lr;
    uint32_t sp;
    uint32_t cpsr;
    uint32_t spsr;
    uint32_t timestamp;
    uint32_t ipvf_active;
    uint32_t ipvf_phase;
    uint32_t ipvf_frame;
    uint32_t ipvf_slot;
    uint32_t ipvf_heartbeat_age;
    char panic[48];
};

bool crash_record_get(struct crash_record *record);
void crash_record_clear(void);

/* Called from panic/UIE before the normal fatal-screen path. */
void crash_record_panic(uint32_t pc, uint32_t sp, const char *message);
void crash_record_exception(uint32_t pc, uint32_t kind,
                            uint32_t sp, uint32_t lr, bool has_spsr);

/* Small target-neutral status hook for the IPVF plugin. */
void crash_record_ipvf_update(uint32_t active, uint32_t phase,
                              uint32_t frame, uint32_t slot);

#endif /* ROCKBOX_CRASH_RECORD_H */
