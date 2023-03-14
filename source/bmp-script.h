/*
 * General purpose "script" support for the Black Magic Probe, so that it can
 * automatically handle device-specific settings. It can use the GDB-RSP serial
 * interface, or the GDB-MI console interface.
 *
 * Copyright 2019-2023 CompuPhase
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

enum {
  OT_LITERAL,
  OT_ADDRESS,
  OT_PARAM,
};
typedef struct tagOPERAND {
  uint32_t data;  /* register or memory address, literal value, or parameter index */
  uint8_t type;   /* one of the OT_xxx values */
  uint8_t size;   /* operand size in bytes */
  uint8_t pshift; /* for parameters: shift-left of parameter value */
  uint32_t plit;  /* for parameters: literal value added to parameter value */
} OPERAND;

enum {
  OP_MOV,       /* a = b */
  OP_ORR,       /* a |= b */
  OP_AND,       /* a &= b */
  OP_AND_INV,   /* a &= ~b */
};

int bmscript_load(const char *mcu, const char *architecture);
void bmscript_clear(void);
void bmscript_clearcache(void);

bool bmscript_line(const char *name, uint16_t *oper, OPERAND *lvalue, OPERAND *rvalue);
bool bmscript_line_fmt(const char *name, char *line, const unsigned long *params, size_t paramcount);

bool architecture_match(const char *architecture, const char *mcufamily);

#if defined __cplusplus
  }
#endif

#endif /* _BMP_SCRIPT_H */

