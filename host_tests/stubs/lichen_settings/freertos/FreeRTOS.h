// Host stand-in for FreeRTOS.h, for lichen_settings_test.c: single-threaded, so the critical sections are empty
#pragma once

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m)        ((void)(m))
#define portEXIT_CRITICAL(m)         ((void)(m))
