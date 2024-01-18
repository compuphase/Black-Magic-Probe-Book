/*
 * A Nuklear control for a message box (which, by the design of Nuklear, is not
 * truly modal).
 *
 * Copyright 2024 CompuPhase
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

#ifndef _NK_MSGBOX_H
#define _NK_MSGBOX_H

#include <stdbool.h>
#include "nuklear.h"

enum {
  MSGBOX_INACTIVE = -1,
  MSGBOX_ACTIVE,
  MSGBOX_CLOSE,
};

bool nk_msgbox(struct nk_context *ctx, const char *message, const char *caption);
int nk_msgbox_popup(struct nk_context *ctx);

#endif /* _NK_MSGBOX_H */
