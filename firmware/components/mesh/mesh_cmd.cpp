// "fw mesh": the dispatch to its subcommands, its status, and the registration of MeshCore's console commands.
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"
#include "mesh_cmd.h"
#include "mesh_cmd_table.h"

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
    char const *why = meshcore_run_request_console(start);
    if (why) {
        console_reply("MeshCore not %s: %s", start ? "started" : "stopped", why);
    } else {
        console_reply("MeshCore %s (\"fw mesh\" shows when it is done)", start ? "starting..." : "stopping...");
    }
}

// "fw mesh autostart [on|off]" (#115): a reboot loop without MeshCore needs the start-at-boot setting from the console
static void cmd_mesh_autostart(char const *rest) {
    bool on;
    if (arg_onoff(rest, &on)) {
        console_reply("start at boot %s (%s)", on ? "on" : "off", meshcore_autostart_set(on) ? "saved" : "not saved");
    } else {
        console_reply("start at boot: %s (mesh autostart on|off)", meshcore_autostart() ? "on" : "off");
    }
}

// "fw mesh" with a line it does not know
static void cmd_mesh_usage(void) {
    console_reply("mesh | mesh start | mesh stop | mesh autostart [on|off] | mesh name <name> | mesh radio <preset index | freq bw sf cr> "
                  "[txdbm] | mesh link | mesh out | mesh chats | mesh ident simulate | mesh hist [compact|scan [n] [bad|bad1] [int] [from<offset>]] | mesh send <channel> <text> | "
                  "mesh dm <12 hex> <text> | mesh scope [console <region> YES] | mesh remote [<name|hex> login [pw]|status|cli <text>|settings|logout] | mesh remote settings | mesh history [<name|hex> [on [min] YES|off|interval <min>]] | mesh trace [<hex>] | mesh discover <hex> | mesh advert [interval <off|180|6h|2d>] [manual <zero|flood>] | mesh loc "
                  "[<lat> <lon> | share on|off] | mesh autoadd [on|off | types <all|chat,repeater,room,sensor> | hops "
                  "<any|direct|n> | overwrite on|off]");
}

// "fw mesh" alone: MeshCore's figures, copied under the lock and printed after it
static void cmd_mesh_status(void) {
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

// The handlers of MESH_SUBS (mesh_cmd_table.c), by id; the hist, notify and order ones answer whether MeshCore runs
// or not (the journal's status, #50's settings and counters, #189's grid order)
struct mesh_sub_run_t {
    mesh_sub_id_t id;
    void (*run)(char const *rest);
};
static constexpr mesh_sub_run_t MESH_SUB_RUN[] = {
    {MESH_SUB_START, [](char const *) { cmd_mesh_run(true); }},
    {MESH_SUB_STOP, [](char const *) { cmd_mesh_run(false); }},
    {MESH_SUB_AUTOSTART, cmd_mesh_autostart},
    {MESH_SUB_HIST, mesh_journal_console},
    {MESH_SUB_NOTIFY, mesh_notify_console},
    {MESH_SUB_ORDER, mesh_order_console},
    {MESH_SUB_NAME, cmd_mesh_name},
    {MESH_SUB_RADIO, cmd_mesh_radio},
    {MESH_SUB_PATHHASH, cmd_mesh_pathhash},
    {MESH_SUB_PATH, cmd_mesh_path},
    {MESH_SUB_SEND, cmd_mesh_send},
    {MESH_SUB_DM, cmd_mesh_dm},
    {MESH_SUB_OUT, [](char const *) { cmd_mesh_out(); }},
    {MESH_SUB_TRACE, [](char const *rest) { cmd_mesh_trace(rest, MT_TRACE); }},
    {MESH_SUB_DISCOVER, [](char const *rest) { cmd_mesh_trace(rest, MT_DISCOVER); }},
    {MESH_SUB_TELEMETRY, cmd_mesh_telemetry},
    {MESH_SUB_HISTORY, cmd_mesh_history},
    {MESH_SUB_REMOTE, cmd_mesh_remote},
    {MESH_SUB_LINK, [](char const *) { cmd_mesh_link(); }},
    {MESH_SUB_CHATS, [](char const *) { cmd_mesh_chats(); }},
    {MESH_SUB_IDENT_SIMULATE, [](char const *) { cmd_mesh_ident_simulate(); }},
    {MESH_SUB_ADVERT, cmd_mesh_advert},
    {MESH_SUB_LOC, cmd_mesh_loc},
    {MESH_SUB_AUTOADD, cmd_mesh_autoadd},
    {MESH_SUB_SCOPE, mesh_cscope_console},
};

template <typename T, size_t N> static constexpr bool ids_in_order(T const (&table)[N], size_t count) {
    for (size_t i = 0; i < N; i++) {
        if ((size_t)table[i].id != i) {
            return false;
        }
    }
    return N == count;
}
static_assert(ids_in_order(MESH_SUB_RUN, MESH_SUB_COUNT), "MESH_SUB_RUN: one handler per MESH_SUBS entry, in its order");

// Every "fw mesh" subcommand runs here, after its match. Not the place for #289's on-air allow-list: here the target is
// still raw text. The sites that see it resolved are the MESH_FROM_CONSOLE checks next to the #290 scope refusals:
// send_ready (mesh_api_send.cpp: channel and DM sends, and the trace, discover, remote and telemetry starts, with
// mesh_cscope_channel_refusal for channels) and advert_tx_locked (mesh_api_settings.cpp). #289 must still decide two
// cases: "fw advert" (cmd_advert), which has no target, and the status requests that "fw mesh history <who> on"
// schedules, which the mesh task sends later (mesh_rhist_tick), not from the console.
static void mesh_sub_run(mesh_sub_id_t id, char const *rest) {
    MESH_SUB_RUN[id].run(rest);
}

// A line MeshCore needs running for (a subcommand that needs it, an unknown one, the status) says why not while it
// does not run; the others answer either way
static void cmd_mesh(char const *args) {
    char const   *rest;
    mesh_sub_id_t id = mesh_sub_find(args, &rest);
    if (mesh_sub_needs_running(id) && console_not_running()) {
        return;
    }
    if (id != MESH_SUB_COUNT) {
        mesh_sub_run(id, rest);
    } else if (*args) {
        cmd_mesh_usage();
    } else {
        cmd_mesh_status();
    }
}

// The top-level commands' handlers, by MESH_CONSOLE_CMDS id
struct mesh_con_run_t {
    mesh_con_id_t id;
    console_fn_t  run;
};
static constexpr mesh_con_run_t MESH_CON_RUN[] = {
    {MESH_CON_MESH, cmd_mesh},         {MESH_CON_CONTACTS, cmd_contacts}, {MESH_CON_MSGS, cmd_msgs},
    {MESH_CON_CHANNELS, cmd_channels}, {MESH_CON_CHANNEL, cmd_channel},   {MESH_CON_ADVERT, cmd_advert},
};
static_assert(ids_in_order(MESH_CON_RUN, MESH_CON_COUNT), "MESH_CON_RUN: one handler per MESH_CONSOLE_CMDS entry");

extern "C" void mesh_console_init(void) {
    static bool registered; // boot, from start_background: one task
    if (registered) {
        return;
    }
    registered = true;
    // #229: the per-app command prefix goes here, the one place MeshCore's commands get their names
    for (mesh_con_run_t const &c : MESH_CON_RUN) {
        console_register(MESH_CONSOLE_CMDS[c.id].name, MESH_CONSOLE_CMDS[c.id].usage, c.run);
    }
}
