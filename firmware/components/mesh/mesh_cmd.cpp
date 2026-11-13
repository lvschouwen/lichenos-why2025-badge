// "fw mesh": the dispatch to its subcommands, its status, and the registration of MeshCore's console commands.
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"
#include "mesh_cmd.h"

using namespace mesh_int;

// The console commands while MeshCore does not run (#103): "MeshCore stopped (autostart off)" and the like
bool mesh_int::console_not_running(void) {
    if (mesh_running()) {
        return false;
    }
    char const *text = mesh_run_console_text(meshcore_run_get());
    console_reply("MeshCore %s", text ? text : "not running");
    mesh_ident_t state = mesh_identity_state();
    if (state != MESH_IDENT_UNCHECKED) {
        console_reply("identity: %s", mesh_ident_text(state)); // #110: also when the drive kept it from starting
    }
    return true;
}
// "fw mesh start|stop" (#103): the same job as the tile and the Settings row; "fw mesh" shows the result
static void cmd_mesh_run(bool start) {
    char const *why = meshcore_run_request(start);
    if (why) {
        console_reply("MeshCore not %s: %s", start ? "started" : "stopped", why);
    } else {
        console_reply("MeshCore %s (\"fw mesh\" shows when it is done)", start ? "starting..." : "stopping...");
    }
}

// The subcommands that need MeshCore running, in the order they are tried. SUB_EXACT: the whole line; SUB_PREFIX: the
// word with its space ("send "), the handler gets what follows; SUB_WORD: the word alone or with a space, the handler
// gets what follows that space; SUB_WORD_RAW: as SUB_WORD, with the space (handlers that skip it themselves).
enum sub_match_t : uint8_t { SUB_EXACT, SUB_PREFIX, SUB_WORD, SUB_WORD_RAW };
struct mesh_sub_t {
    char const *word;
    sub_match_t match;
    void (*run)(char const *rest);
};
static mesh_sub_t const MESH_SUBS[] = {
    {"name ", SUB_PREFIX, cmd_mesh_name},
    {"radio", SUB_WORD_RAW, cmd_mesh_radio},
    {"pathhash", SUB_WORD_RAW, cmd_mesh_pathhash},
    {"path ", SUB_PREFIX, cmd_mesh_path},
    {"send ", SUB_PREFIX, cmd_mesh_send},
    {"dm ", SUB_PREFIX, cmd_mesh_dm},
    {"out", SUB_EXACT, [](char const *) { cmd_mesh_out(); }},
    {"trace", SUB_WORD, [](char const *rest) { cmd_mesh_trace(rest, MT_TRACE); }},
    {"discover ", SUB_PREFIX, [](char const *rest) { cmd_mesh_trace(rest, MT_DISCOVER); }},
    {"telemetry", SUB_WORD, cmd_mesh_telemetry},
    {"history", SUB_WORD, cmd_mesh_history},
    {"remote", SUB_WORD, cmd_mesh_remote},
    {"link", SUB_EXACT, [](char const *) { cmd_mesh_link(); }},
    {"chats", SUB_EXACT, [](char const *) { cmd_mesh_chats(); }},
    {"ident simulate", SUB_EXACT, [](char const *) { cmd_mesh_ident_simulate(); }},
    {"advert", SUB_WORD, cmd_mesh_advert},
    {"loc", SUB_WORD, cmd_mesh_loc},
    {"autoadd", SUB_WORD, cmd_mesh_autoadd},
    {"scope", SUB_WORD, mesh_cscope_console}, // #290: the console scope
};

