#pragma once

#include "shell.h"

// Between the shell's own files (#66), not for apps

extern shell_sources_t shell_sources;

lv_obj_t *boot_screen_create(void);       // the boot screen; calls shell_home() itself when boot is done
lv_group_t *boot_screen_group(void);     // the keypad group of the boot screen, NULL once it is gone
lv_obj_t *status_bar_create(lv_obj_t *parent); // on the top layer, over every screen after boot

// home.c
lv_obj_t *home_create(void);   // a fresh home screen for the registered apps and the saved layout; focus object = itself
char const *home_selected_name(lv_obj_t *home); // the selected tile's app name; NULL if none or not a home screen
void      home_init(void);     // registers the "fw home" console command

// shell.c
void shell_esc(void);                // Esc pressed: the top screen's shell_on_esc() hook first, else shell_back()
void shell_back(void);               // like Esc: pops the top screen (later, outside key handling)
lv_group_t *shell_top_group(void);   // the keypad group of the screen on top (ui_probe, LVGL context)
void shell_open_app(size_t index);     // opens lichen_app_at(index); from home's Enter
void shell_rebuild_home(void);         // after the app list or the layout changed

// ui_probe.c: bench diagnostics
void ui_probe_init(void);           // registers "fw ui"
void ui_probe_send(char const *seq); // "fw keys send <seq>"
void ui_check_start(void);           // #89: 1 s LVGL object-tree check, registers "fw uicheck"

// app_settings.c, app_about.c: the built-in apps (shell_start registers them)
void settings_app_register(void);
void about_app_register(void);
void keytest_app_register(void);

// time_set_screen.c: Settings > Time > Set date & time (#64), a builder for shell_open()
lv_obj_t *time_set_build(void);

// wifi_screen.c: Settings > Wi-Fi
void wifi_screen_open(void);
// The same screen as the first-run Wi-Fi step: a Skip row on top; done() runs (LVGL task) after a join or a skip
void wifi_screen_open_setup(void (*done)(void));

// setup_screens.c: first-run setup
void setup_init(void);  // registers "fw setup reset"
void setup_start(void); // opens the welcome on top of home unless setup_done is saved

// dialog.c: the open dialog's keypad group, NULL when none is open (#14 T-B); shell.c: give the keys to the dialog, or
// back to the top screen once it is gone
lv_group_t *shell_dialog_group(void);
void        shell_refocus(void);

// shell.c: home is on screen and its fade-in is over (screens may open)
bool shell_home_shown(void);

void update_screen_init(void); // update_screen.c; from shell_start, with the LVGL lock held
