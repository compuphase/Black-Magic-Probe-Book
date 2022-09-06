/*
 * Microcontroller description lookup, based on brand and part id.
 *
 * Copyright 2022 CompuPhase
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
#ifndef _MCU_INFO_H
#define _MCU_INFO_H

#include <stdint.h>

#if defined __cplusplus
  extern "C" {
#endif

typedef struct tagMCUINFO {
  uint32_t partid;
  uint32_t maskid;
  const char *prefix;   /* brand/family prefix (Black Magic Probe "driver" name) */
  const char *mcuname;  /* name to use in scripts, if applicable */
  const char *description;
} MCUINFO;

const MCUINFO *mcuinfo_lookup(const char *family, uint32_t id);

#if defined __cplusplus
  }
#endif

#endif /* _MCU_INFO_H */

