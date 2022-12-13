/*
 * A Nuklear control for displaying help texts in the QuickGuide Markup format
 * (a flavour of Markdown).
 *
 * Copyright 2022 CompuPhase
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
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "guidriver.h"
#include "nuklear_guide.h"
#include "nuklear_style.h"
#include "nuklear_tooltip.h"

#if defined WIN32 || defined _WIN32
  #if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
    #include "strlcpy.h"
  #endif
#elif defined __linux__
  #include <bsd/string.h>
#endif

#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif


#define TOPIC_LENGTH  32
#define TOPIC_STACK   8
static char topic_stack[TOPIC_STACK][TOPIC_LENGTH];
static int topic_cur = 0;
static int topic_top = 0;

static void clear_stack(void)
{
  topic_cur = 0;
  topic_top = 0;
}

static void push_stack(const char *topic)
{
  assert(topic_cur < TOPIC_STACK);
  /* check for stack overflow, drop the bottom entry if so */
  if (topic_cur + 1 == TOPIC_STACK) {
    memmove(&topic_stack[0], &topic_stack[1], (TOPIC_STACK - 1) * TOPIC_LENGTH);
    topic_cur -= 1;
  }

  if (topic_top > 0)
    topic_cur += 1; /* advance stack position, except if the stack was empty */

  strlcpy(topic_stack[topic_cur],topic,TOPIC_LENGTH);
  topic_top = topic_cur + 1;
}

static const char *cur_topic(void)
{
  return topic_stack[topic_cur];
}

static bool move_back(bool testonly)
{
  if (topic_cur <= 0)
    return false;
  if (!testonly)
    topic_cur -= 1;
  return true;
}

static bool move_forward(bool testonly)
{
  if (topic_cur + 1 >= topic_top)
    return false;
  if (!testonly)
    topic_cur += 1;
  return true;
}


typedef struct tagLINELIST {
  struct tagLINELIST *next;
  const char *text;
  short type;
  short indent;     /* indent level */
  short numcolumns;
  float *columns;   /* widths of table columns in a string, NULL if not in a table */
  float x, y, w, h; /* full line size & position */
} LINELIST;

enum {
  TYPE_TEXT,
  TYPE_HEADING1,
  TYPE_HEADING2,
  TYPE_HEADING3,
  TYPE_BULLETLIST,
  TYPE_NUMBERLIST,
  TYPE_INDENTBLOCK, /* also used for description list */
  TYPE_TABLE,
  TYPE_PREFMT,
  TYPE_HLINE,
  TYPE_LINK,        /* actually an in-line code, but in this viewer, a link must be on a line of its own */
  TYPE_EMPHASIZED,  /* actually an in-line code, but in this viewer, only full lines can be emphasized */
  TYPE_COMMENT,
};
#define INDENTSIZE    24
#define EXTRAMARGIN   5 /* must be bigger than NK_SPACING */
#define NK_SPACING    4
#define CELL_SPACING  8 /* extra spacing between table cells (added to left & right of each cell) */

#define HARDSPACE     "\xc2\xa0"
#define SOFTHYPHEN    '\x01'
#define HYPHEN        '\x02'

static LINELIST linelist_root = { NULL };

static void linelist_clear(void)
{
  while (linelist_root.next != NULL) {
    LINELIST *item = linelist_root.next;
    linelist_root.next = item->next;
    assert(item->text != NULL);
    free((void*)item->text);
    free((void*)item);
  }
}

static const char *skipwhite(const char *string, bool newline)
{
  assert(string != NULL);
  char stop_char = newline ? '\n' : '\0';
  while (*string != '\0' && *string <= ' ' && *string != stop_char)
    string++;
  return string;
}

static void trim(char *string)
{
  assert(string != NULL);
  char *end = string + strlen(string);
  while (end > string && *(end - 1) <= ' ')
    end -= 1;
  *end = '\0';
}

static void strdel(char *string, int count)
{
  assert(string != NULL);
  if (count > 0) {
    size_t len = strlen(string);
    if (count >= len)
      *string = '\0';
    else
      memmove(string, string + count, (len - count + 1) * sizeof(char));
  }
}

static char *strdup2(const char *head, const char *tail)
{
  assert(head != NULL && tail != NULL);
  assert(tail >= head);
  size_t len = tail - head;
  char *txt = malloc((len + 1) * sizeof(char));
  if (txt) {
    if (len > 0)
      memcpy(txt, head, len);
    txt[len] = '\0';
  }
  return txt;
}

