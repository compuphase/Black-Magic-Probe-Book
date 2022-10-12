/*
 * Common styling & layout functions for the Nuklear GUI.
 *
 * Copyright 2021-2022 CompuPhase
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

#ifndef _NUKLEAR_STYLE_H
#define _NUKLEAR_STYLE_H

#include "nuklear.h"

void nuklear_style(struct nk_context *ctx);
float *nk_ratio(int count, ...);

#define COLOUR_BG0_S      nk_rgb_hex("#32302f") /* window background colour */
#define COLOUR_BG0        nk_rgb_hex("#1d2021") /* background colour for controls near black) */
#define COLOUR_BG_RED     nk_rgb_hex("#cc241d")
#define COLOUR_BG_YELLOW  nk_rgb_hex("#d79921")
#define COLOUR_TEXT       nk_rgb_hex("#ebdbb2")
#define COLOUR_HIGHLIGHT  nk_rgb_hex("#fff4ca") /* highlighted text */
#define COLOUR_FG_GRAY    nk_rgb_hex("#928374") /* disabled text */
#define COLOUR_FG_YELLOW  nk_rgb_hex("#fabd2f")
#define COLOUR_FG_RED     nk_rgb_hex("#fb4934")
#define COLOUR_FG_GREEN   nk_rgb_hex("#0ad074")
#define COLOUR_FG_BLUE    nk_rgb_hex("#83a598")
#define COLOUR_FG_PURPLE  nk_rgb_hex("#d3869b")
#define COLOUR_FG_AQUA    nk_rgb_hex("#8ec07c")

#define SWO_TRACE_DEFAULT_COLOR   COLOUR_TEXT

#endif /* _NUKLEAR_STYLE_H */
