/* v3 bring-up image for the ESP32-P4, loaded by the ROM download mode (esptool load-ram), never written to flash. RAM only:
 * no segment outside L2MEM. Numbers mirror platform/esp32p4/src/mem.rs and v3/tools/check_image.py.
 *  - LOAD   0x4FF10000 up to 0x4FF28ED0: what the ROM copies in, and below the v3 boot stage (#615) that sits at 0x4FF28ED0..0x4FF3ABD0. It is where esptool's own rev1 stub
 *           keeps its text, so download mode is known to leave it alone.
 *  - NOLOAD from 0x4FF40000 up to the L2 cache carve (0x4FF80000 for the 256 KB reset-state cache; the kernel reads the
 *           real size at boot and CHECKs it): bss and stacks, zeroed by the kernel itself, untouched while loading.
 * .text.trap stays in the script so #612 does not have to edit it. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
  LOAD   (rwx) : ORIGIN = 0x4FF10000, LENGTH = 0x18ED0
  NOLOAD (rw)  : ORIGIN = 0x4FF40000, LENGTH = 0x40000
}
SECTIONS {
  /* .text.ram: code that must stay in internal RAM even if the rest later moves (cache maintenance, platform psram/map.rs); it
     goes right after the entry code so .text.boot keeps ORIGIN(LOAD), and its range is marked for tools/check_image.py */
  .text : { _load_start = .; KEEP(*(.text.boot)) _ram_text_start = .; *(.text.ram .text.ram.*) _ram_text_end = .; *(.text.trap) *(.text .text.*) } > LOAD
  .utext : ALIGN(4096) { _utext = .; KEEP(*(.utext)) . = ALIGN(4096); _eutext = .; } > LOAD
  .rodata : ALIGN(4) { *(.rodata .rodata.*) *(.srodata .srodata.*) } > LOAD
  .data : ALIGN(4) { *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(4); _load_end = .; } > LOAD
  .bss (NOLOAD) : ALIGN(4) { _noload_start = .; _sbss = .; *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) . = ALIGN(4); _ebss = .; } > NOLOAD
  .stack (NOLOAD) : ALIGN(16) { . += 16K; _stack_top = .; . += 4K; _trap_stack_top = .; . += 2K; _fault_stack_top = .; } > NOLOAD
  /* Core 1 (#612 checkpoint 3): its own kernel, trap and fault stacks; _start1 and nested_trap pick them by mhartid. */
  .stack1 (NOLOAD) : ALIGN(16) { . += 8K; _stack1_top = .; . += 4K; _trap_stack1_top = .; . += 2K; _fault_stack1_top = .; _noload_end = .; } > NOLOAD
  /* U data page for the stubs (#612): the first data slot of the plan (0x4FF70000), 4 KB of it. */
  .udata 0x4FF70000 (NOLOAD) : { _udata = .; . += 4K; _eudata = .; } > NOLOAD
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.riscv.attributes) }
  ASSERT(_load_end <= 0x4FF28ED0, "loaded image runs into the v3 boot stage")
  ASSERT(_udata == 0x4FF70000, "the U data page must be the first data slot (SLOT_DATA in kcore/src/slots.rs)")
  ASSERT(_eutext - _utext == 4096, "the U stub page must be exactly one 4 KB NAPOT page")
  ASSERT(_noload_end <= 0x4FF60000, "kernel bss and stacks must stay below the app slots at 0x4FF60000")
  ASSERT(_noload_end <= 0x4FF80000, "NOLOAD memory runs into the L2 cache carve")
  ASSERT(ORIGIN(LOAD) == 0x4FF10000 && ORIGIN(NOLOAD) == 0x4FF40000, "memory map drifted from platform/esp32p4 mem.rs")
}
