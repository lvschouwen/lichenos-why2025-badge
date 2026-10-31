// Host test: MeshCore's own receive path decodes packets the badge captured off the air (#61).
// The captured vectors are in vectors/meshcore_rx.txt (path passed as argv[1]).
#include <helpers/BaseChatMesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// The shim's platform hooks
static unsigned long fake_millis;
extern "C" unsigned long millis(void) {
    return fake_millis;
}
extern "C" void shim_fill_random(void *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ((uint8_t *)buf)[i] = (uint8_t)rand();
    }
}

struct Vector {
    std::string          kind;
    std::vector<uint8_t> bytes;
};

static std::vector<Vector> load_vectors(char const *path) {
    std::vector<Vector> out;
    FILE               *f = fopen(path, "r");
    if (!f) {
        return out;
    }
    char line[1200];
    while (fgets(line, sizeof(line), f)) {
        char kind[16], hex[1100];
        if (line[0] == '#' || sscanf(line, "%15s %1099s", kind, hex) != 2) {
            continue;
        }
        Vector v{kind, {}};
        for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
            unsigned b;
            sscanf(hex + i, "%2x", &b);
            v.bytes.push_back((uint8_t)b);
        }
        out.push_back(v);
    }
    fclose(f);
    return out;
}

class TestRadio : public mesh::Radio {
public:
    std::vector<uint8_t> pending;
    int recvRaw(uint8_t *bytes, int sz) override {
        if (pending.empty() || (int)pending.size() > sz) {
            return 0;
        }
        memcpy(bytes, pending.data(), pending.size());
        int n = (int)pending.size();
        pending.clear();
        return n;
    }
    uint32_t getEstAirtimeFor(int len_bytes) override { return (uint32_t)len_bytes; }
    float    packetScore(float snr, int packet_len) override { return 1.0f; }
    bool     startSendRaw(uint8_t const *bytes, int len) override { return false; } // receive only
    bool     isSendComplete() override { return true; }
    void     onSendFinished() override {}
    bool     isInRecvMode() const override { return true; }
};

class TestClock : public mesh::MillisecondClock {
public:
    unsigned long getMillis() override { return fake_millis; }
};

class TestRng : public mesh::RNG {
public:
    void random(uint8_t *dest, size_t sz) override { shim_fill_random(dest, sz); }
};

class TestRtc : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override { return 1793300000; }
    void     setCurrentTime(uint32_t time) override {}
};

class TestMesh : public BaseChatMesh {
public:
    std::vector<std::string> contact_names;
    std::vector<std::string> channel_texts;

    TestMesh(mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng, mesh::RTCClock &rtc,
             mesh::PacketManager &mgr, mesh::MeshTables &tables)
        : BaseChatMesh(radio, ms, rng, rtc, mgr, tables) {}

protected:
    void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, uint8_t const *path) override {
        contact_names.push_back(contact.name);
    }
    ContactInfo *processAck(uint8_t const *data) override { return nullptr; }
    void         onContactPathUpdated(ContactInfo const &contact) override {}
    void onMessageRecv(ContactInfo const &contact, mesh::Packet *pkt, uint32_t ts, char const *text) override {}
    void onCommandDataRecv(ContactInfo const &contact, mesh::Packet *pkt, uint32_t ts, char const *text) override {}
    void onCLICommandRecv(ContactInfo const &contact, mesh::Packet *pkt, uint32_t ts, char const *text,
                          char *reply) override {}
    void onSignedMessageRecv(ContactInfo const &contact, mesh::Packet *pkt, uint32_t ts, uint8_t const *prefix,
                             char const *text) override {}
    uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override { return 1000; }
    uint32_t calcDirectTimeoutMillisFor(uint32_t airtime, uint8_t path_len) const override { return 1000; }
    void     onSendTimeout() override {}
    void     onChannelMessageRecv(mesh::GroupChannel const &channel, mesh::Packet *pkt, uint32_t ts,
                                  char const *text) override {
        channel_texts.push_back(text);
    }
    uint8_t onContactRequest(ContactInfo const &contact, uint32_t ts, uint8_t const *data, uint8_t len,
                             uint8_t *reply) override {
        return 0;
    }
    void onContactResponse(ContactInfo const &contact, uint8_t const *data, uint8_t len) override {}
};

int main(int argc, char **argv) {
    std::vector<Vector> vectors = load_vectors(argc > 1 ? argv[1] : "vectors/meshcore_rx.txt");
    CHECK(!vectors.empty());

    static TestRadio               radio;
    static TestClock               clock;
    static TestRng                 rng;
    static TestRtc                 rtc;
    static StaticPoolPacketManager pool(16);
    static SimpleMeshTables        tables;
    static TestMesh                mesh(radio, clock, rng, rtc, pool, tables);
    mesh.self_id = mesh::LocalIdentity(&rng);
    mesh.begin();
    // MeshCore's default public channel
    CHECK(mesh.addChannel("Public", "izOH6cXN6mrJ5e26oRXNcg==") != nullptr);

    size_t adverts = 0, channel_msgs = 0;
    for (Vector const &v : vectors) {
        adverts += v.kind == "advert";
        channel_msgs += v.kind == "grp_txt";
        radio.pending = v.bytes;
        for (int i = 0; i < 50; i++) { // run the loop over a few seconds of fake time: inbound packets may be delayed
            mesh.loop();
            fake_millis += 100;
        }
    }

    // Every advert is signed by its sender; MeshCore only reports it after checking the signature
    CHECK(mesh.contact_names.size() == adverts);
    for (std::string const &name : mesh.contact_names) {
        CHECK(!name.empty());
        printf("contact: %s\n", name.c_str());
    }
    // A channel message only decrypts with the right key and a valid MAC; MeshCore formats it as "sender: text"
    CHECK(mesh.channel_texts.size() == channel_msgs);
    for (std::string const &text : mesh.channel_texts) {
        CHECK(text.find(": ") != std::string::npos);
        printf("channel: %s\n", text.c_str());
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("meshcore_decode_test: all checks passed\n");
    return 0;
}
