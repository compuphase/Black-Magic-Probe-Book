/*
 * Memory Dump widget and support functions, for the Black Magic Debugger
 * front-end based on the Nuklear GUI.
 *
 * Copyright 2021-2023 CompuPhase
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
#include "memdump.h"
#include "nuklear_style.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if !defined sizearray
# define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif


static const char *skipwhite(const char *text)
{
  assert(text != NULL);
  while (*text != '\0' && *text <= ' ')
    text++;
  return text;
}

static const char *skipstring(const char *buffer)
{
  assert(buffer != NULL);
  if (*buffer == '"') {
    buffer++;
    while (*buffer != '"' && *buffer != '\0') {
      if (*buffer == '\\' && *(buffer + 1) != '\0')
        buffer++;
      buffer++;
    }
    if (*buffer == '"')
      buffer++;
  } else {
    while (*buffer > ' ')
      buffer++;
  }
  return buffer;
}

static const char *fieldvalue(const char *field, size_t *len)
{
  const char *ptr;

  ptr = strchr(field, '=');
  if (ptr == NULL)
    return NULL;
  ptr = skipwhite(ptr + 1);
  if (*ptr != '"')
    return NULL;
  if (len != NULL) {
    const char *tail = skipstring(ptr);
    *len = tail - (ptr + 1) - 1;
  }
  return ptr + 1;
}

void memdump_init(MEMDUMP *memdump)
{
  assert(memdump != NULL);
  memset(memdump, 0, sizeof(MEMDUMP));
}

void memdump_cleanup(MEMDUMP *memdump)
{
  assert(memdump != NULL);

  if (memdump->data != NULL) {
    free(memdump->data);
    memdump->data = NULL;
  }
  if (memdump->prev != NULL) {
    free(memdump->prev);
    memdump->prev = NULL;
  }
  if (memdump->message != NULL) {
    free(memdump->message);
    memdump->message = NULL;
  }
}

int memdump_validate(MEMDUMP *memdump)
{
  assert(memdump != NULL);

  /* set defaults, if applicable */
  if (memdump->fmt == '\0')
    memdump->fmt = 'x';
  if (memdump->size == 0)
    memdump->size = 1;
  if (memdump->count == 0)
    memdump->count = (memdump->size == 1) ? 16 : 8;
  if (memdump->fmt == 'f' && memdump->size != 32 && memdump->size != 64)
    memdump->size = 32; /* size must be 32 or 64 for floating point type -> default to 32 */

  /* reset everything if no valid address expression is present */
  if (memdump->expr == NULL || strlen(memdump->expr) == 0) {
    memdump->count = 0;
    memdump->size = 0;
  }

  return (memdump->count * memdump->size) > 0;
}

