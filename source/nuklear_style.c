/*
 * Common styling & layout functions for the Nuklear GUI.
 *
 * Copyright 2021 CompuPhase
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

#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include "nuklear_style.h"

void nuklear_style(struct nk_context *ctx)
{
  struct nk_color table[NK_COLOR_COUNT];

  assert(ctx != NULL);

  table[NK_COLOR_TEXT] = nk_rgba(205, 201, 171, 255);
  table[NK_COLOR_WINDOW] = nk_rgba(35, 52, 71, 255);
  table[NK_COLOR_HEADER] = nk_rgba(58, 86, 117, 255);
  table[NK_COLOR_BORDER] = nk_rgba(128, 128, 128, 255);
  table[NK_COLOR_BUTTON] = nk_rgba(58, 86, 117, 255);
  table[NK_COLOR_BUTTON_HOVER] = nk_rgba(127, 23, 45, 255);
  table[NK_COLOR_BUTTON_ACTIVE] = nk_rgba(127, 23, 45, 255);
  table[NK_COLOR_TOGGLE] = nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_TOGGLE_HOVER] = nk_rgba(58, 86, 117, 255);
  table[NK_COLOR_TOGGLE_CURSOR] = nk_rgba(179, 175, 132, 255);
  table[NK_COLOR_SELECT] = nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_SELECT_ACTIVE] = nk_rgba(204, 199, 141, 255);
  table[NK_COLOR_SLIDER] = nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_SLIDER_CURSOR] = nk_rgba(179, 175, 132, 255);
  table[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgba(127, 23, 45, 255);
  table[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgba(127, 23, 45, 255);
  table[NK_COLOR_PROPERTY] = nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_EDIT] = nk_rgba(20, 29, 38, 225);
  table[NK_COLOR_EDIT_CURSOR] = nk_rgba(205, 201, 171, 255);
  table[NK_COLOR_COMBO] = nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_CHART] = nk_rgba(20, 29, 38, 255);
  table[NK_COLOR_CHART_COLOR] = nk_rgba(170, 40, 60, 255);
  table[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgba(255, 0, 0, 255);
  table[NK_COLOR_SCROLLBAR] = nk_rgba(30, 40, 60, 255);
  table[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgba(179, 175, 132, 255);
  table[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgba(204, 199, 141, 255);
  table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgba(204, 199, 141, 255);
  table[NK_COLOR_TAB_HEADER] = nk_rgba(58, 86, 117, 255);
  table[NK_COLOR_TOOLTIP] = nk_rgba(204, 199, 141, 255);
  table[NK_COLOR_TOOLTIP_TEXT] = nk_rgba(35, 52, 71, 255);

  nk_style_from_table(ctx, table);

  /* button */
  ctx->style.button.rounding = 0;
  ctx->style.button.padding.x = 2;
}

float *nk_ratio(int count, ...)
{
  #define MAX_ROW_FIELDS 10
  static float r_array[MAX_ROW_FIELDS];
  va_list ap;
  int i;

  assert(count < MAX_ROW_FIELDS);
  va_start(ap, count);
  for (i = 0; i < count; i++)
    r_array[i] = (float) va_arg(ap, double);
  va_end(ap);
  return r_array;
}

