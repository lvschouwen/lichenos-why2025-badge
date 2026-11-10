// Host test: MeshCore's own companion (examples/companion_radio MyMesh + DataStore) on the badge's target, fed with
// captured packets (#65): contacts land in MeshCore's table, channel messages reach the app, and everything that
// should survive a reboot does.
#include "MyMesh.h"
#include "identity_move.h"
#include "mesh_prefs.h"
#include "mesh_ident.h"
#include "mesh_link.h"

#include "SPIFFS.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static unsigned long fake_millis = 1000;
extern "C" unsigned long millis(void) {
    return fake_millis;
}
extern "C" void delay(unsigned long ms) {
    fake_millis += ms;
}
extern "C" void shim_fill_random(void *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ((uint8_t *)buf)[i] = (uint8_t)rand();
    }
}

// The target's radio, fed one captured packet at a time
class TestRadio : public BadgeRadio {
public:
    std::vector<uint8_t> pending;
    std::vector<std::vector<uint8_t>> sent; // what MyMesh put on air
    int                  recvRaw(uint8_t *bytes, int sz) override {
        int n = (int)pending.size();
        if (!n || n > sz) {
            return 0;
        }
        memcpy(bytes, pending.data(), n);
        pending.clear();
        return n;
    }
    uint32_t getEstAirtimeFor(int len) override { return (uint32_t)len; }
    float    packetScore(float snr, int len) override { return 1.0f; }
    bool     startSendRaw(uint8_t const *bytes, int len) override {
        sent.emplace_back(bytes, bytes + len);
        return true;
    }
    bool     isSendComplete() override { return true; }
    void     onSendFinished() override {}
    bool     isInRecvMode() const override { return true; }
    float    getLastRSSI() const override { return -63.0f; }
    float    getLastSNR() const override { return 9.0f; }
    void     setParams(float, float, uint8_t, uint8_t) override {}
    void     setTxPower(int8_t) override {}
};
static TestRadio test_radio;

// What target.h leaves to the platform
BadgeRadio &radio_driver = test_radio;
BadgeBoard  board;
SensorManager sensors;
BadgeRtc   rtc_clock;
bool        radio_init() { return true; }
uint32_t    radio_get_rng_seed() { return 1; }
// What the settings told the radio last (#67)
static float   tuned_freq, tuned_bw;
static uint8_t tuned_sf, tuned_cr;
static int     tuned_tx = 1000;
void        radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
    tuned_freq = freq;
    tuned_bw   = bw;
    tuned_sf   = sf;
    tuned_cr   = cr;
}
void        radio_set_tx_power(uint8_t dbm) { tuned_tx = (int8_t)dbm; }
mesh::LocalIdentity radio_new_identity() {
    class Rng : public mesh::RNG {
        void random(uint8_t *d, size_t n) override { shim_fill_random(d, n); }
    } rng;
    return mesh::LocalIdentity(&rng);
}
void BadgeBoard::reboot() {}
uint16_t BadgeBoard::getBattMilliVolts() { return 0; }

// The outbox is shared with the UI on the badge (a critical section there); one thread here
extern "C" void mesh_ob_enter(void) {}
extern "C" void mesh_ob_exit(void) {}
extern "C" void mesh_advert_limited(uint32_t refused) {
    (void)refused;
}
extern "C" void mesh_contacts_full(void) {}
extern "C" void mesh_airlog_note(uint8_t, uint8_t, uint8_t const *, uint8_t, float, float) {}
extern "C" void mesh_signal_note(uint8_t const *, bool, uint8_t, uint8_t const *, float, float) {}

// The app's end of the companion protocol: frames to deliver, frames received
class FakeApp : public BaseSerialInterface {
public:
    std::vector<std::vector<uint8_t>> to_badge, from_badge;
    void   enable() override {}
    void   disable() override {}
    bool   isEnabled() const override { return true; }
    bool   isConnected() const override { return true; }
    bool   isWriteBusy() const override { return false; }
    size_t writeFrame(uint8_t const src[], size_t len) override {
        from_badge.emplace_back(src, src + len);
        return len;
    }
    size_t checkRecvFrame(uint8_t dest[]) override {
        if (to_badge.empty()) {
            return 0;
        }
        size_t n = to_badge.front().size();
        memcpy(dest, to_badge.front().data(), n);
        to_badge.erase(to_badge.begin());
        return n;
    }
};

static std::vector<std::vector<uint8_t>> load(char const *path) {
    std::vector<std::vector<uint8_t>> out;
    FILE                             *f = fopen(path, "r");
    char                              line[1200], kind[16], hex[1100];
    while (f && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || sscanf(line, "%15s %1099s", kind, hex) != 2) {
            continue;
        }
        std::vector<uint8_t> b;
        for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
            unsigned v;
            sscanf(hex + i, "%2x", &v);
            b.push_back((uint8_t)v);
        }
        out.push_back(b);
    }
    if (f) {
        fclose(f);
    }
    return out;
}

static void run(MyMesh &mesh, int loops) {
    for (int i = 0; i < loops; i++) {
        mesh.loop();
        fake_millis += 100;
    }
}

static bool has_text(std::vector<uint8_t> const &frame, char const *text) {
    std::string s(frame.begin(), frame.end());
    return s.find(text) != std::string::npos;
}

static uint8_t const *self_key(MyMesh &mesh) {
    return mesh.self_id.pub_key;
}

class Rng : public mesh::RNG {
public:
    void random(uint8_t *d, size_t n) override { shim_fill_random(d, n); }
};
static mesh::RNG &rng_for_pipe() {
    static Rng r;
    return r;
}

// MeshCore's clock: the system's real time when it has one (SNTP on the badge), never MeshCore's 2024 default (#65:
// other nodes drop adverts that aren't newer than the last one they saw)
static void clock_uses_real_time(void) {
    BadgeRtc rtc;
    uint32_t  now = (uint32_t)time(NULL); // the host's clock is set
    CHECK(rtc.getCurrentTime() >= now && rtc.getCurrentTime() <= now + 1);
}



// Messages seen by the Listener, as the badge's screens get them
struct CountingListener : MyMesh::Listener {
    int         channel_msgs = 0, dm_msgs = 0, queue_len = -1;
    std::string last_dm;
    uint8_t     last_type = 0xFF;
    void onMessageRecv(mesh::Packet *, ContactInfo const &, uint8_t type, uint32_t, char const *text) override {
        dm_msgs++;
        last_dm   = text;
        last_type = type;
    }
    void onChannelMessageRecv(mesh::Packet *, ChannelDetails &, char const *) override { channel_msgs++; }
    void onQueueSizeChanged(int n) override { queue_len = n; }
};

static void settle(BadgeMesh &mesh, int loops = 60) {
    for (int i = 0; i < loops; i++) {
        mesh.loop(); // BadgeMesh's own (#257), as the mesh task calls it
        fake_millis += 100;
    }
}

// The text of a GRP_TXT packet MyMesh put on air, decrypted with the channel's key
static std::string group_text(std::vector<uint8_t> const &raw, mesh::GroupChannel const &channel) {
    mesh::Packet pkt;
    if (!pkt.readFrom(raw.data(), (uint8_t)raw.size()) || pkt.getPayloadType() != PAYLOAD_TYPE_GRP_TXT) {
        return "";
    }
    uint8_t plain[200];
    int     n = mesh::Utils::MACThenDecrypt(channel.secret, plain, &pkt.payload[1], pkt.payload_len - 1);
    return n > 5 ? std::string((char *)&plain[5]) : "";
}