static int utf8_charsize(const char *ptr)
{
  if ((*ptr & 0x80) != 0x80)
    return 1;
  if ((*ptr & 0xe0) == 0xc0)
    return 2;
  if ((*ptr & 0xf0) == 0xe0)
    return 3;
  if ((*ptr & 0xf8) == 0xf0)
    return 4;
  return 1; /* invalid UTF-8 encoding */
}

static bool is_heading(const char *head, int level)
{
  assert(head != NULL);
  int count = 0;
  while (*head == '#') {
    count++;
    head++;
  }
  if (count == 0 || (*head != ' ' && *head != '\t'))
    return false;
  return level == 0 || level == count;
}

static bool is_directive(const char *head)
{
  assert(head != NULL);
  if (*head != '#')
    return false;
  head++;
  if (strncmp(head, "keywords ", 9) == 0)
    return true;
  if (strncmp(head, "format ", 7) == 0)
    return true;
  if (strncmp(head, "include ", 8) == 0)
    return true;  /* includes should have been removed by the QuickGuide preprocessor */
  if (strncmp(head, "macro ", 6) == 0)
    return true;  /* macros should have been replaced by the QuickGuide preprocessor */
  return false;
}

static bool is_comment(const char *head)
{
  assert(head != NULL);
  return *head == '-' && *(head + 1) == '-' && (*(head + 2) == ' ' || *(head + 2) == '\t');
}

static bool is_hline(const char *head)
{
  assert(head != NULL);
  head = skipwhite(head, true);
  int count = 0;
  while (*head == '-') {
    count++;
    head++;
  }
  while (*head == ' ' || *head == '\t')
    head++;
  return count >= 3 && (*head == '\0' || *head == '\n');
}

static bool is_preformat(const char *head)
{
  assert(head != NULL);
  head = skipwhite(head, true);
  int count = 0;
  while (*head == '`') {
    count++;
    head++;
  }
  while (*head == ' ' || *head == '\t')
    head++;
  return count >= 3 && (*head == '\0' || *head == '\n');
}

static bool is_table(const char *head, int *columns)
{
  assert(head != NULL);
  head = skipwhite(head, true);
  if (*head != '|')
    return false;
  /* check that the line also ends with a '|' */
  const char *ptr = strchr(head, '\n');
  if (ptr == NULL)
    ptr = head + strlen(head);
  while (ptr > head && *(ptr - 1) <= ' ')
    ptr--;
  if (ptr == head || *(ptr - 1) != '|')
    return false;
  /* optionally count the number of columns */
  if (columns != NULL) {
    /* skip first '|', then count the number of non-escaped '|' */
    int count = 0;
    assert(*head == '|');
    for (ptr = head + 1; *ptr != '\0' && *ptr != '\n'; ptr++) {
      if (*ptr == '\\' && *(ptr + 1) != '\0' && *(ptr + 1) != '\n')
        ptr += 1;
      else if (*ptr == '|')
        count++;
    }
    *columns = count;
  }
  return true;
}

static bool is_link(const char* head)
{
  assert(head != NULL);
  head = skipwhite(head, true);
  if (*head != '[' || *(head + 1) != '[')
    return false;
  /* check that the line also ends with ']]' */
  const char* ptr = strchr(head, '\n');
  if (ptr == NULL)
    ptr = head + strlen(head);
  while (ptr > head && *(ptr - 1) <= ' ')
    ptr--;
  return ptr > head + 1 && *(ptr - 1) == ']' && *(ptr - 2) == ']';
}

static bool is_bulletlist(const char* head)
{
  head = skipwhite(head, true);
  if (*head != '*')
    return false;
  head++;
  if (*head == '>') {
    /* bullet list with a symbol */
    head++;
    head += utf8_charsize(head);
  }
  return *head == ' ';
}

static bool is_numberlist(const char* head)
{
  head = skipwhite(head, true);
  if (!isdigit(*head))
    return false;
  while (isdigit(*head))
    head++;
  return *head == ')';
}

static bool is_indentblock(const char* head)
{
  head = skipwhite(head, true);
  return *head == ':' && (*(head + 1) == ' ' || *(head + 1) == '\t');
}

