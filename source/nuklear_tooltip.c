/*
 * Tooltip with delay for the Nuklear GUI.
 *
 * Copyright 2019-2021 CompuPhase
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined _WIN32
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
# include <mmsystem.h>
#elif defined __linux__
# include <sys/time.h>
#endif
#include <stdbool.h>
#include <string.h>
#include "nuklear_tooltip.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif


/* timestamp() returns the timestamp (time since start-up) in ms */
unsigned long timestamp(void)
{
# if defined _WIN32
    static bool init = true;
    if (init) {
      timeBeginPeriod(1); /* force millisecond resolution */
      init = false;
    }
    return timeGetTime();
# else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return 1000 * tv.tv_sec + tv.tv_usec / 1000;
# endif
}

int tooltip(struct nk_context *ctx, struct nk_rect bounds, const char *text)
{
  static struct nk_rect recent_bounds;
  static unsigned long start_tstamp;
  unsigned long tstamp = timestamp();

  /* only a single popup may be active at the same time, but tooltips are also
     pop-ups -> disable tooltips if a popup is active */
  struct nk_window *win = ctx->current;
  struct nk_panel *panel = win->layout;
  if (panel->type & NK_PANEL_SET_POPUP)
    return 0;

  if (!nk_input_is_mouse_hovering_rect(&ctx->input, bounds))
    return 0;           /* not hovering this control/area */

  if (memcmp(&bounds, &recent_bounds, sizeof(struct nk_rect)) != 0) {
    /* hovering this control/area, but it's a different one from the previous;
       restart timer */
    recent_bounds = bounds;
    start_tstamp = tstamp;
    return 0;
  }

  if (tstamp - start_tstamp < TOOLTIP_DELAY
      || tstamp - start_tstamp > TOOLTIP_TIMEOUT)
    return 0;           /* delay time has not reached its value yet */

  if (text != NULL)
    nk_tooltip(ctx, text);

  return 1;
}

void label_tooltip(struct nk_context *ctx, const char *label, nk_flags align,
                   const char *tiptext)
{
  struct nk_rect bounds = nk_widget_bounds(ctx);
  nk_label(ctx, label, align);
  tooltip(ctx, bounds, tiptext);
}

nk_bool button_tooltip(struct nk_context *ctx, const char *title,
                       enum nk_keys hotkey, nk_bool enabled,
                       const char *tiptext)
{
  nk_bool result;
  struct nk_rect bounds = nk_widget_bounds(ctx);
  struct nk_style_button *style = &ctx->style.button;
  if (!enabled) {
    nk_style_push_color(ctx, &style->text_normal, style->text_disabled);
    nk_style_push_color(ctx, &style->text_hover, style->text_disabled);
    nk_style_push_color(ctx, &style->text_active, style->text_disabled);
    nk_style_push_color(ctx, &style->hover.data.color, style->normal.data.color);
    nk_style_push_color(ctx, &style->active.data.color, style->normal.data.color);
  }

  result = nk_button_label(ctx, title);
  if (tiptext != NULL)
    tooltip(ctx, bounds, tiptext);
  if (!result && hotkey != NK_KEY_NONE)
    result = nk_input_is_key_pressed(&ctx->input, hotkey);

  if (!enabled) {
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
    result = nk_false;  /* any input is to be ignored */
  }

  return result;
}

nk_bool button_symbol_tooltip(struct nk_context *ctx, enum nk_symbol_type symbol,
                              enum nk_keys hotkey, nk_bool enabled, const char *tiptext)
{
  struct nk_rect bounds = nk_widget_bounds(ctx);
  struct nk_style_button *style = &ctx->style.button;
  if (!enabled) {
    nk_style_push_color(ctx, &style->text_normal, style->text_disabled);
    nk_style_push_color(ctx, &style->text_hover, style->text_disabled);
    nk_style_push_color(ctx, &style->text_active, style->text_disabled);
    nk_style_push_color(ctx, &style->hover.data.color, style->normal.data.color);
    nk_style_push_color(ctx, &style->active.data.color, style->normal.data.color);
  }

  nk_bool result = nk_button_symbol(ctx, symbol);
  if (tiptext != NULL)
    tooltip(ctx, bounds, tiptext);
  if (!result && hotkey != NK_KEY_NONE)
    result = nk_input_is_key_pressed(&ctx->input, hotkey);

  if (!enabled) {
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
    nk_style_pop_color(ctx);
    result = nk_false;  /* any input is to be ignored */
  }

  return result;
}

nk_bool checkbox_tooltip(struct nk_context *ctx, const char *label, nk_bool *active, nk_flags align,
                         const char *tiptext)
{
  struct nk_rect bounds = nk_widget_bounds(ctx);
  nk_bool result = nk_checkbox_label(ctx, label, active, align);
  tooltip(ctx, bounds, tiptext);
  return result;
}

nk_bool option_tooltip(struct nk_context *ctx, const char *label, nk_bool active, nk_flags align,
                       const char *tiptext)
{
  struct nk_rect bounds = nk_widget_bounds(ctx);
  nk_bool result = nk_option_text(ctx, label, nk_strlen(label), active, align);
  tooltip(ctx, bounds, tiptext);
  return result;
}

nk_flags editctrl_tooltip(struct nk_context *ctx, nk_flags flags,
                          char *buffer, int max, nk_plugin_filter filter,
                          const char *tiptext)
{
  struct nk_rect bounds = nk_widget_bounds(ctx);
  nk_flags result = nk_edit_string_zero_terminated(ctx, flags, buffer, max, filter);
  tooltip(ctx, bounds, tiptext);
  return result;
}
