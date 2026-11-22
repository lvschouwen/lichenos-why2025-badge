#pragma once

// The version of the app API (#521): what an app built against it may call. MAJOR changes when a call is removed or
// changes meaning, MINOR when one is added. An SD app declares the version it was built for in its manifest;
// lichen_api_compatible() (lichen_manifest.h) decides whether this OS runs it.
#define LICHEN_API_VERSION_MAJOR 1
#define LICHEN_API_VERSION_MINOR 0
// "1.0", for the `fw version` reply (GET /info) and logs
#define LICHEN_API_VERSION_STR2(x) #x
#define LICHEN_API_VERSION_STR(x)  LICHEN_API_VERSION_STR2(x)
#define LICHEN_API_VERSION_TEXT \
    LICHEN_API_VERSION_STR(LICHEN_API_VERSION_MAJOR) "." LICHEN_API_VERSION_STR(LICHEN_API_VERSION_MINOR)
