/* v3 bring-up image for the ESP32-P4, loaded by the ROM download mode (esptool load-ram), never written to flash. RAM only:
 * no segment outside L2MEM. Numbers mirror platform/esp32p4/src/mem.rs and v3/tools/check_image.py.
 *  - LOAD   0x4FF10000 up to the ROM stack at 0x4FF3AFC0: what the ROM copies in. It is where esptool's own rev1 stub
 *           keeps its text, so download mode is known to leave it alone.
 *  - NOLOAD from 0x4FF40000 up to the L2 cache carve (0x4FF80000 for the 256 KB reset-state cache; the kernel reads the
 *           real size at boot and CHECKs it): bss and stacks, zeroed by the kernel itself, untouched while loading.
 * .text.trap stays in the script so #612 does not have to edit it. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
  LOAD   (rwx) : ORIGIN = 0x4FF10000, LENGTH = 0x2AFC0
  NOLOAD (rw)  : ORIGIN = 0x4FF40000, LENGTH = 0x40000
}
SECTIONS {
  .text : { _load_start = .; KEEP(*(.text.boot)) *(.text.trap) *(.text .text.*) } > LOAD
  .rodata : ALIGN(4) { *(.rodata .rodata.*) *(.srodata .srodata.*) } > LOAD
  .data : ALIGN(4) { *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(4); _load_end = .; } > LOAD
  .bss (NOLOAD) : ALIGN(4) { _noload_start = .; _sbss = .; *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) . = ALIGN(4); _ebss = .; } > NOLOAD
  .stack (NOLOAD) : ALIGN(16) { . += 16K; _stack_top = .; . += 2K; _trap_stack_top = .; _noload_end = .; } > NOLOAD
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.riscv.attributes) }
  ASSERT(_load_end <= 0x4FF3AFC0, "loaded image runs into the ROM stack")
  ASSERT(_noload_end <= 0x4FF80000, "NOLOAD memory runs into the L2 cache carve")
  ASSERT(ORIGIN(LOAD) == 0x4FF10000 && ORIGIN(NOLOAD) == 0x4FF40000, "memory map drifted from platform/esp32p4 mem.rs")
}