// The GRP_TXT packet on air since `from` whose text is want; false when none
static bool find_group(size_t from, mesh::GroupChannel const &channel, std::string const &want, mesh::Packet &out) {
    for (size_t i = from; i < test_radio.sent.size(); i++) {
        auto const &raw = test_radio.sent[i];
        if (group_text(raw, channel) == want) {
            return out.readFrom(raw.data(), (uint8_t)raw.size());
        }
    }
    return false;
}

// The console scope (#290) on the real MyMesh: a console line's channel message floods under the console region, a
// screen's under the default (None here: no transport code), and with the console scope unset the console one never
// goes on air at all. Made-up region name.
static void console_scope_checks(BadgeMesh &mesh, ob_t &ob, mesh::GroupChannel const &pub) {
    TransportKey      key;
    TransportKeyStore ks;
    ks.getAutoKeyFor(0, "#zz-test", key);
    CHECK(mesh.consoleScope()[0] == 0); // not set until someone sets it
    CHECK(mesh.setConsoleScope("ZZ-Test-Longer-Name") == NULL);
    CHECK(mesh.setConsoleScope(" #zz-test ") == NULL && strcmp(mesh.consoleScope(), "zz-test") == 0);
    CHECK(mesh.setConsoleScope("zz test") != NULL && strcmp(mesh.consoleScope(), "zz-test") == 0);
    mesh.loadConsoleScope(); // as at the next start: the saved name, nothing of the longer one before it
    CHECK(strcmp(mesh.consoleScope(), "zz-test") == 0);
    std::string  prefix = std::string(mesh.getNodeName()) + ": ";
    char const  *why    = NULL;
    size_t       before = test_radio.sent.size();
    mesh_msg_id_t ui    = ob_add_channel(&ob, "public", "scope ui", 8, (uint32_t)millis(), &why);
    settle(mesh, 30);
    mesh_msg_id_t con = ob_add_channel(&ob, "public", "scope console", 13, (uint32_t)millis(), &why);
    CHECK(ui && con && ob_set_console(&ob, con));
    settle(mesh, 30);
    mesh::Packet pkt;
    CHECK(find_group(before, pub, prefix + "scope ui", pkt) && pkt.getRouteType() == ROUTE_TYPE_FLOOD);
    CHECK(find_group(before, pub, prefix + "scope console", pkt) && pkt.hasTransportCodes() &&
          pkt.transport_codes[0] == key.calcTransportCode(&pkt));

    CHECK(mesh.setConsoleScope("") == NULL && mesh.consoleScope()[0] == 0);
    before            = test_radio.sent.size();
    uint32_t refused  = mesh.consoleRefused();
    con               = ob_add_channel(&ob, "public", "scope unset", 11, (uint32_t)millis(), &why);
    CHECK(con && ob_set_console(&ob, con));
    settle(mesh, 30);
    CHECK(!find_group(before, pub, prefix + "scope unset", pkt) && mesh.consoleRefused() == refused + 1);
    // a screen's message right after still floods (the console mark ends with the loop that handled its frame)
    ui = ob_add_channel(&ob, "public", "scope ui again", 14, (uint32_t)millis(), &why);
    settle(mesh, 30);
    CHECK(find_group(before, pub, prefix + "scope ui again", pkt) && pkt.getRouteType() == ROUTE_TYPE_FLOOD);
}

// The console scope lives in MeshCore's own directory (#290): the factory reset's format, which takes the default scope
// with prefs.json, takes it too; a damaged file loads as not set. Its own directory, so the other checks keep theirs.
static void console_scope_reset_checks(BadgeMesh &mesh, char const *root) {
    std::string sub = std::string(root) + "/cscope";
    fs_shim_set_root(sub.c_str());
    CHECK(SPIFFS.begin(true));
    CHECK(mesh.setConsoleScope("zz-test") == NULL);
    mesh.loadConsoleScope();
    CHECK(strcmp(mesh.consoleScope(), "zz-test") == 0);
    CHECK(SPIFFS.format());
    CHECK(SPIFFS.begin(true));
    mesh.loadConsoleScope();
    CHECK(mesh.consoleScope()[0] == 0);
    File f = SPIFFS.open("/console_scope", "w", true);
    CHECK(f && f.write((uint8_t const *)"zz test", 7) == 7);
    f.close();
    mesh.loadConsoleScope();
    CHECK(mesh.consoleScope()[0] == 0);
    f = SPIFFS.open("/console_scope", "w", true);
    CHECK(f && f.write((uint8_t const *)"zz-test\n", 8) == 8); // a line end typed by hand is fine
    f.close();
    mesh.loadConsoleScope();
    CHECK(strcmp(mesh.consoleScope(), "zz-test") == 0);
    CHECK(mesh.setConsoleScope("") == NULL);
    fs_shim_set_root(root);
}