static bool getpage(struct nk_context *ctx, float pagewidth, const char *content, const char *topic)
{
  assert(content != NULL && topic != NULL);

  /* find the level-1 heading with the topic */
  const char *block = content;
  const char *sentinel;
  while (*block != '\0') {
    sentinel = strchr(block, '\n');
    if (sentinel == NULL)
      sentinel = block + strlen(block);
    if (is_heading(block, 1)) {
      block = skipwhite(block + 1, true);
      size_t maxlen = sentinel - block;
      if (maxlen >= TOPIC_LENGTH)
        maxlen = TOPIC_LENGTH - 1;
      if (strcmp(topic, "(root)") == 0 || strncmp(block, topic, maxlen) == 0)
        break;  /* entry found */
    }
    block = (*sentinel == '\n') ? sentinel + 1 : sentinel;
  }

  if (*block == '\0')
    return false;
  linelist_clear();

  LINELIST *item, *last;
  /* copy level-1 heading (unless it is an anonymous heading) */
  if (*block != '(') {
    item = malloc(sizeof(LINELIST));
    if (item != NULL) {
      memset(item, 0, sizeof(LINELIST));
      item->text = strdup2(block, sentinel);
      item->type = TYPE_HEADING1;
      if (item->text != NULL) {
        for (last = &linelist_root; last->next != NULL; last = last->next)
          {}
        last->next = item;
      } else {
        free(item);
      }
    }
  }
  block = (*sentinel == '\n') ? sentinel + 1 : sentinel;

  int listindent = 0;
  bool in_preformat = false;
  struct nk_user_font const* font = ctx->style.font;
  /* copy lines until the next level-1 heading */
  while (*block != '\0' && (!is_heading(block, 1) || in_preformat)) {
    bool concat = !in_preformat;
    if ((sentinel = strchr(block, '\n')) == NULL) {
      sentinel = block + strlen(block);
      concat = false;
    }
    if (concat && skipwhite(block, true) == sentinel) {
      concat = false; /* this line is empty, never concatenate with the next */
      /* check for multiple consecutive empty lines, gobble these up */
      const char *last_newline = sentinel;
      for (const char *ptr = sentinel; *ptr != '\0' && *ptr <= ' '; ptr++)
        if (*ptr == '\n')
          last_newline = ptr;
      sentinel = last_newline;
    }
    /* for concatenating lines up to the next paragraph, consider both the type
       of the current line and the type of the next line
       e.g. when *this* line is a heading, the next line is not concatenated */
    if (is_heading(block, 0) || is_directive(block) || is_comment(block)
        || is_hline(block) || is_preformat(block) || is_table(block, NULL)
        || is_link(block))
      concat = false;
    while (concat) {
      assert(sentinel != NULL);
      /* check if the next line is empty or has a special prefix; if not,
         the lines should be concatenated */
      if (sentinel > block + 1 && *(sentinel - 1) == '\\' && *(sentinel - 2) != '\\') {
        concat = false;   /* current line ends with a line-break */
      } else if (*skipwhite(sentinel + 1, true) == '\n') {
        concat = false;   /* next line is empty -> paragraph end */
      } else {
        const char *ptr = sentinel + 1;
        if (is_heading(ptr, 0) || is_directive(ptr) || is_comment(ptr)
            || is_hline(ptr) || is_preformat(ptr) || is_table(ptr, NULL)
            || is_link(ptr) || is_bulletlist(ptr) || is_numberlist(ptr)
            || is_indentblock(ptr))
          concat = false;
      }
      if (concat) {
        sentinel = strchr(sentinel + 1, '\n');  /* skip the '\n' that we landed on, find the next one */
        if (sentinel == NULL) {
          sentinel = block + strlen(block);
          concat = false;
        }
      }
    }

    int type = in_preformat ? TYPE_PREFMT : TYPE_TEXT;
    if (in_preformat) {
      /* only special code allowed in preformat, is ``` to end it */
      if (is_preformat(block)) {
        in_preformat = false;
        type = TYPE_COMMENT; /* set to comment, as to gobble up this line */
      }
    } else {
      if (is_directive(block) || is_comment(block)) {
        type = TYPE_COMMENT; /* directives are not supported in this viewer */
      } else if (is_preformat(block)) {
        in_preformat = true;
        type = TYPE_COMMENT; /* set to comment, as to gobble up this line */
      } else if (is_heading(block, 2)) {
        type = TYPE_HEADING2;
      } else if (is_heading(block, 3)) {
        type = TYPE_HEADING3;
      } else if (is_bulletlist(block)) {
        type = TYPE_BULLETLIST;
      } else if (is_numberlist(block)) {
        type = TYPE_NUMBERLIST;
      } else if (is_indentblock(block)) {
        type = TYPE_INDENTBLOCK;
      } else if (is_table(block, NULL)) {
        type = TYPE_TABLE;
      } else if (is_link(block)) {
        type = TYPE_LINK;
      } else if (is_hline(block)) {
        type = TYPE_HLINE;
      }
    }

    int indent = 0;
    char* segment = NULL;
    if (type != TYPE_COMMENT)
      segment = strdup2(block, sentinel);
    if (segment != NULL) {
      /* clean the string from some mark-up */
      char *ptr;
      while ((ptr = strchr(segment, '\t')) != NULL)
        *ptr = ' ';
      if (type != TYPE_PREFMT) {
        while ((ptr = strchr(segment, '\n')) != NULL) {
          /* replace newlines (left over from concatenating lines) by spaces, but
             also gobble up and whitespace following the newline */
          *ptr++ = ' ';
          strdel(ptr, skipwhite(ptr, true) - ptr);
        }
        if ((ptr = strrchr(segment, '\\')) != NULL && *(ptr + 1) == '\0' && !in_preformat)
          *ptr = ' '; /* remove line break character at the end of the line */
      }
      if (type == TYPE_HEADING2 || type == TYPE_HEADING3) {
        strdel(segment, (type == TYPE_HEADING3) ? 2 : 3);
      } else if (type == TYPE_BULLETLIST || type == TYPE_INDENTBLOCK) {
        ptr = (char*)skipwhite(segment, false);
        assert(*ptr == '*' || *ptr == ':');
        strdel(ptr, 2);
        listindent = 1;
        if (ptr - segment >= 2)
          listindent += 1;
      } else if (type == TYPE_LINK) {
        ptr = segment;
        while (*ptr != '\0' && (*ptr != '[' || *(ptr + 1) != '['))
          ptr++;
        if (*ptr == '[')
          strdel(ptr, 2);
        while (*ptr != '\0' && (*ptr != ']' || *(ptr + 1) != ']'))
          ptr++;
        if (*ptr == ']')
          strdel(ptr, 2);
        if (*segment <= ' ')
          strdel(segment, skipwhite(segment, true) - segment);
        indent = 1;
      } else if (type == TYPE_HLINE) {
        *segment = '\0';  /* clear entire string */
      }

      if (type == TYPE_TEXT) {
        /* process/remove in-line attributes */
        char *head = segment;
        while (*head != '\0') {
          while (*head != '\0' && *head != '*' && *head != '`') {
            if (*head == '\\' && ispunct(*(head + 1)))
              head++;
            head++;
          }
          if (*head == '\0')
            break;
          char match = *head;
          int count = 1;
          if (*(head + 1) == match && match == '*')
            count = 2;  /* found "**" */
          if (isspace(*(head + count)) || *(head + count) == match) {
            head += count;
            continue; /* not a valid text attribute, continue */
          }
          char *tail = head + count;
          while (*tail != '\0' && (*tail != match || (count == 2 && *(tail + 1) != match)))
            tail++;
          if (*tail != match) {
            head = tail;  /* no matching end attribute found, ignore the attribute */
            continue;
          }
          //??? store the attribute in a separate "markup" list before removing them
          type = TYPE_EMPHASIZED;  /* the Nuklear viewer only supports emphasizing full lines */
          strdel(tail, count);
          strdel(head, count);
          head = tail - count;
        }
      }

      /* handle escaped characters (\* -> *, etc.), plus non-breaking space and
         hard/soft hyphen */
      if (type != TYPE_PREFMT) {
        for (ptr = segment; *ptr != '\0'; ptr++) {
          if (*ptr == '\\' && ispunct(*(ptr + 1))) {
            strdel(ptr, 1); /* all punctuation can be escaped */
          } else if (*ptr == '\\' && *(ptr + 1) == ' ') {
            memcpy(ptr, HARDSPACE, 2);
          } else if (*ptr == '~') {
            if (ptr != segment && isalpha(*(ptr - 1)) && isalpha(*(ptr + 1)))
              *ptr = SOFTHYPHEN;
          } else if (*ptr == '-') {
            if (ptr != segment && isalpha(*(ptr - 1)) && isalpha(*(ptr + 1)))
              *ptr = HYPHEN;
          } else if (*ptr == ' ') {
            while (*(ptr + 1) == ' ')
              strdel(ptr, 1); /* replace multiple spaces by a single space */
          }
        }
      }

      trim(segment);
      if (*segment == '\0')
        listindent = 0;
      indent += listindent;

      /* do a word-wrap routine */
      assert(font != NULL && font->width != NULL);
      float hyphenwidth = font->width(font->userdata, font->height, "-", 1);
      char *head = segment;
      do {
        char *breakpos = head;
        if (type != TYPE_PREFMT && type != TYPE_TABLE) {
          char *pos = breakpos;
          float textwidth = 0;
          /* find next potential break point */
          while (*breakpos != '\0') {
            for (pos = breakpos + 1; *pos > ' '; pos++)
              {}
            textwidth = font->width(font->userdata, font->height, head, (int)(pos - head));
            if (*pos == SOFTHYPHEN || *pos == HYPHEN)
              textwidth += hyphenwidth;
            if (textwidth > pagewidth - indent * INDENTSIZE)
              break;  /* text too long, revert to previous line break position */
            /* if arriving here, the string fits -> update the break position */
            if (*breakpos == SOFTHYPHEN) {
              /* previous break position was on a soft-hyphen, which we can now
                 delete */
              strdel(breakpos, 1);
              pos -= 1;
            } else if (*breakpos == HYPHEN) {
              /* previous break position was on a breaking hyphen, which is now
                 non-breaking */
              *breakpos = '-';
            }
            breakpos = pos;
          }
        }
        if (breakpos == head)
          breakpos = head + strlen(head); /* a non-breakable word or preformat, or table */
        if (*breakpos == SOFTHYPHEN || *breakpos == HYPHEN)
          *breakpos++ = '-';
        /* add the string to the list */
        item=malloc(sizeof(LINELIST));
        if (item != NULL) {
          memset(item, 0, sizeof(LINELIST));
          item->text = strdup2(head, breakpos);
          item->type = type;
          item->indent = indent;
          item->w = font->width(font->userdata, font->height, item->text, strlen(item->text));
          for (last = &linelist_root; last->next != NULL; last = last->next)
            {}
          last->next = item;
        }
        /* for table type, get the (minimal) column widths */
        int columns;
        if (is_table(item->text, &columns)) {
          assert(columns >= 1);
          item->columns = malloc(columns * sizeof(float));
          if (item ->columns != NULL) {
            item->numcolumns = columns;
            head = strchr(item->text, '|');
            assert(head != NULL);
            for (int c = 0; c < columns; c++) {
              assert(*head == '|');
              head++;
              char *pos = (char*)skipwhite(head, false);
              if (pos != head)
                strdel(head, pos - head);
              pos = strchr(head, '|');
              assert(pos != NULL);
              int spaces = 0;
              while (pos > head && *(pos - 1) <= ' ') {
                pos--;
                spaces++;
              }
              if (spaces > 0)
                strdel(pos, spaces);
              assert(*pos == '|');
              item->columns[c] = font->width(font->userdata, font->height, head, pos - head);
              head = pos;
            }
          }
        }
        /* prepare for next part to the string */
        head = (char*)skipwhite(breakpos, false);
      } while (*head != '\0');
      free((void*)segment);
    }

    block = (*sentinel == '\n') ? sentinel + 1 : sentinel;
  }

  /* align columns of all tables on the page */
  item = linelist_root.next;
  while (item != NULL) {
    if (item->type != TYPE_TABLE) {
      item = item->next;
      continue;
    }
    for (last = item->next; last != NULL && last->type == TYPE_TABLE; last = last->next)
      {}
    /* normally, all rows in a table have the same number of columns, but check
       it anyway */
    int maxcolumns = 0;
    for (LINELIST *cur = item; cur != last; cur = cur->next)
      if (cur->numcolumns > maxcolumns)
        maxcolumns = cur->numcolumns;
    float* colwidths = malloc(maxcolumns * sizeof(float));
    if (colwidths != NULL) {
      for (int c = 0; c < maxcolumns; c++) {
        /* first find the maximum width... */
        float maxwidth = 0;
        for (LINELIST *cur = item; cur != last; cur = cur->next)
          if (c < cur->numcolumns && cur->columns[c] > maxwidth)
            maxwidth = cur->columns[c];
        if (maxwidth < 1.0)
          maxwidth = 1.0;
        /* ... then update the section in all rows */
        for (LINELIST *cur = item; cur != last; cur = cur->next)
          if (c < cur->numcolumns)
            cur->columns[c] = maxwidth;
      }
      free(colwidths);
    }
    item = last;
  }

  return true;
}

