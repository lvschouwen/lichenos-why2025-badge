// The badge as a MeshCore target (#65): what examples/companion_radio expects from a variant's target.h. The radio is
// the SX1262 behind the C6 (LoraRadio on the badge, a test radio on the host); the board, radio_* functions and the
// globals are defined by the firmware (mesh_task.cpp) or by the host test.
#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/KeyValueStore.h>
#include <helpers/SensorManager.h>
#include <lichen_clock.h>

#include <math.h>
#include <sys/time.h>
#include <time.h>

// What MyMesh calls on radio_driver: MeshCore's RadioLibWrapper interface, without RadioLib
class BadgeRadio : public mesh::Radio {
public:
    virtual void     setParams(float freq, float bw, uint8_t sf, uint8_t cr) = 0;
    virtual void     setTxPower(int8_t dbm)                                  = 0;
    virtual uint32_t getRngSeed() { return 0; }
    virtual bool     setRxBoostedGainMode(bool on) { return false; }
    virtual bool     getRxBoostedGainMode() const { return false; }
    uint32_t         getPacketsRecv() const { return n_recv; }
    uint32_t         getPacketsRecvErrors() const { return n_recv_errors; }
    uint32_t         getPacketsSent() const { return n_sent; }
    void             resetStats() { n_recv = n_sent = n_recv_errors = 0; }

protected:
    uint32_t n_recv = 0, n_sent = 0, n_recv_errors = 0;
};

class BadgeBoard : public mesh::MainBoard {
public:
    uint16_t    getBattMilliVolts() override;
    char const *getManufacturerName() const override { return "WHY2025 badge"; }
    void        reboot() override;
    uint8_t     getStartupReason() const override { return BD_STARTUP_NORMAL; }
    void        attachDynamicPrefs(KeyValueStore *prefs) {} // as ESP32Board: no board prefs
};

// MeshCore's clock: the system's real time once it has one (SNTP over Wi-Fi, or the app setting it), else MeshCore's
// default start (May 2024) plus uptime. Other nodes drop adverts that aren't newer than the last they saw, so a
// clock that starts over at every boot would silence the badge after a reboot (#65).
class BadgeRtc : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override {
        time_t now = time(NULL);
        return clock_is_set() ? (uint32_t)now : base_ + (uint32_t)(millis() / 1000);
    }
    void setCurrentTime(uint32_t t) override {
        struct timeval tv = {(time_t)t, 0};
        if (clock_in_window((int64_t)t, clock_build_epoch())) { // same bounds as SNTP (#8)
            settimeofday(&tv, NULL);
        } // may be refused (host tests): the fallback below still follows the app
        base_ = t - (uint32_t)(millis() / 1000);
    }

private:
    uint32_t base_ = 1715770351; // MeshCore's default (May 2024)
};

extern BadgeRadio   &radio_driver;
extern BadgeBoard    board;
extern BadgeRtc      rtc_clock;
extern SensorManager sensors;

bool                radio_init();
uint32_t            radio_get_rng_seed();
void                radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr);
void                radio_set_tx_power(uint8_t dbm);
mesh::LocalIdentity radio_new_identity();
