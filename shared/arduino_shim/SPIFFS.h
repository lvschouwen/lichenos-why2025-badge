// MeshCore's ESP32 code includes <SPIFFS.h>; on the badge "SPIFFS" is the LittleFS drive's /storage/meshcore (#65)
#pragma once

#include "FS.h"

extern fs::SPIFFSFS SPIFFS;