/** guide_widget() draws the text in the log window, with support for minimal
 *  formatting.
 */
static float guide_widget(struct nk_context *ctx, const char *id, float fontsize,
                          const char *content)
{
  float pagewidth = 0.0;
  float pagebottom = 0.0;
  float pagetop = 0.0;
  int cur_fonttype = FONT_STD;

  /* black background on group */
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, id, NK_WINDOW_BORDER)) {
    struct nk_rect rcline = nk_layout_widget_bounds(ctx); /* get the line width and y-start (to calculate page height) */
    pagewidth = rcline.w - 2*NK_SPACING;
    pagetop = rcline.y;
    if (linelist_root.next == NULL)
      getpage(ctx, pagewidth, content, cur_topic());
    for (LINELIST *item = linelist_root.next; item != NULL; item = item->next) {
      if (item->type == TYPE_HLINE) {
        nk_layout_row(ctx, NK_DYNAMIC, 1, 3, nk_ratio(3, 0.025, 0.95, 0.025));
        nk_spacing(ctx, 1);
        nk_rule_horizontal(ctx, COLOUR_TEXT, nk_false);
        nk_spacing(ctx, 1);
        continue;
      }
      if (item->type == TYPE_TABLE) {
        if (cur_fonttype != FONT_STD) {
          cur_fonttype = FONT_STD;
          guidriver_setfont(ctx, cur_fonttype);
        }
        nk_layout_row_begin(ctx, NK_STATIC, fontsize, 1 + item->numcolumns);
        nk_layout_row_push(ctx, (EXTRAMARGIN + CELL_SPACING - NK_SPACING));
        nk_spacing(ctx, 1);
        assert(item->columns != NULL);
        const char *col_start = strchr(item->text, '|');
        for (int c = 0; c < item->numcolumns; c++) {
          assert(col_start != NULL);
          const char *col_end = strchr(col_start + 1, '|');
          assert(col_end != NULL);
          nk_layout_row_push(ctx, item->columns[c] + 2 * CELL_SPACING);
          nk_text_colored(ctx, col_start + 1, col_end - col_start - 1, NK_TEXT_LEFT, COLOUR_TEXT);
          col_start = col_end;
        }
        nk_layout_row_end(ctx);
        continue;
      }
      float textwidth = pagewidth - item->indent * INDENTSIZE;
      nk_layout_row_begin(ctx, NK_STATIC, fontsize, 2 + item->indent);
      nk_layout_row_push(ctx, (EXTRAMARGIN - NK_SPACING));
      nk_spacing(ctx, 1);
      for (int indent = 0; indent < item->indent; indent++) {
        nk_layout_row_push(ctx, INDENTSIZE - NK_SPACING);
        if (item->type == TYPE_BULLETLIST && indent == item->indent - 1)
          nk_symbol(ctx, (indent == 0) ? NK_SYMBOL_CIRCLE_SOLID_SMALL : NK_SYMBOL_CIRCLE_OUTLINE_SMALL, NK_TEXT_CENTERED);
        else if (item->type == TYPE_LINK && indent == item->indent - 1)
          nk_symbol_colored(ctx, NK_SYMBOL_LINK_ALT, NK_TEXT_RIGHT, COLOUR_HIGHLIGHT);
        else
          nk_spacing(ctx, 1);
      }
      nk_layout_row_push(ctx, (item->type == TYPE_PREFMT) ? item->w : textwidth);
      rcline = nk_layout_widget_bounds(ctx);
      item->x = rcline.x + item->indent * INDENTSIZE;
      item->y = rcline.y;
      item->h = rcline.h; /* "w" field is set to the text width when parsing the page */
      int fonttype = FONT_STD;
      struct nk_color clr = COLOUR_TEXT;
      switch (item->type) {
      case TYPE_HEADING1:
        fonttype = FONT_HEADING1;
        break;
      case TYPE_HEADING2:
        fonttype = FONT_HEADING2;
        break;
      case TYPE_HEADING3:
        clr = COLOUR_FG_GREEN;
        break;
      case TYPE_LINK:
        clr = COLOUR_HIGHLIGHT;
        break;
      case TYPE_PREFMT:
        clr = COLOUR_FG_AQUA;
        fonttype = FONT_MONO;
        break;
      case TYPE_EMPHASIZED:
        clr = COLOUR_FG_YELLOW;
        break;
      }
      if (cur_fonttype != fonttype) {
        cur_fonttype = fonttype;
        guidriver_setfont(ctx, cur_fonttype);
      }
      nk_label_colored(ctx, item->text, NK_TEXT_LEFT, clr);
      nk_layout_row_end(ctx);
      if (pagebottom < item->y + item->h)
        pagebottom = item->y + item->h;
    }
    nk_group_end(ctx);
  }
  if (cur_fonttype != FONT_STD)
    guidriver_setfont(ctx, FONT_STD);
  nk_style_pop_color(ctx);
  return pagebottom - pagetop;
}