// The details screen's actions (#49) on the real MyMesh: each one goes through MyMesh's own command, so it is saved
// with MyMesh's lazy contacts write, and a fresh MyMesh on the same files sees it
static void contact_action_checks(BadgeMesh &mesh) {
    ContactsIterator it = mesh.startContactsIterator();
    ContactInfo      a, b;
    CHECK(it.hasNext(&mesh, a) && it.hasNext(&mesh, b));
    int before = mesh.getNumContacts();

    // Reset path: a known path (one hop, 2-byte hashes) is gone at once, and MyMesh answers OK
    ContactInfo *pa   = mesh.lookupContactByPubKey(a.id.pub_key, PUB_KEY_SIZE);
    pa->out_path_len  = 0x41;
    pa->out_path[0]   = 0x12;
    uint32_t lastmod  = pa->lastmod;
    LinkStats const &st = mesh.pipe().stats();
    uint32_t oks      = st.reply_count[0];
    CHECK(mesh.contactAction(a.id.pub_key, MESH_CO_RESET_PATH) == BadgeMesh::CONTACT_QUEUED);
    CHECK(pa->out_path_len == OUT_PATH_UNKNOWN);
    settle(mesh, 3);
    CHECK(st.reply_count[0] == oks + 1);

    // Favourite on: the flag at once, lastmod kept (the order by "last heard" does not jump), the path not brought back
    CHECK(mesh.contactAction(a.id.pub_key, MESH_CO_FAV_ON) == BadgeMesh::CONTACT_QUEUED);
    CHECK(pa->flags & MESH_CONTACT_FAV);
    settle(mesh, 3);
    pa = mesh.lookupContactByPubKey(a.id.pub_key, PUB_KEY_SIZE);
    CHECK(pa && (pa->flags & MESH_CONTACT_FAV) && pa->lastmod == lastmod && pa->out_path_len == OUT_PATH_UNKNOWN);
    CHECK(st.reply_count[0] == oks + 2);

    // Remove: gone after MyMesh's loop; a second try finds nothing
    CHECK(mesh.contactAction(b.id.pub_key, MESH_CO_REMOVE) == BadgeMesh::CONTACT_QUEUED);
    settle(mesh, 3);
    CHECK(mesh.getNumContacts() == before - 1 && !mesh.lookupContactByPubKey(b.id.pub_key, PUB_KEY_SIZE));
    CHECK(mesh.contactAction(b.id.pub_key, MESH_CO_REMOVE) == BadgeMesh::CONTACT_GONE);

    // MyMesh's own lazy write (5 s after the last change), then a fresh node on the same files
    settle(mesh, 80);
    static DataStore        store(SPIFFS, rtc_clock);
    static SimpleMeshTables tables;
    static MyMesh           fresh(radio_driver, rng_for_pipe(), rtc_clock, tables, store);
    store.begin();
    fresh.begin();
    ContactInfo *fa = fresh.lookupContactByPubKey(a.id.pub_key, PUB_KEY_SIZE);
    CHECK(fresh.getNumContacts() == before - 1 && !fresh.lookupContactByPubKey(b.id.pub_key, PUB_KEY_SIZE));
    CHECK(fa && (fa->flags & MESH_CONTACT_FAV) && fa->out_path_len == OUT_PATH_UNKNOWN);

    // Favourite off: the other bits of flags stay
    pa        = mesh.lookupContactByPubKey(a.id.pub_key, PUB_KEY_SIZE);
    pa->flags = MESH_CONTACT_FAV | 0x02;
    CHECK(mesh.contactAction(a.id.pub_key, MESH_CO_FAV_OFF) == BadgeMesh::CONTACT_QUEUED);
    settle(mesh, 3);
    CHECK(pa->flags == 0x02);

    // A full command queue must not drop a favourite toggle (#112): it waits in the overflow and is applied after the
    // queue drains; only when that is full too is the change refused, and then the flag stays as it was
    uint8_t sync[1];
    mf_encode_sync_next(sync);
    pa->flags = 0;
    for (int i = 0; i < LINK_FIFO; i++) {
        CHECK(mesh.pipe().queue_cmd(sync, 1));
    }
    CHECK(mesh.pipe().can_queue()); // the overflow still has room
    CHECK(mesh.contactAction(a.id.pub_key, MESH_CO_FAV_ON) == BadgeMesh::CONTACT_QUEUED);
    CHECK(pa->flags & MESH_CONTACT_FAV);
    for (int i = 1; i < LINK_OVF; i++) {
        CHECK(mesh.pipe().queue_cmd(sync, 1)); // fills the rest of the overflow
    }
    pa->flags = 0;
    CHECK(mesh.contactAction(a.id.pub_key, MESH_CO_FAV_ON) == BadgeMesh::CONTACT_QUEUE_FULL);
    CHECK(!(pa->flags & MESH_CONTACT_FAV));
    settle(mesh, 40);
    CHECK(mesh.pipe().can_queue());
    pa->flags = MESH_CONTACT_FAV;
    CHECK(mesh.contactAction(a.id.pub_key, MESH_CO_FAV_OFF) == BadgeMesh::CONTACT_QUEUED);
    CHECK(!(pa->flags & MESH_CONTACT_FAV));

    // Order (review of #112): while commands wait in the overflow, a later one does not jump ahead through the FIFO.
    // ON waits in the overflow, one loop frees a FIFO place, OFF must queue behind ON: the contact ends up off.
    settle(mesh, 40);
    pa->flags = 0;
    for (int i = 0; i < LINK_FIFO; i++) {
        CHECK(mesh.pipe().queue_cmd(sync, 1));
    }
    CHECK(mesh.contactAction(a.id.pub_key, MESH_CO_FAV_ON) == BadgeMesh::CONTACT_QUEUED);
    settle(mesh, 1); // takes one frame: the FIFO has room, the overflow still holds ON
    CHECK(mesh.contactAction(a.id.pub_key, MESH_CO_FAV_OFF) == BadgeMesh::CONTACT_QUEUED);
    settle(mesh, 40);
    pa = mesh.lookupContactByPubKey(a.id.pub_key, PUB_KEY_SIZE);
    CHECK(pa && !(pa->flags & MESH_CONTACT_FAV));
}

// Trace path and path discovery (#53) through the pipe on the real MyMesh: the trace goes out along the contact's
// path, comes back with an SNR per hop; a discovery's answer becomes the stored path through MyMesh's own command
static void trace_checks(BadgeMesh &mesh, mt_t &mt) {
    ContactsIterator it = mesh.startContactsIterator();
    ContactInfo      c;
    CHECK(it.hasNext(&mesh, c));
    ContactInfo *pc   = mesh.lookupContactByPubKey(c.id.pub_key, PUB_KEY_SIZE);
    uint8_t      type = pc->type;
    pc->type          = ADV_TYPE_REPEATER;
    pc->out_path_len  = 1;
    pc->out_path[0]   = 0xA1;
    char const *why   = NULL;
    CHECK(mt_request(&mt, MT_TRACE, pc->id.pub_key, 0x12345678, (uint32_t)millis(), &why) != 0);
    size_t before = test_radio.sent.size();
    settle(mesh, 10);
    CHECK(mt.state == MT_SENT && mt.on_air_ms != 0 && mt.target == 1);
    mesh::Packet pkt;
    bool         found = false;
    for (size_t i = before; i < test_radio.sent.size() && !found; i++) {
        auto const &raw = test_radio.sent[i];
        found = pkt.readFrom(raw.data(), (uint8_t)raw.size()) && pkt.getPayloadType() == PAYLOAD_TYPE_TRACE;
    }
    CHECK(found);
    uint32_t tag = 0;
    memcpy(&tag, pkt.payload, 4);
    uint8_t const want[] = {0xA1, pc->id.pub_key[0], 0xA1};
    CHECK(tag == 0x12345678 && pkt.payload[8] == 0 && pkt.payload_len == 12 && memcmp(pkt.payload + 9, want, 3) == 0);

    // It comes back from A1 with the three SNRs the hops added
    pkt.path_len = 3;
    pkt.path[0]  = 28;
    pkt.path[1]  = (uint8_t)-8;
    pkt.path[2]  = 20;
    uint8_t back[256];
    uint8_t n    = pkt.writeTo(back);
    test_radio.pending.assign(back, back + n);
    settle(mesh, 5);
    CHECK(mt.state == MT_DONE && mt.trace.hops == 3 && mt.trace.snr_q[0] == 28 && mt.trace.snr_q[1] == -8);
    CHECK(mt.trace.final_snr_q == 36); // TestRadio's SNR 9 dB, in quarters
    CHECK(mesh.pipe().stats().traces == 1);

    // A companion with no path: refused before anything goes on air
    pc->type         = ADV_TYPE_CHAT;
    pc->out_path_len = OUT_PATH_UNKNOWN;
    before           = test_radio.sent.size();
    CHECK(mt_request(&mt, MT_TRACE, pc->id.pub_key, 1, (uint32_t)millis(), &why) != 0);
    settle(mesh, 3);
    CHECK(mt.state == MT_FAILED && test_radio.sent.size() == before);

    // Discovery: MyMesh floods its request, the answer (pushed as MyMesh does) is stored as the path
    CHECK(mt_request(&mt, MT_DISCOVER, pc->id.pub_key, 0, (uint32_t)millis(), &why) != 0);
    settle(mesh, 10);
    CHECK(mt.state == MT_SENT && mt.flood == 1 && test_radio.sent.size() > before);
    uint8_t push[14] = {0x8D, 0};
    memcpy(push + 2, pc->id.pub_key, 6);
    push[8]  = 0x41; // one hop, 2-byte hash
    push[9]  = 0xB1;
    push[10] = 0xB2;
    push[11] = 0x02; // came back over two 1-byte hops
    push[12] = 0xC1;
    push[13] = 0xC2;
    uint32_t oks = mesh.pipe().stats().reply_count[0];
    mesh.pipe().writeFrame(push, sizeof(push));
    CHECK(mt.state == MT_DONE && pc->out_path_len == 0x41 && pc->out_path[1] == 0xB2);
    settle(mesh, 3);
    pc = mesh.lookupContactByPubKey(c.id.pub_key, PUB_KEY_SIZE);
    CHECK(pc && pc->out_path_len == 0x41 && pc->out_path[0] == 0xB1 && pc->out_path[1] == 0xB2);
    CHECK(mesh.pipe().stats().reply_count[0] == oks + 1 && mesh.pipe().stats().paths_stored == 1);
    pc->type = type;
}

