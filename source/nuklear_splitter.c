/*
 * Support functions for splitter bars.
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
#include <stdlib.h>
#include "nuklear_splitter.h"

/** nk_splitter_init() initializes the values of the structure. It must be
 *  called before any of the other functions.
 *
 *  \param splitter   The splitter data structure.
 *  \param size       The width (horizontal splitter) or height (vertical
 *                    splitter) of the group/panelwindow in pixels.
 *  \param barsize    Size of the splitter bar in pixels.
 *  \param ratio      The initial position of the splitter bar, as a ratio of
 *                    size; it must be between 0.0 and 1.0.
 */
void nk_splitter_init(SPLITTERBAR *splitter, float size, float barsize, float ratio)
{
  assert(splitter != NULL);
  assert(ratio>=0.0 && ratio <= 1.0);
  splitter->size = size;
  splitter->ratio = ratio;
  splitter->barsize = barsize;
  splitter->hover = nk_false;
  splitter->dragging = nk_false;
  splitter->ratio_new = ratio;
}

static float clamp_ratio(float ratio)
{
  if (ratio < 0.0)
    ratio = 0.0;
  else if (ratio > 1.0)
    ratio = 1.0;
  return ratio;
}

/** nk_splitter_resize() must be called when the width of the parent
 *  group/panel/window changes. Parameter size is the new width or height (in
 *  pixels); parameter resize_part indicates which column must grow or shrink.
 *  This function does nothing if the size parameter does not change.
 */
void nk_splitter_resize(SPLITTERBAR *splitter, float size, int resize_part)
{
  assert(splitter != NULL);
  splitter->ratio = splitter->ratio_new;
  float delta = size - splitter->size;
  if (delta < -0.5 || delta > 0.5) {
    /* size changed, adjust splitter */
    float ratio = splitter->ratio;
    if (resize_part == RESIZE_TOPLEFT) {
      float topleft_size = (splitter->size - splitter->barsize) * ratio;        /* current (pixel) size */
      ratio = (topleft_size + delta) / size;                                    /* adjusted (relative) size */
    } else if (resize_part == RESIZE_BOTTOMRIGHT) {
      float btmright_sze = (splitter->size - splitter->barsize) * (1.0 - ratio);/* current (pixel) size */
      ratio = 1.0 - (btmright_sze + delta) / size;                              /* adjusted (relative) size */
    }
    ratio = clamp_ratio(ratio);
    nk_splitter_init(splitter, size, splitter->barsize, ratio);
  }
}

/** nk_hsplitter_layout() must be called instead of nk_layout_row() for a
 *  group that is split horizontally (the splitter bar is vertical).
 */
void nk_hsplitter_layout(struct nk_context *ctx, SPLITTERBAR *splitter, float height)
{
  assert(ctx != NULL);
  assert(splitter != NULL);
  assert(splitter->ratio >= 0.0 && splitter->ratio <= 1.0);
  splitter->parts[0] = nk_hsplitter_colwidth(splitter, 0);
  splitter->parts[1] = splitter->barsize;
  splitter->parts[2] = nk_hsplitter_colwidth(splitter, 1);
  nk_layout_row(ctx, NK_STATIC, height, 3, splitter->parts);
}

float nk_hsplitter_colwidth(SPLITTERBAR *splitter, int column)
{
  assert(splitter != NULL);
  assert(splitter->ratio >= 0.0 && splitter->ratio <= 1.0);
  assert(column == 0 || column == 1);
  float size = (splitter->size - splitter->barsize) * splitter->ratio;
  if (column != 0)
    size = (splitter->size - splitter->barsize) - size;
  return size;
}

/** nk_hsplitter() draws the splitter bar and handles dragging the bar with
 *  the mouse.
 */
