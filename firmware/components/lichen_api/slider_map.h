#pragma once

// The menu slider's value mapping (#344): LVGL's slider always runs 0..span and the row's value is min + position,
// so a row with a minimum (Display > Screen, 10..80) draws exactly like one that starts at 0.

// The widget position of a value, clamped to the row's range
int slider_map_pos(int min, int max, int value);
// The row's value of a widget position, clamped to the row's range
int slider_map_value(int min, int max, int pos);
// The size of one Left/Right step (at least 1), `steps` presses from end to end
int slider_map_step(int min, int max, int steps);