// A packet from a peer to us, as MeshCore sends it: [header][path_len 0][our hash][its hash][MAC + encrypted plain]
static std::vector<uint8_t> peer_packet(BadgeMesh &mesh, mesh::LocalIdentity &peer, uint8_t type,
                                        std::vector<uint8_t> const &plain) {
    uint8_t secret[PUB_KEY_SIZE];
    peer.calcSharedSecret(secret, mesh.self_id.pub_key);
    uint8_t raw[200];
    raw[0]  = (uint8_t)((type << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD);
    raw[1]  = 0;
    raw[2]  = mesh.self_id.pub_key[0];
    raw[3]  = peer.pub_key[0];
    int enc = mesh::Utils::encryptThenMAC(secret, raw + 4, plain.data(), (int)plain.size());
    return std::vector<uint8_t>(raw, raw + 4 + enc);
}

static std::vector<uint8_t> with_tag(uint32_t tag, std::vector<uint8_t> rest) {
    uint8_t bytes[4];
    memcpy(bytes, &tag, sizeof(bytes));
    // cppcheck-suppress returnDanglingLifetime  # the vector copies the 4 bytes, it keeps no pointer to them
    std::vector<uint8_t> v(bytes, bytes + sizeof(bytes));
    v.insert(v.end(), rest.begin(), rest.end());
    return v;
}

// The datagram we put on air to the peer (TXT_MSG), decrypted: its text type and text, or false when there is none
static bool datagram_to(BadgeMesh &mesh, mesh::LocalIdentity &peer, size_t from, uint8_t want_type, uint8_t *type,
                        std::string *text) {
    uint8_t secret[PUB_KEY_SIZE];
    peer.calcSharedSecret(secret, mesh.self_id.pub_key);
    for (size_t i = from; i < test_radio.sent.size(); i++) {
        mesh::Packet pkt;
        auto const  &raw = test_radio.sent[i];
        if (!pkt.readFrom(raw.data(), (uint8_t)raw.size()) || pkt.getPayloadType() != want_type || pkt.payload_len < 4 ||
            pkt.payload[0] != peer.pub_key[0]) {
            continue;
        }
        uint8_t plain[200];
        int     n = mesh::Utils::MACThenDecrypt(secret, plain, &pkt.payload[2], pkt.payload_len - 2);
        if (n > 5) {
            *type = plain[4] >> 2;
            *text = std::string((char *)&plain[5], strnlen((char *)&plain[5], (size_t)n - 5)); // a full block has no NUL
            return true;
        }
    }
    return false;
}

static bool sent_has_type(size_t from, uint8_t want_type) {
    for (size_t i = from; i < test_radio.sent.size(); i++) {
        mesh::Packet pkt;
        auto const  &raw = test_radio.sent[i];
        if (pkt.readFrom(raw.data(), (uint8_t)raw.size()) && pkt.getPayloadType() == want_type) {
            return true;
        }
    }
    return false;
}

// Log in, status, CLI and a room's posts (#51) through the pipe on the real MyMesh, against a peer that answers as
// a repeater does (its replies built as simple_repeater builds them)
static void remote_checks(BadgeMesh &mesh, rr_t &rr, CountingListener &listener) {
    static Rng          peer_rng;
    mesh::LocalIdentity peer(&peer_rng);
    ContactInfo         ci{}; // value-initialised: Identity has a constructor, so no memset (-Wclass-memaccess)
    ci.id            = mesh::Identity(peer.pub_key);
    ci.type          = ADV_TYPE_REPEATER;
    ci.out_path_len  = OUT_PATH_UNKNOWN;
    ci.lastmod       = rtc_clock.getCurrentTime();
    strcpy(ci.name, "Rpt");
    CHECK(mesh.addContact(ci));
    char const *why = NULL;
    uint32_t    now = (uint32_t)time(NULL) + 5000;

    // Log in: the password leaves the request, an ANON_REQ goes on air, the repeater's answer logs us in
    size_t before = test_radio.sent.size();
    CHECK(rr_request(&rr, RR_LOGIN, peer.pub_key, "hunter2", (uint32_t)millis(), &why) != 0);
    settle(mesh, 10);
    CHECK(rr.state == RR_SENT && rr.arg[0] == 0 && sent_has_type(before, PAYLOAD_TYPE_ANON_REQ));
    CHECK(mesh.pipe().stats().reply_count[MF_RESP_SENT] >= 1);
    test_radio.pending = peer_packet(mesh, peer, PAYLOAD_TYPE_RESPONSE,
                                     with_tag(now++, {0 /* LOGIN_OK */, 0, 1 /* admin */, 3, 1, 2, 3, 4, 8}));
    settle(mesh, 5);
    CHECK(rr.state == RR_DONE && rr.logged_in && rr.admin && rr.acl == 3 && rr.fw_level == 8);
    CHECK(rr_logged_in(&rr, peer.pub_key) && mesh.pipe().stats().remote_answers == 1);

    // Status: the repeater's RepeaterStats come back as the fields
    before = test_radio.sent.size();
    CHECK(rr_request(&rr, RR_STATUS, peer.pub_key, NULL, (uint32_t)millis(), &why) != 0);
    settle(mesh, 10);
    CHECK(rr.state == RR_SENT);
    std::vector<uint8_t> stats(56, 0);
    stats[0] = 0x10; stats[1] = 0x10; // 4112 mV
    stats[2] = 2;                     // queue
    stats[4] = 0x91; stats[5] = 0xFF; // noise -111
    stats[20] = 0x2C; stats[21] = 0x01; // up 300 s
    test_radio.pending = peer_packet(mesh, peer, PAYLOAD_TYPE_RESPONSE, with_tag(now++, stats));
    settle(mesh, 5);
    CHECK(rr.state == RR_DONE && rr.have_status && rr.status.bytes == 56 && rr.status.batt_mv == 4112 &&
          rr.status.tx_queue == 2 && rr.status.noise_floor == -111 && rr.status.up_secs == 300);

    // CLI: the command goes out as a CLI_DATA datagram; the reply comes back as a message and is the CLI's, not a chat's
    before = test_radio.sent.size();
    int dms = listener.dm_msgs;
    CHECK(rr_request(&rr, RR_CLI, peer.pub_key, "get name", (uint32_t)millis(), &why) != 0);
    settle(mesh, 10);
    uint8_t     type = 0xFF;
    std::string text;
    CHECK(rr.state == RR_SENT && datagram_to(mesh, peer, before, PAYLOAD_TYPE_TXT_MSG, &type, &text));
    // the badge's tag in front (#123): the repeater reflects it, a reply with another one is not the answer
    CHECK(type == MF_TXT_CLI_DATA && text.size() == 11 && text[2] == '|' && text.substr(3) == "get name");
    std::string tag = text.substr(0, 3);
    std::vector<uint8_t> stray = with_tag(now++, {(uint8_t)(MF_TXT_CLI_DATA << 2)});
    for (char c : std::string("ZZ|late reply")) stray.push_back((uint8_t)c);
    stray.push_back(0);
    test_radio.pending = peer_packet(mesh, peer, PAYLOAD_TYPE_TXT_MSG, stray);
    settle(mesh, 15);
    CHECK(rr.state == RR_SENT && mesh.pipe().stats().cli_replies == 0);
    std::vector<uint8_t> reply = with_tag(now++, {(uint8_t)(MF_TXT_CLI_DATA << 2)});
    for (char c : tag + "> Rpt") reply.push_back((uint8_t)c);
    reply.push_back(0);
    test_radio.pending = peer_packet(mesh, peer, PAYLOAD_TYPE_TXT_MSG, reply);
    settle(mesh, 20);
    CHECK(rr.state == RR_DONE && rr.cli_n == 1 && strcmp(rr.cli[rr.cli_head].reply, "> Rpt") == 0);
    CHECK(mesh.pipe().stats().cli_replies == 1 && listener.dm_msgs == dms + 2 && listener.last_type == MF_TXT_CLI_DATA);

    // A room's post (signed text): the screens see "<author>: <text>"
    std::vector<uint8_t> post = with_tag(now++, {(uint8_t)(MF_TXT_SIGNED_PLAIN << 2)});
    post.insert(post.end(), peer.pub_key, peer.pub_key + 4); // the author is the repeater's key, a known contact
    for (char c : std::string("good morning")) post.push_back((uint8_t)c);
    post.push_back(0);
    test_radio.pending = peer_packet(mesh, peer, PAYLOAD_TYPE_TXT_MSG, post);
    settle(mesh, 20);
    CHECK(listener.last_type == MF_TXT_SIGNED_PLAIN && listener.last_dm == "Rpt: good morning");

    // Log out: MyMesh answers its OK, nothing goes on air
    before = test_radio.sent.size();
    uint32_t oks = mesh.pipe().stats().reply_count[0];
    CHECK(mesh.remoteLogout(peer.pub_key));
    rr_logout(&rr, peer.pub_key);
    settle(mesh, 5);
    CHECK(!rr_logged_in(&rr, peer.pub_key) && mesh.pipe().stats().reply_count[0] == oks + 1);
    CHECK(test_radio.sent.size() == before);
    uint8_t gone[6] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    CHECK(!mesh.remoteLogout(gone));

    // A contact that is not there: refused by the pipe, not sent
    CHECK(rr_request(&rr, RR_STATUS, gone, NULL, (uint32_t)millis(), &why) != 0);
    settle(mesh, 3);
    CHECK(rr.state == RR_FAILED && strstr(rr.why, "not found"));

    // #123: the frame with a password is wiped from the buffer MyMesh was handed when the pipe is asked for the next one
    // (MyMesh has handled it by then), not left in its command buffer
    CHECK(rr_request(&rr, RR_LOGIN, peer.pub_key, "hunter3", (uint32_t)millis(), &why) != 0);
    uint8_t frame[MF_MAX_FRAME + 1], other[MF_MAX_FRAME + 1];
    size_t  n = mesh.pipe().checkRecvFrame(frame);
    CHECK(n > 7 && memmem(frame, n, "hunter3", 7) != NULL);
    mesh.pipe().checkRecvFrame(other);
    bool zero = true;
    for (size_t i = 0; i < n; i++) {
        zero = zero && frame[i] == 0;
    }
    CHECK(zero);
    settle(mesh, 3);
}

// Packets left in MyMesh's pool (16): taken until none is left, then all given back
static int free_packets(BadgeMesh &mesh) {
    std::vector<mesh::Packet *> got;
    for (mesh::Packet *p; (p = mesh.obtainNewPacket()) != NULL;) {
        got.push_back(p);
    }
    for (mesh::Packet *p : got) {
        mesh.releasePacket(p);
    }
    return (int)got.size();
}

// #110: a second importContact before MyMesh's loop ran replaced the pending loop-back packet without releasing it,
// one packet of the pool lost for good per overwrite
static void import_checks(BadgeMesh &mesh) {
    uint8_t       raw[MAX_TRANS_UNIT];
    mesh::Packet *advert = mesh.createSelfAdvert(mesh.getNodeName());
    CHECK(advert != NULL);
    if (!advert) {
        return;
    }
    int len = advert->writeTo(raw);
    mesh.releasePacket(advert);
    settle(mesh, 3);
    int before = free_packets(mesh);
    CHECK(before > 2);
    CHECK(mesh.importContact(raw, (uint8_t)len));
    CHECK(mesh.importContact(raw, (uint8_t)len)); // before loop() took the first
    settle(mesh, 3);
    CHECK(free_packets(mesh) == before);
}

// Files below dir/bl (MeshCore's advert blobs)
static int blob_files(char const *dir) {
    char path[300];
    snprintf(path, sizeof(path), "%s/bl", dir);
    DIR *d = opendir(path);
    int  n = 0;
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
        n += e->d_name[0] != '.';
    }
    if (d) {
        closedir(d);
    }
    return n;
}