void nk_hsplitter(struct nk_context *ctx, SPLITTERBAR *splitter)
{
  assert(ctx != NULL);
  assert(splitter != NULL);
  struct nk_rect bounds = nk_widget_bounds(ctx);
  splitter->hover = nk_input_is_mouse_hovering_rect(&ctx->input, bounds);

  nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_VERTICAL | NK_SYMBOL_REPEAT(3));

  if (splitter->hover && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
    splitter->dragging = nk_true;   /* if mouse inside the splitter bar and left button down -> dragging */
  else if (splitter->dragging && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
    splitter->dragging = nk_false;  /* if left button up -> not dragging */

  if (splitter->dragging) {
    float content_size = splitter->size - splitter->barsize;
    if (content_size > 0.0) {
      float left_col_size = content_size * splitter->ratio;                     /* current (pixel) size */
      float ratio = (left_col_size + ctx->input.mouse.delta.x) / content_size;  /* adjusted (relative) size */
      splitter->ratio_new = clamp_ratio(ratio);
    } else {
      splitter->ratio_new = 0.0;
    }
  }
}

float nk_vsplitter_rowheight(SPLITTERBAR *splitter, int row)
{
  assert(splitter != NULL);
  assert(splitter->ratio >= 0.0 && splitter->ratio <= 1.0);
  assert(row == 0 || row == 1);
  float size = (splitter->size - splitter->barsize) * splitter->ratio;
  if (row != 0)
    size = (splitter->size - splitter->barsize) - size;
  return size;
}

/** nk_vsplitter() draws the splitter bar and handles dragging the bar with the
 *  mouse.
 */
void nk_vsplitter(struct nk_context *ctx, SPLITTERBAR *splitter)
{
  assert(ctx != NULL);
  assert(splitter != NULL);

  nk_layout_row_dynamic(ctx, splitter->barsize, 1);
  struct nk_rect bounds = nk_widget_bounds(ctx);
  splitter->hover = nk_input_is_mouse_hovering_rect(&ctx->input, bounds);

  nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_REPEAT(3));

  if (splitter->hover && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
    splitter->dragging = nk_true;   /* if mouse inside the splitter bar and left button down -> dragging */
  else if (splitter->dragging && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
    splitter->dragging = nk_false;  /* if left button up -> not dragging */

  if (splitter->dragging) {
    float content_size = splitter->size - splitter->barsize;
    if (content_size > 0.0) {
      float top_row_size = content_size * splitter->ratio;                    /* current (pixel) size */
      float ratio = (top_row_size + ctx->input.mouse.delta.y) / content_size; /* adjusted (relative) size */
      splitter->ratio_new = clamp_ratio(ratio);
    } else {
      splitter->ratio_new = 0.0;
    }
  }
}

void nk_sizer_init(SIZERBAR *sizer, float size, float minsize, float bar_width)
{
  assert(sizer != NULL);
  sizer->size = size;
  sizer->minsize = minsize;
  sizer->barsize = bar_width;
  sizer->hover = nk_false;
  sizer->dragging = nk_false;
  sizer->size_new = size;
}

void nk_sizer_refresh(SIZERBAR *sizer)
{
  assert(sizer != NULL);
  sizer->size = sizer->size_new;
}

void nk_sizer(struct nk_context *ctx, SIZERBAR *sizer)
{
  assert(ctx != NULL);
  assert(sizer != NULL);

  nk_layout_row_dynamic(ctx, sizer->barsize, 1);
  struct nk_rect bounds = nk_widget_bounds(ctx);
  sizer->hover = nk_input_is_mouse_hovering_rect(&ctx->input, bounds);

  nk_symbol(ctx, NK_SYMBOL_CIRCLE_SOLID, NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_MIDDLE | NK_SYMBOL_REPEAT(3));

  if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds) && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
    sizer->dragging = nk_true;
  else if (sizer->dragging && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
    sizer->dragging = nk_false;

  if (sizer->dragging) {
    sizer->size_new = sizer->size_new + ctx->input.mouse.delta.y;
    if (sizer->size_new < sizer->minsize)
      sizer->size_new = sizer->minsize;
  }
}

