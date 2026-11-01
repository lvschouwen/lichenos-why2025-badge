#pragma once

#include "mesh.h"

// A contact's details and actions (#49): opened with Enter on the Contacts, Repeaters or Heard list (#88). Shows the
// type, key, last heard and route; Message (companions only, the first row, so Enter Enter still starts a DM), Reset
// path, Favourite, Share (the meshcore:// link as text) and Remove (confirmed first). LVGL task only; Esc goes back.
void meshcore_contact_open(mesh_contact_t const *c);