// Removes the advert's node through MyMesh's own command (it deletes the blob file), then brings it back as new
static void remove_node(BadgeMesh &mesh, uint8_t const *key) {
    CHECK(mesh.contactAction(key, MESH_CO_REMOVE) == BadgeMesh::CONTACT_QUEUED);
    settle(mesh, 5);
    CHECK(mesh.getNumContacts() == 0);
}

static void readd_node(BadgeMesh &mesh, std::vector<uint8_t> const &advert) {
    CHECK(mesh.importContact(advert.data(), (uint8_t)advert.size()));
    settle(mesh, 5);
    CHECK(mesh.getNumContacts() == 1 && mesh.blobsQueued() == 1);
}

// #257 review: a removed contact's queued blob is never written, also when it was taken just before the removal; a
// stop writes what is queued
static void blob_lifecycle_checks(BadgeMesh &mesh, std::vector<uint8_t> const &advert, char const *root) {
    ContactsIterator it = mesh.startContactsIterator();
    ContactInfo      c;
    CHECK(it.hasNext(&mesh, c));
    remove_node(mesh, c.id.pub_key);
    CHECK(blob_files(root) == 0); // MyMesh deleted the file

    readd_node(mesh, advert); // queued, then removed before its write
    remove_node(mesh, c.id.pub_key);
    mesh.takeBlob(true);
    mesh.writeTakenBlob();
    CHECK(blob_files(root) == 0 && mesh.blobsQueued() == 0);

    readd_node(mesh, advert); // taken, then removed by MyMesh's loop before the write after the unlock
    mesh.takeBlob(true);
    remove_node(mesh, c.id.pub_key);
    mesh.writeTakenBlob();
    CHECK(blob_files(root) == 0);
    mesh.takeBlob(true);
    CHECK(mesh.blobsQueued() == 0);

    readd_node(mesh, advert); // a stop: written at once, the queue empty
    mesh.flushBlobs();
    CHECK(blob_files(root) == 1 && mesh.blobsQueued() == 0);
    mesh.takeBlob(true);
    mesh.writeTakenBlob();
    CHECK(blob_files(root) == 1);

    remove_node(mesh, c.id.pub_key); // a factory reset: nothing queued comes back
    readd_node(mesh, advert);
    mesh.dropBlobs();
    mesh.takeBlob(true);
    mesh.writeTakenBlob();
    CHECK(blob_files(root) == 0 && mesh.blobsQueued() == 0);
}

// #257: an advert's blob waits in RAM and is written after the loop, as the mesh task does after the unlock; until then
// "share" reads the queued copy
static void blob_checks(std::vector<std::vector<uint8_t>> const &packets, char const *old_root) {
    char root[] = "/tmp/companion_blobs.XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    fs_shim_set_root(root);
    CHECK(SPIFFS.begin(true));
    static SimpleMeshTables tables;
    static DataStore        store(SPIFFS, rtc_clock);
    static ob_t             ob;
    static mt_t             mt;
    static rr_t             rr;
    static BadgeMesh        mesh(radio_driver, rng_for_pipe(), rtc_clock, tables, store, &ob);
    ob_init(&ob);
    mt_init(&mt);
    rr_init(&rr);
    mesh.attachTrace(&mt);
    mesh.attachRemote(&rr);
    store.begin();
    mesh.begin();
    mesh.startPipe();
    settle(mesh, 10);
    test_radio.pending = packets[0]; // an advert of a new node
    settle(mesh, 10);
    CHECK(mesh.getNumContacts() == 1);
    CHECK(mesh.blobsQueued() == 1);
    CHECK(blob_files(root) == 0); // nothing written inside the loop
    ContactInfo c;
    uint8_t     out[MAX_TRANS_UNIT];
    ContactsIterator it = mesh.startContactsIterator();
    CHECK(it.hasNext(&mesh, c) && mesh.exportContact(c, out) > 0); // the queued copy
    mesh.takeBlob(false);                                               // a packet came in: not yet
    mesh.writeTakenBlob();
    CHECK(blob_files(root) == 0);
    mesh.takeBlob(true); // quiet: written after the "unlock", taken off at the next take
    mesh.writeTakenBlob();
    CHECK(blob_files(root) == 1 && mesh.blobsQueued() == 1);
    mesh.takeBlob(true);
    CHECK(mesh.blobsQueued() == 0 && mesh.exportContact(c, out) > 0); // now from the file
    blob_lifecycle_checks(mesh, packets[0], root);
    fs_shim_set_root(old_root);
}

