#define __ASSEMBLER__
#include "cpu.h"

/* Will have been included from app.lds */
ENTRY(start)

OUTPUT_FORMAT(elf32-littlearm)
OUTPUT_ARCH(arm)
STARTUP(target/arm/pp/crt0-pp.o)

#define PLUGINSIZE PLUGIN_BUFFER_SIZE
#define CODECSIZE CODEC_SIZE
#ifdef HAVE_IPOD_CRASH_RECORD
#define CRASH_RECORD_SIZE 0x100
#else
#define CRASH_RECORD_SIZE 0
#endif

#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGINSIZE - CODECSIZE - CRASH_RECORD_SIZE

#define DRAMORIG 0x00000000
#define IRAMORIG 0x40000000
#define IRAMSIZE 0xc000

/* End of the audio buffer, where the codec buffer starts */
#define ENDAUDIOADDR  (DRAMORIG + DRAMSIZE)

/* Where the codec buffer ends, and the plugin buffer starts */
#define ENDADDR (ENDAUDIOADDR + CODECSIZE)

MEMORY
{
    DRAM : ORIGIN = DRAMORIG,     LENGTH = DRAMSIZE
    IRAM : ORIGIN = IRAMORIG,     LENGTH = IRAMSIZE
#ifdef HAVE_IPOD_CRASH_RECORD
    RETAINED : ORIGIN = DRAMORIG + MEMORYSIZE * 0x100000 - CRASH_RECORD_SIZE,
               LENGTH = CRASH_RECORD_SIZE
#endif
}

SECTIONS
{
    .text :
    {
        loadaddress = .;
        _loadaddress = .;
        . = ALIGN(0x200);
        *(.init.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(0x4);
    } > DRAM

    .rodata :
    {
        *(.rodata)  /* problems without this, dunno why */
        *(.rodata*)
        *(.rodata.str1.1)
        *(.rodata.str1.4)
        . = ALIGN(0x4);
    } > DRAM

    .data :
    {
        *(.data*)
        . = ALIGN(0x4);
    } > DRAM

#if NOCACHE_BASE != 0
    /* .ncdata section is placed at uncached physical alias address and is
     * loaded at the proper cached virtual address - no copying is
     * performed in the init code */
    .ncdata . + NOCACHE_BASE :
    {
        . = ALIGN(CACHEALIGN_SIZE);
        *(.ncdata*)
        . = ALIGN(CACHEALIGN_SIZE);
    } AT> DRAM
#endif
    
    /DISCARD/ :
    {
        *(.eh_frame)
    }

    .vectors 0x0 :
    {
        _vectorsstart = .;
        KEEP(*(.vectors));
        _vectorsend = .;
    } AT> DRAM

    _vectorscopy = LOADADDR(.vectors);
    _noloaddram  = LOADADDR(.vectors);

    .ibss IRAMORIG (NOLOAD) :
    {
        _iedata = .;
        *(.qharray)
        *(.ibss*)
        . = ALIGN(0x4);
        _iend = .;
    } > IRAM

    .iram _iend :
    {
        _iramstart = .;
        *(.icode*)
        *(.irodata*)
        *(.idata*)
        . = ALIGN(0x4);
        _iramend = .;
    } > IRAM AT> DRAM

    _iramcopy = LOADADDR(.iram);


    .init ENDAUDIOADDR : 
    {
        . = ALIGN(4);
        _initstart = .;
        *(.init*)
        _initend = .;
    } AT> DRAM

    _initcopy = LOADADDR(.init);

    .idle_stacks (NOLOAD) :
    {
       *(.idle_stacks)
       . = ALIGN(8);
#if NUM_CORES > 1
       cpu_idlestackbegin = .;
       . += IDLE_STACK_SIZE;
       cpu_idlestackend = .;
#endif
       cop_idlestackbegin = .;
       . += IDLE_STACK_SIZE;
       cop_idlestackend = .;
    } > IRAM

    .stack (NOLOAD) :
    {
       *(.stack)
       stackbegin = .;
       . += 0x2000;
       stackend = .;
    } > IRAM
    
    /* .bss and .ncbss are treated as a single section to use one init loop to
     * zero it - note "_edata" and "_end" */
    .bss _noloaddram (NOLOAD) :
    {
       _edata = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(0x4);
    } > DRAM

#if NOCACHE_BASE != 0
    .ncbss . + NOCACHE_BASE (NOLOAD):
    {
    	. = ALIGN(CACHEALIGN_SIZE);
        *(.ncbss*)
    	. = ALIGN(CACHEALIGN_SIZE);
    } AT> DRAM
#endif

    /* This will be aligned by preceding alignments */
    .endaddr . - NOCACHE_BASE (NOLOAD) :
    {
        _end = .;
    } > DRAM

    .audiobuf _end (NOLOAD) :
    {
        _audiobuffer = .;
        . = ALIGN(0x4);
        audiobuffer = .;
    } > DRAM
    
    .audiobufend ENDAUDIOADDR (NOLOAD) :
    {
#ifdef IPOD_VIDEO
        audiobufend_lds = .;
#else
        audiobufend = .;
#endif
        _audiobufend = .;
    } > DRAM

    .codec ENDAUDIOADDR (NOLOAD) :
    {
        codecbuf = .;
        _codecbuf = .;
    }

    .plugin ENDADDR (NOLOAD) :
    {
        _pluginbuf = .;
        pluginbuf = .;
    }

#ifdef HAVE_IPOD_CRASH_RECORD
    /* Reserve the final 256 bytes of physical SDRAM. It is outside the
     * advertised plugin buffer, is not loadable, and is not cleared by crt0. */
    .crash_record_noinit (NOLOAD) :
    {
        . = ALIGN(CACHEALIGN_SIZE);
        KEEP(*(.crash_record_noinit))
        . = ALIGN(CACHEALIGN_SIZE);
    } > RETAINED
    ASSERT(SIZEOF(.crash_record_noinit) <= CRASH_RECORD_SIZE,
           "retained crash record exceeds reservation")
#endif
}
