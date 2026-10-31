#pragma once
// Moves a key pair into MeshCore's DataStore (#65): true only when the saved copy reads back identical. MeshCore's
// save reports success once the file opened, even if the write failed (disk full, damaged FAT), and the caller erases
// the old copy on true, so a bad save must never pass. A template so the host test can hand in a lying store.
#include <Identity.h>

#include <string.h>

template <class Store> bool identity_move(Store &store, mesh::LocalIdentity id) {
    mesh::LocalIdentity check;
    uint8_t             want[PRV_KEY_SIZE + PUB_KEY_SIZE], got[sizeof(want)];
    size_t              n = id.writeTo(want, sizeof(want)); // the private key is only reachable this way
    return store.saveMainIdentity(id) && store.loadMainIdentity(check) && check.writeTo(got, sizeof(got)) == n &&
           memcmp(want, got, n) == 0;
}
