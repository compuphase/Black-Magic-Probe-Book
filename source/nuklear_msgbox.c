/*
 * A Nuklear control for a g_message box (which, by the design of Nuklear, is not
 * truly modal).
 *
 * Copyright 2024 CompuPhase
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
#include <string.h>
#include "guidriver.h"
#include "nuklear_msgbox.h"
#include "nuklear_style.h"

#define BTN_WIDTH     48
#define NK_SPACING    4
#define ROW_SPACING   2
#define MIN_WIDTH     (3 * BTN_WIDTH)

static bool is_active = false;
static char *g_message = NULL;
static char *g_caption = NULL;
static float row_height = 0;
static float font_height = 0;
static struct nk_rect popup_rc;

/** nk_msgbox() sets up a message box. To show the message box, you must call
 *  nk_msgbox_popup() from within the GUI loop.
 */
bool nk_msgbox(struct nk_context *ctx, const char *message, const char *caption)
{
  assert(ctx != NULL);
  assert(message != NULL);
  assert(caption != NULL);

  /* initialize fields */
  const struct nk_style *style = &ctx->style;
  struct nk_vec2 padding = style->window.padding;
  font_height = style->font->height;
  row_height = 1.6f * font_height;
  g_message = strdup(message);
  if (g_message == NULL)
    return false;
  g_caption = strdup(caption);
  if (g_caption == NULL) {
    free(g_message);
    return false;
  }

  /* calculate text size */
  float text_width = 0;
  int num_lines = 0;
  char *head, *tail;
  for (head = g_message; *head != '\0'; head = tail) {
    tail = strchr(head, '\n');
    if (!tail)
      tail = strchr(head, '\0');
    float width = style->font->width(style->font->userdata,
                                     style->font->height, head,
                                     (int)(tail - head));
    if (width > text_width)
      text_width = width;
    if (*tail == '\n')
      tail++;
    num_lines++;
  }

  int canvas_width, canvas_height;
  guidriver_appsize(&canvas_width, &canvas_height);

  float contents_width = text_width + 3 * NK_SPACING;
  if (contents_width < MIN_WIDTH)
    contents_width = MIN_WIDTH;
  float contents_height = num_lines * (font_height + 2 * padding.y) + row_height + ROW_SPACING + 5 * NK_SPACING;
  if (caption != NULL)
    contents_height += font_height + NK_SPACING + ROW_SPACING + 4 * NK_SPACING;
  popup_rc = nk_rect((canvas_width - contents_width) / 2,
                     (canvas_height - contents_height) / 2,
                     contents_width, contents_height);
  is_active = true;

  return true;
}

/** nk_msgbox_popup() displays a message box, if one was set up and active.
 *  \return While the message box is active, the return value is MSGBOX_ACTIVE.
 *          When the message box is closed, the function returns the
 *          MSGBOX_CLOSE once, and after that it returns MSGBOX_INACTIVE.
 */
int nk_msgbox_popup(struct nk_context *ctx)
{
  if (!is_active)
    return MSGBOX_INACTIVE;

  int msgbox_result = MSGBOX_ACTIVE;

  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0_S);
  nk_style_push_color(ctx, &ctx->style.window.popup_border_color, COLOUR_BG_YELLOW);
  nk_style_push_float(ctx, &ctx->style.window.popup_border, 2);
  if (nk_popup_begin(ctx, NK_POPUP_STATIC, "MsgBox", NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER, popup_rc)) {
    struct nk_rect widgetbounds = nk_layout_widget_bounds(ctx);

    if (g_caption != NULL) {
      nk_layout_row_dynamic(ctx, font_height + NK_SPACING, 1);
      nk_layout_row_background(ctx, COLOUR_BG_BLUE);
      int font = guidriver_setfont(ctx, FONT_BOLD);
      nk_text_colored(ctx, g_caption, strlen(g_caption), NK_TEXT_ALIGN_BOTTOM|NK_TEXT_ALIGN_CENTERED, COLOUR_BG0);
      guidriver_setfont(ctx, font);
      nk_layout_row_dynamic(ctx, ROW_SPACING, 1);
      nk_spacing(ctx, 1);
    }

    const char *head, *tail;
    for (head = g_message; *head != '\0'; head = tail) {
      tail = strchr(head, '\n');
      if (!tail)
        tail = strchr(head, '\0');
      nk_layout_row_dynamic(ctx, font_height, 1);
      nk_text(ctx, head, (int)(tail - head), NK_TEXT_LEFT);
      if (*tail == '\n')
        tail++;
    }

    nk_layout_row_dynamic(ctx, ROW_SPACING, 1);
    nk_spacing(ctx, 1);
    nk_layout_row_begin(ctx, NK_STATIC, row_height, 2);
    float spacewidth = widgetbounds.w - BTN_WIDTH - NK_SPACING;
    nk_layout_row_push(ctx, spacewidth);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, BTN_WIDTH);
    if (nk_button_label(ctx, "Close") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ESCAPE)) {
      is_active = false;
      msgbox_result = MSGBOX_CLOSE;
      free(g_message);
      g_message = NULL;
      free(g_caption);
      g_caption = NULL;
      nk_popup_close(ctx);
    }
    nk_layout_row_end(ctx);

    nk_popup_end(ctx);
  }
  nk_style_pop_float(ctx);
  nk_style_pop_color(ctx);
  nk_style_pop_color(ctx);

  return msgbox_result;
}

