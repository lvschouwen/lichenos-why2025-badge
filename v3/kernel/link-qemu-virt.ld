/* QEMU virt, -bios none: the reset vector jumps to 0x80000000. App slots are at 0x80400000 (text) / 0x80800000 (data). */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { KRAM : ORIGIN = 0x80000000, LENGTH = 2M }
SECTIONS {
  .text : { KEEP(*(.text.boot)) *(.text.trap) *(.text .text.*) } > KRAM
  .rodata : ALIGN(4) { *(.rodata .rodata.*) *(.srodata .srodata.*) } > KRAM
  .data : ALIGN(4) { *(.data .data.*) *(.sdata .sdata.*) } > KRAM
  .bss (NOLOAD) : ALIGN(4) { _sbss = .; *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) . = ALIGN(4); _ebss = .; } > KRAM
  .stack (NOLOAD) : ALIGN(16) { _stack_bottom = .; . += 16K; _stack_top = .; . += 2K; _kfault_stack_top = .; } > KRAM
  /DISCARD/ : { *(.eh_frame*) }
}
