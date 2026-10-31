// Writes vectors/meshcore_rx.txt: synthetic MeshCore packets made with MeshCore's own code, in the shape the badge
// receives them (fw lora raw on). Made-up identities, names, positions and text, so the fixture holds nobody's real
// node; the adverts are properly signed and the channel message is properly encrypted, so the tests still exercise
// MeshCore's checks. Deterministic: same output every run. Usage: make_vectors > vectors/meshcore_rx.txt
#include <Mesh.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

#include <base64.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The shim's platform hooks; srand() below makes the identities repeatable
extern "C" unsigned long millis(void) {
    return 0;
}
extern "C" void shim_fill_random(void *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ((uint8_t *)buf)[i] = (uint8_t)rand();
    }
}

class NoRadio : public mesh::Radio {
public:
    int      recvRaw(uint8_t *bytes, int sz) override { return 0; }
    uint32_t getEstAirtimeFor(int len_bytes) override { return (uint32_t)len_bytes; }
    float    packetScore(float snr, int packet_len) override { return 1.0f; }
    bool     startSendRaw(uint8_t const *bytes, int len) override { return false; }
    bool     isSendComplete() override { return true; }
    void     onSendFinished() override {}
    bool     isInRecvMode() const override { return true; }
};

class NoClock : public mesh::MillisecondClock {
public:
    unsigned long getMillis() override { return 0; }
};

class Rng : public mesh::RNG {
public:
    void random(uint8_t *dest, size_t sz) override { shim_fill_random(dest, sz); }
};

class Rtc : public mesh::RTCClock {
public:
    uint32_t now = 1793300000; // late 2026
    uint32_t getCurrentTime() override { return now; }
    void     setCurrentTime(uint32_t time) override { now = time; }
};

class Maker : public mesh::Mesh {
public:
    Maker(mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng, mesh::RTCClock &rtc,
          mesh::PacketManager &mgr, mesh::MeshTables &tables)
        : Mesh(radio, ms, rng, rtc, mgr, tables) {}
    using Mesh::createAdvert;
    using Mesh::createGroupDatagram;
};

static Rtc rtc;

// One line per packet, as a flood packet that came in over `hops` repeaters (path hashes are made up)
static void emit(char const *kind, mesh::Packet *pkt, uint8_t hops) {
    pkt->header = (uint8_t)((pkt->header & ~PH_ROUTE_MASK) | ROUTE_TYPE_FLOOD);
    pkt->setPathHashSizeAndCount(1, hops);
    for (uint8_t i = 0; i < hops; i++) {
        pkt->path[i] = (uint8_t)(0xa0 + i);
    }
    uint8_t raw[MAX_TRANS_UNIT + 1];
    uint8_t len = pkt->writeTo(raw);
    printf("%s ", kind);
    for (uint8_t i = 0; i < len; i++) {
        printf("%02x", raw[i]);
    }
    printf("\n");
}

static void advert(Maker &mesh, mesh::LocalIdentity const &id, AdvertDataBuilder builder, uint8_t hops) {
    uint8_t app_data[MAX_ADVERT_DATA_SIZE];
    uint8_t len = builder.encodeTo(app_data);
    emit("advert", mesh.createAdvert(id, app_data, len), hops);
}

int main() {
    srand(2025);
    static NoRadio                 radio;
    static NoClock                 clock;
    static Rng                     rng;
    static StaticPoolPacketManager pool(16);
    static SimpleMeshTables        tables;
    static Maker                   mesh(radio, clock, rng, rtc, pool, tables);

    mesh::LocalIdentity repeater(&rng), node(&rng), room(&rng);

    // MeshCore's default public channel (its key is public)
    mesh::GroupChannel  pub;
    memset(&pub, 0, sizeof(pub)); // a 16-byte key is zero-padded, as BaseChatMesh::addChannel does
    char const          psk[] = "izOH6cXN6mrJ5e26oRXNcg==";
    int                 key_len = decode_base64((unsigned char *)psk, strlen(psk), pub.secret);
    mesh::Utils::sha256(pub.hash, sizeof(pub.hash), pub.secret, key_len);

    printf("# Synthetic MeshCore packets (made by make_vectors.cpp: made-up nodes, names, positions and text), in the\n");
    printf("# form the badge receives them. One packet per line: <kind> <hex of the whole LoRa payload>.\n");
    printf("# Used by meshcore_decode_test and companion_test: 4 adverts from 3 nodes (Test Node twice) + 1 Public message.\n");

    advert(mesh, repeater, AdvertDataBuilder(ADV_TYPE_REPEATER, "Test Repeater", 0.5, 0.5), 2);

    rtc.now += 60;
    uint8_t     msg[5 + 64];
    char const  text[] = "Test Node: hello from the test vectors";
    memcpy(msg, &rtc.now, 4);
    msg[4] = 0; // TXT_TYPE_PLAIN
    memcpy(&msg[5], text, strlen(text));
    emit("grp_txt", mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, pub, msg, 5 + strlen(text)), 3);

    rtc.now += 60;
    advert(mesh, node, AdvertDataBuilder(ADV_TYPE_CHAT, "Test Node"), 1);
    rtc.now += 60;
    advert(mesh, room, AdvertDataBuilder(ADV_TYPE_ROOM, "Test Room", -0.5, 0.25), 2);
    rtc.now += 600; // the same node again later: a newer advert, not a duplicate packet
    advert(mesh, node, AdvertDataBuilder(ADV_TYPE_CHAT, "Test Node"), 2);
    return 0;
}
