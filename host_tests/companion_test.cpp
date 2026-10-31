// Host test: MeshCore's own companion (examples/companion_radio MyMesh + DataStore) on the badge's target, fed with
// captured packets (#65): contacts land in MeshCore's table, channel messages reach the app, and everything that
// should survive a reboot does.
#include "MyMesh.h"
#include "identity_move.h"
#include "mesh_prefs.h"

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
    bool     startSendRaw(uint8_t const *bytes, int len) override { return false; }
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

// MeshCore's clock: the system's real time when it has one (SNTP on the badge), never MeshCore's 2024 default (#65:
// other nodes drop adverts that aren't newer than the last one they saw)
static void clock_uses_real_time(void) {
    BadgeRtc rtc;
    uint32_t  now = (uint32_t)time(NULL); // the host's clock is set
    CHECK(rtc.getCurrentTime() >= now && rtc.getCurrentTime() <= now + 1);
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

int main(int argc, char **argv) {
    clock_uses_real_time();
    auto packets = load(argc > 1 ? argv[1] : "vectors/meshcore_rx.txt");
    CHECK(packets.size() == 5);

    char root[] = "/tmp/companion_test.XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    fs_shim_set_root(root);
    CHECK(SPIFFS.begin(true));

    static Rng              rng;
    static SimpleMeshTables tables1, tables2, tables3, tables4, tables5;
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
        CHECK(mesh.getNumContacts() == 3);
    }

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

    SPIFFS.format();
    rmdir(root);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("companion_test: all checks passed\n");
    return 0;
}
