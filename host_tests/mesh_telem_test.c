// Host test: telemetry request to a contact (#162): LPP values, the 30 s rate limit, frames, and the request's states
#include "mesh_telem.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static uint8_t const A[6] = {1, 2, 3, 4, 5, 6}, B[6] = {9, 9, 9, 9, 9, 9};

static void lpp(void) {
    tl_line_t l[TL_LINES_MAX];
    // MeshCore's own telemetry: battery 4.12 V and temperature 21.5 C on channel 1 (CayenneLPP bytes), a negative one
    uint8_t d[] = {1, 116, 0x01, 0x9C, 1, 103, 0x00, 0xD7, 2, 103, 0xFF, 0x9C};
    size_t  n   = tl_lpp_lines(d, sizeof(d), l, TL_LINES_MAX);
    CHECK(n == 3);
    CHECK(strcmp(l[0].text, "Battery: 4.12 V") == 0);
    CHECK(strcmp(l[1].text, "Temperature: 21.5 \xC2\xB0""C") == 0);
    CHECK(strcmp(l[2].text, "Temperature (channel 2): -10.0 \xC2\xB0""C") == 0);
    // GPS: 52.4620, 4.8833, 3.00 m
    uint8_t g[] = {1, 136, 0x08, 0x01, 0x4C, 0x00, 0xBE, 0xC1, 0x00, 0x01, 0x2C};
    CHECK(tl_lpp_lines(g, sizeof(g), l, TL_LINES_MAX) == 1);
    CHECK(strcmp(l[0].text, "Location: 52.4620, 4.8833, 3 m") == 0);
    // negative longitude
    uint8_t g2[] = {3, 136, 0x00, 0x00, 0x0A, 0xFF, 0xFF, 0xF6, 0x00, 0x00, 0x64};
    CHECK(tl_lpp_lines(g2, sizeof(g2), l, TL_LINES_MAX) == 1);
    CHECK(strcmp(l[0].text, "Location (channel 3): 0.0010, -0.0010, 1 m") == 0);
    // humidity 0.5 % steps, pressure 0.1 hPa
    uint8_t h[] = {4, 104, 101, 4, 115, 0x27, 0x96};
    CHECK(tl_lpp_lines(h, sizeof(h), l, TL_LINES_MAX) == 2);
    CHECK(strcmp(l[0].text, "Humidity (channel 4): 50.5 %") == 0);
    CHECK(strcmp(l[1].text, "Pressure (channel 4): 1013.4 hPa") == 0);
    // a cut value and an unknown type end the list with a line that says so
    uint8_t cut[] = {1, 116, 0x01, 0x9C, 1, 103, 0x00};
    CHECK(tl_lpp_lines(cut, sizeof(cut), l, TL_LINES_MAX) == 2 && strstr(l[1].text, "cut off"));
    uint8_t unk[] = {1, 116, 0x01, 0x9C, 1, 77, 0x00, 0x00};
    CHECK(tl_lpp_lines(unk, sizeof(unk), l, TL_LINES_MAX) == 2 && strstr(l[1].text, "type 77"));
    CHECK(tl_lpp_lines(d, 0, l, TL_LINES_MAX) == 0);
    // more values than lines: the last line says how many are not shown (the replaced one included)
    CHECK(tl_lpp_lines(d, sizeof(d), l, 2) == 2);
    CHECK(strcmp(l[0].text, "Battery: 4.12 V") == 0 && strcmp(l[1].text, "2 more values not shown") == 0);
    CHECK(tl_lpp_lines(d, sizeof(d), l, 3) == 3 && strstr(l[2].text, "channel 2")); // fits exactly: no extra line
    // channel 0 digital input is a value, not padding
    uint8_t z[] = {0, 0, 1, 1, 116, 0x01, 0x9C};
    CHECK(tl_lpp_lines(z, sizeof(z), l, TL_LINES_MAX) == 2);
    CHECK(strcmp(l[0].text, "Input (channel 0): on") == 0 && strcmp(l[1].text, "Battery: 4.12 V") == 0);
}

// #212: every Cayenne LPP type the decoder knows, pinned byte for byte before its switch ladders became a table. Per type:
// zero on channel 0, all ones on channel 255, the most negative and most positive raw value, a plain value, the most
// negative on channel 255 (the longest lines), the header alone, and a value that stops one byte short. Then the
// cut-off and more-values tails. The rows were printed by the decoder as it was; this is the trust boundary for what
// other nodes send, so any change of a byte here is a change of what the page shows.
typedef struct {
    uint8_t     bytes[32];
    size_t      len, max, n;
    char const *line[5];
} lpp_pin_t;