int memdump_parse(const char *gdbresult, MEMDUMP *memdump)
{
  const char *start, *ptr;
  int in_char;
  size_t count;

  assert(memdump != NULL);

  /* check for error messages first */
  if (strncmp(gdbresult, "error", 5) == 0) {
    unsigned i, j;
    memdump_cleanup(memdump); /* forget any existing data */
    if ((start = strstr(gdbresult, "msg=")) == NULL)
      return 0;
    if ((start = fieldvalue(start, &count)) == NULL)
      return 0;
    memdump->message = malloc((count + 1) * sizeof(char));
    for (i = j = 0; i < count; i++) { /* copy string, but replace \" by " */
      if (start[i] != '\\' || start[i + 1] != '"')
        memdump->message[j++] = start[i];
    }
    memdump->message[j] = '\0';
    return 1; /* return 1 because the packet was successfully parsed */
  }

  /* get the start address */
  if ((start = strstr(gdbresult, "addr=")) == NULL)
    return 0;
  if ((start = fieldvalue(start, NULL)) == NULL)
    return 0;
  memdump->address = strtoul(start, NULL, 0);

  /* get start if memory contents list */
  if ((start = strstr(start, "memory=")) == NULL)
    return 0;
  start = skipwhite(start + 7);
  if (*start == '[')
    start = skipwhite(start + 1);
  if (*start == '{')
    start = skipwhite(start + 1);
  if (strncmp(start, "addr", 4) != 0)
    return 0;

  /* get start if the data part of this list */
  if ((start = strstr(start, "data=")) == NULL)
    return 0;
  start = skipwhite(start + 5);
  if (*start == '[')
    start = skipwhite(start + 1);

  /* get the length of the data */
  count = 0;
  if (memdump->fmt == 'c') {
    in_char = 0;
    for (ptr = start; *ptr != ']' && *ptr != '\0'; ptr++) {
      if (*ptr == '\'') {
        count += 1;
        in_char = !in_char;
      } else if (in_char) {
        count += 1;
        if (*ptr == '\\' && *(ptr + 1) == '\'') {
          ptr += 1;
          count += 1;
        } else if (*ptr == '\\' && (isdigit(*(ptr + 1)) || (*(ptr + 1) == '\\' && isdigit(*(ptr + 2))))) {
          int cnt;
          count += 3; /* GDB octal syntax for characters is converted to hex. and double backslash is removed */
          if (*(ptr + 1) == '\\')
            ptr += 1;
          for (cnt = 0; cnt < 3 && isdigit(*(ptr + 1)); cnt++)
            ptr += 1;
        }
      } else if (*ptr == ',') {
        count++;
      }
    }
  } else {
    for (ptr = start; *ptr != ']' && *ptr != '\0'; ptr++) {
      if (*ptr != '"' || memdump->fmt == 's')
        count += 1; /* ignore double quotes, except on string type */
    }
  }

  /* allocate memory and copy the data */
  if (memdump->prev != NULL)
    free(memdump->prev);
  memdump->prev = memdump->data;
  memdump->data = malloc((count+1) * sizeof(char));
  if (memdump->data != NULL) {
    char *tgt = memdump->data;
    if (memdump->fmt == 'c') {
      in_char = 0;
      for (ptr = start; *ptr != ']' && *ptr != '\0'; ptr++) {
        if (*ptr == '\'') {
          *tgt++ = *ptr;
          in_char = !in_char;
        } else if (in_char) {
          *tgt++ = *ptr;
          if (*ptr == '\\' && *(ptr + 1) == '\'') {
            *tgt++ = *++ptr;
          } else if (*ptr == '\\' && (isdigit(*(ptr + 1)) || (*(ptr + 1) == '\\' && isdigit(*(ptr + 2))))) {
            int cnt;
            char field[10];
            if (*(ptr + 1) == '\\')
              ptr += 1;
            for (cnt = 0; cnt < 3 && isdigit(*(ptr + 1)); cnt++) {
              ptr += 1;
              field[cnt] = *ptr;
            }
            field[cnt] = '\0';
            sprintf(tgt, "x%02x", (int)strtol(field, NULL, 8));
            tgt += 3;
          }
        } else if (*ptr == ',') {
          *tgt++ = *ptr;
        }
      }
    } else {
      for (ptr = start; *ptr != ']' && *ptr != '\0'; ptr++) {
        if (*ptr != '"' || memdump->fmt == 's')
          *tgt++ = *ptr;
      }
    }
    *tgt = '\0';
  }

  memdump->columns = 0;             /* force recalculation of the field sizes and number of columns */
  if (memdump->message != NULL) {   /* clear old error message, if any */
    free(memdump->message);
    memdump->message = NULL;
  }

  return 1;
}

