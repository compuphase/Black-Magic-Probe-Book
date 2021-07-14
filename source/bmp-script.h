/*
 * General purpose "script" support for the Black Magic Probe, so that it can
 * automatically handle device-specific settings. It can use the GDB-RSP serial
 * interface, or the GDB-MI console interface.
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
#ifndef _BMP_SCRIPT_H
#define _BMP_SCRIPT_H

#include <stdint.h>

#if defined __cplusplus
  extern "C" {
#endif

#define SCRIPT_MAGIC  0x6dce7fd0  /**< magic value for parameter replacement */

int bmscript_load(const char *mcu, const char *architecture);
void bmscript_clear(void);
void bmscript_clearcache(void);

int bmscript_line(const char *name, char *oper, uint32_t *address, uint32_t *value, uint8_t *size);
int bmscript_line_fmt(const char *name, char *line, const unsigned long *params);

int architecture_match(const char *architecture, const char *mcufamily);

#if defined __cplusplus
  }
#endif

#endif /* _BMP_SCRIPT_H */

