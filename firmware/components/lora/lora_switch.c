// The LoRa radio switch's state and its owner (#108, #222). Pure: no ESP headers, so the host tests link it as is.
#include "lora_os.h"
#include "lora_switch.h"

#include <stddef.h>

static bool want_on = true; // the OS radio switch, read by lora_send and the LoRa task on every turn
// Set by main's boot task before lora_start registers the console command that calls it: console_register publishes
// the command with a release store (n_commands), which orders this write before the console task's read. Read-only
// after that: no lock
static void (*owner)(bool on);

void lora_set_switch_owner(void (*request)(bool on)) {
    owner = request;
}

void lora_set_enabled(bool on) {
    __atomic_store_n(&want_on, on, __ATOMIC_RELEASE);
}

bool lora_enabled(void) {
    return __atomic_load_n(&want_on, __ATOMIC_ACQUIRE);
}

bool lora_switch_request(bool on) {
    if (owner) {
        owner(on); // saves the switch, and its listener calls lora_set_enabled before this returns
    } else {
        lora_set_enabled(on);
    }
    return lora_enabled();
}