static void calc_layout(struct nk_context *ctx, struct nk_user_font const *font,
                        float widget_width, MEMDUMP *memdump)
{
  /* check size of address label and max. size of field */
  assert(font != NULL && font->width != NULL);
  float char_width = font->width(font->userdata, font->height, "A", 1);
  memdump->addr_width = (8 + 1) * char_width;

  unsigned maxlen = 0;
  const char *head = memdump->data;
  while (*head != '\0') {
    const char *tail;
    if (*head == '"') {
      tail = skipstring(head);
    } else {
      tail = strchr(head, ',');
      if (tail == NULL)
        tail = strchr(head, '\0');
    }
    unsigned len = (tail - head);
    if (len >= maxlen)
      maxlen = len;
    head = tail;
    if (*head == ',')
      head += 1;
  }
  memdump->item_width = (maxlen + 0.5) * char_width;

  memdump->columns = (int)((widget_width - memdump->addr_width) / memdump->item_width);
  for (int idx = 1; idx < 8; idx++) { /* round # columns down to the nearest power of 2 below it */
    if (memdump->columns < (1 << idx)) {
      memdump->columns = 1 << (idx - 1);
      break;
    }
  }
}

void memdump_widget(struct nk_context *ctx, MEMDUMP *memdump, float widgetheight, float rowheight)
{
  int fonttype;
  struct nk_user_font const *font;

  assert(ctx != NULL);
  assert(memdump != NULL);
  assert(memdump->data != NULL);

  nk_layout_row_dynamic(ctx, rowheight, 2);
  nk_label(ctx, "Address", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);
  nk_label(ctx, memdump->expr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE);

  /* switch to mono-spaced font */
  fonttype = guidriver_setfont(ctx, FONT_MONO);
  font = ctx->style.font;

  nk_layout_row_dynamic(ctx, widgetheight, 1);
  nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, COLOUR_BG0);
  if (nk_group_begin(ctx, "memory", 0)) {
    struct nk_rect rcwidget = nk_layout_widget_bounds(ctx);
    int col = 0;
    unsigned long addr = memdump->address;
    const char *head, *tail, *prev_head;

    /* calculate values for lay-out (but only on a refresh) */
    if (memdump->columns == 0) {
      calc_layout(ctx, font, rcwidget.w, memdump);
      assert(memdump->columns > 0);
    }

    head = memdump->data;
    prev_head = memdump->prev;
    while (*head != '\0') {
      char field[128];
      unsigned len;
      int modified = 0;
      /* check for a new row */
      if (col == 0) {
        nk_layout_row_begin(ctx, NK_STATIC, rowheight, memdump->columns + 1);
        nk_layout_row_push(ctx, memdump->addr_width);
        sprintf(field, "%08lx", addr);
        nk_label(ctx, field, NK_TEXT_LEFT);
      }
      /* extract the field */
      if (*head == '"') {
        tail = skipstring(head);
      } else {
        tail = strchr(head, ',');
        if (tail == NULL)
          tail = strchr(head, '\0');
      }
      len = (tail - head);
      /* extract field in back-up data and compare */
      if (prev_head != NULL) {
        const char *prev_tail;
        if (*prev_head == '"') {
          prev_tail = skipstring(prev_head);
        } else {
          prev_tail = strchr(prev_head, ',');
          if (prev_tail == NULL)
            prev_tail = strchr(prev_head, '\0');
        }
        modified = memcmp(head, prev_head, len + 1);
        prev_head = (*prev_tail == ',') ? prev_tail + 1 : prev_tail;
      }
      if (len >= sizearray(field))
        len = sizearray(field) - 1;
      strncpy(field, head, len);
      field[len] = '\0';
      nk_layout_row_push(ctx, memdump->item_width);
      if (modified)
        nk_label_colored(ctx, field, NK_TEXT_LEFT, COLOUR_FG_RED);
      else
        nk_label(ctx, field, NK_TEXT_LEFT);
      /* advance to next field */
      col = (col + 1) % memdump->columns;
      addr += memdump->size;
      head = tail;
      if (*head == ',')
        head += 1;
    }

    nk_group_end(ctx);
  }
  nk_style_pop_color(ctx);
  guidriver_setfont(ctx, fonttype);
}

