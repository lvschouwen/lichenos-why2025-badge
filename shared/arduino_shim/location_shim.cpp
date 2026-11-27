// The badge has no GPS: MeshCore's CommonGpsPrefs.cpp (linked for the prefs commands) names LocationProvider, whose
// key virtual upstream leaves to the GPS drivers. Defining it here gives the vtable a home; nothing derives from it (#589).
#include <Arduino.h>
#include <helpers/sensors/LocationProvider.h>

void LocationProvider::sendSentence(const char *) {}
