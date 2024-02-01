/*
 * A Nuklear control for displaying help texts in the QuickGuide Markup format
 * (a flavour of Markdown).
 *
 * Copyright 2022-2024 CompuPhase
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
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "guidriver.h"
#include "nuklear_guide.h"
#include "nuklear_style.h"
#include "nuklear_tooltip.h"
#include "qglib.h"
#include "quickguide.h"

#if defined WIN32 || defined _WIN32
# if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
#   include "strlcpy.h"
# endif
#elif defined __linux__
# include <bsd/string.h>
#endif

#if defined FORTIFY
# include <alloc/fortify.h>
#endif


#if !defined sizearray
# define sizearray(e)   (sizeof(e) / sizeof((e)[0]))
#endif


static uint32_t qg_topic = ~0;
static uint32_t qg_contextmask = 0;
static QG_HISTORY history;
#define TOPIC_STACK   8

#define INDENTSIZE    24
#define EXTRAMARGIN   5 /* must be bigger than NK_SPACING */
#define NK_SPACING    4
#define CELL_SPACING  8 /* extra spacing between table cells (added to left & right of each cell) */
#define MAX_COLUMNS   16

#define HARDSPACE     "\xc2\xa0"

#if 0 //????
typedef struct TEXTBUFFER {
  char *buffer;
  size_t size;
} TEXTBUFFER;

static void textbuffer_clear(TEXTBUFFER *txt)
{
  assert(txt != NULL);
  if (txt->buffer != NULL)
    free(txt->buffer);
  txt->buffer = NULL;
  txt->size = 0;
}

static bool textbuffer_append(TEXTBUFFER *txt, const char *str)
{
  assert(txt != NULL);
  assert(str != NULL);
  size_t len = (txt-buffer != NULL) ? strlen(txt->buffer) : 0;
  size_t needed = len + strlen(str) + 1;
  if (needed < txt->size) {
    int newsize = (txt->size == 0) ? 256 : 2*txt->size;
    while (newsize < txt->size)
      newsize *= 2;
    char *newbuf = malloc(newsize * sizeof(char));
    if (newbuf != NULL) {
      if (txt->buffer != NULL) {
        strcpy(newbuf, txt->buffer);
        free(txt->buffer);
      } else {
        *newbuf = '\0';
      }
      txt->buffer = newbuf;
      txt->size = newsize;
    }
  }
  if (len + strlen(str) >= txt->size)
    return false;
  strcat(txt->buffer, str);
  return true;
}
#endif  //???

static int collectcolumns(struct nk_context *ctx, const QG_LINE_RECORD* content, int maxrows, unsigned *columns)
{
  assert(maxrows > 0);
  assert(content && content->type == QPAR_TABLE);

  struct nk_user_font const *font = ctx->style.font;
  assert(font != NULL && font->width != NULL);

  assert(columns != NULL);
  for (int idx = 0; idx < MAX_COLUMNS; idx++)
    columns[idx] = 0;

  int count=0;
  for (int r = 0; r < maxrows && content->type == QPAR_TABLE; r++) {
    const char *utf8 = (const char*)content + sizeof(QG_LINE_RECORD) + content->fmtcodes * sizeof(QG_FORMATCODE);
    const QG_FORMATCODE *fmtcode = (const QG_FORMATCODE*)((const char*)content + sizeof(QG_LINE_RECORD));
    unsigned column = 0;
    unsigned start = 0;
    for (unsigned fmtidx = 0; fmtidx < content->fmtcodes; fmtidx++) {
      if (fmtcode[fmtidx].type == QFMT_COLBREAK || fmtcode[fmtidx].type == QFMT_SENTINEL) {
        unsigned stop = fmtcode[fmtidx].pos;
        int textwidth = (int)font->width(font->userdata, font->height, utf8 + start, stop - start);
        if (textwidth > columns[column])
            columns[column] = textwidth;
        column++;
      }
    }
    if (column>count)
      count=column;
    content=(const QG_LINE_RECORD*)((const unsigned char*)content+content->size);
  }

  return count;
}

/** guide_widget() draws the text in the log window, with support for minimal
 *  formatting.
 */
