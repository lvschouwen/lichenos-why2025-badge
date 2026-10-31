#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

// Type-to-search on a menu (#32, #57): a line under the heading ("Type to search..." / "Search: jac (1 of 7)") and the
// rows that do not match are hidden, not rebuilt (so Up/Down and the caller's row pointers stay valid). Typing and
// Backspace come in through menu_on_text; Esc clears the query first (shell_on_esc), then leaves. Matching is the
// caller's (list_filter.h has the pieces).
//
// Call menu_filter_create() right after menu_create(), then menu_filter_add() for each row that can be filtered. The
// state is freed with the screen.
typedef struct menu_filter menu_filter_t;

// match(item, query, ctx): does item (the number given to menu_filter_add) show for this query; an empty query is
// never asked
typedef bool (*menu_filter_match_fn)(size_t item, char const *query, void *ctx);

menu_filter_t *menu_filter_create(lv_obj_t *screen, lv_obj_t *menu, menu_filter_match_fn match, void *ctx);
void           menu_filter_add(menu_filter_t *f, lv_obj_t *row, size_t item);
// The row that takes the focus when the focused one is hidden and nothing matches ("Custom..."); never hidden.
// Without one, nothing matching leaves nothing focused and Enter does nothing.
void           menu_filter_fallback(menu_filter_t *f, lv_obj_t *row);
