#pragma once
#include "lvgl.h"

/* Show a full-screen modal overlay with a spinner + message.
   Safe to call from any task (acquires LVGL lock internally). */
void ui_loading_show(const char *message);

/* Update the text of the currently-shown loading overlay. No-op if not shown. */
void ui_loading_set_text(const char *message);

/* Dismiss the overlay. Safe to call from any task and when nothing is shown. */
void ui_loading_hide(void);