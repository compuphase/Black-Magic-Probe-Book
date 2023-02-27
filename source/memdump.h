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
#ifndef _MEMDUMP_H
#define _MEMDUMP_H

#include "nuklear.h"

typedef struct tagMEMDUMP {
  char *expr;                   /* number or expression that evaluates to an address */
  unsigned short count;
  char fmt;                     /* default = x -> hexadecimal */
  unsigned char size;           /* default = 1 (byte) */
  unsigned long address;        /* returned address */
  char *message;                /* error message (or NULL) */
  char *data;                   /* current data */
  char *prev;                   /* old data (for checking changes) */
  int columns;                  /* reset to 0 on parsing a new memory block */
  float addr_width, item_width;
} MEMDUMP;

void memdump_init(MEMDUMP *memdump);
void memdump_cleanup(MEMDUMP *memdump);
int memdump_validate(MEMDUMP *memdump);
int memdump_parse(const char *gdbresult, MEMDUMP *memdump);
void memdump_widget(struct nk_context *ctx, MEMDUMP *memdump, float widgetheight, float rowheight);

#endif /* _MEMDUMP_H*/
