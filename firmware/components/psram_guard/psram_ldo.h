#pragma once

// "fw psram ldo [<mV> | raw <dref> <mul>]": show or move the PSRAM supply (LDO VO2) until the next reboot (#86 diagnosis)
void psram_ldo_cmd(char const *args);
