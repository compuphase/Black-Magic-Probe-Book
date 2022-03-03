/*
 * Helper functions for the back-end driver for the Nuklear GUI. Currently, GDI+
 * (for Windows) and GLFW with OpenGL (for Linux) are supported.
 *
 * Copyright 2019 CompuPhase
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
#ifndef _GUIDRIVER_H
#define _GUIDRIVER_H

#include "nuklear.h"

#define GUIDRV_RESIZEABLE 0x0001
#define GUIDRV_CENTER     0x0002
#define GUIDRV_TIMER      0x0004

enum {
  FONT_STD = 0,
  FONT_MONO,
};

struct nk_context* guidriver_init(const char *caption, int width, int height, int flags,
                                  const char *fontstd, const char *fontmono, float fontsize);
void  guidriver_close(void);
int   guidriver_appsize(int *width, int *height);
void  guidriver_render(struct nk_color clear);
int   guidriver_poll(int waitidle);
void *guidriver_apphandle(void);
int   guidriver_setfont(struct nk_context *ctx, int type);

struct nk_image guidriver_image_from_memory(const unsigned char *data, unsigned size);

#endif /* _GUIDRIVER_H */
