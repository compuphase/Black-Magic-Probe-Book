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

#ifndef _NK_SPLITTER_H
#define _NK_SPLITTER_H

#include "nuklear.h"

#define RESIZE_PROPORTIONAL 0
#define RESIZE_TOPLEFT      1
#define RESIZE_BOTTOMRIGHT  2

typedef struct tagSPLITTERBAR {
  float size;     /* total width/height */
  float ratio;    /* position of the splitter bar, in percentage of the total size */
  float barsize;  /* width/height of the splitter bar */
  nk_bool hover;
  nk_bool dragging;
  /* ----- */
  float ratio_new;/* updated ratio after dragging, copied to "ratio" on a resize */
  float parts[3]; /* internal use: column widths, Nuklear keeps a pointer to this array, so it must stay valid */
} SPLITTERBAR;

void  nk_splitter_init(SPLITTERBAR *splitter, float size, float bar_width, float ratio);
void  nk_splitter_resize(SPLITTERBAR *splitter, float size, int resize_part);

void  nk_hsplitter_layout(struct nk_context *ctx, SPLITTERBAR *splitter, float height);
float nk_hsplitter_colwidth(SPLITTERBAR *splitter, int column);
void  nk_hsplitter(struct nk_context *ctx, SPLITTERBAR *splitter);

float nk_vsplitter_rowheight(SPLITTERBAR *splitter, int row);
void  nk_vsplitter(struct nk_context *ctx, SPLITTERBAR *splitter);

typedef struct tagSIZERBAR {
  float size;     /* content width/height */
  float minsize;  /* minimum size for the content */
  float barsize;  /* width/height of the splitter bar */
  nk_bool hover;
  nk_bool dragging;
  /* ----- */
  float size_new;/* updated size after dragging, copied to "size" on a refresh */
} SIZERBAR;

void  nk_sizer_init(SIZERBAR *sizer, float size, float minsize, float bar_width);
void  nk_sizer_refresh(SIZERBAR *sizer);
void  nk_sizer(struct nk_context *ctx, SIZERBAR *sizer);

#endif /* _NK_SPLITTER_H */


