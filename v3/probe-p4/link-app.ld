/* #608 step 3b: the probe as an app image for the v6.1 bootloader (LICHEN_PROBE_RUN=app). link.ld plus the
 * esp_app_desc_t in one flash-mapped segment at 0x40000020 (flash page 0 of the slot, past the 24-byte image header and
 * 8-byte segment header), which esptool places first. The RAM layout is link.ld's: below the bootloader's own
 * 0x4FF28ED0-0x4FF3ABD0 (rev<3 bootloader.memory.ld.in). */
INCLUDE link.ld
SECTIONS {
  .app_desc 0x40000020 : { KEEP(*(.app_desc)) }
}
