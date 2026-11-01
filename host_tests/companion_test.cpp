// Host test: MeshCore's own companion (examples/companion_radio MyMesh + DataStore) on the badge's target, fed with
// captured packets (#65): contacts land in MeshCore's table, channel messages reach the app, and everything that
// should survive a reboot does.
#include "MyMesh.h"
#include "identity_move.h"
#include "mesh_prefs.h"
#include "mesh_ident.h"
#include "mesh_link.h"

#include "SPIFFS.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    int channel_msgs = 0, dm_msgs = 0, queue_len = -1;
    void onMessageRecv(mesh::Packet *, ContactInfo const &, uint8_t, uint32_t, char const *) override { dm_msgs++; }
    void onChannelMessageRecv(mesh::Packet *, ChannelDetails &, char const *) override { channel_msgs++; }
    void onQueueSizeChanged(int n) override { queue_len = n; }
};

static void settle(BadgeMesh &mesh, int loops = 60) {
    run(mesh, loops);
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
}

// The companion pipe (#45, #55) on the real MyMesh: start sequence, offline queue drained, sends through the outbox
static void pipe_checks(std::vector<std::vector<uint8_t>> const &packets, SimpleMeshTables &tables) {
    static DataStore store(SPIFFS, rtc_clock);
    static ob_t      ob;
    static BadgeMesh mesh(radio_driver, rng_for_pipe(), rtc_clock, tables, store, &ob);
    static CountingListener listener;
    ob_init(&ob);
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
    contact_action_checks(mesh);
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

    pipe_checks(packets, tables6);

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
        CHECK(mesh_prefs_set_radio(prefs, 850.0f, 62.5f, 7, 5, 1, 22) != NULL);
        CHECK(mesh_prefs_set_radio(prefs, 869.618f, 62.5f, 7, 5, 4, 22) != NULL); // path hash size 1..3
        CHECK(tuned_freq == 0 && prefs->freq != 850.0f);

        // Netherlands (Limburg): SF8, CR8, 2-byte path hashes, 14 dBm
        CHECK(mesh_prefs_set_radio(prefs, 869.618f, 62.5f, 8, 8, 2, 14) == NULL);
        CHECK(tuned_freq == 869.618f && tuned_bw == 62.5f && tuned_sf == 8 && tuned_cr == 8 && tuned_tx == 14);
        CHECK(prefs->path_hash_mode == 1); // MyMesh floods with path_hash_mode + 1 bytes per hop
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
