// The part of electroniccats/CayenneLPP 1.6.1 that MeshCore uses (#65): MeshCore's telemetry adds a voltage and a
// temperature. Same bytes as the library: channel, type, value * multiplier MSB first (two's complement if negative).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define LPP_TEMPERATURE 103 // 2 bytes, 0.1 degC signed
#define LPP_VOLTAGE     116 // 2 bytes, 0.01 V unsigned

class CayenneLPP {
public:
    explicit CayenneLPP(size_t size) : max_(size), buf_((uint8_t *)malloc(size)) {}
    ~CayenneLPP() { free(buf_); }
    CayenneLPP(CayenneLPP const &)            = delete;
    CayenneLPP &operator=(CayenneLPP const &) = delete;

    void           reset() { cursor_ = 0; }
    uint8_t        getSize() const { return (uint8_t)cursor_; }
    uint8_t const *getBuffer() const { return buf_; }
    uint8_t        addTemperature(uint8_t channel, float value) { return add(LPP_TEMPERATURE, channel, value, 10); }
    uint8_t        addVoltage(uint8_t channel, float value) { return add(LPP_VOLTAGE, channel, value, 100); }

private:
    // As the library's addField for a 2-byte field: the magnitude is truncated, then negated within 16 bits
    uint8_t add(uint8_t type, uint8_t channel, float value, uint32_t multiplier) {
        if (!buf_ || cursor_ + 4 > max_) {
            return 0;
        }
        bool     negative = value < 0;
        uint32_t v        = (uint32_t)((negative ? -value : value) * multiplier) & 0xFFFF;
        if (negative && type == LPP_TEMPERATURE) {
            v = (0x10000 - v) & 0xFFFF;
        }
        buf_[cursor_++] = channel;
        buf_[cursor_++] = type;
        buf_[cursor_++] = (uint8_t)(v >> 8);
        buf_[cursor_++] = (uint8_t)v;
        return (uint8_t)cursor_;
    }

    size_t   max_;
    uint8_t *buf_;
    size_t   cursor_ = 0;
};
