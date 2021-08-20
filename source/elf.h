/*
 * Routines for querying information on ELF files and post-processing them
 * for requirements of specific micro-controllers. At this moment, the utility
 * supports various ranges of the LPC family by NXP.
 *
 * Build this file with the macro STANDALONE defined on the command line to
 * create a self-contained executable.
 *
 * Copyright 2015,2019 CompuPhase
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
#ifndef _ELF__H
#define _ELF__H

#if defined __cplusplus
  extern "C" {
#endif

enum {
  ELFERR_NONE = 0,
  ELFERR_CHKSUMSET,        /* checksum was already the correct value (no error, but no change either) */
  ELFERR_UNKNOWNDRIVER,    /* unknown microcontroller driver name */
  ELFERR_FILEFORMAT,       /* unsupported file format */
  ELFERR_NOMATCH,          /* no matching section / segment */
};

int elf_info(FILE *fp,int *wordsize,int *bigendian,int *machine);

int elf_segment_by_index(FILE *fp,int index,
                         int *type,
                         unsigned long *offset,unsigned long *filesize,
                         unsigned long *vaddr,unsigned long *paddr,
                         unsigned long *memsize);

int elf_section_by_name(FILE *fp,const char *sectionname,unsigned long *offset,
                        unsigned long *address,unsigned long *length);

int elf_section_by_address(FILE *fp,unsigned long baseaddr,
                           char *sectionname,size_t namelength,unsigned long *offset,
                           unsigned long *address,unsigned long *length);

int elf_patch_vecttable(FILE *fp,const char *driver,unsigned int *checksum);
int elf_check_crp(FILE *fp,int *crp);

#if defined __cplusplus
  }
#endif

#endif /* _ELF_H */

