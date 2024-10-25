/*
 * File loading support for binary (executable) files, with support for ELF,
 * Intel HEX and BIN formats.
 *
 * Copyright 2023 CompuPhase
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
#ifndef _FILELOADER_H
#define _FILELOADER_H

#include <stdbool.h>

#if defined __cplusplus
  extern "C" {
#endif

enum {
  FILETYPE_NONE,  /* no file is loaded */
  FILETYPE_ELF,
  FILETYPE_HEX,
  FILETYPE_UNKNOWN,
};

enum {
  SECTIONTYPE_UNKNOWN,
  SECTIONTYPE_CODE,
  SECTIONTYPE_DATA,
};

enum {
  FSERR_NONE,
  FSERR_CHKSUMSET,
  FSERR_NO_DRIVER,
  FSERR_NO_VECTTABLE,
};

void filesection_clearall(void);
bool hex_isvalid(FILE *fp);
bool filesection_loadall(const char *filename);
bool filesection_getdata(unsigned index, unsigned long *address, unsigned char **buffer, unsigned long *size, int *type);
int  filesection_filetype(void);
void filesection_relocate(unsigned long offset);
unsigned char *filesection_get_address(unsigned long address, size_t size);

int  filesection_patch_vecttable(const char *driver, unsigned int *checksum);
int  filesection_get_crp(void);
bool filesection_set_crp(int crp);

#if defined __cplusplus
  }
#endif

#endif /* _FILELOADER_H */