static float guide_widget(struct nk_context *ctx, const char *id, float fontsize,
                          const char *guide, QG_LINK *links)
{
  const QG_TOPICHDR *topichdr = qg_topic_by_id(guide, qg_topic);
  if (!topichdr)
    return false;
  const QG_LINE_RECORD *content = (const QG_LINE_RECORD *)(guide + topichdr->content_offs);

  float pagebottom = 0.0;
  float pagetop = 0.0;
  int cur_fonttype = FONT_STD;

  /* black background on group */
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, id, NK_WINDOW_BORDER)) {
    struct nk_rect rcline = nk_layout_widget_bounds(ctx); /* get the line width and y-start (to calculate page height) */
    float pagewidth = rcline.w - 2*NK_SPACING;
    pagetop = rcline.y;
    unsigned table_columns[MAX_COLUMNS];
    int table_column_count = 0;
    for (unsigned row = 0; row < topichdr->content_count; row++) {
      /* handle context */
      if (!qg_passcontext(content, qg_contextmask)) {
        content = (const QG_LINE_RECORD*)((const unsigned char*)content + content->size);
        continue;
      }

      /* add vertical space between paragraphs */
      if (content->type == QPAR_HEADING && row > 0) {
        nk_layout_row_dynamic(ctx, fontsize, 1);;
        nk_spacing(ctx, 1);
      } else if (content->flags & QFLG_VSPACE) {
        nk_layout_row_dynamic(ctx, 2, 1);;
        nk_spacing(ctx, 1);
      }

      /* horizontal line must be handled in a special way */
      if (content->type == QPAR_HLINE) {
        nk_layout_row(ctx, NK_DYNAMIC, 1, 3, nk_ratio(3, 0.025, 0.95, 0.025));
        nk_spacing(ctx, 1);
        nk_rule_horizontal(ctx, COLOUR_TEXT, nk_false);
        nk_spacing(ctx, 1);
        content = (const QG_LINE_RECORD*)((const unsigned char*)content + content->size);
        continue;
      }

      /* set base font and text colour, based on paragraph type */
      int fonttype = FONT_STD;
      struct nk_color clr = COLOUR_TEXT;
      if (content->type == QPAR_HEADING) {
        switch (content->param) {
        case 0:
          fonttype = FONT_HEADING1;
          break;
        case 1:
          fonttype = FONT_HEADING2;
          break;
        case 2:
          clr = COLOUR_FG_GREEN;
          break;
        }
      } else if (content->type == QPAR_PREFMT) {
        fonttype = FONT_MONO;
        clr = COLOUR_FG_AQUA;
      }

      /* also check first format code, because inline formatting is currently not supported */
      int linkTopic = QG_INVALID_LINK;
      const QG_FORMATCODE *fmtcode = (const QG_FORMATCODE*)((const char*)content + sizeof(QG_LINE_RECORD));
      assert(content->fmtcodes > 0);
      switch (fmtcode[0].type) {
      case QFMT_LINK:
        clr = COLOUR_HIGHLIGHT;
        linkTopic = fmtcode[0].param;
        break;
      case QFMT_STYLE:
        clr = COLOUR_FG_YELLOW; //??? should make a difference between emphasized, strong & monospaced
        break;
      }

      if (cur_fonttype != fonttype) {
        cur_fonttype = fonttype;
        guidriver_setfont(ctx, cur_fonttype);
      }
      const char *utf8 = (const char*)content + sizeof(QG_LINE_RECORD) + content->fmtcodes * sizeof(QG_FORMATCODE);
      struct nk_user_font const *font = ctx->style.font;
      assert(font != NULL && font->width != NULL);

      /* for a table, no wrap-around in the cells */
      if (content->type == QPAR_TABLE) {
        if (table_column_count == 0)
          table_column_count = collectcolumns(ctx, content, topichdr->content_count - row, table_columns);
        if (content->param == 1)
          clr = COLOUR_FG_YELLOW;
        nk_layout_row_begin(ctx, NK_STATIC, font->height, 1 + table_column_count);
        nk_layout_row_push(ctx, (EXTRAMARGIN + CELL_SPACING - NK_SPACING));
        nk_spacing(ctx, 1);
        unsigned column = 0;
        unsigned start = 0;
        for (unsigned fmtidx = 0; fmtidx < content->fmtcodes; fmtidx++) {
          if (fmtcode[fmtidx].type == QFMT_COLBREAK || fmtcode[fmtidx].type == QFMT_SENTINEL) {
            unsigned stop = fmtcode[fmtidx].pos;
            nk_layout_row_push(ctx, table_columns[column] + 2 * CELL_SPACING);
            nk_text_colored(ctx, utf8 + start, stop - start, NK_TEXT_LEFT, clr);
            column++;
          }
        }
        nk_layout_row_end(ctx);
      } else {
        table_column_count = 0; /* not in table, reset variable (if needed) */
        unsigned start = 0;
        while (utf8[start] != '\0') {
          /* add indent and markers for bullet lists/numbered lists */
          float lineheight = (fonttype == FONT_HEADING1 || fonttype == FONT_HEADING2) ? 2*font->height : font->height;
          nk_layout_row_begin(ctx, NK_STATIC, lineheight, 2 + content->indent);
          nk_layout_row_push(ctx, (EXTRAMARGIN - NK_SPACING));
          nk_spacing(ctx, 1);
          for (int indent = 0; indent < content->indent; indent++) {
            nk_layout_row_push(ctx, INDENTSIZE - NK_SPACING);
            if (content->type == QPAR_ULIST && indent == content->indent - 1 && start == 0) {
              nk_symbol(ctx, (indent == 0) ? NK_SYMBOL_CIRCLE_SOLID_SMALL : NK_SYMBOL_CIRCLE_OUTLINE_SMALL, NK_TEXT_CENTERED);
            } else if (content->type == QPAR_OLIST && indent == content->indent - 1 && start == 0) {
              char str[20];
              sprintf(str, "%d.", content->param);
              nk_label(ctx, str, NK_TEXT_RIGHT);
            } else if (linkTopic != QG_INVALID_LINK && indent == content->indent - 1 && start == 0) {
              nk_symbol_colored(ctx, NK_SYMBOL_LINK_ALT, NK_TEXT_RIGHT, COLOUR_HIGHLIGHT);
            } else {
              nk_spacing(ctx, 1);
            }
          }
          float textwidth = pagewidth - content->indent * INDENTSIZE;
          unsigned cutpoint = start;
          if (content->type == QPAR_PREFMT) {
            cutpoint = strlen(utf8);
            textwidth = font->width(font->userdata, font->height, utf8, cutpoint);
          } else {
            /* search wrap-point */
            unsigned stop = start;
            for ( ;; ) {
              while (utf8[stop] != ' ' && utf8[stop] != '\0')
                stop++;
              float width = font->width(font->userdata, font->height, utf8 + start, stop - start);
              if (width >= textwidth)
                break;          /* width exceeds margins, use previous cut-point */
              cutpoint = stop;
              if (utf8[stop] == '\0')
                break;
              if (utf8[stop]==' ')
                stop++;
            }
            if (cutpoint == start)
              cutpoint = stop;  /* a non-breaking word is wider than the page */
          }
          /* set the text */
          nk_layout_row_push(ctx, textwidth);
          rcline = nk_layout_widget_bounds(ctx);
          nk_text_colored(ctx, utf8 + start, cutpoint - start, NK_TEXT_LEFT, clr);
          nk_layout_row_end(ctx);
          if (linkTopic != QG_INVALID_LINK && links != NULL)
            qg_link_set(links, rcline.x, rcline.y, rcline.x + rcline.w, rcline.y + rcline.h, linkTopic);
          if (pagebottom<rcline.y+rcline.h)
            pagebottom = rcline.y + rcline.h;
          start = cutpoint;
          if (utf8[start] == ' ')
            start++;
        }
      }

      content = (const QG_LINE_RECORD*)((const unsigned char*)content + content->size);
    }
    nk_group_end(ctx);
  }
  if (cur_fonttype != FONT_STD)
    guidriver_setfont(ctx, FONT_STD);
  nk_style_pop_color(ctx);
  return pagebottom - pagetop;
}

