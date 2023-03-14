/*
 * A basic control for displaying help texts.
 *
 * Copyright 2022-2023 CompuPhase
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

#ifndef _NK_GUIDE_H
#define _NK_GUIDE_H

#include <stdbool.h>
#include "nuklear.h"

bool nk_guide(struct nk_context *ctx, struct nk_rect *viewport, float fontsize,
              const char *content, const char *topic);
void nk_guide_cleanup(void);

#endif /* _NK_GUIDE_H */