static lpp_pin_t const PINS[] = {
    // type 0, 1 bytes
    {{0x00, 0x00, 0x00}, 3, 24, 1, {"Input (channel 0): off"}},
    {{0xFF, 0x00, 0xFF}, 3, 24, 1, {"Input (channel 255): on"}},
    {{0x01, 0x00, 0x80}, 3, 24, 1, {"Input: on"}},
    {{0x01, 0x00, 0x7F}, 3, 24, 1, {"Input: on"}},
    {{0x03, 0x00, 0x01}, 3, 24, 1, {"Input (channel 3): on"}},
    {{0xFF, 0x00, 0x80}, 3, 24, 1, {"Input (channel 255): on"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x00}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 1, 1 bytes
    {{0x00, 0x01, 0x00}, 3, 24, 1, {"Input (channel 0): off"}},
    {{0xFF, 0x01, 0xFF}, 3, 24, 1, {"Input (channel 255): on"}},
    {{0x01, 0x01, 0x80}, 3, 24, 1, {"Input: on"}},
    {{0x01, 0x01, 0x7F}, 3, 24, 1, {"Input: on"}},
    {{0x03, 0x01, 0x01}, 3, 24, 1, {"Input (channel 3): on"}},
    {{0xFF, 0x01, 0x80}, 3, 24, 1, {"Input (channel 255): on"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x01}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 2, 2 bytes
    {{0x00, 0x02, 0x00, 0x00}, 4, 24, 1, {"Analog value (channel 0): 0.00"}},
    {{0xFF, 0x02, 0xFF, 0xFF}, 4, 24, 1, {"Analog value (channel 255): -0.01"}},
    {{0x01, 0x02, 0x80, 0x00}, 4, 24, 1, {"Analog value: -327.68"}},
    {{0x01, 0x02, 0x7F, 0xFF}, 4, 24, 1, {"Analog value: 327.67"}},
    {{0x03, 0x02, 0x01, 0x02}, 4, 24, 1, {"Analog value (channel 3): 2.58"}},
    {{0xFF, 0x02, 0x80, 0x00}, 4, 24, 1, {"Analog value (channel 255): -327.68"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x02}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x02, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 3, 2 bytes
    {{0x00, 0x03, 0x00, 0x00}, 4, 24, 1, {"Analog value (channel 0): 0.00"}},
    {{0xFF, 0x03, 0xFF, 0xFF}, 4, 24, 1, {"Analog value (channel 255): -0.01"}},
    {{0x01, 0x03, 0x80, 0x00}, 4, 24, 1, {"Analog value: -327.68"}},
    {{0x01, 0x03, 0x7F, 0xFF}, 4, 24, 1, {"Analog value: 327.67"}},
    {{0x03, 0x03, 0x01, 0x02}, 4, 24, 1, {"Analog value (channel 3): 2.58"}},
    {{0xFF, 0x03, 0x80, 0x00}, 4, 24, 1, {"Analog value (channel 255): -327.68"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x03}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x03, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 100, 4 bytes
    {{0x00, 0x64, 0x00, 0x00, 0x00, 0x00}, 6, 24, 1, {"Value (channel 0): 0"}},
    {{0xFF, 0x64, 0xFF, 0xFF, 0xFF, 0xFF}, 6, 24, 1, {"Value (channel 255): 4294967295"}},
    {{0x01, 0x64, 0x80, 0x00, 0x00, 0x00}, 6, 24, 1, {"Value: 2147483648"}},
    {{0x01, 0x64, 0x7F, 0xFF, 0xFF, 0xFF}, 6, 24, 1, {"Value: 2147483647"}},
    {{0x03, 0x64, 0x01, 0x02, 0x03, 0x04}, 6, 24, 1, {"Value (channel 3): 16909060"}},
    {{0xFF, 0x64, 0x80, 0x00, 0x00, 0x00}, 6, 24, 1, {"Value (channel 255): 2147483648"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x64}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x64, 0x01, 0x02, 0x03}, 9, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 101, 2 bytes
    {{0x00, 0x65, 0x00, 0x00}, 4, 24, 1, {"Light (channel 0): 0 lux"}},
    {{0xFF, 0x65, 0xFF, 0xFF}, 4, 24, 1, {"Light (channel 255): 65535 lux"}},
    {{0x01, 0x65, 0x80, 0x00}, 4, 24, 1, {"Light: 32768 lux"}},
    {{0x01, 0x65, 0x7F, 0xFF}, 4, 24, 1, {"Light: 32767 lux"}},
    {{0x03, 0x65, 0x01, 0x02}, 4, 24, 1, {"Light (channel 3): 258 lux"}},
    {{0xFF, 0x65, 0x80, 0x00}, 4, 24, 1, {"Light (channel 255): 32768 lux"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x65}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x65, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 102, 1 bytes
    {{0x00, 0x66, 0x00}, 3, 24, 1, {"Presence (channel 0): off"}},
    {{0xFF, 0x66, 0xFF}, 3, 24, 1, {"Presence (channel 255): on"}},
    {{0x01, 0x66, 0x80}, 3, 24, 1, {"Presence: on"}},
    {{0x01, 0x66, 0x7F}, 3, 24, 1, {"Presence: on"}},
    {{0x03, 0x66, 0x01}, 3, 24, 1, {"Presence (channel 3): on"}},
    {{0xFF, 0x66, 0x80}, 3, 24, 1, {"Presence (channel 255): on"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x66}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 103, 2 bytes
    {{0x00, 0x67, 0x00, 0x00}, 4, 24, 1, {"Temperature (channel 0): 0.0 \xC2""\xB0""C"}},
    {{0xFF, 0x67, 0xFF, 0xFF}, 4, 24, 1, {"Temperature (channel 255): -0.1 \xC2""\xB0""C"}},
    {{0x01, 0x67, 0x80, 0x00}, 4, 24, 1, {"Temperature: -3276.8 \xC2""\xB0""C"}},
    {{0x01, 0x67, 0x7F, 0xFF}, 4, 24, 1, {"Temperature: 3276.7 \xC2""\xB0""C"}},
    {{0x03, 0x67, 0x01, 0x02}, 4, 24, 1, {"Temperature (channel 3): 25.8 \xC2""\xB0""C"}},
    {{0xFF, 0x67, 0x80, 0x00}, 4, 24, 1, {"Temperature (channel 255): -3276.8 \xC2""\xB0""C"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 104, 1 bytes
    {{0x00, 0x68, 0x00}, 3, 24, 1, {"Humidity (channel 0): 0.0 %"}},
    {{0xFF, 0x68, 0xFF}, 3, 24, 1, {"Humidity (channel 255): 127.5 %"}},
    {{0x01, 0x68, 0x80}, 3, 24, 1, {"Humidity: 64.0 %"}},
    {{0x01, 0x68, 0x7F}, 3, 24, 1, {"Humidity: 63.5 %"}},
    {{0x03, 0x68, 0x01}, 3, 24, 1, {"Humidity (channel 3): 0.5 %"}},
    {{0xFF, 0x68, 0x80}, 3, 24, 1, {"Humidity (channel 255): 64.0 %"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x68}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 113, 6 bytes
    {{0x00, 0x71, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 24, 1, {"Acceleration (channel 0): x 0.000, y 0.000, z 0.000 g"}},
    {{0xFF, 0x71, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 8, 24, 1, {"Acceleration (channel 255): x -0.001, y -0.001, z -0.001 g"}},
    {{0x01, 0x71, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00}, 8, 24, 1, {"Acceleration: x -32.768, y -32.768, z -32.768 g"}},
    {{0x01, 0x71, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF}, 8, 24, 1, {"Acceleration: x 32.767, y 32.767, z 32.767 g"}},
    {{0x03, 0x71, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06}, 8, 24, 1, {"Acceleration (channel 3): x 0.258, y 0.772, z 1.286 g"}},
    {{0xFF, 0x71, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00}, 8, 24, 1, {"Acceleration (channel 255): x -32.768, y -32.768, z -32.768 g"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x71}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x71, 0x01, 0x02, 0x03, 0x04, 0x05}, 11, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 115, 2 bytes
    {{0x00, 0x73, 0x00, 0x00}, 4, 24, 1, {"Pressure (channel 0): 0.0 hPa"}},
    {{0xFF, 0x73, 0xFF, 0xFF}, 4, 24, 1, {"Pressure (channel 255): 6553.5 hPa"}},
    {{0x01, 0x73, 0x80, 0x00}, 4, 24, 1, {"Pressure: 3276.8 hPa"}},
    {{0x01, 0x73, 0x7F, 0xFF}, 4, 24, 1, {"Pressure: 3276.7 hPa"}},
    {{0x03, 0x73, 0x01, 0x02}, 4, 24, 1, {"Pressure (channel 3): 25.8 hPa"}},
    {{0xFF, 0x73, 0x80, 0x00}, 4, 24, 1, {"Pressure (channel 255): 3276.8 hPa"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x73}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x73, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 116, 2 bytes
    {{0x00, 0x74, 0x00, 0x00}, 4, 24, 1, {"Voltage (channel 0): 0.00 V"}},
    {{0xFF, 0x74, 0xFF, 0xFF}, 4, 24, 1, {"Voltage (channel 255): 655.35 V"}},
    {{0x01, 0x74, 0x80, 0x00}, 4, 24, 1, {"Battery: 327.68 V"}},
    {{0x01, 0x74, 0x7F, 0xFF}, 4, 24, 1, {"Battery: 327.67 V"}},
    {{0x03, 0x74, 0x01, 0x02}, 4, 24, 1, {"Voltage (channel 3): 2.58 V"}},
    {{0xFF, 0x74, 0x80, 0x00}, 4, 24, 1, {"Voltage (channel 255): 327.68 V"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x74}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x74, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 117, 2 bytes
    {{0x00, 0x75, 0x00, 0x00}, 4, 24, 1, {"Current (channel 0): 0.000 A"}},
    {{0xFF, 0x75, 0xFF, 0xFF}, 4, 24, 1, {"Current (channel 255): 65.535 A"}},
    {{0x01, 0x75, 0x80, 0x00}, 4, 24, 1, {"Current: 32.768 A"}},
    {{0x01, 0x75, 0x7F, 0xFF}, 4, 24, 1, {"Current: 32.767 A"}},
    {{0x03, 0x75, 0x01, 0x02}, 4, 24, 1, {"Current (channel 3): 0.258 A"}},
    {{0xFF, 0x75, 0x80, 0x00}, 4, 24, 1, {"Current (channel 255): 32.768 A"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x75}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x75, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 118, 4 bytes
    {{0x00, 0x76, 0x00, 0x00, 0x00, 0x00}, 6, 24, 1, {"Frequency (channel 0): 0 Hz"}},
    {{0xFF, 0x76, 0xFF, 0xFF, 0xFF, 0xFF}, 6, 24, 1, {"Frequency (channel 255): 4294967295 Hz"}},
    {{0x01, 0x76, 0x80, 0x00, 0x00, 0x00}, 6, 24, 1, {"Frequency: 2147483648 Hz"}},
    {{0x01, 0x76, 0x7F, 0xFF, 0xFF, 0xFF}, 6, 24, 1, {"Frequency: 2147483647 Hz"}},
    {{0x03, 0x76, 0x01, 0x02, 0x03, 0x04}, 6, 24, 1, {"Frequency (channel 3): 16909060 Hz"}},
    {{0xFF, 0x76, 0x80, 0x00, 0x00, 0x00}, 6, 24, 1, {"Frequency (channel 255): 2147483648 Hz"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x76}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x76, 0x01, 0x02, 0x03}, 9, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 120, 1 bytes
    {{0x00, 0x78, 0x00}, 3, 24, 1, {"Level (channel 0): 0 %"}},
    {{0xFF, 0x78, 0xFF}, 3, 24, 1, {"Level (channel 255): 255 %"}},
    {{0x01, 0x78, 0x80}, 3, 24, 1, {"Level: 128 %"}},
    {{0x01, 0x78, 0x7F}, 3, 24, 1, {"Level: 127 %"}},
    {{0x03, 0x78, 0x01}, 3, 24, 1, {"Level (channel 3): 1 %"}},
    {{0xFF, 0x78, 0x80}, 3, 24, 1, {"Level (channel 255): 128 %"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x78}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 121, 2 bytes
    {{0x00, 0x79, 0x00, 0x00}, 4, 24, 1, {"Altitude (channel 0): 0 m"}},
    {{0xFF, 0x79, 0xFF, 0xFF}, 4, 24, 1, {"Altitude (channel 255): -1 m"}},
    {{0x01, 0x79, 0x80, 0x00}, 4, 24, 1, {"Altitude: -32768 m"}},
    {{0x01, 0x79, 0x7F, 0xFF}, 4, 24, 1, {"Altitude: 32767 m"}},
    {{0x03, 0x79, 0x01, 0x02}, 4, 24, 1, {"Altitude (channel 3): 258 m"}},
    {{0xFF, 0x79, 0x80, 0x00}, 4, 24, 1, {"Altitude (channel 255): -32768 m"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x79}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x79, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 125, 2 bytes
    {{0x00, 0x7D, 0x00, 0x00}, 4, 24, 1, {"Concentration (channel 0): 0 ppm"}},
    {{0xFF, 0x7D, 0xFF, 0xFF}, 4, 24, 1, {"Concentration (channel 255): 65535 ppm"}},
    {{0x01, 0x7D, 0x80, 0x00}, 4, 24, 1, {"Concentration: 32768 ppm"}},
    {{0x01, 0x7D, 0x7F, 0xFF}, 4, 24, 1, {"Concentration: 32767 ppm"}},
    {{0x03, 0x7D, 0x01, 0x02}, 4, 24, 1, {"Concentration (channel 3): 258 ppm"}},
    {{0xFF, 0x7D, 0x80, 0x00}, 4, 24, 1, {"Concentration (channel 255): 32768 ppm"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x7D}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x7D, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 128, 2 bytes
    {{0x00, 0x80, 0x00, 0x00}, 4, 24, 1, {"Power (channel 0): 0 W"}},
    {{0xFF, 0x80, 0xFF, 0xFF}, 4, 24, 1, {"Power (channel 255): 65535 W"}},
    {{0x01, 0x80, 0x80, 0x00}, 4, 24, 1, {"Power: 32768 W"}},
    {{0x01, 0x80, 0x7F, 0xFF}, 4, 24, 1, {"Power: 32767 W"}},
    {{0x03, 0x80, 0x01, 0x02}, 4, 24, 1, {"Power (channel 3): 258 W"}},
    {{0xFF, 0x80, 0x80, 0x00}, 4, 24, 1, {"Power (channel 255): 32768 W"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x80}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x80, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 130, 4 bytes
    {{0x00, 0x82, 0x00, 0x00, 0x00, 0x00}, 6, 24, 1, {"Distance (channel 0): 0.000 m"}},
    {{0xFF, 0x82, 0xFF, 0xFF, 0xFF, 0xFF}, 6, 24, 1, {"Distance (channel 255): 4294967.295 m"}},
    {{0x01, 0x82, 0x80, 0x00, 0x00, 0x00}, 6, 24, 1, {"Distance: 2147483.648 m"}},
    {{0x01, 0x82, 0x7F, 0xFF, 0xFF, 0xFF}, 6, 24, 1, {"Distance: 2147483.647 m"}},
    {{0x03, 0x82, 0x01, 0x02, 0x03, 0x04}, 6, 24, 1, {"Distance (channel 3): 16909.060 m"}},
    {{0xFF, 0x82, 0x80, 0x00, 0x00, 0x00}, 6, 24, 1, {"Distance (channel 255): 2147483.648 m"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x82}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x82, 0x01, 0x02, 0x03}, 9, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 131, 4 bytes
    {{0x00, 0x83, 0x00, 0x00, 0x00, 0x00}, 6, 24, 1, {"Energy (channel 0): 0.000 kWh"}},
    {{0xFF, 0x83, 0xFF, 0xFF, 0xFF, 0xFF}, 6, 24, 1, {"Energy (channel 255): 4294967.295 kWh"}},
    {{0x01, 0x83, 0x80, 0x00, 0x00, 0x00}, 6, 24, 1, {"Energy: 2147483.648 kWh"}},
    {{0x01, 0x83, 0x7F, 0xFF, 0xFF, 0xFF}, 6, 24, 1, {"Energy: 2147483.647 kWh"}},
    {{0x03, 0x83, 0x01, 0x02, 0x03, 0x04}, 6, 24, 1, {"Energy (channel 3): 16909.060 kWh"}},
    {{0xFF, 0x83, 0x80, 0x00, 0x00, 0x00}, 6, 24, 1, {"Energy (channel 255): 2147483.648 kWh"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x83}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x83, 0x01, 0x02, 0x03}, 9, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 132, 2 bytes
    {{0x00, 0x84, 0x00, 0x00}, 4, 24, 1, {"Direction (channel 0): 0\xC2""\xB0"""}},
    {{0xFF, 0x84, 0xFF, 0xFF}, 4, 24, 1, {"Direction (channel 255): 65535\xC2""\xB0"""}},
    {{0x01, 0x84, 0x80, 0x00}, 4, 24, 1, {"Direction: 32768\xC2""\xB0"""}},
    {{0x01, 0x84, 0x7F, 0xFF}, 4, 24, 1, {"Direction: 32767\xC2""\xB0"""}},
    {{0x03, 0x84, 0x01, 0x02}, 4, 24, 1, {"Direction (channel 3): 258\xC2""\xB0"""}},
    {{0xFF, 0x84, 0x80, 0x00}, 4, 24, 1, {"Direction (channel 255): 32768\xC2""\xB0"""}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x84}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x84, 0x01}, 7, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 133, 4 bytes
    {{0x00, 0x85, 0x00, 0x00, 0x00, 0x00}, 6, 24, 1, {"Time (channel 0): 0 s since 1970"}},
    {{0xFF, 0x85, 0xFF, 0xFF, 0xFF, 0xFF}, 6, 24, 1, {"Time (channel 255): 4294967295 s since 1970"}},
    {{0x01, 0x85, 0x80, 0x00, 0x00, 0x00}, 6, 24, 1, {"Time: 2147483648 s since 1970"}},
    {{0x01, 0x85, 0x7F, 0xFF, 0xFF, 0xFF}, 6, 24, 1, {"Time: 2147483647 s since 1970"}},
    {{0x03, 0x85, 0x01, 0x02, 0x03, 0x04}, 6, 24, 1, {"Time (channel 3): 16909060 s since 1970"}},
    {{0xFF, 0x85, 0x80, 0x00, 0x00, 0x00}, 6, 24, 1, {"Time (channel 255): 2147483648 s since 1970"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x85}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x85, 0x01, 0x02, 0x03}, 9, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 134, 6 bytes
    {{0x00, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 24, 1, {"Rotation (channel 0): x 0.00, y 0.00, z 0.00 \xC2""\xB0""/s"}},
    {{0xFF, 0x86, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 8, 24, 1, {"Rotation (channel 255): x -0.01, y -0.01, z -0.01 \xC2""\xB0""/s"}},
    {{0x01, 0x86, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00}, 8, 24, 1, {"Rotation: x -327.68, y -327.68, z -327.68 \xC2""\xB0""/s"}},
    {{0x01, 0x86, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF}, 8, 24, 1, {"Rotation: x 327.67, y 327.67, z 327.67 \xC2""\xB0""/s"}},
    {{0x03, 0x86, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06}, 8, 24, 1, {"Rotation (channel 3): x 2.58, y 7.72, z 12.86 \xC2""\xB0""/s"}},
    {{0xFF, 0x86, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00}, 8, 24, 1, {"Rotation (channel 255): x -327.68, y -327.68, z -327.68 \xC2""\xB0""/s"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x86}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x86, 0x01, 0x02, 0x03, 0x04, 0x05}, 11, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 136, 9 bytes
    {{0x00, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 11, 24, 1, {"Location (channel 0): 0.0000, 0.0000, 0 m"}},
    {{0xFF, 0x88, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 11, 24, 1, {"Location (channel 255): -0.0001, -0.0001, -0 m"}},
    {{0x01, 0x88, 0x80, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00, 0x00}, 11, 24, 1, {"Location: -838.8608, -838.8608, -83886 m"}},
    {{0x01, 0x88, 0x7F, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF}, 11, 24, 1, {"Location: 838.8607, 838.8607, 83886 m"}},
    {{0x03, 0x88, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09}, 11, 24, 1, {"Location (channel 3): 6.6051, 26.3430, 4608 m"}},
    {{0xFF, 0x88, 0x80, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00, 0x00}, 11, 24, 1, {"Location (channel 255): -838.8608, -838.8608, -83886 m"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x88}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x88, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}, 14, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // type 142, 1 bytes
    {{0x00, 0x8E, 0x00}, 3, 24, 1, {"Switch (channel 0): off"}},
    {{0xFF, 0x8E, 0xFF}, 3, 24, 1, {"Switch (channel 255): on"}},
    {{0x01, 0x8E, 0x80}, 3, 24, 1, {"Switch: on"}},
    {{0x01, 0x8E, 0x7F}, 3, 24, 1, {"Switch: on"}},
    {{0x03, 0x8E, 0x01}, 3, 24, 1, {"Switch (channel 3): on"}},
    {{0xFF, 0x8E, 0x80}, 3, 24, 1, {"Switch (channel 255): on"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x8E}, 6, 24, 2, {"Battery: 4.12 V", "(the answer is cut off)"}},
    // odd lengths and the more-values tail
    {{0x01}, 1, 24, 1, {"(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02, 0x88, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x64, 0x03, 0x68, 0x09, 0x04, 0x4D, 0x01, 0x02}, 26, 0, 0, {}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02, 0x88, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x64, 0x03, 0x68, 0x09, 0x04, 0x4D, 0x01, 0x02}, 26, 1, 1, {"5 more values not shown"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02, 0x88, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x64, 0x03, 0x68, 0x09, 0x04, 0x4D, 0x01, 0x02}, 26, 2, 2, {"Battery: 4.12 V", "4 more values not shown"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02, 0x88, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x64, 0x03, 0x68, 0x09, 0x04, 0x4D, 0x01, 0x02}, 26, 3, 3, {"Battery: 4.12 V", "Temperature: 21.5 \xC2""\xB0""C", "3 more values not shown"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02, 0x88, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x64, 0x03, 0x68, 0x09, 0x04, 0x4D, 0x01, 0x02}, 26, 4, 4, {"Battery: 4.12 V", "Temperature: 21.5 \xC2""\xB0""C", "Location (channel 2): 0.0001, 0.0001, 1 m", "2 more values not shown"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02, 0x88, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x64, 0x03, 0x68, 0x09, 0x04, 0x4D, 0x01, 0x02}, 26, 5, 5, {"Battery: 4.12 V", "Temperature: 21.5 \xC2""\xB0""C", "Location (channel 2): 0.0001, 0.0001, 1 m", "Humidity (channel 3): 4.5 %", "(a value of type 77 could not be read)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02, 0x88, 0x00, 0x00}, 12, 1, 1, {"3 more values not shown"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02, 0x88, 0x00, 0x00}, 12, 2, 2, {"Battery: 4.12 V", "2 more values not shown"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02, 0x88, 0x00, 0x00}, 12, 3, 3, {"Battery: 4.12 V", "Temperature: 21.5 \xC2""\xB0""C", "(the answer is cut off)"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02}, 9, 1, 1, {"2 more values not shown"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02}, 9, 2, 2, {"Battery: 4.12 V", "Temperature: 21.5 \xC2""\xB0""C"}},
    {{0x01, 0x74, 0x01, 0x9C, 0x01, 0x67, 0x00, 0xD7, 0x02}, 9, 3, 3, {"Battery: 4.12 V", "Temperature: 21.5 \xC2""\xB0""C", "(the answer is cut off)"}},
};

static uint8_t const KNOWN[] = {0,   1,   2,   3,   100, 101, 102, 103, 104, 113, 115, 116, 117,
                                118, 120, 121, 125, 128, 130, 131, 132, 133, 134, 136, 142};

static void lpp_pins(void) {
    for (size_t i = 0; i < sizeof(PINS) / sizeof(PINS[0]); i++) {
        lpp_pin_t const *p = &PINS[i];
        tl_line_t        l[TL_LINES_MAX];
        memset(l, 0x55, sizeof(l));
        size_t n = tl_lpp_lines(p->bytes, p->len, l, p->max);
        if (n != p->n) {
            printf("FAIL pin %zu: %zu lines, want %zu\n", i, n, p->n);
            failures++;
            continue;
        }
        for (size_t k = 0; k < n; k++) {
            if (strcmp(l[k].text, p->line[k]) != 0) {
                printf("FAIL pin %zu line %zu: \"%s\", want \"%s\"\n", i, k, l[k].text, p->line[k]);
                failures++;
            }
        }
    }
    // every other type is unknown: its length cannot be known, so the list ends there
    for (unsigned t = 0; t < 256; t++) {
        if (memchr(KNOWN, (int)t, sizeof(KNOWN))) {
            continue;
        }
        uint8_t   d[] = {1, (uint8_t)t, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        tl_line_t l[TL_LINES_MAX];
        char      want[TL_LINE_MAX];
        snprintf(want, sizeof(want), "(a value of type %u could not be read)", t);
        CHECK(tl_lpp_lines(d, sizeof(d), l, TL_LINES_MAX) == 1 && strcmp(l[0].text, want) == 0);
    }
}

static void rate(void) {
    tl_rate_t t;
    tl_rate_init(&t);
    CHECK(tl_rate_wait_s(&t, A, 1000) == 0);
    tl_rate_note(&t, A, 1000);
    CHECK(tl_rate_wait_s(&t, A, 1000) == 30);
    CHECK(tl_rate_wait_s(&t, A, 1001) == 30); // rounded up
    CHECK(tl_rate_wait_s(&t, A, 1000 + 29000) == 1);
    CHECK(tl_rate_wait_s(&t, A, 1000 + 30000) == 0);
    CHECK(tl_rate_wait_s(&t, B, 1000) == 0); // per contact
    // the clock wraps
    tl_rate_note(&t, B, 0xFFFFFF00u);
    CHECK(tl_rate_wait_s(&t, B, 0xFFFFFF00u + 5000) == 25);
    // a full table reuses the oldest slot, never forgets a recent one
    tl_rate_init(&t);
    for (uint8_t i = 0; i < TL_RATE_SLOTS; i++) {
        uint8_t p[6] = {i, 1, 1, 1, 1, 1};
        tl_rate_note(&t, p, 100u * (i + 1));
    }
    uint8_t extra[6] = {200, 1, 1, 1, 1, 1}, first[6] = {0, 1, 1, 1, 1, 1}, last[6] = {TL_RATE_SLOTS - 1, 1, 1, 1, 1, 1};
    tl_rate_note(&t, extra, 5000);
    CHECK(tl_rate_wait_s(&t, first, 5000) == 0);
    CHECK(tl_rate_wait_s(&t, last, 5000) > 0 && tl_rate_wait_s(&t, extra, 5000) == 30);
}

static void frames(void) {
    uint8_t pub[32], f[MF_MAX_FRAME];
    for (int i = 0; i < 32; i++) {
        pub[i] = (uint8_t)i;
    }
    CHECK(mf_encode_telemetry_req(f, pub) == 36 && f[0] == 39 && f[1] == 0 && f[3] == 0 && f[4] == 0 && f[35] == 31);
    uint8_t  push[] = {0x8B, 0, 1, 2, 3, 4, 5, 6, 1, 116, 0x01, 0x9C};
    mf_msg_t m;
    mf_decode(push, sizeof(push), &m);
    CHECK(m.kind == MF_TELEMETRY && m.telem_len == 4 && m.telem[1] == 116 && m.rprefix[5] == 6);
    mf_decode(push, 7, &m);
    CHECK(m.kind == MF_BAD);
}

static void request(void) {
    rr_t        r;
    char const *why = NULL;
    rr_init(&r);
    uint32_t seq = rr_request(&r, RR_TELEMETRY, A, NULL, 100, &why);
    CHECK(seq == 1 && rr_busy(&r));
    rr_op_t op;
    uint8_t prefix[6];
    char    arg[RR_ARG_MAX + 1];
    CHECK(rr_take(&r, &op, prefix, arg, sizeof(arg), 100) && op == RR_TELEMETRY);
    mf_msg_t sent = {.kind = MF_SENT, .flood = 0, .est_timeout = 4000};
    rr_on_reply(&r, &sent, 150);
    CHECK(r.state == RR_SENT);
    char text[128];
    rr_state_text(&r, text, sizeof(text));
    CHECK(strstr(text, "telemetry") && strstr(text, "waiting"));
    // an answer from someone else is not ours
    mf_msg_t ans = {.kind = MF_TELEMETRY, .telem_len = 4};
    memcpy(ans.rprefix, B, 6);
    CHECK(!rr_on_telemetry(&r, &ans, 200));
    memcpy(ans.rprefix, A, 6);
    uint8_t d[] = {1, 116, 0x01, 0x9C};
    memcpy(ans.telem, d, 4);
    CHECK(rr_on_telemetry(&r, &ans, 300) && r.state == RR_DONE && r.have_telem);
    char view[600];
    tl_view_text(&r, A, view, sizeof(view));
    CHECK(strcmp(view, "Battery: 4.12 V") == 0); // a good answer: values only, no state line
    // the next request times out: "No answer", the old values stay above with the reason
    CHECK(rr_request(&r, RR_TELEMETRY, A, NULL, 1000, &why) == 2);
    CHECK(rr_take(&r, &op, prefix, arg, sizeof(arg), 1000));
    rr_on_reply(&r, &sent, 1100);
    rr_poll(&r, 1100 + rr_deadline_ms(4000) - 1, NULL);
    CHECK(r.state == RR_SENT);
    rr_poll(&r, 1100 + rr_deadline_ms(4000) + 1, NULL);
    CHECK(r.state == RR_TIMEOUT);
    tl_view_text(&r, A, view, sizeof(view));
    CHECK(strstr(view, "Battery: 4.12 V") && strstr(view, "No answer:") && strstr(view, "last answer above"));
    tl_view_text(&r, B, view, sizeof(view)); // another contact's page never shows these values
    CHECK(strncmp(view, "No answer:", 10) == 0);
    // MyMesh refusing it ends it with the reason
    CHECK(rr_request(&r, RR_TELEMETRY, A, NULL, 9000, &why) == 3);
    CHECK(rr_take(&r, &op, prefix, arg, sizeof(arg), 9000));
    mf_msg_t err = {.kind = MF_ERR, .err = MF_ERR_NOT_FOUND};
    rr_on_reply(&r, &err, 9010);
    CHECK(r.state == RR_FAILED && strstr(r.why, "not found"));
}

int main(void) {
    lpp();
    lpp_pins();
    rate();
    frames();
    request();
    if (failures) {
        printf("mesh_telem_test: %d failures\n", failures);
        return 1;
    }
    printf("mesh_telem_test: ok\n");
    return 0;
}