bool nk_guide(struct nk_context *ctx, struct nk_rect *viewport, float fontsize,
              const char *content, uint32_t topic)
{
  assert(ctx != NULL);
  assert(viewport != NULL);
  assert(content != NULL);

  if (qg_topic == ~0) {
    qg_history_init(&history, TOPIC_STACK);
    qg_topic = topic;
  }
  qg_history_push(&history, qg_topic);
  QG_LINK links = { NULL };

  if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Guide", NK_WINDOW_NO_SCROLLBAR, *viewport)) {
    #define ROW_HEIGHT  (2*fontsize)
    nk_layout_row_dynamic(ctx, viewport->h - ROW_HEIGHT - fontsize, 1);
    struct nk_rect widgetbounds = nk_widget_bounds(ctx);
    float pageheight = guide_widget(ctx, "guide_widget", fontsize, content, &links);

    /* button bar: optional back and forward buttons, plus a close button */
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 4);
    nk_layout_row_push(ctx, ROW_HEIGHT);
    if (button_symbol_tooltip(ctx, NK_SYMBOL_TRIANGLE_LEFT, NK_KEY_BACKSPACE, qg_history_goback(&history,NULL,NULL), "Go Back")) {
      qg_history_goback(&history, &qg_topic, NULL);
      nk_group_set_scroll(ctx, "guide_widget", 0, 0);
    }
    nk_layout_row_push(ctx, ROW_HEIGHT);
    if (button_symbol_tooltip(ctx, NK_SYMBOL_TRIANGLE_RIGHT, NK_KEY_NONE, qg_history_goforward(&history,NULL,NULL), "Go Forward")) {
      qg_history_goforward(&history, &qg_topic, NULL);
      nk_group_set_scroll(ctx, "guide_widget", 0, 0);
    }
    #define BTN_WIDTH   5*ROW_HEIGHT
    float spacewidth = widgetbounds.w - 2*ROW_HEIGHT - BTN_WIDTH - 3*NK_SPACING;
    nk_layout_row_push(ctx, spacewidth);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, BTN_WIDTH);
    if (nk_button_label(ctx, "Close") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ESCAPE)) {
      qg_topic = ~0;
      qg_history_clear(&history);
      nk_popup_close(ctx);
    }
    nk_layout_row_end(ctx);

    /* handle ArrowUp/Down & PageUp/Down keys */
    if (pageheight > widgetbounds.h - NK_SPACING) {
      nk_uint xscroll, yscroll;
      nk_group_get_scroll(ctx, "guide_widget", &xscroll, &yscroll);
      nk_uint new_y = yscroll;
      float scrolldim = pageheight - widgetbounds.h - NK_SPACING;
      if (nk_input_is_key_pressed(&ctx->input, NK_KEY_DOWN))
        new_y = (nk_uint)((yscroll + fontsize < scrolldim) ? yscroll + fontsize : scrolldim);
      else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_UP))
        new_y = (nk_uint)((yscroll > fontsize) ? yscroll - fontsize : 0);
      else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_TOP)
               || nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_START))
        new_y = 0;
      else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_BOTTOM)
               || nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_END))
        new_y = (nk_uint)scrolldim;
      if (new_y != yscroll)
        nk_group_set_scroll(ctx, "guide_widget", xscroll, new_y);
    }

    /* handle clicks on links */
    if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, widgetbounds)) {
      /* get mouse position, adjust for vertical scrolling */
      struct nk_mouse *mouse = &ctx->input.mouse;
      assert(mouse != NULL);
      unsigned xscroll, yscroll;
      nk_group_get_scroll(ctx, "guide_widget", &xscroll, &yscroll);
      float mouse_x = mouse->pos.x;
      float mouse_y = mouse->pos.y + yscroll;
      uint32_t t = qg_link_get(&links, mouse_x, mouse_y);
      if (t != QG_INVALID_LINK) {
        qg_topic = t;
        nk_group_set_scroll(ctx, "guide_widget", 0, 0);
      }
    }

    qg_link_clearall(&links);
    nk_popup_end(ctx);
  } else {
    qg_topic = ~0;
    qg_history_clear(&history);
  }
  return (qg_topic != ~0);
}

/* only needed when exiting the program while "help" is still open */
void nk_guide_cleanup(void)
{
  qg_history_clear(&history);
}
