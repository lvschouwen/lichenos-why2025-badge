#pragma once

// Every screen id in one place (#148). The shell's identity of a screen is (id, key), so two screens must never share an
// id: host_tests/screen_ids_test.c fails on a duplicate and on an overlap with the app ranges. Add new ids here.
//   1..SCREEN_APP_MAX      the apps' own screens (shell_open_app: 1 + app index)
//   99..198                Lichen's and MeshCore's fixed screens (below)
//   SCREEN_APP_BASE + i    app i's Settings / About page (app_settings.c)

#define SCREEN_APP_MAX  32  // most apps the id ranges allow
#define SCREEN_APP_BASE 200 // + app index: 200..231

#define SCREEN_ID_LIST(X) \
    X(SCREEN_WELCOME, 99) \
    X(SCREEN_WIFI, 100) \
    X(SCREEN_DISPLAY, 101) \
    X(SCREEN_TZ, 102) \
    X(SCREEN_ABOUT, 103) \
    X(SCREEN_TIME, 104) \
    X(SCREEN_TIME_SET, 105) \
    X(SCREEN_SD, 106) \
    X(SCREEN_CRASH_ENTRY, 107) \
    X(SCREEN_NOTIFY_DETAIL, 108) \
    X(SCREEN_NOTIFY_SETTINGS, 109) \
    X(SCREEN_TEXT_ENTRY, 120) \
    X(SCREEN_RADIO, 130) \
    X(SCREEN_CUSTOM, 131) \
    X(SCREEN_CHANNELS, 132) \
    X(SCREEN_SWITCH, 134) \
    X(SCREEN_CHANNEL, 135) \
    X(SCREEN_ADVERT, 138) \
    X(SCREEN_CHAT, 142) \
    X(SCREEN_DM_PICK, 143) \
    X(SCREEN_DM, 144) \
    X(SCREEN_UPDATE, 150) \
    X(SCREEN_POWER_BYE, 151) \
    X(SCREEN_BATTERY, 152) \
    X(SCREEN_CONTACT, 160) \
    X(SCREEN_SHARE, 161) \
    X(SCREEN_TRACE, 162) \
    X(SCREEN_LOGIN, 163) \
    X(SCREEN_STATUS, 164) \
    X(SCREEN_CLI, 165) \
    X(SCREEN_RSET, 166) \
    X(SCREEN_CHATS, 170) \
    X(SCREEN_CONTACTS, 171) \
    X(SCREEN_SETTINGS, 172) \
    X(SCREEN_NOTIFY_PAGE, 173) \
    X(SCREEN_FACTORY, 174) \
    X(SCREEN_CONSOLE, 175) \
    X(SCREEN_MC_HEALTH, 177) \
    X(SCREEN_MC_AIRLOG, 178) \
    X(SCREEN_MC_SIGNAL, 179) \
    X(SCREEN_FILES_VIEW, 180) \
    X(SCREEN_NOTES_ENTRY, 190) \
    X(SCREEN_NOTES_NEW, 191) \
    X(SCREEN_TIMER_PAGE, 250) \
    X(SCREEN_STOPWATCH_PAGE, 251) \
    X(SCREEN_WORLD_PAGE, 252) \
    X(SCREEN_QR, 197) \
    X(SCREEN_NOTE_ITEM, 198)

#define SCREEN_ID_DEFINE(name, value) name = value,
enum { SCREEN_ID_LIST(SCREEN_ID_DEFINE) };
#define SCREEN_APP SCREEN_APP_BASE
