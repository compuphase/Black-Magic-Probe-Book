/*
 * Tooltip with delay for the Nuklear GUI.
 *
 * Copyright 2019-2020 CompuPhase
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

#define TOOLTIP_DELAY 1000

unsigned long timestamp(void);
int tooltip(struct nk_context *ctx, struct nk_rect bounds, const char *text, struct nk_rect *viewport);

#endif /* _NK_TOOLTIP_H */
