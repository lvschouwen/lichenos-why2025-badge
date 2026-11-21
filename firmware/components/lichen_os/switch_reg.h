#pragma once
// The owners' side of domain 0x16 (#501): one small adapter per switch, so reg_p4_switch.c (the rows and the dispatch)
// has no ESP include and the host test links it with recording stubs (host_tests/reg_stubs_switch.c). The definitions
// are switch_reg.c's; each calls the same owner function the owner's own console command calls, so `fw vib on` and
// `fw set haptics.on on` end in one setter. Safe from any task, none waits on a radio or a bus.
#include <stdbool.h>
#include <stdint.h>

// Radio switches by index: 0 Wi-Fi, 1 LoRa, 2 Bluetooth (radio_id_t, asserted in switch_reg.c)
bool sw_radio_get(unsigned radio);   // what the driver does now (Wi-Fi: the service flag, LoRa: the driver switch, BT: the OS switch)
bool sw_radio_saved(unsigned radio); // the person's switch (lichen_radio_switch_get)
bool sw_radio_put(unsigned radio, bool on); // lichen_radio_set: saves, tells the owners; true when saved
bool sw_airplane_get(void);
bool sw_airplane_put(bool on);

bool    sw_haptics_get(void);
bool    sw_haptics_put(bool on);
uint8_t sw_haptics_strength_get(void);
bool    sw_haptics_strength_put(uint8_t strength);

uint8_t sw_led_mode_get(void);         // led_override_t
void    sw_led_mode_put(uint8_t mode); // not saved, as `fw led`

bool    sw_ota_get(void);
bool    sw_ota_put(bool on); // false: not saved
uint8_t sw_ota_autooff_get(void);
bool    sw_ota_autooff_put(uint8_t minutes);

// The next four are switch_logic.c: pure, linked by the host test too.
// A saved badge_fw u8 the shell or an app reads on its own (the ui.* rows, configured-only): its value or the default
// the table of lichen_api/settings_backup.c gives. false: no such key there.
bool sw_cfg_get(char const *key, uint32_t *out);
// Saves it and tells the shell (the bound hook); true when saved
bool sw_cfg_put(char const *key, uint32_t value);
// The saved column of `fw get` for a row of this domain: a radio's person switch, airplane, or the saved key's stored
// value; false when nothing is stored (or the row has nothing saved)
bool sw_saved(uint16_t id, char const *saved_key, uint32_t *out);
// main.c, once: what the shell does after one of its policy settings changed (shell_ui_policy_changed), told the key
void switch_reg_bind_ui(void (*changed)(char const *key));

bool sw_sensors_boot_get(void);
void sw_sensors_periods(uint32_t *imu_ms, uint32_t *env_ms);
uint8_t sw_r16_get(void);
uint8_t sw_bg_off_count(void); // apps with their background off
bool    sw_led_get(void);       // the saved status LED setting
bool    sw_led_put(bool on);    // false: not saved
