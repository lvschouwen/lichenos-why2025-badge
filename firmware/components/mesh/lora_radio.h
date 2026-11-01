#pragma once

#include "target.h"

// MeshCore's radio on top of the C6 LoRa bridge: received packets come in through a queue the LoRa task fills,
// packets to send go to the C6 one at a time (lora_send), which receives again once each has left (#65).
class LoraRadio : public BadgeRadio {
public:
    void     begin() override;
    bool     claim(); // begin(), with the answer: false when another owner holds the radio (#103: a restart fails)
    void     end();   // releases the LoRa claim (#103); claim() takes it again
    int      recvRaw(uint8_t *bytes, int sz) override;
    uint32_t getEstAirtimeFor(int len_bytes) override;
    float    packetScore(float snr, int packet_len) override;
    bool     startSendRaw(uint8_t const *bytes, int len) override;
    bool     isSendComplete() override;
    void     onSendFinished() override;
    bool     isInRecvMode() const override { return !sending; }
    float    getLastRSSI() const override { return last_rssi; }
    float    getLastSNR() const override { return last_snr; }

    void     setParams(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr) override;
    void     setTxPower(int8_t dbm) override;
    uint32_t getRngSeed() override;

    // Packets that arrived while the queue was full: counted by the LoRa task, read by the console (#6)
    uint32_t rx_dropped() const { return __atomic_load_n(&dropped, __ATOMIC_RELAXED); }
    void     count_dropped() { __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED); }
    // The rest are counted on the mesh task inside MeshCore's loop and read under the mesh lock
    uint32_t send_failed = 0; // packets the C6 didn't get out
    // MeshCore gives up on a send after 1.5 x its airtime; the C6's answer can come later than that (#65 review)
    uint32_t send_timeouts = 0; // given up by MeshCore before the C6 answered
    uint32_t send_refused  = 0; // not started: the previous packet was still waiting for the C6's answer
    // The stored identity failed its check at load (#80, mesh_ident.h): nothing leaves the radio until a restart
    bool     tx_blocked    = false;

private:
    uint32_t dropped   = 0;
    float    last_rssi = 0, last_snr = 0;
    bool  sending   = false;
    bool  completed = false; // isSendComplete() saw the C6's answer for the current packet
};
