#pragma once

#include <stdint.h>

// LoRa maths MeshCore's radio wrapper normally gets from RadioLib (#61). Pure logic, no ESP headers.

// Time on air in microseconds of a packet with len payload bytes: explicit header, CRC on, low data rate
// optimisation when a symbol lasts 16 ms or more (Semtech AN1200.13). cr is the denominator: 5 = 4/5 ... 8 = 4/8.
uint32_t lora_airtime_us(int len, uint8_t sf, uint32_t bw_hz, uint8_t cr, uint16_t preamble);

// MeshCore's RadioLibWrapper::packetScoreInt(): 0..1, how likely the packet arrived intact (0 below the SF's SNR
// floor), less for long packets
float lora_packet_score(float snr, uint8_t sf, int len);
