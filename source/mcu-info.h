/*
 * Microcontroller description lookup, based on brand and part id.
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
#ifndef _MCU_INFO_H
#define _MCU_INFO_H

#include <stdint.h>

#if defined __cplusplus
  extern "C" {
#endif

typedef struct tagMCUINFO {
  uint32_t partid;
  const char *prefix;   /* brand/family prefix (part of Black Magic Probe "driver" name), may be NULL */
  uint32_t flash;
  uint32_t sram;        /* main SRAM (not including buffers, caches & FIFOs for peripherals) */
  const char *description;
} MCUINFO;

const MCUINFO *mcuinfo_data(const char *family, uint32_t id);
const char *mcuinfo_lookup(const char *family, uint32_t id);

#if defined __cplusplus
  }
#endif

#endif /* _MCU_INFO_H */

