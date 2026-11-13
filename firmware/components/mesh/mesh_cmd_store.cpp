// "fw contacts|msgs|channels|channel|advert".
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"
#include "mesh_cmd.h"

using namespace mesh_int;

// ---- "fw contacts export|import" (#54) ------------------------------------------------------------------------------

// PSRAM: the parts of a link pasted so far (the USB console takes 95 bytes a line, the Wi-Fi console a whole link)
static mesh_import_t *import_text;
// A contact link fits one Wi-Fi console line (#110): console_cmd_test pins it (console_cmd.h is the OS's, #283)

// "fw contacts export [<12 hex>]": the contact's (or our own) export link
static void contacts_export(char const *arg) {
    uint8_t  prefix[MF_PREFIX];
    unsigned b[MF_PREFIX];
    bool     self = !*arg;
    if (!self && (strspn(arg, "0123456789abcdefABCDEF") != 12 || arg[12] ||
                  sscanf(arg, "%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) < 6)) {
        console_reply("contacts export [<12 hex digits of the public key>] (none: this badge)");
        return;
    }
    for (int i = 0; i < MF_PREFIX; i++) {
        prefix[i] = (uint8_t)b[i];
    }
    char *uri = (char *)lichen_alloc(meshcore_ctx(), MESH_ADVERT_URI_MAX, LICHEN_MEM_BULK);
    if (!uri) {
        console_reply("no memory");
        return;
    }
    char const *why = mesh_contact_export(self ? NULL : prefix, uri, MESH_ADVERT_URI_MAX);
    if (why) {
        console_reply("not exported: %s", why);
    } else {
        console_reply("%s", uri);
        console_reply("(%u characters; to import on a badge: \"fw contacts import <link>\" (over USB in parts of up to "
                      "79 characters), then \"fw contacts import end\")", (unsigned)strlen(uri));
    }
    lichen_free(meshcore_ctx(), uri);
}

// "fw contacts import <part>" ... "fw contacts import end" (or "cancel")
static void contacts_import(char const *arg) {
    if (!import_text) {
        import_text = (mesh_import_t *)lichen_alloc(meshcore_ctx(), sizeof(*import_text), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
        if (!import_text) {
            console_reply("no memory");
            return;
        }
    }
    if (strcmp(arg, "cancel") == 0) {
        import_text->len = 0;
        console_reply("import cancelled");
        return;
    }
    if (strcmp(arg, "end") != 0) {
        char const *why = *arg ? mesh_import_add(import_text, arg) : "contacts import <part> | end | cancel";
        if (why) {
            console_reply("%s", why);
        } else {
            console_reply("%u characters so far (\"fw contacts import end\" when all parts are in)",
                          (unsigned)import_text->len);
        }
        return;
    }
    uint8_t    *pkt = (uint8_t *)lichen_alloc(meshcore_ctx(), MESH_ADVERT_MAX, LICHEN_MEM_BULK);
    char const *why = NULL;
    size_t      len = 0;
    if (!pkt) {
        console_reply("no memory");
        return;
    }
    if (import_text->len == 0) {
        why = "nothing pasted (\"fw contacts import meshcore://...\" first)";
    } else {
        len = mesh_advert_uri_parse(import_text->text, pkt, MESH_ADVERT_MAX, &why);
    }
    import_text->len = 0;
    char name[MESH_NAME_MAX + 1] = "";
    if (len) {
        why = mesh_contact_import(pkt, len, name, sizeof(name));
    }
    if (why) {
        console_reply("not imported: %s", why);
    } else {
        console_reply("contact %02x%02x%02x%02x%02x%02x \"%s\" added", pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7],
                      name);
    }
    lichen_free(meshcore_ctx(), pkt);
}

void mesh_int::cmd_contacts(char const *args) {
    if (console_not_running()) {
        return;
    }
    if (strncmp(args, "export", 6) == 0 && (args[6] == ' ' || !args[6])) {
        contacts_export(args[6] ? args + 7 : "");
        return;
    }
    if (strncmp(args, "import", 6) == 0 && (args[6] == ' ' || !args[6])) {
        contacts_import(args[6] ? args + 7 : "");
        return;
    }
    if (*args) {
        console_reply("contacts | contacts export [<12 hex>] | contacts import <part> | end | cancel");
        return;
    }
    size_t          total;
    mesh_contact_t *list = (mesh_contact_t *)lichen_alloc(meshcore_ctx(), MAX_CONTACTS * sizeof(mesh_contact_t), LICHEN_MEM_BULK);
    if (!list) {
        console_reply("no memory");
        return;
    }
    size_t n = mesh_contacts_snapshot(list, MAX_CONTACTS, &total, MESH_WAIT_FOREVER);
    for (size_t i = 0; i < n; i++) {
        char age[24], hops[12];
        mesh_format_age(list[i].age_s, age, sizeof(age));
        mesh_format_hops(list[i].path_len, hops, sizeof(hops)); // the hash size bits are not hops (#85)
        console_reply("contact %02x%02x%02x%02x%02x%02x %-32s %-8s %-8s %s", list[i].pubkey[0], list[i].pubkey[1], list[i].pubkey[2],
                      list[i].pubkey[3], list[i].pubkey[4], list[i].pubkey[5], list[i].name,
                      mesh_type_name(list[i].type), hops, age);
    }
    lichen_free(meshcore_ctx(), list);
    console_reply("%u contact(s)", (unsigned)total);
}

void mesh_int::cmd_msgs(char const *args) {
    if (console_not_running()) {
        return;
    }
    long want = *args ? strtol(args, NULL, 10) : 10;
    want      = want < 1 || want > STORE_MESSAGES ? 10 : want;
    uint32_t    generation;
    mesh_msg_t *list = (mesh_msg_t *)lichen_alloc(meshcore_ctx(), (size_t)want * sizeof(mesh_msg_t), LICHEN_MEM_BULK);
    if (!list) {
        console_reply("no memory");
        return;
    }
    size_t n   = mesh_messages_snapshot(list, (size_t)want, &generation, MESH_WAIT_FOREVER);
    uint32_t now = mesh_uptime_s(), epoch = mesh_epoch_now();
    for (size_t i = 0; i < n; i++) {
        char age[24];
        mesh_format_msg_age(list[i].loaded, list[i].heard_s, list[i].epoch, now, epoch, age, sizeof(age));
        console_reply("[%s] %s (%s, rssi %d)", list[i].channel, list[i].text, age, list[i].rssi_x10 / 10);
    }
    lichen_free(meshcore_ctx(), list);
    console_reply("%u message(s)", (unsigned)n);
}

void mesh_int::cmd_channels(char const *args) {
    if (console_not_running()) {
        return;
    }
    (void)args;
    ChannelDetails details;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        mesh_lock();
        bool ok = node->getChannel(i, details) && details.name[0];
        mesh_unlock();
        if (ok) {
            console_reply("channel %d: %s (hash %02x)", i, details.name, details.channel.hash[0]);
        }
    }
}

// "fw channel rm <index|name>": the same as the screen's confirm
static void channel_rm(char const *arg) {
    char *end;
    long  index = strtol(arg, &end, 10);
    bool  by_index = end != arg && *end == 0;
    char name[MESH_CHANNEL_MAX + 1];
    if (!by_index && !console_channel_name(arg, name, sizeof(name), "not removed")) {
        return; // "badgetest" means "#badgetest" (#101)
    }
    char const *why = by_index ? mesh_channel_remove((int)index) : mesh_channel_remove_named(name);
    if (why) {
        console_reply("channel %s not removed: %s", arg, why);
    } else {
        console_reply("channel %s removed", arg);
    }
}

// The channel a console argument names: a slot number or a name (channel_find_locked); false (said why) when none
static bool console_channel_info(char const *arg, mesh_channel_info_t *info) {
    char *end;
    long  index = strtol(arg, &end, 10);
    bool  by_index = end != arg && *end == 0 && index >= 0;
    size_t found = mesh_channel_info(by_index ? (int)index : -1, by_index ? NULL : arg, info, MESH_SET_WAIT_MS);
    if (found == MESH_BUSY) {
        console_reply("MeshCore is busy, try again");
    } else if (!found) {
        console_reply("no channel %s (\"fw channels\" lists them)", arg);
    }
    return found == 1;
}

// "fw channel show <index|name>" (#54): the key and the link MeshCore's apps add a channel from. Secret lines, as the
// update token (#123): to the USB or Wi-Fi console that asked, never the log store or the Console app's scrollback.
static void channel_show(char const *arg) {
    struct ShowMem {
        mesh_channel_info_t info;
        char                hex[MESH_KEY_HEX_MAX], b64[MESH_KEY_B64_MAX], uri[MESH_CHANNEL_URI_MAX];
    };
    ShowMem *m = (ShowMem *)lichen_alloc(meshcore_ctx(), sizeof(ShowMem), LICHEN_MEM_BULK | LICHEN_MEM_ZERO); // off the console's 4 KB stack
    if (!m) {
        console_reply("no memory");
        return;
    }
    if (console_channel_info(arg, &m->info)) {
        mesh_channel_info_t const &info = m->info;
        mesh_hex_text(info.secret, info.key_len, m->hex, sizeof(m->hex));
        mesh_base64_text(info.secret, info.key_len, m->b64, sizeof(m->b64));
        console_reply("channel %d: %s (hash %02x, %u-byte key; key and link only to you, never logged)", info.index,
                      info.name, info.hash, info.key_len);
        console_reply_secret("key (hex) %s", m->hex);
        console_reply_secret("key (base64) %s", m->b64);
        if (mesh_channel_uri(info.name, info.secret, info.key_len, m->uri, sizeof(m->uri))) {
            console_reply_secret("link %s", m->uri);
        } else {
            console_reply_secret("no link: MeshCore's apps only take 16-byte keys");
        }
    }
    mesh_wipe(m, sizeof(*m)); // a channel key: not left in the heap (volatile: a memset before a free may be dropped)
    lichen_free(meshcore_ctx(), m);
}

// "fw channel rename <index|name> <new name>" (#54): the name only, the key stays
static void channel_rename(char const *args) {
    char   which[MESH_CHANNEL_MAX + 2];
    size_t n = strcspn(args, " ");
    char const *new_name = args + n + strspn(args + n, " ");
    if (n == 0 || n >= sizeof(which) || !*new_name) {
        console_reply("channel rename <index|name> <new name>");
        return;
    }
    memcpy(which, args, n);
    which[n] = '\0';
    mesh_channel_info_t info;
    if (!console_channel_info(which, &info)) {
        return;
    }
    memset(info.secret, 0, sizeof(info.secret));
    char const *why = mesh_channel_rename(info.name, new_name);
    if (why) {
        console_reply("channel %s not renamed: %s", info.name, why);
    } else {
        console_reply("channel %d renamed from %s", info.index, info.name);
    }
}

// "fw channel add <name> <base64 key>" or "fw channel add #name": the same as the screen's
void mesh_int::cmd_channel(char const *args) {
    if (console_not_running()) {
        return;
    }
    while (*args == ' ') {
        args++;
    }
    if (strncmp(args, "rm ", 3) == 0 && args[3]) {
        channel_rm(args + 3);
        return;
    }
    if (strncmp(args, "show ", 5) == 0 && args[5]) {
        channel_show(args + 5);
        return;
    }
    if (strncmp(args, "rename ", 7) == 0) {
        channel_rename(args + 7);
        return;
    }
    char name[48] = "", key[64] = "";
    if (sscanf(args, "add %47s %63s", name, key) < 1) {
        mesh_wipe(key, sizeof(key)); // sscanf may have stopped after a partial match
        console_reply("channel add <name> <base64 key> | channel add #name | channel rm <index|name> | "
                      "channel show <index|name> | channel rename <index|name> <new name>");
        return;
    }
    char const *why = mesh_channel_add(name, key[0] ? key : NULL);
    mesh_wipe(key, sizeof(key)); // a channel key: not left on the console task's stack (#123)
    if (why) {
        console_reply("channel %s not added: %s", name, why);
    } else {
        console_reply("channel %s added", name);
    }
}

// "fw advert": a zero-hop advert now (MyMesh::advert(): nodes in direct range hear it), for testing transmit
void mesh_int::cmd_advert(char const *args) {
    if (console_not_running()) {
        return;
    }
    bool flood;
    mesh_advert_cfg_t cfg;
    mesh_get_advert_cfg(&cfg);
    flood = strcmp(args, "flood") == 0 ? true : strcmp(args, "zero") == 0 ? false : cfg.flood_manual;
    if (*args && strcmp(args, "flood") != 0 && strcmp(args, "zero") != 0) {
        console_reply("advert [zero|flood] (default: the Advertise setting, now %s)", cfg.flood_manual ? "flood" : "zero");
        return;
    }
    if (!ident_tx_allowed()) {
        console_reply("advert not sent: identity %s", mesh_ident_text(MESH_IDENT_MISMATCH));
        return;
    }
    if (!lichen_radio_get(RADIO_LORA)) {
        console_reply("advert not sent: LoRa is off (fw lora on)");
        return;
    }
    if (char const *why = mesh_advert_refusal(clock_is_set())) {
        console_reply("advert not sent: %s (other nodes would drop it)", why);
        return;
    }
    if (char const *why = flood ? mesh_cscope_refusal(true) : NULL) {
        console_reply("advert not sent: %s", why); // #290: a flood from the console takes the console scope
        return;
    }
    if (!mesh_send_advert_mode(flood, MESH_FROM_CONSOLE)) {
        console_reply("advert not sent");
    } else if (char const *note = mesh_advert_note(clock_is_approximate())) {
        console_reply("advert queued (%s: %s)", note, clock_src_text(CLOCK_SRC_APP)); // allowed, but said (#83)
    } else {
        console_reply("advert queued");
    }
}
