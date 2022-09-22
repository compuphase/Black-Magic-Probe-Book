/*
 * Routines for querying information on ELF files and post-processing them
 * for requirements of specific micro-controllers. At this moment, the utility
 * supports various ranges of the LPC family by NXP.
 *
 * Copyright 2015,2019-2022 CompuPhase
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
  ELFERR_CHKSUMSET,     /* checksum was already the correct value (no error, but no change either) */
  ELFERR_UNKNOWNDRIVER, /* unknown microcontroller driver name */
  ELFERR_FILEFORMAT,    /* unsupported file format */
  ELFERR_NOMATCH,       /* no matching section / segment */
  ELFERR_MEMORY,        /* insufficient memory */
};

/* segment types */
#define ELF_PT_NULL     0
#define ELF_PT_LOAD     1 /* segment is loadable */
#define ELF_PT_DYNAMIC  2 /* segment specifies dynamic linking information */
#define ELF_PT_INTERP   3 /* segment is a path to an interpreter */
#define ELF_PT_NOTE     4
#define ELF_PT_SHLIB    5
#define ELF_PT_PHDR     6

/* segment flags */
#define ELF_PF_X  0x01  /* execute */
#define ELF_PF_W  0x02  /* write */
#define ELF_PF_R  0x04  /* read */


typedef struct tagELF_SYMBOL {
  const char *name;     /* symbol name */
  unsigned long address;/* memory address */
  unsigned long size;   /* code size or data size (0 for unknown) */
  unsigned char is_func;/* 1 for a function symbol, 0 for a variable/data */
  unsigned char is_ext; /* 1 for external scope, 0 for file local scope */
} ELF_SYMBOL;

int elf_info(FILE *fp,int *wordsize,int *bigendian,int *machine,unsigned long *entry_addr);

int elf_segment_by_index(FILE *fp,int index,
                         int *type, int *flags,
                         unsigned long *offset,unsigned long *filesize,
                         unsigned long *vaddr,unsigned long *paddr,
                         unsigned long *memsize);

int elf_section_by_name(FILE *fp,const char *sectionname,unsigned long *offset,
                        unsigned long *address,unsigned long *length);

int elf_section_by_address(FILE *fp,unsigned long baseaddr,
                           char *sectionname,size_t namelength,unsigned long *offset,
                           unsigned long *address,unsigned long *length);

int elf_load_symbols(FILE *fp,ELF_SYMBOL *symbols,unsigned *number);
void elf_clear_symbols(ELF_SYMBOL *symbols,unsigned number);

int elf_patch_vecttable(FILE *fp,const char *driver,unsigned *checksum);
int elf_check_crp(FILE *fp,int *crp);

#if defined __cplusplus
  }
#endif

#endif /* _ELF_H */

