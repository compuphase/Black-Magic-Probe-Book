/* Routines to get the line number and symbol tables from the DWARF debug
 * information * in an ELF file. For the symbol table, only the function
 * symbols are stored.
 *
 * Copyright (c) 2015,2019-2020 CompuPhase
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
#ifndef _DWARF_H
#define _DWARF_H

#if defined __cplusplus
  extern "C" {
#endif

typedef struct tagDWARF_PATHLIST {
  struct tagDWARF_PATHLIST *next;
  char *name;
} DWARF_PATHLIST;

typedef struct tagDWARF_SYMBOLLIST {
  struct tagDWARF_SYMBOLLIST *next;
  char *name;
  unsigned address; /* function address, 0 for a variable */
  int line;
  int fileindex;
} DWARF_SYMBOLLIST;

typedef struct tagDWARF_LINELOOKUP {
  struct tagDWARF_LINELOOKUP *next;
  unsigned address;
  int line;
  int fileindex;
} DWARF_LINELOOKUP;

int dwarf_read(FILE *fp,DWARF_LINELOOKUP *linetable,DWARF_SYMBOLLIST *symboltable,DWARF_PATHLIST *filetable,int *address_size);
void dwarf_cleanup(DWARF_LINELOOKUP *linetable,DWARF_SYMBOLLIST *symboltable,DWARF_PATHLIST *filetable);

const DWARF_SYMBOLLIST *dwarf_sym_from_name(const DWARF_SYMBOLLIST *symboltable,const char *name);
const DWARF_SYMBOLLIST *dwarf_sym_from_address(const DWARF_SYMBOLLIST *symboltable,unsigned address,int exact);
const DWARF_SYMBOLLIST *dwarf_sym_from_index(const DWARF_SYMBOLLIST *symboltable,unsigned index);
const char *dwarf_path_from_index(const DWARF_PATHLIST *filetable,int fileindex);
const DWARF_LINELOOKUP *dwarf_line_from_address(const DWARF_LINELOOKUP *linetable,unsigned address);

#if defined __cplusplus
  }
#endif

#endif /* _DWARF_H */
