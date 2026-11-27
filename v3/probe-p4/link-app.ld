/* #608 step 3b: the probe as an app image for the v6.1 bootloader (LICHEN_PROBE_RUN=app). link.ld plus the
 * esp_app_desc_t in one flash-mapped segment at 0x40000020 (flash page 0 of the slot, past the 24-byte image header and
 * 8-byte segment header), which esptool places first, and a second flash segment: on the P4 (D/I share their
 * vaddrs) unpack_load_app asserts exactly two (rom_index == 2, bootloader_utility.c:839; an IDF app's rodata and text).
 * esptool wants each in its own 64 KB page, so the second is at 0x40010020; esptool pads the file up to it with
 * segments loaded below 0x10000000, which the bootloader skips (esp_image_format.c should_load). The RAM layout is link.ld's: below the bootloader's own
 * 0x4FF28ED0-0x4FF3ABD0 (rev<3 bootloader.memory.ld.in). */
INCLUDE link.ld
SECTIONS {
  .app_desc 0x40000020 : { KEEP(*(.app_desc)) }
  .app_rom2 0x40010020 : { KEEP(*(.app_rom2)) }
}
