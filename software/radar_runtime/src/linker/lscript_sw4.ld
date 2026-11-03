/******************************************************************************
* Copyright (C) 2023 - 2025 Advanced Micro Devices, Inc. All Rights Reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

_STACK_SIZE = DEFINED(_STACK_SIZE) ? _STACK_SIZE : 0x4000;
_HEAP_SIZE = DEFINED(_HEAP_SIZE) ? _HEAP_SIZE : 0x10000;

_EL0_STACK_SIZE = DEFINED(_EL0_STACK_SIZE) ? _EL0_STACK_SIZE : 1024;
_EL1_STACK_SIZE = DEFINED(_EL1_STACK_SIZE) ? _EL1_STACK_SIZE : 2048;
_EL2_STACK_SIZE = DEFINED(_EL2_STACK_SIZE) ? _EL2_STACK_SIZE : 1024;

MEMORY
{
	psu_ddr_0_memory_0 : ORIGIN = 0x0, LENGTH = 0x7ff00000
	psu_ocm_ram_0_memory_0 : ORIGIN = 0xfffc0000, LENGTH = 0x40000
}

/* Specify the default entry point to the program */

ENTRY(_vector_table)

/* Define the sections, and where they are mapped in memory */

SECTIONS
{
.text : {
   KEEP (*(.vectors))
   *(.boot)
   *(.text)
   *(.text.*)
   *(.gnu.linkonce.t.*)
   *(.plt)
   *(.gnu_warning)
   *(.gcc_execpt_table)
   *(.glue_7)
   *(.glue_7t)
   *(.ARM.extab)
   *(.gnu.linkonce.armextab.*)
} > psu_ddr_0_memory_0

.note.gnu.build-id : {
   KEEP (*(.note.gnu.build-id))
} > psu_ddr_0_memory_0


.init (ALIGN(64)) : {
   KEEP (*(.init))
} > psu_ddr_0_memory_0

.fini (ALIGN(64)) : {
   KEEP (*(.fini))
} > psu_ddr_0_memory_0

.interp : {
   KEEP (*(.interp))
} > psu_ddr_0_memory_0

.note-ABI-tag : {
   KEEP (*(.note-ABI-tag))
} > psu_ddr_0_memory_0

.rodata : {
   . = ALIGN(64);
   __rodata_start = .;
   *(.rodata)
   *(.rodata.*)
   *(.gnu.linkonce.r.*)
   __rodata_end = .;
} > psu_ddr_0_memory_0

.rodata1 : {
   . = ALIGN(64);
   __rodata1_start = .;
   *(.rodata1)
   *(.rodata1.*)
   __rodata1_end = .;
} > psu_ddr_0_memory_0

.sdata2 : {
   . = ALIGN(64);
   __sdata2_start = .;
   *(.sdata2)
   *(.sdata2.*)
   *(.gnu.linkonce.s2.*)
   __sdata2_end = .;
} > psu_ddr_0_memory_0

.sbss2 : {
   . = ALIGN(64);
   __sbss2_start = .;
   *(.sbss2)
   *(.sbss2.*)
   *(.gnu.linkonce.sb2.*)
   __sbss2_end = .;
} > psu_ddr_0_memory_0

.data : {
   . = ALIGN(64);
   __data_start = .;
   *(.data)
   *(.data.*)
   *(.gnu.linkonce.d.*)
   *(.jcr)
   *(.got)
   *(.got.plt)
   __data_end = .;
} > psu_ddr_0_memory_0

.data1 : {
   . = ALIGN(64);
   __data1_start = .;
   *(.data1)
   *(.data1.*)
   __data1_end = .;
} > psu_ddr_0_memory_0

.got : {
   *(.got)
} > psu_ddr_0_memory_0

.got1 : {
   *(.got1)
} > psu_ddr_0_memory_0

.got2 : {
   *(.got2)
} > psu_ddr_0_memory_0

.ctors : {
   . = ALIGN(64);
   __CTOR_LIST__ = .;
   ___CTORS_LIST___ = .;
   KEEP (*crtbegin.o(.ctors))
   KEEP (*(EXCLUDE_FILE(*crtend.o) .ctors))
   KEEP (*(SORT(.ctors.*)))
   KEEP (*(.ctors))
   __CTOR_END__ = .;
   ___CTORS_END___ = .;
} > psu_ddr_0_memory_0

.dtors : {
   . = ALIGN(64);
   __DTOR_LIST__ = .;
   ___DTORS_LIST___ = .;
   KEEP (*crtbegin.o(.dtors))
   KEEP (*(EXCLUDE_FILE(*crtend.o) .dtors))
   KEEP (*(SORT(.dtors.*)))
   KEEP (*(.dtors))
   __DTOR_END__ = .;
   ___DTORS_END___ = .;
} > psu_ddr_0_memory_0

.fixup : {
   __fixup_start = .;
   *(.fixup)
   __fixup_end = .;
} > psu_ddr_0_memory_0

.eh_frame : {
   *(.eh_frame)
} > psu_ddr_0_memory_0

.eh_framehdr : {
   __eh_framehdr_start = .;
   *(.eh_framehdr)
   __eh_framehdr_end = .;
} > psu_ddr_0_memory_0

.gcc_except_table : {
   *(.gcc_except_table)
} > psu_ddr_0_memory_0

.mmu_tbl0 (ALIGN(4096)) : {
   __mmu_tbl0_start = .;
   *(.mmu_tbl0)
   __mmu_tbl0_end = .;
} > psu_ddr_0_memory_0

.mmu_tbl1 (ALIGN(4096)) : {
   __mmu_tbl1_start = .;
   *(.mmu_tbl1)
   __mmu_tbl1_end = .;
} > psu_ddr_0_memory_0

.mmu_tbl2 (ALIGN(4096)) : {
   __mmu_tbl2_start = .;
   *(.mmu_tbl2)
   __mmu_tbl2_end = .;
} > psu_ddr_0_memory_0

.ARM.exidx : {
   __exidx_start = .;
   *(.ARM.exidx*)
   *(.gnu.linkonce.armexidix.*.*)
   __exidx_end = .;
} > psu_ddr_0_memory_0

.preinit_array : {
   . = ALIGN(64);
   __preinit_array_start = .;
   KEEP (*(SORT(.preinit_array.*)))
   KEEP (*(.preinit_array))
   __preinit_array_end = .;
} > psu_ddr_0_memory_0

.init_array : {
   . = ALIGN(64);
   __init_array_start = .;
   KEEP (*(SORT(.init_array.*)))
   KEEP (*(.init_array))
   __init_array_end = .;
} > psu_ddr_0_memory_0

.fini_array : {
   . = ALIGN(64);
   __fini_array_start = .;
   KEEP (*(SORT(.fini_array.*)))
   KEEP (*(.fini_array))
   __fini_array_end = .;
} > psu_ddr_0_memory_0

.drvcfg_sec : {
    . = ALIGN(8);
     __drvcfgsecdata_start = .;
    KEEP (*(.drvcfg_sec))
    __drvcfgsecdata_end = .;
    __drvcfgsecdata_size = __drvcfgsecdata_end - __drvcfgsecdata_start;
} > psu_ddr_0_memory_0

.ARM.attributes : {
   __ARM.attributes_start = .;
   *(.ARM.attributes)
   __ARM.attributes_end = .;
} > psu_ddr_0_memory_0

.sdata : {
   . = ALIGN(64);
   __sdata_start = .;
   *(.sdata)
   *(.sdata.*)
   *(.gnu.linkonce.s.*)
   __sdata_end = .;
} > psu_ddr_0_memory_0

.sbss (NOLOAD) : {
   . = ALIGN(64);
   __sbss_start = .;
   *(.sbss)
   *(.sbss.*)
   *(.gnu.linkonce.sb.*)
   . = ALIGN(64);
   __sbss_end = .;
} > psu_ddr_0_memory_0

.tdata : {
   . = ALIGN(64);
   __tdata_start = .;
   *(.tdata)
   *(.tdata.*)
   *(.gnu.linkonce.td.*)
   __tdata_end = .;
} > psu_ddr_0_memory_0

.tbss : {
   . = ALIGN(64);
   __tbss_start = .;
   *(.tbss)
   *(.tbss.*)
   *(.gnu.linkonce.tb.*)
   __tbss_end = .;
} > psu_ddr_0_memory_0

.bss (NOLOAD) : {
   . = ALIGN(64);
   __bss_start__ = .;
   *(.bss)
   *(.bss.*)
   *(.gnu.linkonce.b.*)
   *(COMMON)
   . = ALIGN(64);
   __bss_end__ = .;
} > psu_ddr_0_memory_0

_SDA_BASE_ = __sdata_start + ((__sbss_end - __sdata_start) / 2 );

_SDA2_BASE_ = __sdata2_start + ((__sbss2_end - __sdata2_start) / 2 );

/* Generate Stack and Heap definitions */

.heap (NOLOAD) : {
   . = ALIGN(64);
   _heap = .;
   HeapBase = .;
   _heap_start = .;
   . += _HEAP_SIZE;
   _heap_end = .;
   HeapLimit = .;
} > psu_ddr_0_memory_0

.stack (NOLOAD) : {
   . = ALIGN(64);
   _el3_stack_end = .;
   . += _STACK_SIZE;
   __el3_stack = .;
   _el2_stack_end = .;
   . += _EL2_STACK_SIZE;
   . = ALIGN(64);
   __el2_stack = .;
   _el1_stack_end = .;
   . += _EL1_STACK_SIZE;
   . = ALIGN(64);
   __el1_stack = .;
   _el0_stack_end = .;
   . += _EL0_STACK_SIZE;
   . = ALIGN(64);
   __el0_stack = .;
} > psu_ddr_0_memory_0


_end = .;

__radar_ddr_start = ORIGIN(psu_ddr_0_memory_0);
__radar_ddr_end = ORIGIN(psu_ddr_0_memory_0)
                + LENGTH(psu_ddr_0_memory_0);
__radar_normal_end = .;

/* 2 MiB dedicated SG descriptor region */
.radar_dma_bd ALIGN(0x200000) (NOLOAD) : {
    __radar_bd_zone_start = .;
    KEEP(*(.radar_dma_bd .radar_dma_bd.*))
    . = MAX(., __radar_bd_zone_start + 0x200000);
    __radar_bd_zone_end = .;
} > psu_ddr_0_memory_0

/* 8 MiB DDR receive buffer pool */
.radar_rx_pool ALIGN(0x200000) (NOLOAD) : {
    __radar_rx_pool_start = .;
    KEEP(*(.radar_rx_pool .radar_rx_pool.*))
    . = MAX(., __radar_rx_pool_start + 0x800000);
    __radar_rx_pool_end = .;
} > psu_ddr_0_memory_0

/* SW4 immutable CPU result store. NOT a DMA buffer and never overlaps BD/RX.
 * Keeps the last validated transport result after DMA reset/PL RECOVER. */
.radar_capture_store ALIGN(0x200000) (NOLOAD) : {
    __radar_capture_store_start = .;
    KEEP(*(.radar_capture_store .radar_capture_store.*))
    . = MAX(., __radar_capture_store_start + 0x800000);
    __radar_capture_store_end = .;
} > psu_ddr_0_memory_0
ASSERT(SIZEOF(.radar_capture_store) == 0x800000,
       "SW4: CPU result store must be exactly 8MiB")
ASSERT(__radar_capture_store_start == __radar_rx_pool_end,
       "SW4: unexpected result store offset")

/* Two 8 MiB cached RGBA slots, independent of radar DMA and retained data. */
.radar_display_frames ALIGN(0x200000) (NOLOAD) : {
    __radar_display_frames_start = .;
    KEEP(*(.radar_display_frames .radar_display_frames.*))
    . = MAX(., __radar_display_frames_start + 0x1000000);
    __radar_display_frames_end = .;
} > psu_ddr_0_memory_0

/* Isolated 2 MiB Normal-NC block; XDpDma embeds the live descriptors. */
.radar_display_control ALIGN(0x200000) (NOLOAD) : {
    __radar_display_control_start = .;
    KEEP(*(.radar_display_control .radar_display_control.*))
    . = MAX(., __radar_display_control_start + 0x200000);
    __radar_display_control_end = .;
} > psu_ddr_0_memory_0
ASSERT(__radar_display_frames_start == 0x01400000,
       "DP: framebuffer base changed")
ASSERT(__radar_display_frames_start == __radar_capture_store_end,
       "DP: unexpected framebuffer offset")
ASSERT(SIZEOF(.radar_display_frames) == 0x1000000,
       "DP: framebuffer reservation must be 16MiB")
ASSERT(__radar_display_control_start == 0x02400000,
       "DP: control base changed")
ASSERT(SIZEOF(.radar_display_control) == 0x200000,
       "DP: control reservation must be 2MiB")
.radar_dsp_workspace ALIGN(0x200000) (NOLOAD) : {
    __radar_dsp_workspace_start = .;
    KEEP(*(.radar_dsp_workspace .radar_dsp_workspace.*))
    . = MAX(., __radar_dsp_workspace_start + 0x1000000);
    __radar_dsp_workspace_end = .;
} > psu_ddr_0_memory_0
ASSERT(__radar_dsp_workspace_start == 0x02600000,
       "DSP: workspace must follow DP control without moving existing regions")
ASSERT(__radar_dsp_workspace_end == 0x03600000,
       "DSP: workspace must be exactly16MiB")
ASSERT(__radar_normal_end <= 0x00200000,
       "DSP: ordinary program/BSS exceeded frozen2MiB boundary")
__radar_image_end = .;

ASSERT(SIZEOF(.radar_dma_bd) == 0x200000,
       "SW0: BD area must be exactly 2MiB")
ASSERT(SIZEOF(.radar_rx_pool) == 0x800000,
       "SW0: RX pool must be exactly 8MiB")
ASSERT((__radar_bd_zone_start & 0x1fffff) == 0,
       "SW0: BD alignment")
ASSERT(__radar_normal_end <= __radar_bd_zone_start,
       "SW0: normal image overlaps BD area")
ASSERT(__radar_rx_pool_start == __radar_bd_zone_end,
       "SW0: unexpected RX pool offset")
ASSERT(__radar_image_end <= __radar_ddr_end,
       "SW0: memory region overflow")
ASSERT(__radar_image_end <= 0x7ff00000,
       "SW0: top reserved MiB must remain untouched")
ASSERT(_HEAP_SIZE >= 0x10000,
       "SW0: heap must be at least 64KiB")
ASSERT(_STACK_SIZE >= 0x4000,
       "SW0: EL3 stack must be at least 16KiB")
}
