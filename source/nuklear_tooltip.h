/*
 * Tooltip with delay for the Nuklear GUI.
 *
 * Copyright 2019-2021 CompuPhase
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

#ifndef _NK_TOOLTIP_H
#define _NK_TOOLTIP_H

#include "nuklear.h"

#define TOOLTIP_DELAY   1000  /* in ms, time that the mouse pointer must hover
                                 over the control before the tooltip pops up */
#define TOOLTIP_TIMEOUT 6000  /* in ms, time that the tooltip stays visible */

unsigned long timestamp(void);
int tooltip(struct nk_context *ctx, struct nk_rect bounds, const char *text);

nk_bool button_tooltip(struct nk_context *ctx, const char *title,
                       enum nk_keys hotkey, nk_bool enabled,
                       const char *tiptext);
nk_bool button_symbol_tooltip(struct nk_context *ctx, enum nk_symbol_type symbol,
                              enum nk_keys hotkey, const char *tiptext);
nk_bool checkbox_tooltip(struct nk_context *ctx, const char *label,
                         nk_bool *active, const char *tiptext);
nk_flags editctrl_tooltip(struct nk_context *ctx, nk_flags flags,
                          char *buffer, int max, nk_plugin_filter filter,
                          const char *tiptext);

#endif /* _NK_TOOLTIP_H */
