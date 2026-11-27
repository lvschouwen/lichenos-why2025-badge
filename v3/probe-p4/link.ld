/* #608 bench probe, loaded by the P4 ROM's download mode (esptool load-ram), never written to flash.
 * Everything the ROM loads sits where esptool's rev1 P4 stub keeps its text (0x4FF10000), so download mode is known
 * to leave it alone. NOLOAD memory sits where the stub keeps its bss (0x4FF50000): untouched while the ROM loads,
 * zeroed by the probe itself. U-mode code and stack each get their own 4 KB NAPOT window. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
  LOAD   (rwx) : ORIGIN = 0x4FF10000, LENGTH = 0x8000
  ULOAD  (rx)  : ORIGIN = 0x4FF18000, LENGTH = 0x1000
  NOLOAD (rw)  : ORIGIN = 0x4FF50000, LENGTH = 0x8000
  USTACK (rw)  : ORIGIN = 0x4FF58000, LENGTH = 0x1000
}
SECTIONS {
  .text : { KEEP(*(.text.boot)) *(.text.trap) *(.text .text.*) } > LOAD
  .rodata : ALIGN(4) { *(.rodata .rodata.*) *(.srodata .srodata.*) } > LOAD
  .data : ALIGN(4) { *(.data .data.*) *(.sdata .sdata.*) } > LOAD
  .utext : { _utext = .; KEEP(*(.utext)) _eutext = .; } > ULOAD
  .bss (NOLOAD) : ALIGN(4) { _sbss = .; *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) . = ALIGN(4); _ebss = .; } > NOLOAD
  .mstack (NOLOAD) : ALIGN(16) { . += 8K; _mstack_top = .; . += 4K; _trap_stack_top = .; } > NOLOAD
  .ustack (NOLOAD) : { _ustack_bottom = .; . += 4K; _ustack_top = .; } > USTACK
  /DISCARD/ : { *(.eh_frame*) }
}
