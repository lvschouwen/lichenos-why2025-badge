#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Text for the console and the screens (#61). Pure logic, no ESP headers.

// "just now", "42 s ago", "3 min ago", "2 h ago", "5 d ago"
void mesh_format_age(uint32_t age_s, char *out, size_t cap);
// A message's age (#48): a live one by uptime (now_s - heard_s); one loaded from the journal by the wall clock
// (now_epoch - epoch) when both are known, else "before restart"
void mesh_format_msg_age(bool loaded, uint32_t heard_s, uint32_t epoch, uint32_t now_s, uint32_t now_epoch, char *out,
                         size_t cap);
// MeshCore's advert types: "chat", "repeater", "room", "sensor", "?"
char const *mesh_type_name(uint8_t type);
// A name for the mesh (#67): spaces trimmed at both ends, control bytes dropped, cut to cap - 1 bytes on a UTF-8
// character boundary. false (out empty) when nothing is left.
bool mesh_name_clean(char const *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
