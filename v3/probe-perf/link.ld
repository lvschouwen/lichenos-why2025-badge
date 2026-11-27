/* #648 perf lab probe, loaded by the P4 ROM download mode (load-ram), never written to flash. Code and data below the v3 boot stage
 * as in probe-psram. Unlike probe-psram, bss, buffers and stacks sit BELOW 0x4FF3_A000 (the ROM stack starts at 0x4FF3_AFC0): the
 * cache carve of a 512 KB L2 starts at 0x4FF4_0000, so probe-psram's NOLOAD window would be eaten by it. Every L2 size fits. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
  LOAD   (rwx) : ORIGIN = 0x4FF10000, LENGTH = 0x18ED0
  NOLOAD (rw)  : ORIGIN = 0x4FF29000, LENGTH = 0x11000
}
SECTIONS {
  .text : { KEEP(*(.text.boot)) _ram_text_start = .; *(.text.ram .text.ram.*) _ram_text_end = .; *(.text.trap) *(.text .text.*) } > LOAD
  .rodata : ALIGN(4) { *(.rodata .rodata.*) *(.srodata .srodata.*) } > LOAD
  .data : ALIGN(4) { *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(4); _load_end = .; } > LOAD
  /* DMA buffers first: 128 B aligned (the larger L2 line) */
  .bss (NOLOAD) : ALIGN(128) { _sbss = .; *(.bss.dma_buf .bss.dma_buf.*) *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) . = ALIGN(4); _ebss = .; } > NOLOAD
  .stack (NOLOAD) : ALIGN(16) { . += 16K; _stack_top = .; . += 4K; _trap_stack_top = .; . += 4K; _core1_stack_top = .; . += 1K; _core1_trap_top = .; _noload_end = .; } > NOLOAD
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.riscv.attributes) }
  ASSERT(_load_end <= 0x4FF28ED0, "loaded image runs into the v3 boot stage")
  ASSERT(_noload_end <= 0x4FF3A000, "NOLOAD memory runs into the ROM stack or a 512 KB L2 carve")
}
