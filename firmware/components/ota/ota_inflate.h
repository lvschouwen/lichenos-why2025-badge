// A compressed upload body (#298), decoded with a hard cap on what it may produce. The decoder itself is passed in (the
// ROM's tinfl on the badge, zlib in host_tests/ota_inflate_test.c); this part owns the rules a stream from the network
// must keep: no more than cap bytes out, checked before a byte reaches the sink; nothing after the end of the stream;
// no step that makes no progress. Pure, host-tested under ASan+UBSan.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OTA_INFLATE_NEEDS_INPUT, // every input byte taken, the stream goes on
    OTA_INFLATE_HAS_OUTPUT,  // more output waits: call again, with or without input
    OTA_INFLATE_DONE,        // the stream ended (its check value matched)
    OTA_INFLATE_ERROR,       // not a valid stream
} ota_inflate_status_t;

// One decoder step: takes up to *in_len bytes of in (sets *in_len to what it took) and sets *out / *out_len to the bytes
// it produced (they stay valid until the next step).
typedef ota_inflate_status_t (*ota_inflate_step_fn)(void *dec, uint8_t const *in, size_t *in_len, uint8_t const **out,
                                                    size_t *out_len);
// Where decoded bytes go; false stops the stream
typedef bool (*ota_inflate_sink_fn)(void *ctx, uint8_t const *data, size_t len);

typedef struct {
    ota_inflate_step_fn step;
    void               *dec;
    size_t              cap;      // at most this many bytes out
    size_t              produced; // bytes handed to the sink so far
    bool                done;     // the stream ended
    bool                failed;   // refused: every later feed fails too
} ota_inflate_t;

// Start a stream into a decoder already reset by the caller
void ota_inflate_begin(ota_inflate_t *z, ota_inflate_step_fn step, void *dec, size_t cap);
// Feed len bytes of the stream. False (and failed from then on) for a decoder error, output past cap, bytes after the
// end of the stream, a step without progress, or a sink that refused.
bool ota_inflate_feed(ota_inflate_t *z, uint8_t const *in, size_t len, ota_inflate_sink_fn sink, void *ctx);
// The stream ended cleanly and produced exactly want bytes (a truncated stream or a short piece is refused)
bool ota_inflate_complete(ota_inflate_t const *z, size_t want);
