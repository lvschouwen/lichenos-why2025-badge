/* #620 PSRAM probe, loaded by the P4 ROM download mode (load-ram), never written to flash. Same map as the v3
 * bring-up image (kernel/link-esp32p4.ld, platform/esp32p4 mem.rs): code below the v3 boot stage, bss and stacks in
 * NOLOAD memory that the ROM does not touch while loading. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
  LOAD   (rwx) : ORIGIN = 0x4FF10000, LENGTH = 0x18ED0
  NOLOAD (rw)  : ORIGIN = 0x4FF40000, LENGTH = 0x40000
}
SECTIONS {
  /* code that must stay in internal RAM even if the rest later moves (cache maintenance, platform psram/map.rs) */
  .text.ram : { *(.text.ram .text.ram.*) } > LOAD
  .text : { KEEP(*(.text.boot)) *(.text.trap) *(.text .text.*) } > LOAD
  .rodata : ALIGN(4) { *(.rodata .rodata.*) *(.srodata .srodata.*) } > LOAD
  .data : ALIGN(4) { *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(4); _load_end = .; } > LOAD
  .bss (NOLOAD) : ALIGN(4) { _sbss = .; *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) . = ALIGN(4); _ebss = .; } > NOLOAD
  .stack (NOLOAD) : ALIGN(16) { . += 16K; _stack_top = .; . += 2K; _trap_stack_top = .; _noload_end = .; } > NOLOAD
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.riscv.attributes) }
  ASSERT(_load_end <= 0x4FF28ED0, "loaded image runs into the v3 boot stage")
  ASSERT(_noload_end <= 0x4FF80000, "NOLOAD memory runs into the L2 cache carve")
}