// The companion pipe (#45, #55) on the real MyMesh: start sequence, offline queue drained, sends through the outbox
static void pipe_checks(std::vector<std::vector<uint8_t>> const &packets, SimpleMeshTables &tables, char const *root) {
    static DataStore store(SPIFFS, rtc_clock);
    static ob_t      ob;
    static BadgeMesh mesh(radio_driver, rng_for_pipe(), rtc_clock, tables, store, &ob);
    static CountingListener listener;
    static mt_t             mt;
    static rr_t             rr;
    ob_init(&ob);
    mt_init(&mt);
    rr_init(&rr);
    mesh.attachTrace(&mt);
    mesh.attachRemote(&rr);
    store.begin();
    mesh.begin();
    mesh.setListener(&listener);
    mesh.startPipe();
    settle(mesh, 10);
    LinkStats const &st = mesh.pipe().stats();
    CHECK(st.app_ver != 0);   // DEVICE_INFO answered
    CHECK(st.self_ok == 1 && st.self_bad == 0); // APP_START: SELF_INFO carries our key
    CHECK(st.no_more >= 1 && !mesh.pipe().draining());

    // A Public message arrives: the Listener gets it once, the pipe drains the offline queue and drops the frame (#55)
    test_radio.pending = packets[1];
    settle(mesh, 20);
    CHECK(listener.channel_msgs == 1);
    CHECK(st.msg_waiting >= 1 && st.drained >= 1 && listener.queue_len == 0 && !mesh.pipe().draining());
    ChannelDetails pub;
    CHECK(mesh.getChannel(0, pub));
    for (int i = 0; i < 300; i++) { // more than the queue holds (256): it never fills up
        uint8_t plain[48] = {};
        uint32_t ts       = (uint32_t)time(NULL) + 1000 + i;
        memcpy(plain, &ts, 4);
        int tlen = snprintf((char *)plain + 5, sizeof(plain) - 5, "Peer: message %d", i);
        uint8_t raw[128];
        raw[0]    = (PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
        raw[1]    = 0;
        raw[2]    = pub.channel.hash[0];
        int enc   = mesh::Utils::encryptThenMAC(pub.channel.secret, raw + 3, plain, 5 + tlen + 1);
        test_radio.pending.assign(raw, raw + 3 + enc);
        settle(mesh, 2);
    }
    CHECK(listener.channel_msgs == 301);
    CHECK(st.drained >= 301);
    CHECK(listener.queue_len == 0);

    // A channel send: the frame's text decrypts to "<name>: <text>", then ON_AIR, then HEARD when it is repeated
    char const *why = NULL;
    mesh_msg_id_t id = ob_add_channel(&ob, "public", "hello pipe", 10, (uint32_t)millis(), &why);
    CHECK(id != 0);
    size_t before = test_radio.sent.size();
    settle(mesh, 60);
    mesh_out_state_t state = MESH_OUT_QUEUED;
    CHECK(ob_get(&ob, id, &state, NULL, NULL, NULL) && state == MESH_OUT_ON_AIR);
    CHECK(test_radio.sent.size() > before);
    std::string want = std::string(mesh.getNodeName()) + ": hello pipe";
    bool        found = false;
    std::vector<uint8_t> on_air;
    for (auto const &raw : test_radio.sent) {
        if (group_text(raw, pub.channel) == want) {
            found  = true;
            on_air = raw;
        }
    }
    CHECK(found);
    test_radio.pending = on_air; // a repeater sends it back
    settle(mesh, 5);
    uint8_t reps = 0;
    CHECK(ob_get(&ob, id, &state, NULL, &reps, NULL) && state == MESH_OUT_HEARD && reps == 1);

    mesh_msg_id_t bad = ob_add_channel(&ob, "#nosuch", "x", 1, (uint32_t)millis(), &why);
    settle(mesh, 5);
    CHECK(ob_get(&ob, bad, &state, NULL, NULL, NULL) && state == MESH_OUT_FAILED);

    // A DM to a known contact: RESP_SENT with an ACK, then the ACK packet confirms it
    ContactsIterator it = mesh.startContactsIterator();
    ContactInfo      c;
    CHECK(it.hasNext(&mesh, c));
    mesh_msg_id_t dm = ob_add_dm(&ob, c.id.pub_key, "dm hi", 5, (uint32_t)millis(), &why);
    CHECK(dm != 0);
    settle(mesh, 20);
    CHECK(ob_get(&ob, dm, &state, NULL, NULL, NULL) && (state == MESH_OUT_SENT || state == MESH_OUT_ON_AIR));
    uint32_t ack = 0;
    for (auto const &e : ob.e) {
        if (e.used && e.id == dm && e.n_acks) {
            ack = e.acks[0];
        }
    }
    CHECK(ack != 0);
    uint8_t ack_pkt[6] = {(PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD, 0, 0, 0, 0, 0};
    memcpy(ack_pkt + 2, &ack, 4);
    test_radio.pending.assign(ack_pkt, ack_pkt + 6);
    settle(mesh, 5);
    uint32_t trip = 0;
    CHECK(ob_get(&ob, dm, &state, NULL, NULL, &trip) && state == MESH_OUT_DELIVERED);
    CHECK(st.confirmed == 1);
    console_scope_checks(mesh, ob, pub.channel);
    console_scope_reset_checks(mesh, root);
    trace_checks(mesh, mt);
    remote_checks(mesh, rr, listener);
    contact_action_checks(mesh);
    import_checks(mesh);
}

// Moving the milestone-2 key into DataStore (#65 review): only a saved copy that reads back identical counts, because
// MeshCore's save reports success once the file opened, even when the write itself failed (disk full, bad FAT)
struct FakeIdStore {
    bool                saved_ok = true;
    bool                corrupt  = false;
    mesh::LocalIdentity kept;
    bool saveMainIdentity(mesh::LocalIdentity const &id) {
        kept = id;
        if (corrupt) {
            uint8_t raw[PRV_KEY_SIZE + PUB_KEY_SIZE];
            size_t  n = kept.writeTo(raw, sizeof(raw));
            raw[0] ^= 0xFF;
            kept.readFrom(raw, n);
        }
        return saved_ok;
    }
    bool loadMainIdentity(mesh::LocalIdentity &id) {
        id = kept;
        return true;
    }
};

static void identity_move_checks_the_saved_copy(mesh::RNG &rng) {
    mesh::LocalIdentity id(&rng);
    FakeIdStore         good;
    CHECK(identity_move(good, id));
    FakeIdStore refused;
    refused.saved_ok = false;
    CHECK(!identity_move(refused, id));
    FakeIdStore short_write; // "saved", but what is on disk differs
    short_write.corrupt = true;
    CHECK(!identity_move(short_write, id));
}

static std::vector<uint8_t> file_bytes(std::string const &path) {
    std::vector<uint8_t> out;
    FILE                *f = fopen(path.c_str(), "rb");
    if (f) {
        int c;
        while ((c = fgetc(f)) != EOF) {
            out.push_back((uint8_t)c);
        }
        fclose(f);
    }
    return out;
}

static void put_bytes(std::string const &path, std::vector<uint8_t> const &bytes) {
    FILE *f = fopen(path.c_str(), "wb");
    CHECK(f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
    if (f) {
        fclose(f);
    }
}

static bool same_key(mesh::LocalIdentity a, mesh::LocalIdentity b) {
    uint8_t x[PRV_KEY_SIZE + PUB_KEY_SIZE], y[sizeof(x)];
    return a.writeTo(x, sizeof(x)) == sizeof(x) && b.writeTo(y, sizeof(y)) == sizeof(y) && memcmp(x, y, sizeof(x)) == 0;
}

// #110: the identity is saved atomically (written to "<name>.id.new", read back, renamed over "<name>.id"), every
// point a power cut can stop it at leaves an identity that loads or none at all, and a zeroed one is never saved
static void identity_store_checks(char const *root, mesh::RNG &rng) {
    std::string   dir = std::string(root) + "/idtest", path = dir + "/_main.id", tmp = path + ".new";
    IdentityStore st(SPIFFS, "/idtest");
    st.begin();
    mesh::LocalIdentity a(&rng), b(&rng), got;
    for (int fat = 0; fat < 2; fat++) { // POSIX rename, then FatFs's (no rename over an existing file)
        fs_shim_test_fat_rename(fat == 1);
        unlink(path.c_str());
        CHECK(!st.exists("_main") && !st.load("_main", got));
        CHECK(st.save("_main", a) && st.load("_main", got) && same_key(got, a));
        CHECK(st.save("_main", b) && st.load("_main", got) && same_key(got, b)); // over an existing one
        CHECK(access(tmp.c_str(), F_OK) != 0);                                  // nothing left behind
    }
    fs_shim_test_fat_rename(false);
    std::vector<uint8_t> good = file_bytes(path), half(good.begin(), good.begin() + 40);

    // Cut while writing the new copy: the old file is what loads, the half ".new" is ignored
    put_bytes(tmp, half);
    CHECK(st.exists("_main") && st.load("_main", got) && same_key(got, b));
    // Cut after FatFs's remove, before the rename: the verified ".new" loads and is put in place
    unlink(path.c_str());
    put_bytes(tmp, good);
    CHECK(st.exists("_main") && st.load("_main", got) && same_key(got, b));
    CHECK(file_bytes(path) == good && access(tmp.c_str(), F_OK) != 0);
    // The first save ever cut short: no identity at all (a new one may be made), not an unreadable one
    unlink(path.c_str());
    put_bytes(tmp, half);
    CHECK(!st.exists("_main") && !st.load("_main", got));
    // A damaged "<name>.id" is never replaced by a ".new" on load: it stays unreadable, as #109 wants
    put_bytes(path, half);
    put_bytes(tmp, good);
    CHECK(st.exists("_main") && !st.load("_main", got) && file_bytes(path) == half);

    // What MyMesh holds for an unreadable file (all zeroes) is never saved, and the file stays as it was
    CHECK(!st.save("_main", mesh::LocalIdentity()));
    CHECK(file_bytes(path) == half);
    unlink(path.c_str());
    unlink(tmp.c_str());
    rmdir(dir.c_str());
}

// #109: only a missing identity file makes a new identity. One that exists but can't be read (cut short by a power
// loss) is kept byte for byte, nothing is saved over it, and the badge must not transmit.
static void identity_file_checks(char const *root, uint8_t const *first_key, SimpleMeshTables &tables) {
    std::string          path = std::string(root) + "/identity/_main.id";
    std::vector<uint8_t> good = file_bytes(path);
    CHECK(good.size() == PUB_KEY_SIZE + PRV_KEY_SIZE); // the first boot saved it: public key, then private key
    CHECK(good.size() >= PUB_KEY_SIZE && memcmp(good.data(), first_key, PUB_KEY_SIZE) == 0);

    CHECK(truncate(path.c_str(), 50) == 0);
    static DataStore store(SPIFFS, rtc_clock);
    static MyMesh    mesh(radio_driver, rng_for_pipe(), rtc_clock, tables, store);
    store.begin();
    mesh.begin();
    std::vector<uint8_t> after = file_bytes(path);
    CHECK(after.size() == 50 && memcmp(after.data(), good.data(), 50) == 0); // untouched
    CHECK(mesh.identityUnreadable());
    uint8_t prv[PRV_KEY_SIZE];
    CHECK(mesh.self_id.writeTo(prv, sizeof(prv)) == PRV_KEY_SIZE);
    mesh_ident_t state = mesh_ident_after_load(mesh.identityUnreadable(), prv, self_key(mesh));
    CHECK(state == MESH_IDENT_UNREADABLE && !mesh_ident_tx_allowed(state));

    // put back as it was, for anything after this
    FILE *f = fopen(path.c_str(), "wb");
    CHECK(f && fwrite(good.data(), 1, good.size(), f) == good.size());
    if (f) {
        fclose(f);
    }
}

int main(int argc, char **argv) {
    clock_uses_real_time();
    auto packets = load(argc > 1 ? argv[1] : "vectors/meshcore_rx.txt");
    CHECK(packets.size() == 5);

    char root[] = "/tmp/companion_test.XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    fs_shim_set_root(root);
    CHECK(SPIFFS.begin(true));

    static Rng              rng;
    static SimpleMeshTables tables1, tables2, tables3, tables4, tables5, tables6, tables7;
    identity_move_checks_the_saved_copy(rng);
    identity_store_checks(root, rng);
    static FakeApp          app;
    uint8_t                 first_key[PUB_KEY_SIZE];

    {
        // First boot: no files yet
        static DataStore store(SPIFFS, rtc_clock);
        static MyMesh    mesh(radio_driver, rng, rtc_clock, tables1, store);
        store.begin();
        mesh.begin();
        mesh.startInterface(app);
        CHECK(!mesh.identityUnreadable()); // no file: a new identity, saved (identity_file_checks reads it back)
        memcpy(first_key, self_key(mesh), PUB_KEY_SIZE);
        for (auto const &p : packets) {
            test_radio.pending = p;
            run(mesh, 50);
        }
        CHECK(mesh.getNumContacts() == 3); // Test Node advertised twice

        // The app syncs: the Public message is waiting for it
        app.from_badge.clear();
        app.to_badge.push_back({10}); // CMD_SYNC_NEXT_MESSAGE
        run(mesh, 2);
        CHECK(!app.from_badge.empty() && (app.from_badge.back()[0] == 8 || app.from_badge.back()[0] == 17) &&
              has_text(app.from_badge.back(), "Test Node: hello from the test vectors"));

        // #110: the badge builds without ENABLE_PRIVATE_KEY_IMPORT, so a key import is answered "disabled" and the
        // identity file is not touched (and the badge's own pipe never sends this command)
        std::vector<uint8_t> import_key(65, 0x11);
        import_key[0] = 24; // CMD_IMPORT_PRIVATE_KEY
        app.from_badge.clear();
        app.to_badge.push_back(import_key);
        run(mesh, 2);
        CHECK(!app.from_badge.empty() && app.from_badge.back()[0] == 15); // RESP_CODE_DISABLED
        CHECK(memcmp(self_key(mesh), first_key, PUB_KEY_SIZE) == 0);

        run(mesh, 100); // let MyMesh finish its delayed contacts save (5 s after the last change)
    }

    {
        // Reboot: same identity, same contacts
        static DataStore store(SPIFFS, rtc_clock);
        static MyMesh    mesh(radio_driver, rng, rtc_clock, tables2, store);
        store.begin();
        mesh.begin();
        CHECK(memcmp(self_key(mesh), first_key, PUB_KEY_SIZE) == 0);
        CHECK(!mesh.identityUnreadable());
        CHECK(mesh.getNumContacts() == 3);
    }

    pipe_checks(packets, tables6, root);
    blob_checks(packets, root);

    {
        // Settings from the badge (#67): name and radio through the setters' own path, saved as the badge saves them
        static DataStore store(SPIFFS, rtc_clock);
        static MyMesh    mesh(radio_driver, rng, rtc_clock, tables4, store);
        store.begin();
        mesh.begin();
        NodePrefs *prefs = mesh.getNodePrefs();
        CHECK(!mesh_prefs_set_name(prefs, "   ")); // empty after trimming: refused, nothing changed
        CHECK(strcmp(prefs->node_name, "   ") != 0);
        CHECK(mesh_prefs_set_name(prefs, "  Lucas badge  "));
        CHECK(strcmp(mesh.getNodeName(), "Lucas badge") == 0);
        CHECK(mesh_prefs_set_name(prefs, "0123456789012345678901234567890123456789"));
        CHECK(strlen(prefs->node_name) == 31);
        CHECK(mesh_prefs_set_name(prefs, "Lucas badge"));

        // Out of range: the reason, and neither the prefs nor the radio change
        tuned_freq = 0;
        CHECK(mesh_prefs_set_radio(prefs, 850.0f, 62.5f, 7, 5, 22) != NULL);
        prefs->path_hash_mode = 1;
        CHECK(tuned_freq == 0 && prefs->freq != 850.0f);

        // Netherlands (Limburg): SF8, CR8, 14 dBm
        CHECK(mesh_prefs_set_radio(prefs, 869.618f, 62.5f, 8, 8, 14) == NULL);
        CHECK(tuned_freq == 869.618f && tuned_bw == 62.5f && tuned_sf == 8 && tuned_cr == 8 && tuned_tx == 14);
        CHECK(prefs->path_hash_mode == 1); // the radio does not touch the path hash size (#129)
        // Its own setting: size 1..3, MyMesh floods with path_hash_mode + 1 bytes per hop
        CHECK(mesh_prefs_set_path_hash(prefs, 0) != NULL && mesh_prefs_set_path_hash(prefs, 4) != NULL);
        CHECK(prefs->path_hash_mode == 1);
        CHECK(mesh_prefs_set_path_hash(prefs, 1) == NULL && prefs->path_hash_mode == 0);
        CHECK(mesh_prefs_set_path_hash(prefs, 3) == NULL && prefs->path_hash_mode == 2);
        CHECK(mesh_prefs_set_path_hash(prefs, 2) == NULL && prefs->path_hash_mode == 1);
        mesh.savePrefs();

        // Auto-add, location and its sharing (#52): defaults are MeshCore's (add everything) and private (no location)
        advcfg_autoadd_t aa;
        mesh_prefs_get_autoadd(prefs, &aa);
        CHECK(aa.on && aa.types == ADVCFG_AA_TYPES && !aa.overwrite && aa.max_hops == 0);
        CHECK(prefs->advert_loc_policy == ADVERT_LOC_NONE);
        CHECK(mesh_prefs_set_loc_share(prefs, true) != NULL); // no location yet
        CHECK(prefs->advert_loc_policy == ADVERT_LOC_NONE);
        CHECK(mesh_prefs_set_location(prefs, 91.0, 0) != NULL && mesh_prefs_set_location(prefs, 0, 181.0) != NULL);
        CHECK(mesh_prefs_set_location(prefs, NAN, 0) != NULL && sensors.node_lat == 0 && sensors.node_lon == 0);
        CHECK(mesh_prefs_set_location(prefs, 52.3702, 4.8952) == NULL);
        CHECK(mesh_prefs_set_loc_share(prefs, true) == NULL && prefs->advert_loc_policy == ADVERT_LOC_SHARE);
        aa = {true, (uint8_t)(ADVCFG_AA_CHAT | ADVCFG_AA_ROOM), true, 3};
        mesh_prefs_set_autoadd(prefs, &aa);
        CHECK(prefs->manual_add_contacts == 1 && prefs->autoadd_config == (ADVCFG_AA_CHAT | ADVCFG_AA_ROOM | 1));

        // Default flood scope (#163): None by default (name and key all zero), a name keeps SHA-256("#name") as its key
        uint8_t zero_key[16] = {};
        CHECK(prefs->default_scope_name[0] == 0 && memcmp(prefs->default_scope_key, zero_key, 16) == 0);
        CHECK(mesh_prefs_set_scope(prefs, "a b") != NULL && prefs->default_scope_name[0] == 0);
        CHECK(mesh_prefs_set_scope(prefs, " #NL-Limburg ") == NULL && strcmp(prefs->default_scope_name, "nl-limburg") == 0);
        {
            TransportKey want;
            TransportKeyStore ks;
            ks.getAutoKeyFor(0, "#nl-limburg", want);
            CHECK(memcmp(prefs->default_scope_key, want.key, 16) == 0 && memcmp(want.key, zero_key, 16) != 0);
        }
        mesh.savePrefs();
    }

    {
        // A fresh MyMesh and DataStore on the same files: the settings are back
        static DataStore store(SPIFFS, rtc_clock);
        static MyMesh    mesh(radio_driver, rng, rtc_clock, tables5, store);
        store.begin();
        mesh.begin();
        NodePrefs *prefs = mesh.getNodePrefs();
        CHECK(strcmp(mesh.getNodeName(), "Lucas badge") == 0);
        CHECK(prefs->freq == 869.618f && prefs->bw == 62.5f && prefs->sf == 8 && prefs->cr == 8);
        CHECK(prefs->tx_power_dbm == 14 && prefs->path_hash_mode == 1);
        advcfg_autoadd_t aa;
        mesh_prefs_get_autoadd(prefs, &aa);
        CHECK(aa.on && aa.types == (ADVCFG_AA_CHAT | ADVCFG_AA_ROOM) && aa.overwrite && aa.max_hops == 3);
        CHECK(prefs->advert_loc_policy == ADVERT_LOC_SHARE);
        {
            // the scope survives the restart; None clears name and key
            uint8_t zero_key[16] = {};
            CHECK(strcmp(prefs->default_scope_name, "nl-limburg") == 0 && memcmp(prefs->default_scope_key, zero_key, 16) != 0);
            CHECK(mesh_prefs_set_scope(prefs, "") == NULL && prefs->default_scope_name[0] == 0
                  && memcmp(prefs->default_scope_key, zero_key, 16) == 0);
        }
        CHECK(fabs(sensors.node_lat - 52.3702) < 1e-6 && fabs(sensors.node_lon - 4.8952) < 1e-6);
        CHECK(mesh_prefs_set_location(prefs, 0, 0) == NULL && prefs->advert_loc_policy == ADVERT_LOC_NONE);
        CHECK(memcmp(self_key(mesh), first_key, PUB_KEY_SIZE) == 0);
    }

    {
        // A contacts file cut short (power lost while saving): boots, no crash
        std::string path = std::string(root) + "/contacts3";
        CHECK(truncate(path.c_str(), 50) == 0);
        static DataStore store(SPIFFS, rtc_clock);
        static MyMesh    mesh(radio_driver, rng, rtc_clock, tables3, store);
        store.begin();
        mesh.begin();
        CHECK(mesh.getNumContacts() <= 3);
        CHECK(memcmp(self_key(mesh), first_key, PUB_KEY_SIZE) == 0);
    }

    identity_file_checks(root, first_key, tables7);

    SPIFFS.format();
    rmdir(root);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("companion_test: all checks passed\n");
    return 0;
}
