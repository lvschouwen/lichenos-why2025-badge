// "fw mesh name|radio|pathhash|advert|loc|autoadd|ident simulate".
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"
#include "mesh_cmd.h"

using namespace mesh_int;

// "fw mesh name <name>": the same setter as the screen
void mesh_int::cmd_mesh_name(char const *name) {
    if (mesh_set_name(name)) {
        mesh_settings_t s;
        mesh_get_settings(&s);
        console_reply("name \"%s\" saved (others see it in the next advert)", s.name);
    } else {
        console_reply("name not set (empty after trimming, or MeshCore busy)");
    }
}

// "fw mesh radio <preset index> [txdbm]" or "fw mesh radio <freq> <bw> <sf> <cr> [txdbm]"
void mesh_int::cmd_mesh_radio(char const *args) {
    mesh_settings_t now;
    mesh_get_settings(&now);
    float    v[5];
    int      n = sscanf(args, "%f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4]);
    float    freq = now.freq, bw = now.bw;
    uint8_t  sf = now.sf, cr = now.cr;
    int      tx = now.tx_dbm;
    char     what[48];
    if (n == 1 || n == 2) {
        radio_preset_t const *p = v[0] >= 0 && v[0] == (int)v[0] ? radio_preset_at((size_t)v[0]) : NULL;
        if (!p) {
            console_reply("no preset %g (0..%u)", v[0], (unsigned)radio_preset_count() - 1);
            return;
        }
        freq = p->freq_mhz, bw = p->bw_khz, sf = p->sf, cr = p->cr;
        tx   = n == 2 ? (int)lroundf(v[1]) : tx;
        snprintf(what, sizeof(what), "preset %s", p->title);
    } else if (n == 4 || n == 5) {
        // Out of the uint8_t range stays out of range for radio_check
        freq = v[0], bw = v[1];
        sf   = v[2] >= 0 && v[2] <= 255 ? (uint8_t)v[2] : 255;
        cr   = v[3] >= 0 && v[3] <= 255 ? (uint8_t)v[3] : 255;
        tx   = n == 5 ? (int)lroundf(v[4]) : tx;
        snprintf(what, sizeof(what), "custom");
    } else {
        console_reply("mesh radio <preset index> [txdbm] | mesh radio <freq MHz> <bw kHz> <sf> <cr> [txdbm]");
        for (size_t i = 0; i < radio_preset_count(); i++) {
            radio_preset_t const *p = radio_preset_at(i);
            console_reply("  %2u %-26s %.3f MHz %g kHz SF%u CR%u", (unsigned)i, p->title, p->freq_mhz, p->bw_khz,
                          p->sf, p->cr);
        }
        return;
    }
    int8_t      tx8 = tx < -128 ? -128 : tx > 127 ? 127 : (int8_t)tx;
    char const *why = mesh_set_radio(freq, bw, sf, cr, tx8);
    if (why) {
        console_reply("radio not changed: %s", why);
    } else {
        console_reply("radio set (%s): %.3f MHz, %g kHz, SF%u, CR%u, %d dBm", what, freq, bw, sf, cr, tx8);
    }
}

// "fw mesh pathhash [1|2|3]": the size now, or set it (saved, nothing is sent)
void mesh_int::cmd_mesh_pathhash(char const *args) {
    while (*args == ' ') {
        args++;
    }
    if (!*args) {
        mesh_settings_t st;
        mesh_get_settings(&st);
        console_reply("path hash %u B (mesh pathhash <1|2|3> to change)", st.path_hash_size);
        return;
    }
    char *end;
    long  v = strtol(args, &end, 10);
    if (end == args || *end || v < 1 || v > 3) {
        console_reply("mesh pathhash [1|2|3]: bytes per hop in a flood's path");
        return;
    }
    char const *why = mesh_set_path_hash((uint8_t)v);
    if (why) {
        console_reply("path hash not changed: %s", why);
    } else {
        console_reply("path hash set to %ld B (saved)", v);
    }
}

// "fw mesh ident simulate" (#80): the mismatch path on the bench without touching the identity file. A copy of the
// live public key with one bit flipped must fail the check; then TX is blocked and the notice raised, as for a real
// mismatch, until the next restart (which checks the real file again).
void mesh_int::cmd_mesh_ident_simulate(void) {
    uint8_t prv[PRV_KEY_SIZE], pub[PUB_KEY_SIZE];
    mesh_lock();
    size_t got = node->self_id.writeTo(prv, sizeof(prv));
    memcpy(pub, node->self_id.pub_key, sizeof(pub));
    mesh_unlock();
    pub[PUB_KEY_SIZE / 2] ^= 0x10;
    mesh_ident_t flipped = got == PRV_KEY_SIZE ? mesh_ident_check(prv, pub) : MESH_IDENT_UNCHECKED;
    mh_wipe(prv, sizeof(prv));
    console_reply("check of a copy with one bit flipped: %s", mesh_ident_text(flipped));
    ident_apply(MESH_IDENT_MISMATCH, true);
    console_reply("identity: %s (simulated until restart; the file is untouched)", mesh_ident_text(MESH_IDENT_MISMATCH));
}

// ---- advert, location, auto-add (#52): "fw mesh advert|loc|autoadd" ---------------------------------------------------

bool mesh_int::arg_onoff(char const *t, bool *v) {
    if (strcmp(t, "on") == 0 || strcmp(t, "off") == 0) {
        *v = t[1] == 'n';
        return true;
    }
    return false;
}

static void reply_result(char const *what, char const *why) {
    console_reply(why ? "%s not changed: %s" : "%s saved", what, why);
}

void mesh_int::cmd_mesh_advert(char const *args) {
    mesh_advert_cfg_t c;
    if (strncmp(args, "interval ", 9) == 0) {
        uint32_t    min;
        char const *why = advcfg_parse_interval(args + 9, &min);
        reply_result("advert interval", why ? why : mesh_set_advert_interval(min));
    } else if (strncmp(args, "manual ", 7) == 0 && (strcmp(args + 7, "zero") == 0 || strcmp(args + 7, "flood") == 0)) {
        reply_result("manual advert", mesh_set_advert_manual_flood(args[7] == 'f'));
    } else if (*args) {
        console_reply("mesh advert | mesh advert interval <off|180|6h|2d> (flood, 3 h to 7 d) | mesh advert manual <zero|flood>");
        return;
    }
    if (!mesh_get_advert_cfg(&c)) {
        return;
    }
    char iv[16], next[12];
    advcfg_interval_text(c.interval_min, iv, sizeof(iv));
    advcfg_next_badge(c.next_in_s, next, sizeof(next));
    console_reply("advert every %s (flood), next %s%s; the Advertise button sends %s", iv, next[0] ? "in " : "",
                  next[0] ? next : "-",
                  c.flood_manual ? "flood" : "zero-hop");
}

void mesh_int::cmd_mesh_loc(char const *args) {
    mesh_advert_cfg_t c;
    double            lat, lon;
    char              a_lat[24], a_lon[24];
    if (strncmp(args, "share ", 6) == 0) {
        bool on;
        if (arg_onoff(args + 6, &on)) {
            reply_result("location sharing", mesh_set_location_share(on));
        } else {
            console_reply("mesh loc share on|off");
            return;
        }
    } else if (*args) {
        if (sscanf(args, "%23s %23s", a_lat, a_lon) == 2) {
            char const *why = advcfg_parse_coord(a_lat, true, &lat);
            if (!why) {
                why = advcfg_parse_coord(a_lon, false, &lon);
            }
            reply_result("location", why ? why : mesh_set_location(lat, lon));
        } else {
            console_reply("mesh loc | mesh loc <lat> <lon> (degrees, 52.3702 4.8952) | mesh loc share on|off");
            return;
        }
    }
    if (!mesh_get_advert_cfg(&c)) {
        return;
    }
    char pos[40];
    advcfg_coord_text(c.lat, c.lon, pos, sizeof(pos));
    console_reply("location %s, %s in adverts", advcfg_location_set(c.lat, c.lon) ? pos : "not set",
                  c.share_loc ? "shared" : "not shared");
}

void mesh_int::cmd_mesh_autoadd(char const *args) {
    mesh_advert_cfg_t c;
    if (!mesh_get_advert_cfg(&c)) {
        return;
    }
    advcfg_autoadd_t a = c.autoadd;
    bool             change = true, on;
    char const      *why = NULL;
    if (arg_onoff(args, &on)) {
        a.on = on;
        if (on && (a.types & ADVCFG_AA_TYPES) == 0) {
            a.types = ADVCFG_AA_TYPES;
        }
    } else if (strncmp(args, "types ", 6) == 0) {
        why = advcfg_parse_types(args + 6, &a.types);
        a.on = true;
    } else if (strncmp(args, "hops ", 5) == 0) {
        why = advcfg_parse_hops(args + 5, &a.max_hops);
    } else if (strncmp(args, "overwrite ", 10) == 0 && arg_onoff(args + 10, &on)) {
        a.overwrite = on;
    } else if (*args) {
        console_reply("mesh autoadd | on|off | types <all|chat,repeater,room,sensor> | hops <any|direct|n> | overwrite on|off");
        return;
    } else {
        change = false;
    }
    if (change) {
        reply_result("auto-add", why ? why : mesh_set_autoadd(&a));
        mesh_get_advert_cfg(&c);
    }
    char types[48], hops[20];
    advcfg_types_text(c.autoadd.types, types, sizeof(types));
    advcfg_hops_text(c.autoadd.max_hops, hops, sizeof(hops));
    console_reply("auto-add %s (%s), max hops %s, overwrite oldest when full: %s", c.autoadd.on ? "on" : "off", types,
                  hops, c.autoadd.overwrite ? "on" : "off");
}