static void cmd_mesh(char const *args) {
    if (strcmp(args, "start") == 0 || strcmp(args, "stop") == 0) {
        cmd_mesh_run(strcmp(args, "start") == 0);
        return;
    }
    if (strncmp(args, "autostart", 9) == 0 && (args[9] == ' ' || !args[9])) {
        bool on; // #115: a reboot loop without MeshCore needs the start-at-boot setting from the console
        if (args[9] && arg_onoff(args + 10, &on)) {
            console_reply("start at boot %s (%s)", on ? "on" : "off", meshcore_autostart_set(on) ? "saved" : "not saved");
        } else {
            console_reply("start at boot: %s (mesh autostart on|off)", meshcore_autostart() ? "on" : "off");
        }
        return;
    }
    if (strncmp(args, "hist", 4) == 0 && (args[4] == ' ' || !args[4])) {
        mesh_journal_console(args[4] ? args + 5 : ""); // the journal's own status: there whether MeshCore runs or not
        return;
    }
    if (strncmp(args, "notify", 6) == 0 && (args[6] == ' ' || !args[6])) {
        mesh_notify_console(args[6] ? args + 7 : ""); // #50: the settings and counters are there whether MeshCore runs or not
        return;
    }
    if (strncmp(args, "order", 5) == 0 && (args[5] == ' ' || !args[5])) {
        mesh_order_console(args + 5); // #189: the grid's order is there whether MeshCore runs or not
        return;
    }
    if (console_not_running()) {
        return;
    }
    for (mesh_sub_t const &sub : MESH_SUBS) {
        size_t len = strlen(sub.word);
        if (sub.match == SUB_EXACT ? strcmp(args, sub.word) == 0
                                   : strncmp(args, sub.word, len) == 0 &&
                                         (sub.match == SUB_PREFIX || args[len] == ' ' || !args[len])) {
            sub.run(args + len + (sub.match == SUB_WORD && args[len] ? 1 : 0));
            return;
        }
    }
    if (*args) {
        console_reply("mesh | mesh start | mesh stop | mesh autostart [on|off] | mesh name <name> | mesh radio <preset index | freq bw sf cr> "
                      "[txdbm] | mesh link | mesh out | mesh chats | mesh ident simulate | mesh hist [compact|scan [n] [bad|bad1] [int] [from<offset>]] | mesh send <channel> <text> | "
                      "mesh dm <12 hex> <text> | mesh scope [console <region> YES] | mesh remote [<name|hex> login [pw]|status|cli <text>|settings|logout] | mesh remote settings | mesh history [<name|hex> [on [min] YES|off|interval <min>]] | mesh trace [<hex>] | mesh discover <hex> | mesh advert [interval <off|180|6h|2d>] [manual <zero|flood>] | mesh loc "
                      "[<lat> <lon> | share on|off] | mesh autoadd [on|off | types <all|chat,repeater,room,sensor> | hops "
                      "<any|direct|n> | overwrite on|off]");
        return;
    }
    mesh_lock();
    uint8_t  key[4];
    char     name[sizeof(node->getNodePrefs()->node_name)];
    int      contacts = node->getNumContacts();
    uint32_t rx = radio_driver.getPacketsRecv(), dropped = lora_radio.rx_dropped();
    uint32_t sent = radio_driver.getPacketsSent(), send_failed = lora_radio.send_failed;
    uint32_t timeouts = lora_radio.send_timeouts, refused = lora_radio.send_refused;
    size_t   msgs = mesh_store_message_count();
    memcpy(key, node->self_id.pub_key, sizeof(key));
    snprintf(name, sizeof(name), "%s", node->getNodeName());
    mesh_unlock();
    char      clock[24] = "not set";
    time_t    now       = time(NULL);
    struct tm utc;
    if (clock_is_set() && gmtime_r(&now, &utc)) {
        strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%SZ", &utc);
    }
    console_reply("clock %s", clock);
    console_reply("mesh id %02x%02x%02x%02x \"%s\" | rx %lu, dropped %lu | tx %lu, failed %lu, timed out %lu, "
                  "refused %lu | %d contacts, %u messages",
                  key[0], key[1], key[2], key[3], name, (unsigned long)rx, (unsigned long)dropped,
                  (unsigned long)sent, (unsigned long)send_failed, (unsigned long)timeouts, (unsigned long)refused,
                  contacts, (unsigned)msgs);
    console_reply("identity: %s", mesh_ident_text(__atomic_load_n(&ident_state, __ATOMIC_ACQUIRE)));
    mesh_radio_console(); // mesh_radio_line.c
}

static void cmd_mesh(char const *args);

extern "C" void mesh_console_init(void) {
    static bool registered; // boot, from start_background: one task
    if (registered) {
        return;
    }
    registered = true;
    console_register("mesh",
                     "mesh | mesh start | mesh stop | mesh autostart [on|off] | mesh name <name> | mesh radio <preset | freq bw sf cr> [txdbm] | "
                     "mesh link | mesh out | mesh chats | mesh ident simulate | mesh hist [compact|scan [n] [bad|bad1] [int] [from<offset>]] | mesh send <channel> <text> | "
                     "mesh dm <12 hex> <text> | mesh notify [test dm|chan <name>|fail|mention] | mesh order [reset] | mesh advert ... | mesh loc ... | mesh autoadd ...",
                     cmd_mesh);
    console_register("contacts", "contacts (nodes heard, newest first) | contacts export [<12 hex>] | contacts import "
                     "<part> | end | cancel", cmd_contacts);
    console_register("msgs", "msgs [n] (last n messages)", cmd_msgs);
    console_register("channels", "channels (the channels the badge can read)", cmd_channels);
    console_register("channel", "channel add <name> <base64 key> | channel add #name | channel rm <index|name> | "
                     "channel show <index|name> | channel rename <index|name> <new name>", cmd_channel);
    console_register("advert", "advert [zero|flood] (send an advert now; default as the Advertise setting)", cmd_advert);
}
