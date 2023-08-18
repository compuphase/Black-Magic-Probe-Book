/* Routines to get the line number and symbol tables from the DWARF debug
 * information * in an ELF file. For the symbol table, only the function and
 * variable symbols are stored.
 *
 * Copyright (c) 2015,2019-2023 CompuPhase
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

enum {
  SCOPE_UNKNOWN,
  SCOPE_EXTERNAL,       /* global variable or function */
  SCOPE_UNIT,           /* static variable/function declared at file scope (compilation unit) */
  SCOPE_FUNCTION,       /* local variable (including static locals & function arguments) */
};

typedef struct tagDWARF_SYMBOLLIST {
  struct tagDWARF_SYMBOLLIST *next;
  char *name;
  unsigned code_addr;   /* function address, 0 for a variable */
  unsigned code_range;  /* size of the code (functions only, 0 for variables) */
  unsigned data_addr;   /* variable address (globals & statics only), 0 for a function or a local variable */
  int line;             /* line number of the declaration/definition */
  int line_limit;       /* last line of the definition takes (functions) or line at which the scope ends (variables) */
  short fileindex;      /* file where the declaration/definition appears in */
  short scope;
} DWARF_SYMBOLLIST;

typedef struct tagDWARF_LINEENTRY {
  unsigned address;
  int line;
  int fileindex;
  int view;
} DWARF_LINEENTRY;

typedef struct tagDWARF_LINETABLE {
  DWARF_LINEENTRY *table;
  unsigned entries;     /* number of valid entries in the table */
  unsigned size;        /* number of allocated entries */
} DWARF_LINETABLE;

#define DWARF_IS_FUNCTION(sym)  ((sym)->code_range>0)
#define DWARF_IS_VARIABLE(sym)  ((sym)->code_range==0)

enum {
  DWARF_SORT_NAME,
  DWARF_SORT_ADDRESS,
};

bool dwarf_read(FILE *fp,DWARF_LINETABLE *linetable,DWARF_SYMBOLLIST *symboltable,DWARF_PATHLIST *filetable,int *address_size);
void dwarf_cleanup(DWARF_LINETABLE *linetable,DWARF_SYMBOLLIST *symboltable,DWARF_PATHLIST *filetable);

const DWARF_SYMBOLLIST* dwarf_sym_from_name(const DWARF_SYMBOLLIST *symboltable,const char *name,int fileindex,int lineindex);
const DWARF_SYMBOLLIST* dwarf_sym_from_address(const DWARF_SYMBOLLIST *symboltable,unsigned address,int exact);
const DWARF_SYMBOLLIST* dwarf_sym_from_index(const DWARF_SYMBOLLIST *symboltable,unsigned index);
unsigned                dwarf_collect_functions_in_file(const DWARF_SYMBOLLIST *symboltable,int fileindex,int sort,const DWARF_SYMBOLLIST *list[],int numentries);
const char*             dwarf_path_from_fileindex(const DWARF_PATHLIST *filetable,int fileindex);
int                     dwarf_fileindex_from_path(const DWARF_PATHLIST *filetable,const char *path);
const DWARF_LINEENTRY*  dwarf_line_from_address(const DWARF_LINETABLE *linetable,unsigned address);

#if defined __cplusplus
  }
#endif

#endif /* _DWARF_H */