bool nk_guide(struct nk_context *ctx, struct nk_rect *viewport, float fontsize,
              const char *content, const char *topic)
{
  assert(ctx != NULL);
  assert(viewport != NULL);
  assert(content != NULL);

  if (topic == NULL)
    topic = "(root)";
  if (topic_top==0)
    push_stack(topic);

  bool is_active = true;
  if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Guide", NK_WINDOW_NO_SCROLLBAR, *viewport)) {
    #define ROW_HEIGHT  (2*fontsize)
    nk_layout_row_dynamic(ctx, viewport->h - ROW_HEIGHT - fontsize, 1);
    struct nk_rect widgetbounds = nk_widget_bounds(ctx);
    float pageheight = guide_widget(ctx, "guide_widget", fontsize, content);

    /* button bar: optional back and forward buttons, plus a close button */
    nk_layout_row_begin(ctx, NK_STATIC, ROW_HEIGHT, 4);
    nk_layout_row_push(ctx, ROW_HEIGHT);
    if (button_symbol_tooltip(ctx, NK_SYMBOL_TRIANGLE_LEFT, NK_KEY_BACKSPACE, move_back(true), "Go Back")) {
      move_back(false);
      linelist_clear();
      nk_group_set_scroll(ctx, "guide_widget", 0, 0);
    }
    nk_layout_row_push(ctx, ROW_HEIGHT);
    if (button_symbol_tooltip(ctx, NK_SYMBOL_TRIANGLE_RIGHT, NK_KEY_NONE, move_forward(true), "Go Forward")) {
      move_forward(false);
      nk_group_set_scroll(ctx, "guide_widget", 0, 0);
      linelist_clear();
    }
    #define BTN_WIDTH   5*ROW_HEIGHT
    float spacewidth = widgetbounds.w - 2*ROW_HEIGHT - BTN_WIDTH - 3*NK_SPACING;
    nk_layout_row_push(ctx, spacewidth);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, BTN_WIDTH);
    if (nk_button_label(ctx, "Close") || nk_input_is_key_pressed(&ctx->input, NK_KEY_ESCAPE)) {
      is_active = false;
      linelist_clear();
      clear_stack();
      nk_popup_close(ctx);
    }
    nk_layout_row_end(ctx);

    /* handle ArrowUp/Down & PageUp/Down keys */
    if (pageheight > widgetbounds.h - NK_SPACING) {
      unsigned xscroll, yscroll;
      nk_group_get_scroll(ctx, "guide_widget", &xscroll, &yscroll);
      int new_y = yscroll;
      float scrolldim = pageheight - widgetbounds.h - NK_SPACING;
      if (nk_input_is_key_pressed(&ctx->input, NK_KEY_DOWN))
        new_y = (yscroll + fontsize < scrolldim) ? yscroll + fontsize : scrolldim;
      else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_UP))
        new_y = (yscroll > fontsize) ? yscroll - fontsize : 0;
      else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_TOP)
               || nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_START))
        new_y = 0;
      else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_BOTTOM)
               || nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_END))
        new_y = scrolldim;
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
      for (LINELIST *item = linelist_root.next; item != NULL; item = item->next) {
        if (item->type == TYPE_LINK && NK_INBOX(mouse_x, mouse_y, item->x, item->y, item->w, item->h)) {
          push_stack(item->text);
          nk_group_set_scroll(ctx, "guide_widget", 0, 0);
          linelist_clear();
          break;
        }
      }
    }

    nk_popup_end(ctx);
  } else {
    is_active = false;
  }
  return is_active;
}

