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

#if defined _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#elif defined __linux__
  #include <sys/time.h>
#endif
#include <string.h>
#include "nuklear_tooltip.h"

/* timestamp() returns the timestamp in ms (under Windows, the granularity of
   the timestamp is 55ms, which is not great, but good enough for tooltips */
unsigned long timestamp(void)
{
  #if defined WIN32 || defined _WIN32
    return GetTickCount();  /* 55ms granularity, but good enough */
  #else
    struct timeval  tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
  #endif
}

int tooltip(struct nk_context *ctx, struct nk_rect bounds, const char *text, struct nk_rect *viewport)
{
  static struct nk_rect recent_bounds;
  static unsigned long start_tstamp;
  unsigned long tstamp = timestamp();

  if (!nk_input_is_mouse_hovering_rect(&ctx->input, bounds))
    return 0;           /* not hovering this control/area */
  if (memcmp(&bounds, &recent_bounds, sizeof(struct nk_rect)) != 0) {
    /* hovering this control/area, but it's a different one from the previous;
       restart timer */
    recent_bounds = bounds;
    start_tstamp = tstamp;
    return 0;
  }
  if (tstamp - start_tstamp < TOOLTIP_DELAY)
    return 0;           /* delay time has not reached its value yet */
  if (text != NULL)
    nk_tooltip(ctx, text, viewport);
  return 1;
}

