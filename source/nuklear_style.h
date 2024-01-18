/*
 * Common styling & layout functions for the Nuklear GUI.
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

#ifndef _NUKLEAR_STYLE_H
#define _NUKLEAR_STYLE_H

#include <stdbool.h>
#include "nuklear.h"

void nuklear_style(struct nk_context *ctx);
float *nk_ratio(int count, ...);
bool editctrl_cond_color(struct nk_context *ctx, bool condition, struct nk_color color);
void editctrl_reset_color(struct nk_context *ctx, bool condition);
struct nk_color default_channel_colour(int channel);
struct nk_color severity_bkgnd(int severity);

#define COLOUR_BG0_S      nk_rgb_hex("#32302f") /* window background colour */
#define COLOUR_BG0        nk_rgb_hex("#1d2021") /* background colour for controls near black) */
#define COLOUR_BG_DARKRED nk_rgb_hex("#9d0006")
#define COLOUR_BG_RED     nk_rgb_hex("#cc241d")
#define COLOUR_BG_GREEN   nk_rgb_hex("#78a71a")
#define COLOUR_BG_YELLOW  nk_rgb_hex("#d79921")
#define COLOUR_BG_BLUE    nk_rgb_hex("#458588")
#define COLOUR_BG_PURPLE  nk_rgb_hex("#b16286")
#define COLOUR_BG_AQUA    nk_rgb_hex("#689d6a")
#define COLOUR_BG_GRAY    nk_rgb_hex("#a89984")
#define COLOUR_BG_ORANGE  nk_rgb_hex("#d65d0e")
#define COLOUR_BG_BUTTON  nk_rgb_hex("#104b5b")
#define COLOUR_TEXT       nk_rgb_hex("#ebdbb2")
#define COLOUR_HIGHLIGHT  nk_rgb_hex("#abcfff") /* highlighted text */
#define COLOUR_FG_GRAY    nk_rgb_hex("#928374") /* disabled text */
#define COLOUR_FG_RED     nk_rgb_hex("#fb4934")
#define COLOUR_FG_YELLOW  nk_rgb_hex("#fabd2f")
#define COLOUR_FG_GREEN   nk_rgb_hex("#0ad074")
#define COLOUR_FG_CYAN    nk_rgb_hex("#83a598")
#define COLOUR_FG_PURPLE  nk_rgb_hex("#d3869b")
#define COLOUR_FG_AQUA    nk_rgb_hex("#8ec07c")

#define CONTRAST_COLOUR(c)  (((3*(c).r + 5*(c).g + 2*(c).b) >= 1100) ? COLOUR_BG0 : COLOUR_HIGHLIGHT)

#endif /* _NUKLEAR_STYLE_H */
