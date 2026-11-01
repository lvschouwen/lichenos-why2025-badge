// "fw led" (#74): the bench's way to see the LED without raising notices
#include "fw_console.h"
#include "led.h"
#include "led_cmd.h"

#include <string.h>

static char const *const NAME[] = {"off", "on", "slow blink", "fast blink", "pulse"};

static void cmd_led(char const *args) {
    if (strcmp(args, "on") == 0) {
        led_set_override(LED_FORCE_ON);
    } else if (strcmp(args, "off") == 0) {
        led_set_override(LED_FORCE_OFF);
    } else if (strcmp(args, "blink") == 0) {
        led_set_override(LED_FORCE_BLINK);
    } else if (strcmp(args, "auto") == 0) {
        led_set_override(LED_AUTO);
    } else if (*args) {
        console_reply("led [on|off|blink|auto]");
        return;
    }
    console_reply("led: GPIO%d, setting %s, showing %s (led auto = normal behaviour)", LED_GPIO,
                  led_enabled() ? "on" : "off", NAME[led_current()]);
}

void led_register_console(void) {
    console_register("led", "led [on|off|blink|auto]", cmd_led);
}
