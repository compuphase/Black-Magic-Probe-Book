/*
 * Basic re-implementation of the "ident" utility of the RCS suite, to extract
 * RCS identification strings from source and binary files. This implementation
 * only supports the keywords "Author", "Date", "Id", and "Revision" (which may
 * be abbreviated to "Rev").
 *
 * Copyright 2021 CompuPhase
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

#ifndef _IDENT_H
#define _IDENT_H

#include <stdio.h>

#if defined __cplusplus
  extern "C" {
#endif

int ident(FILE *fp, int skip, char *key, size_t key_size, char *value, size_t value_size);

#if defined __cplusplus
  }
#endif

#endif /* _IDENT_H */

