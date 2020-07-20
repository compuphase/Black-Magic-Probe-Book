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
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dwarf.h"
#include "elf.h"

#if defined __GNUC__
  #define PACKED        __attribute__((packed))
#else
  #define PACKED
#endif

#if defined __LINUX__ || defined __FreeBSD__ || defined __APPLE__
  #pragma pack(1)       /* structures must be packed (byte-aligned) */
#elif defined MACOS && defined __MWERKS__
  #pragma options align=mac68k
#else
  #pragma pack(push)
  #pragma pack(1)       /* structures must be packed (byte-aligned) */
  #if defined __TURBOC__
    #pragma option -a-  /* "pack" pragma for older Borland compilers */
  #endif
#endif
#if defined _MSC_VER
  #define strdup(s)   _strdup(s)
#endif

#if !defined _MAX_PATH
  #define _MAX_PATH 260
#endif

typedef struct tagELF32HDR {
  uint8_t  magic[4];    /* 0x7f + "ELF" */
  uint8_t  wordsize;    /* 1 = 32-bit, 2 = 64-bit */
  uint8_t  endian;      /* 1 = little endian, 2 = big endian */
  uint8_t  elfversion;  /* 1 for the original ELF format version */
  uint8_t  abi;         /* target OS or ABI, 3 = Linux */
  uint8_t  abiversion;  /* ABI version */
  uint8_t  unused[7];
  /* ----- */
  uint16_t type;        /* 1 = relocatable, 2 = executable, 3 = shared, 4 = core */
  uint16_t machine;     /* 0x03 = x86, 0x28 = ARM, 0x32 = IA-64, 0x3e = x86-64 */
  uint32_t version;     /* 1 for the original ELF format */
  uint32_t entry;       /* memory address of the entry point */
  uint32_t phoff;       /* file offset to the start of the program header table */
  uint32_t shoff;       /* file offset to the start of the section header table */
  uint32_t flags;
  uint16_t ehsize;      /* the size of this header (52 bytes) */
  uint16_t phentsize;   /* the size of an entry in the program header table */
  uint16_t phnum;       /* the number of entries in the program header table */
  uint16_t shentsize;   /* the size of an entry in the section header table */
  uint16_t shnum;       /* the number of entries in the section header table */
  uint16_t shtrndx;     /* the index of the entry in the section header table that holds the section names */
} PACKED ELF32HDR;

typedef struct tagELF64HDR {
  uint8_t  magic[4];    /* 0x7f + "ELF" */
  uint8_t  wordsize;    /* 1 = 32-bit, 2 = 64-bit */
  uint8_t  endian;      /* 1 = little endian, 2 = big endian */
  uint8_t  elfversion;  /* 1 for the original ELF format version */
  uint8_t  abi;         /* target OS or ABI, 3 = Linux */
  uint8_t  abiversion;  /* ABI version */
  uint8_t  unused[7];
  /* ----- */
  uint32_t type;        /* 1 = relocatable, 2 = executable, 3 = shared, 4 = core */
  uint32_t machine;     /* 0x03 = x86, 0x28 = ARM, 0x32 = IA-64, 0x3e = x86-64 */
  uint64_t version;     /* 1 for the original ELF format */
  uint64_t entry;       /* memory address of the entry point */
  uint64_t phoff;       /* file offset to the start of the program header table */
  uint64_t shoff;       /* file offset to the start of the section header table */
  uint64_t flags;
  uint32_t ehsize;      /* the size of this header (52 bytes) */
  uint32_t phentsize;   /* the size of an entry in the program header table */
  uint32_t phnum;       /* the number of entries in the program header table */
  uint32_t shentsize;   /* the size of an entry in the section header table */
  uint32_t shnum;       /* the number of entries in the section header table */
  uint32_t shtrndx;     /* the index of the entry in the section header table that holds the string table */
} PACKED ELF64HDR;

typedef struct tagELF32SECTION {
  uint32_t name;        /* index in the string table */
  uint32_t type;
  uint32_t flags;
  uint32_t addr;        /* memory address */
  uint32_t offset;      /* file offset to the start of the section */
  uint32_t size;        /* size of the section */
  uint32_t link;
  uint32_t info;
  uint32_t addralign;
  uint32_t entsize;     /* entry size, for sections that have fixed-length entries */
} PACKED ELF32SECTION;

typedef struct tagDWARFTABLE {
  unsigned long offset;
  unsigned long size;
} DWARFTABLE;

enum {
  TABLE_INFO,
  TABLE_ABBREV,
  TABLE_STR,
  TABLE_LINE,
  TABLE_PUBNAME,
  /* ----- */
  TABLE_COUNT
};

typedef struct tagINFO_HDR32 {
  uint32_t unit_length; /* total length of this block, excluding this field */
  uint16_t version;
  uint32_t abbrev_offs; /* offset into the .debug_abbrev table */
  uint8_t  address_size;/* size in bytes of an address */
} PACKED INFO_HDR32;

typedef struct tagINFO_HDR64 {
  uint32_t reserved;    /* must be 0xffffffff */
  uint64_t unit_length; /* total length of this block, excluding this field */
  uint16_t version;
  uint64_t abbrev_offs; /* offset into the .debug_abbrev table */
  uint8_t  address_size;/* size in bytes of an address */
} PACKED INFO_HDR64;

typedef struct tagDWARF_FIXEDPROLOGUE {
  uint32_t total_length;    /* length of the line number table, minus the 4 bytes for this total_length field */
  uint16_t version;
  uint32_t prologue_length; /* offset to the first opcode of the line number program (relative to this prologue_length field) */
  uint8_t  min_instruction_size;
  uint8_t  default_is_stmt; /* default value to initialize the state machine with */
  int8_t   line_base;
  uint8_t  line_range;
  uint8_t  opcode_base;      /* number of the first special opcode */
  /* standard opcode lengths (array of length "opcode_base") */
  /* include directories, a sequence of zero-terminated strings (and the sequence itself ends with a zero-byte) */
  /* file names, */
} PACKED DWARF_FIXEDPROLOGUE;

typedef struct tagSTATE {
  /* fields for DWARF 2 */
  uint32_t address;
  int file;
  int line;
  int column;
  int is_stmt;          /* whether the address is the start of a statement */
  int basic_block;      /* whether the address is the start of a basic block */
  int end_seq;
  /* added fields for DWARF 3 */
  int prologue_end;     /* function prologue end */
  int epiloge_begin;    /* function epilogue start */
  int isa;              /* instruction set architecture */
  /* added fields for DWARF 4 */
  int discriminator;    /* compiler-assigned id for a "block" to which the instruction belongs */
} STATE;

typedef struct tagPUBNAME_HDR32 {
  uint32_t totallength; /* total length of this block, excluding this field */
  uint16_t version;
  uint32_t info_offs;   /* offset into the comprehensive debug table */
  uint32_t info_size;   /* size of this symbol in the comprehensive debug table */
} PACKED PUBNAME_HDR32;

/* tags */
#define DW_TAG_array_type             0x01
#define DW_TAG_class_type             0x02
#define DW_TAG_entry_point            0x03
#define DW_TAG_enumeration_type       0x04
#define DW_TAG_formal_parameter       0x05
#define DW_TAG_imported_declaration   0x08
#define DW_TAG_label                  0x0a
#define DW_TAG_lexical_block          0x0b
#define DW_TAG_member                 0x0d
#define DW_TAG_pointer_type           0x0f
#define DW_TAG_reference_type         0x10
#define DW_TAG_compile_unit           0x11
#define DW_TAG_string_type            0x12
#define DW_TAG_structure_type         0x13
#define DW_TAG_subroutine_type        0x15
#define DW_TAG_typedef                0x16
#define DW_TAG_union_type             0x17
#define DW_TAG_unspecified_parameters 0x18
#define DW_TAG_variant                0x19
#define DW_TAG_common_block           0x1a
#define DW_TAG_common_inclusion       0x1b
#define DW_TAG_inheritance            0x1c
#define DW_TAG_inlined_subroutine     0x1d
#define DW_TAG_module                 0x1e
#define DW_TAG_ptr_to_member_type     0x1f
#define DW_TAG_set_type               0x20
#define DW_TAG_subrange_type          0x21
#define DW_TAG_with_stmt              0x22
#define DW_TAG_access_declaration     0x23
#define DW_TAG_base_type              0x24
#define DW_TAG_catch_block            0x25
#define DW_TAG_const_type             0x26
#define DW_TAG_constant               0x27
#define DW_TAG_enumerator             0x28
#define DW_TAG_file_type              0x29
#define DW_TAG_friend                 0x2a
#define DW_TAG_namelist               0x2b
#define DW_TAG_namelist_item          0x2c
#define DW_TAG_packed_type            0x2d
#define DW_TAG_subprogram             0x2e
#define DW_TAG_template_type_param    0x2f
#define DW_TAG_template_value_param   0x30
#define DW_TAG_thrown_type            0x31
#define DW_TAG_try_block              0x32
#define DW_TAG_variant_part           0x33
#define DW_TAG_variable               0x34
#define DW_TAG_volatile_type          0x35
#define DW_TAG_lo_user                0x4080
#define DW_TAG_hi_user                0xffff

#define DW_AT_sibling                 0x01  /* reference */
#define DW_AT_location                0x02  /* block, constant */
#define DW_AT_name                    0x03  /* string */
#define DW_AT_ordering                0x09  /* constant */
#define DW_AT_byte_size               0x0b  /* constant */
#define DW_AT_bit_offset              0x0c  /* constant */
#define DW_AT_bit_size                0x0d  /* constant */
#define DW_AT_stmt_list               0x10  /* constant */
#define DW_AT_low_pc                  0x11  /* address */
#define DW_AT_high_pc                 0x12  /* address */
#define DW_AT_language                0x13  /* constant */
#define DW_AT_discr                   0x15  /* reference */
#define DW_AT_discr_value             0x16  /* constant */
#define DW_AT_visibility              0x17  /* constant */
#define DW_AT_import                  0x18  /* reference */
#define DW_AT_string_length           0x19  /* block, constant */
#define DW_AT_common_reference        0x1a  /* reference */
#define DW_AT_comp_dir                0x1b  /* string */
#define DW_AT_const_value             0x1c  /* string, constant, block */
#define DW_AT_containing_type         0x1d  /* reference */
#define DW_AT_default_value           0x1e  /* reference */
#define DW_AT_inline                  0x20  /* constant */
#define DW_AT_is_optional             0x21  /* flag */
#define DW_AT_lower_bound             0x22  /* constant, reference */
#define DW_AT_producer                0x25  /* string */
#define DW_AT_prototyped              0x27  /* flag */
#define DW_AT_return_addr             0x2a  /* block, constant */
#define DW_AT_start_scope             0x2c  /* constant */
#define DW_AT_stride_size             0x2e  /* constant */
#define DW_AT_upper_bound             0x2f  /* constant, reference */
#define DW_AT_abstract_origin         0x31  /* reference */
#define DW_AT_accessibility           0x32  /* constant */
#define DW_AT_address_class           0x33  /* constant */
#define DW_AT_artificial              0x34  /* flag */
#define DW_AT_base_types              0x35  /* reference */
#define DW_AT_calling_convention      0x36  /* constant */
#define DW_AT_count                   0x37  /* constant, reference */
#define DW_AT_data_member_location    0x38  /* block, reference */
#define DW_AT_decl_column             0x39  /* constant */
#define DW_AT_decl_file               0x3a  /* constant */
#define DW_AT_decl_line               0x3b  /* constant */
#define DW_AT_declaration             0x3c  /* flag */
#define DW_AT_discr_list              0x3d  /* block */
#define DW_AT_encoding                0x3e  /* constant */
#define DW_AT_external                0x3f  /* flag */
#define DW_AT_frame_base              0x40  /* block, constant */
#define DW_AT_friend                  0x41  /* reference */
#define DW_AT_identifier_case         0x42  /* constant */
#define DW_AT_macro_info              0x43  /* constant */
#define DW_AT_namelist_item           0x44  /* block */
#define DW_AT_priority                0x45  /* reference */
#define DW_AT_segment                 0x46  /* block, constant */
#define DW_AT_specification           0x47  /* reference */
#define DW_AT_static_link             0x48  /* block, constant */
#define DW_AT_type                    0x49  /* reference */
#define DW_AT_use_location            0x4a  /* block, constant */
#define DW_AT_variable_parameter      0x4b  /* flag */
#define DW_AT_virtuality              0x4c  /* constant */
#define DW_AT_vtable_elem_location    0x4d  /* block, reference */
#define DW_AT_lo_user                 0x2000
#define DW_AT_hi_user                 0x3fff

#define DW_FORM_addr                  0x01  /* address, 4 bytes for 32-bit, 8 bytes for 64-bit */
#define DW_FORM_block2                0x03  /* block, 2-byte length + up to 64K data bytes */
#define DW_FORM_block4                0x04  /* block, 4-byte length + up to 4G data bytes */
#define DW_FORM_data2                 0x05  /* constant, 2 bytes */
#define DW_FORM_data4                 0x06  /* constant, 4 bytes */
#define DW_FORM_data8                 0x07  /* constant, 8 bytes */
#define DW_FORM_string                0x08  /* string, zero-terminated */
#define DW_FORM_block                 0x09  /* block, unsigned LEB128-encoded length + data bytes */
#define DW_FORM_block1                0x0a  /* block, 1-byte length + up to 255 data bytes */
#define DW_FORM_data1                 0x0b  /* constant, 1 bytes */
#define DW_FORM_flag                  0x0c  /* flag, 1 byte (0=false, any non-zero=true) */
#define DW_FORM_sdata                 0x0d  /* constant, signed LEB128 */
#define DW_FORM_strp                  0x0e  /* string, 4-byte offset into the .debug_str section */
#define DW_FORM_udata                 0x0f  /* constant, unsigned LEB128 */
#define DW_FORM_ref_addr              0x10  /* reference, address size (4 bytes on 32-bit, 8 bytes on 64-bit) */
#define DW_FORM_ref1                  0x11  /* reference, 1 bytes */
#define DW_FORM_ref2                  0x12  /* reference, 2 bytes */
#define DW_FORM_ref4                  0x13  /* reference, 4 bytes */
#define DW_FORM_ref8                  0x14  /* reference, 8 bytes */
#define DW_FORM_ref_udata             0x15  /* reference, unsigned LEB128 */
#define DW_FORM_indirect              0x16  /* format is specified in the .debug_info data (not in the abbreviation) */
    /* DWARF 4+ */
#define DW_FORM_sec_offset            0x17  /* offset into the .debug_line section (4 bytes on 32-bit, 8 bytes on 64-bit) */
#define DW_FORM_exprloc               0x18  /* block for expression/location, unsigned LEB128-encoded length + data bytes */
#define DW_FORM_flag_present          0x19  /* implicit flag (no data) */
#define DW_FORM_ref_sig8              0x20  /* 64-bit signature for a type defined in a different unit */

/* line number opcodes */
#define DW_LNS_extended_op        0   /* version 2+ */
#define DW_LNS_copy               1
#define DW_LNS_advance_pc         2
#define DW_LNS_advance_line       3
#define DW_LNS_set_file           4
#define DW_LNS_set_column         5
#define DW_LNS_negate_stmt        6
#define DW_LNS_set_basic_block    7
#define DW_LNS_const_add_pc       8
#define DW_LNS_fixed_advance_pc   9
#define DW_LNS_set_prologue_end   10  /* version 3+ */
#define DW_LNS_set_epilogue_begin 11
#define DW_LNS_set_isa            12
/* line number extended opcodes */
#define DW_LNE_end_sequence       1   /* version 2+ */
#define DW_LNE_set_address        2
#define DW_LNE_define_file        3
#define DW_LNE_set_discriminator  4   /* version 4+ */

#if defined __LINUX__ || defined __FreeBSD__ || defined __APPLE__
  #pragma pack()      /* reset default packing */
#elif defined MACOS && defined __MWERKS__
  #pragma options align=reset
#else
  #pragma pack(pop)   /* reset previous packing */
#endif


#define SWAP16(v)     ((((v) >> 8) & 0xff) | (((v) & 0xff) << 8))
#define SWAP32(v)     ((((v) >> 24) & 0xff) | (((v) & 0xff0000) >> 8) | (((v) & 0xff00) << 8)  | (((v) & 0xff) << 24))


typedef struct tagATTRIBUTE {
  int tag;
  int format;
} ATTRIBUTE;

typedef struct tagABBREVLIST {
  struct tagABBREVLIST *next;
  int unit;     /* the compilation unit */
  int id;
  int tag;
  int has_children;
  int count;    /* number of attributes */
  ATTRIBUTE *attributes;
} ABBREVLIST;

typedef struct tagPATHXREF {
  struct tagPATHXREF *next;
  int unit, file; /* input pair */
  int index;      /* index in DWARF_PATHLIST */
} PATHXREF;

static ABBREVLIST *abbrev_insert(ABBREVLIST *root,int unit,int id,int tag,int has_children,
                                 int num_attributes,const ATTRIBUTE attributes[])
{
  ABBREVLIST *cur;

  assert(root!=NULL);
  assert(attributes!=NULL || num_attributes==0);
  if ((cur=(ABBREVLIST*)malloc(sizeof(ABBREVLIST)))==NULL)
    return NULL;      /* insufficient memory */
  cur->unit=unit;
  cur->id=id;
  cur->tag=tag;
  cur->has_children=has_children;
  cur->count=num_attributes;
  if (num_attributes>0) {
    int idx;
    if ((cur->attributes=malloc(num_attributes*sizeof(ATTRIBUTE)))==NULL) {
      free(cur);
      return NULL;      /* insufficient memory */
    }
    for (idx=0; idx<num_attributes; idx++) {
      cur->attributes[idx].tag=attributes[idx].tag;
      cur->attributes[idx].format=attributes[idx].format;
    }
  } else {
    cur->attributes=NULL;
  }
  /* insert as "last" (append mode) */
  assert(root!=NULL);
  while (root->next!=NULL)
    root=root->next;
  cur->next=root->next;
  root->next=cur;
  return cur;
}

static void abbrev_deletetable(ABBREVLIST *root)
{
  ABBREVLIST *cur,*next;

  assert(root!=NULL);
  cur=root->next;
  while (cur!=NULL) {
    next=cur->next;
    assert(cur->attributes!=NULL || cur->count==0);
    if (cur->attributes!=NULL)
      free(cur->attributes);
    free(cur);
    cur=next;
  } /* while */
  memset(root,0,sizeof(ABBREVLIST));
}

static ABBREVLIST *abbrev_find(const ABBREVLIST *root,int unit,int id)
{
  ABBREVLIST *cur;
  int index;

  assert(root!=NULL);
  index=0;
  for (cur=root->next; cur!=NULL && (cur->unit!=unit || cur->id!=id); cur=cur->next)
    index++;
  return cur;
}


static PATHXREF *pathxref_insert(PATHXREF *root,int unit,int file,int index)
{
  PATHXREF *cur;

  if ((cur=(PATHXREF*)malloc(sizeof(PATHXREF)))==NULL)
    return NULL;      /* insufficient memory */
  cur->unit=unit;
  cur->file=file;
  cur->index=index;
  /* insert as "last" (append mode) */
  assert(root!=NULL);
  while (root->next!=NULL)
    root=root->next;
  cur->next=root->next;
  root->next=cur;
  return cur;
}

static void pathxref_deletetable(PATHXREF *root)
{
  PATHXREF *cur,*next;

  assert(root!=NULL);
  cur=root->next;
  while (cur!=NULL) {
    next=cur->next;
    free(cur);
    cur=next;
  } /* while */
  memset(root,0,sizeof(PATHXREF));
}

static int pathxref_find(const PATHXREF *root,int unit,int file)
{
  PATHXREF *cur;

  assert(root!=NULL);
  for (cur=root->next; cur!=NULL && (cur->unit!=unit || cur->file!=file); cur=cur->next)
    /* nothing */;
  return (cur!=NULL) ? cur->index : -1;
}


static DWARF_PATHLIST *path_insert(DWARF_PATHLIST *root,const char *string)
{
  DWARF_PATHLIST *cur;

  assert(string!=NULL);
  if ((cur=(DWARF_PATHLIST*)malloc(sizeof(DWARF_PATHLIST)))==NULL)
    return NULL;      /* insufficient memory */
  if ((cur->name=strdup(string))==NULL) {
    free(cur);
    return NULL;      /* insufficient memory */
  }
  /* insert as "last" (append mode) */
  assert(root!=NULL);
  while (root->next!=NULL)
    root=root->next;
  cur->next=root->next;
  root->next=cur;
  return cur;
}

static void path_deletetable(DWARF_PATHLIST *root)
{
  DWARF_PATHLIST *cur,*next;

  assert(root!=NULL);
  cur=root->next;
  while (cur!=NULL) {
    next=cur->next;
    assert(cur->name!=NULL);
    free(cur->name);
    free(cur);
    cur=next;
  } /* while */
  memset(root,0,sizeof(DWARF_PATHLIST));
}

static char *path_get(const DWARF_PATHLIST *root,int index)
{
  DWARF_PATHLIST *cur;

  assert(root!=NULL);
  for (cur=root->next; cur!=NULL && index-->0; cur=cur->next)
    /* nothing */;
  if (cur!=NULL) {
    assert(cur->name!=NULL);
    return cur->name;
  }
  return NULL;
}

static int path_find(const DWARF_PATHLIST *root,const char *name)
{
  DWARF_PATHLIST *cur;
  int index;

  assert(root!=NULL);
  assert(name!=NULL);
  index=0;
  for (cur=root->next; cur!=NULL && strcmp(name,cur->name)!=0; cur=cur->next)
    index++;
  return (cur!=NULL) ? index : -1;
}


static DWARF_LINELOOKUP *line_findline(const DWARF_LINELOOKUP *root,int line,int fileindex)
{
  DWARF_LINELOOKUP *cur;

  assert(root!=NULL);
  for (cur=root->next; cur!=NULL && (cur->line!=line || cur->fileindex!=fileindex); cur=cur->next)
    /* nothing */;
  return cur;
}

static DWARF_LINELOOKUP *line_findaddress(const DWARF_LINELOOKUP *root,unsigned address,int fileindex)
{
  DWARF_LINELOOKUP *cur;

  assert(root!=NULL);
  for (cur=root->next; cur!=NULL && (cur->address!=address || cur->fileindex!=fileindex); cur=cur->next)
    /* nothing */;
  return cur;
}

static DWARF_LINELOOKUP *line_insert(DWARF_LINELOOKUP *root,int line,unsigned address,int fileindex)
{
  DWARF_LINELOOKUP *cur;

  assert(root!=NULL);
  /* first try to find an item with that line number, if it exists, keep the
     lowest address in the item;
	 then try to find an item with the same address, if it exists, keep the
	 highest line number */
  if ((cur=line_findline(root,line,fileindex))!=NULL) {
    if (address<cur->address)
      cur->address=address;
  } else if ((cur=line_findaddress(root,address,fileindex))!=NULL) {
    if (line>cur->line)
      cur->line=line;
	assert(fileindex==cur->fileindex);
  } else {
    DWARF_LINELOOKUP *pred;
    if ((cur=(DWARF_LINELOOKUP*)malloc(sizeof(DWARF_LINELOOKUP)))==NULL)
      return NULL;      /* insufficient memory */
    cur->line=line;
    cur->address=address;
    cur->fileindex=fileindex;
    /* find insertion position (keep the list sorted on address) */
    for (pred=root; pred->next!=NULL && pred->next->address<address; pred=pred->next)
      /* nothing */;
    /* insert */
    assert(pred!=NULL);
    cur->next=pred->next;
    pred->next=cur;
  }
  return cur;
}

static void line_deletetable(DWARF_LINELOOKUP *root)
{
  DWARF_LINELOOKUP *cur,*next;

  assert(root!=NULL);
  cur=root->next;
  while (cur!=NULL) {
    next=cur->next;
    free(cur);
    cur=next;
  } /* while */
  memset(root,0,sizeof(DWARF_LINELOOKUP));
}

static DWARF_SYMBOLLIST *symname_insert(DWARF_SYMBOLLIST *root,const char *name,
                                        unsigned address,int file,int line)
{
  DWARF_SYMBOLLIST *cur,*pred;

  assert(root!=NULL);
  assert(name!=NULL);
  if ((cur=(DWARF_SYMBOLLIST*)malloc(sizeof(DWARF_SYMBOLLIST)))==NULL)
    return NULL;      /* insufficient memory */
  if ((cur->name=strdup(name))==NULL) {
    free(cur);
    return NULL;      /* insufficient memory */
  }
  cur->address=address;
  cur->fileindex=file;
  cur->line=line;
  /* insert sorted on name */
  for (pred=root; pred->next!=NULL && strcmp(name,pred->next->name)>0; pred=pred->next)
    /* nothing */;
  /* insert */
  assert(pred!=NULL);
  cur->next=pred->next;
  pred->next=cur;
  return cur;
}

static void symname_deletetable(DWARF_SYMBOLLIST *root)
{
  DWARF_SYMBOLLIST *cur,*next;

  assert(root!=NULL);
  cur=root->next;
  while (cur!=NULL) {
    next=cur->next;
    assert(cur->name!=NULL);
    free(cur->name);
    free(cur);
    cur=next;
  } /* while */
  memset(root,0,sizeof(DWARF_SYMBOLLIST));
}


static char *strins(char *string,const char *sub)
{
  int offs=strlen(sub);
  int len=strlen(string);
  memmove(string+offs,string,len+1);
  memmove(string,sub,offs);
  return string;
}

static long read_leb128(FILE *fp,int sign,int *size)
{
  long value=0;
  int shift=0;
  int byte;

  if (size!=NULL)
    *size=0;
  while ((byte=fgetc(fp))!=EOF) {
    if (size!=NULL)
      *size+=1;
    value |= (long)(byte & 0x7f) << shift;
    shift+=7;
    if ((byte & 0x80)==0)
      break;  /* no continuation, so done */
  }
  /* sign-extend; since bit 7 in the last byte read is the continuation bit,
     bit 6 is the sign bit */
  if (sign && (byte & 0x40)!=0 && shift < (sizeof(long)*8))
    value |= ~0 << shift;

  return value;
}

/* read_value() reads numeric data in various formats. It does not read address
   data or other fields where the data size depends on the bit size of the ELF
   file rather than on the format of the field. */
static int64_t read_value(FILE *fp,int format,int *size)
{
  int64_t value=0;
  int sz;

  switch (format) {
  case DW_FORM_flag_present:      /* implicit "present" flag (no data) */
    value=1;
    sz=0;
    break;
  case DW_FORM_data1:             /* constant, 1 byte */
  case DW_FORM_ref1:              /* reference, 1 bytes */
  case DW_FORM_flag:              /* flag, 1 byte (0=false, any non-zero=true) */
    fread(&value,1,1,fp);
    sz=1;
    break;
  case DW_FORM_data2:             /* constant, 2 bytes */
  case DW_FORM_ref2:              /* reference, 2 bytes */
    fread(&value,2,1,fp);
    sz=2;
    break;
  case DW_FORM_data4:             /* constant, 4 bytes */
  case DW_FORM_ref4:              /* reference, 4 bytes */
    fread(&value,4,1,fp);
    sz=4;
    break;
  case DW_FORM_data8:             /* constant, 8 bytes */
  case DW_FORM_ref8:              /* reference, 8 bytes */
  case DW_FORM_ref_sig8:          /* type signature, 8 bytes */
    fread(&value,8,1,fp);
    sz=8;
    break;
  case DW_FORM_sdata:             /* constant, signed LEB128 */
    value=read_leb128(fp,1,&sz);
    break;
  case DW_FORM_udata:             /* constant, unsigned LEB128 */
  case DW_FORM_ref_udata:         /* reference, unsigned LEB128 */
    value=read_leb128(fp,0,&sz);
    break;
  case DW_FORM_exprloc:           /* block, unsigned LEB128-encoded length + data bytes */
    value=read_leb128(fp,0,&sz);
    sz+=(int)value;
    /* the expression is useful for dynamic address calculations in a debugger,
       but not useful for post-run analysis */
    while (value>0) {
      fgetc(fp);
      value--;
    }
    break;
  default:
    assert(0);
  }
  if (size!=NULL)
    *size=sz;
  return value;
}

static void read_string(FILE *fp,int format,int stringtable,char *string,int max,int *size)
{
  int idx,count,sz,byte;
  int32_t offs;
  long pos;

  assert(fp!=NULL);
  assert(string!=NULL);
  assert(max>0);

  idx=0;
  switch (format) {
  case DW_FORM_string:            /* string, zero-terminated */
    while ((byte=fgetc(fp))!=EOF) {
      if (idx<max)
        string[idx]=(char)byte;
      idx++;
      if (byte==0)
        break;
    }
    sz=idx;
    break;
  case DW_FORM_strp:              /* string, 4-byte offset into the .debug_str section */
    fread(&offs,4,1,fp);
    sz=4;
    /* look up the string */
    assert(stringtable!=0);
    pos=ftell(fp);
    fseek(fp,stringtable+offs,SEEK_SET);
    while ((byte=fgetc(fp))!=EOF) {
      if (idx<max)
        string[idx]=(char)byte;
      idx++;
      if (byte==0)
        break;
    }
    fseek(fp,pos,SEEK_SET);
    break;
  case DW_FORM_block:             /* block, unsigned LEB128-encoded length + data bytes */
  case DW_FORM_block1:            /* block, 1-byte length + up to 255 data bytes */
  case DW_FORM_block2:            /* block, 2-byte length + up to 64K data bytes */
  case DW_FORM_block4:            /* block, 4-byte length + up to 4G data bytes */
    count=0;
    switch (format) {
    case DW_FORM_block:
      count=read_leb128(fp,0,&sz);
      break;
    case DW_FORM_block1:
      fread(&count,1,1,fp);
      sz=1;
      break;
    case DW_FORM_block2:
      fread(&count,2,1,fp);
      sz=2;
      break;
    case DW_FORM_block4:
      fread(&count,4,1,fp);
      sz=4;
      break;
    }
    sz+=count;
    while (idx<count && (byte=fgetc(fp))!=EOF) {
      if (idx<max)
        string[idx]=(char)byte;
      idx++;
    }
    break;
  default:
    assert(0);
  }
  string[max-1]='\0'; /* force the string to be zero-terminated */
  if (size!=NULL)
    *size=sz;
}

static void dwarf_abbrev(FILE *fp,const DWARFTABLE tables[],ABBREVLIST *abbrevlist)
{
  #define MAX_ATTRIBUTES  25  /* max. number of attributes for a single tag (in practice, no more than 11 attributes have been observed) */
  int unit,idx,tag,attrib,format;
  int size,count;
  unsigned char flag;
  unsigned long tablesize;
  ATTRIBUTE attributes[MAX_ATTRIBUTES];

  assert(fp!=NULL);
  assert(tables!=NULL);
  assert(abbrevlist!=NULL);
  assert(abbrevlist->next==NULL); /* abbrevlist should be empty */

  fseek(fp,tables[TABLE_ABBREV].offset,SEEK_SET);
  tablesize=tables[TABLE_ABBREV].size;
  assert(tablesize>0); /* debug information should have been found */

  unit=0;
  while (tablesize > 0) {
    /* get and chech the abbreviation id (a sequence number relative to its unit) */
    idx=(int)read_leb128(fp,0,&size);
    tablesize-=size;
    if (idx==0) {
      unit+=1;  /* an id that is zero, indicates the end of a unit */
      continue;
    }
    /* get the tag and the "has-children" flag */
    tag=(int)read_leb128(fp,0,&size);
    tablesize-=size;
    fread(&flag,1,1,fp);
    tablesize-=1;
    /* get the list of attributes */
    count=0;
    for ( ;; ) {
      attrib=(int)read_leb128(fp,0,&size);
      tablesize-=size;
      format=(int)read_leb128(fp,0,&size);
      tablesize-=size;
      if (attrib==0 && format==0)
        break;
      assert(count<MAX_ATTRIBUTES);
      attributes[count].tag=attrib;
      attributes[count].format=format;
      count++;
    }
    /* store the abbreviation */
    abbrev_insert(abbrevlist,unit,idx,tag,flag,count,attributes);
  }
}

static void clear_state(STATE *state,int default_is_stmt)
{
  /* set default state (see DWARF documentation, DWARF 2.0 pp. 52 / DWARF 3.0 pp. 94) */
  state->address=0;
  state->file=1;
  state->line=1;
  state->column=0;
  state->is_stmt=default_is_stmt;
  state->basic_block=0;
  state->end_seq=0;
  state->prologue_end=0;
  state->epiloge_begin=0;
  state->isa=0;
  state->discriminator=0;
}

/* dwarf_linetable() parses the .debug_line table and retrieves the
   line-number/code-address tupples. DWARF implements the table as a state
   machine with pseudo-instructions to set/clear state fields. There may be
   several of such state programs in the section.
   The output of this function is a list with line information structures and
   a list of filenames. The each element of the line number structure includes
   an index into the file list. The line number list is sorted on the code
   address */
static int dwarf_linetable(FILE *fp,const DWARFTABLE tables[],
                           DWARF_LINELOOKUP *linetable,DWARF_PATHLIST *filetable,
                           PATHXREF *xreftable)
{
  DWARF_FIXEDPROLOGUE prologue;
  STATE state;
  int byte,dirpos,opcode,lebsize;
  int unit,idx;
  long count,value;
  unsigned tableoffset,tablesize;
  char path[_MAX_PATH];
  uint8_t *std_argcnt;  /* array with argument counts for standard opcodes */
  DWARF_PATHLIST include_list = { NULL };
  DWARF_PATHLIST file_list = { NULL };
  DWARF_LINELOOKUP line_list = { NULL };
  DWARF_PATHLIST *fileitem;
  DWARF_LINELOOKUP *lineitem;

  assert(fp!=NULL);
  assert(tables!=NULL);
  assert(linetable!=NULL);
  assert(linetable->next==NULL);  /* linetable should be empty */
  assert(filetable!=NULL);
  assert(filetable->next==NULL);  /* filetable should be empty */
  assert(xreftable!=NULL);
  assert(xreftable->next==NULL);  /* path cross-reference should be empty */

  tableoffset=tables[TABLE_LINE].offset;
  tablesize=tables[TABLE_LINE].size;
  assert(tableoffset>0 && tablesize>0); /* debug information should have been found */
  fseek(fp,tableoffset,SEEK_SET);

  unit=0;
  while (tablesize > sizeof(prologue)) {
    /* check the prologue */
    fread(&prologue,sizeof(prologue),1,fp);
    //??? on big_endian, swap fields
    /* read the argument counts for the standard opcodes */
    std_argcnt=(uint8_t*)malloc(prologue.opcode_base-1*sizeof(uint8_t));
    if (std_argcnt==NULL)
      return 0;
    fread(std_argcnt,1,prologue.opcode_base-1,fp);
    /* read the includes table */
    while ((byte=fgetc(fp))!=EOF && byte!='\0') {
      for (idx=0; byte!=EOF && byte!='\0'; idx++) {
        path[idx]=(char)byte;
        byte=fgetc(fp);
      }
      path[idx]='\0';
      path_insert(&include_list,path);
    }
    /* read the filename table */
    while ((byte=fgetc(fp))!=EOF && byte!='\0') {
      for (idx=0; byte!=EOF && byte!='\0'; idx++) {
        path[idx]=(char)byte;
        byte=fgetc(fp);
      }
      path[idx]='\0';
      dirpos=read_leb128(fp,0,NULL);  /* read directory index */
      read_leb128(fp,0,NULL);         /* skip modification time (GCC sets this to 0) */
      read_leb128(fp,0,NULL);         /* skip source file size (GCC sets this to 0) */
      if (dirpos>0 && strpbrk(path,"\\/")==NULL) {
        char *dir=path_get(&include_list,dirpos-1);
        strins(path,"/");
        strins(path,dir);
      }
      path_insert(&file_list,path);
    }
    path_deletetable(&include_list);

    /* jump to the start of the program, then start running */
    clear_state(&state,prologue.default_is_stmt);
    fseek(fp,tableoffset+prologue.prologue_length+10,SEEK_SET);  /* +10 because the offset is relative to the field position */
    count=prologue.total_length-prologue.prologue_length-6;
    while (count>0) {
      opcode=fgetc(fp);
      count--;
      if (opcode==EOF)
        break;
      if (opcode<prologue.opcode_base) {
        /* standard (or extended) opcode */
        switch (opcode) {
        case DW_LNS_extended_op:
          value=read_leb128(fp,0,&lebsize);
          count-=lebsize+value;
          opcode=fgetc(fp);
          switch (opcode) {
          case DW_LNE_end_sequence:
            state.end_seq=1;
            line_insert(&line_list,state.line,state.address,state.file-1);
            clear_state(&state,prologue.default_is_stmt);  /* reset to default values */
            break;
          case DW_LNE_set_address:
            value=fgetc(fp);
            value|=(long)fgetc(fp) << 8;
            value|=(long)fgetc(fp) << 16;
            value|=(long)fgetc(fp) << 24;
            state.address=value;
            break;
          case DW_LNE_define_file:
            for (idx=0; (byte=fgetc(fp))!=EOF && byte!='\0'; idx++) {
              path[idx]=(char)byte;
              byte=fgetc(fp);
            }
            path[idx]='\0';
            dirpos=read_leb128(fp,0,NULL);  /* read directory index */
            read_leb128(fp,0,NULL);         /* skip modification time (GCC sets this to 0) */
            read_leb128(fp,0,NULL);         /* skip source file size (GCC sets this to 0) */
            if (dirpos>0 && strpbrk(path,"\\/")==NULL) {
              char *dir=path_get(&include_list,dirpos-1);
              strins(path,"/");
              strins(path,dir);
            }
            path_insert(&file_list,path);
            break;
          case DW_LNE_set_discriminator:
            state.discriminator=read_leb128(fp,0,NULL);
            break;
          default:
            while (value-->0) /* skip any unrecognized extended opcode */
              fgetc(fp);
          }
          break;
        case DW_LNS_copy:
          line_insert(&line_list,state.line,state.address,state.file-1);
          state.basic_block=0;
          break;
        case DW_LNS_advance_pc:
          value=read_leb128(fp,0,&lebsize);
          count-=lebsize;
          state.address+=value*prologue.min_instruction_size;
          break;
        case DW_LNS_advance_line:
          value=read_leb128(fp,1,&lebsize);
          count-=lebsize;
          state.line+=value;
          break;
        case DW_LNS_set_file:
          value=read_leb128(fp,0,&lebsize);
          count-=lebsize;
          state.file=value;
          break;
        case DW_LNS_set_column:
          value=read_leb128(fp,0,&lebsize);
          count-=lebsize;
          state.column=value;
          break;
        case DW_LNS_negate_stmt:
          state.is_stmt=!state.is_stmt;
          break;
        case DW_LNS_set_basic_block:
          state.basic_block=1;
          break;
        case DW_LNS_const_add_pc:
          state.address+=((255-prologue.opcode_base)/prologue.line_range)*prologue.min_instruction_size;
          break;
        case DW_LNS_fixed_advance_pc:
          value=fgetc(fp);
          value|=fgetc(fp) << 8;
          state.address+=value;
          count-=2;
          break;
        case DW_LNS_set_prologue_end:
          state.prologue_end=1;
          break;
        case DW_LNS_set_epilogue_begin:
          state.epiloge_begin=1;
          break;
        case DW_LNS_set_isa:
          value=read_leb128(fp,0,&lebsize);
          count-=lebsize;
          state.isa=value;
          break;
        default:
          /* skip opcode and any parameters */
          for (idx=0; idx<std_argcnt[opcode-1]; idx++) {
            read_leb128(fp,0,&lebsize);
            count-=lebsize;
          }
        }
      } else {
        /* special opcode */
        opcode-=prologue.opcode_base;
        state.address+=(opcode/prologue.line_range)*prologue.min_instruction_size;
        state.line+=prologue.line_base+opcode%prologue.line_range;
        line_insert(&line_list,state.line,state.address,state.file-1);
        state.basic_block=0;
        state.prologue_end=0;
        state.epiloge_begin=0;
        state.discriminator=0;
      }
    }

    free(std_argcnt);

    /* merge the local file table with the global one */
    idx=0;
    for (fileitem=file_list.next; fileitem!=NULL; fileitem=fileitem->next) {
      /* check whether this file is referenced at all */
      for (lineitem=line_list.next; lineitem!=NULL && lineitem->fileindex!=idx; lineitem=lineitem->next)
        /* nothing */;
      if (lineitem!=NULL) {
        /* so this file is referenced, now see whether it is already in the global
           file table */
        const char *name=path_get(&file_list,idx);
        assert(name!=NULL);
        if (path_find(filetable,name)<0) {
          int tgt;
          path_insert(filetable,name);
          tgt=path_find(filetable,name);  /* find it back, to add a cross-reference */
          assert(tgt>=0);
          pathxref_insert(xreftable,unit,idx,tgt);
        }
      }
      idx++;
    }

    /* append the local line table to the global table (and translate the index
       in the local file table to the index in the global file table) */
    for (lineitem=line_list.next; lineitem!=NULL; lineitem=lineitem->next) {
      int fileidx=pathxref_find(xreftable,unit,lineitem->fileindex);
      line_insert(linetable,lineitem->line,lineitem->address,fileidx);
    }
    path_deletetable(&file_list);
    line_deletetable(&line_list);

    /* prepare for a next "line program" (if any) */
    value=ftell(fp);
    tablesize-=value-tableoffset;
    tableoffset=value;
    unit+=1;
  } /* while (tablesize) */

  return 1;
}

/* dwarf_infotable() parses the .debug_info table and collects the functions.
 */
static int dwarf_infotable(FILE *fp,const DWARFTABLE tables[],
                           DWARF_SYMBOLLIST *symboltable,int *address_size,
                           const PATHXREF *xreftable)
{
  INFO_HDR32 header;
  ABBREVLIST abbrev_root = { NULL };
  const ABBREVLIST *abbrev;
  int unit,level,idx,size;
  uint32_t address;
  unsigned long tablesize,unitsize;
  char name[256],str[256];
  int64_t value;
  int file,line,external;

  assert(fp!=NULL);
  assert(tables!=NULL);
  assert(symboltable!=NULL);
  assert(symboltable->next==NULL);/* symboltable should be empty */
  assert(address_size!=NULL);
  assert(xreftable!=NULL);

  assert(tables[TABLE_ABBREV].offset>0);/* required table */
  dwarf_abbrev(fp,tables,&abbrev_root);

  assert(tables[TABLE_INFO].offset>0);  /* debug information should have been found */
  fseek(fp,tables[TABLE_INFO].offset,SEEK_SET);

  unit=0;
  tablesize=tables[TABLE_INFO].size;
  while (tablesize>sizeof(header)) {
    fread(&header,sizeof(header),1,fp);
    unitsize=header.unit_length-(sizeof(header)-4);
    assert(unitsize<0xfffffff0);  /* if larger, should read the 64-bit version of the structure */
    *address_size=header.address_size;
    tablesize-=unitsize+sizeof(header);
    name[0]='\0';
    address=0;
    external=0;
    level=0;
    /* browse through the tags */
    while (unitsize>0) {
      /* read the abbreviation code */
      idx=(int)read_leb128(fp,0,&size);
      unitsize-=size;
      if (idx==0) {
        level-=1;
        continue;
      }
      abbrev=abbrev_find(&abbrev_root,unit,idx);
      assert(abbrev!=NULL);
      /* run through the attributes */
      for (idx=0; idx<abbrev->count; idx++) {
        switch (abbrev->attributes[idx].format) {
        case DW_FORM_data1:             /* constant, 1 byte */
        case DW_FORM_data2:             /* constant, 2 bytes */
        case DW_FORM_data4:             /* constant, 4 bytes */
        case DW_FORM_data8:             /* constant, 8 bytes */
        case DW_FORM_sdata:             /* constant, signed LEB128 */
        case DW_FORM_udata:             /* constant, unsigned LEB128 */
        case DW_FORM_ref1:              /* reference, 1 bytes */
        case DW_FORM_ref2:              /* reference, 2 bytes */
        case DW_FORM_ref4:              /* reference, 4 bytes */
        case DW_FORM_ref8:              /* reference, 8 bytes */
        case DW_FORM_ref_udata:         /* reference, unsigned LEB128 */
        case DW_FORM_flag:              /* flag, 1 byte (0=false, any non-zero=true) */
        case DW_FORM_flag_present:      /* flag, no data */
        case DW_FORM_ref_sig8:          /* type signature, 8 bytes */
        case DW_FORM_exprloc:           /* block, unsigned LEB128-encoded length + data bytes */
          value=read_value(fp,abbrev->attributes[idx].format,&size);
          break;
        case DW_FORM_addr:              /* address, 4 bytes for 32-bit, 8 bytes for 64-bit */
        case DW_FORM_ref_addr:          /* reference, address size (4 bytes on 32-bit, 8 bytes on 64-bit) */
        case DW_FORM_sec_offset:        /* offset to line number data (4 bytes on 32-bit, 8 bytes on 64-bit) */
          value=0;
          fread(&value,1,header.address_size,fp);
          size=header.address_size;
          break;
        case DW_FORM_string:            /* string, zero-terminated */
        case DW_FORM_strp:              /* string, 4-byte offset into the .debug_str section */
        case DW_FORM_block:             /* block, unsigned LEB128-encoded length + data bytes */
        case DW_FORM_block1:            /* block, 1-byte length + up to 255 data bytes */
        case DW_FORM_block2:            /* block, 2-byte length + up to 64K data bytes */
        case DW_FORM_block4:            /* block, 4-byte length + up to 4G data bytes */
          read_string(fp,abbrev->attributes[idx].format,tables[TABLE_STR].offset,str,sizeof(str),&size);
          break;
        case DW_FORM_indirect:          /* format is specified in the .debug_info data (not in the abbreviation) */
        default:
          assert(0);
        }
        unitsize-=size;
        if (abbrev->tag==DW_TAG_subprogram || abbrev->tag==DW_TAG_variable) {
          /* store selected fields */
          switch (abbrev->attributes[idx].tag) {
          case DW_AT_name:
            strcpy(name,str);
            break;
          case DW_AT_low_pc:
            if (abbrev->tag==DW_TAG_subprogram)
              address=(uint32_t)value;
            break;
          case DW_AT_decl_file:
            file=pathxref_find(xreftable,unit,(int)value-1);
            break;
          case DW_AT_decl_line:
            line=(int)value;
            break;
          case DW_AT_external:
            if (abbrev->tag==DW_TAG_variable)
              external=(int)value;
            break;
          case DW_AT_location:
            //??? indirect address for variables
            break;
          }
        }
      } /* for (idx<abbrev->count) */
      if (abbrev->tag==DW_TAG_subprogram || abbrev->tag==DW_TAG_variable) {
        /* inlined functions are added as if they have address 0; when inline
           functions get instantiated, these are added as "references" to
           functions; these are not handled */
        if (name[0]!='\0' && file>=0 && (address>0 || external))
          symname_insert(symboltable,name,address,file,line);
        name[0]='\0';
        address=0;
        external=0;
        file=-1;
      }
      if (abbrev->has_children)
        level+=1;
    }
    unit+=1;
  }

  abbrev_deletetable(&abbrev_root);
  return 1;
}

/* dwarf_read() returns three lists: a list with source code line numbers,
   a list with functions and a list with the file paths (referred to by the
   other two lists) */
int dwarf_read(FILE *fp,DWARF_LINELOOKUP *linetable,DWARF_SYMBOLLIST *symboltable,
               DWARF_PATHLIST *filetable,int *address_size)
{
  DWARFTABLE tables[TABLE_COUNT];
  PATHXREF xreftable = { NULL };
  int result,wordsize;

  assert(fp!=NULL);
  assert(linetable!=NULL);        /* tables must be valid, but empty */
  assert(linetable->next==NULL);
  assert(symboltable!=NULL);
  assert(symboltable->next==NULL);
  assert(filetable!=NULL);
  assert(filetable->next==NULL);
  assert(address_size!=NULL);

  result=elf_info(fp,&wordsize,NULL,NULL);
  if (result!=ELFERR_NONE || wordsize!=32) {
    /* only 32-bit architectures at this time */
    fclose(fp);
    return 0;
  }

  /* get offsets to various debug tables */
  elf_section_by_name(fp,".debug_info",&tables[TABLE_INFO].offset,NULL,&tables[TABLE_INFO].size);
  elf_section_by_name(fp,".debug_abbrev",&tables[TABLE_ABBREV].offset,NULL,&tables[TABLE_ABBREV].size);
  elf_section_by_name(fp,".debug_str",&tables[TABLE_STR].offset,NULL,&tables[TABLE_STR].size);
  elf_section_by_name(fp,".debug_line",&tables[TABLE_LINE].offset,NULL,&tables[TABLE_LINE].size);
  elf_section_by_name(fp,".debug_pubnames",&tables[TABLE_PUBNAME].offset,NULL,&tables[TABLE_PUBNAME].size);

  result=1;
  /* the line table also fills in the file path table and the path
     cross-reference; the table is therefore mandatory in the DWARF format
     and it is the first one to parse */
  if (tables[TABLE_LINE].offset!=0)
    result=dwarf_linetable(fp,tables,linetable,filetable,&xreftable);
  /* the information table implicitly parses the abbreviations table, but it
     discards that table before returning */
  if (result && tables[TABLE_INFO].offset!=0)
    result=dwarf_infotable(fp,tables,symboltable,address_size,&xreftable);

  pathxref_deletetable(&xreftable);

  return result;
}

void dwarf_cleanup(DWARF_LINELOOKUP *linetable,DWARF_SYMBOLLIST *symboltable,DWARF_PATHLIST *filetable)
{
  line_deletetable(linetable);
  symname_deletetable(symboltable);
  path_deletetable(filetable);
}

const DWARF_SYMBOLLIST *dwarf_sym_from_name(const DWARF_SYMBOLLIST *symboltable,const char *name)
{
  const DWARF_SYMBOLLIST *sym;

  assert(symboltable!=NULL);
  assert(name!=NULL);
  for (sym=symboltable->next; sym!=NULL; sym=sym->next) {
    assert(sym->name!=NULL);
    if (strcmp(sym->name,name)==0)
      return sym;
  }
  return NULL;
}

const DWARF_SYMBOLLIST *dwarf_sym_from_address(const DWARF_SYMBOLLIST *symboltable,unsigned address,int exact)
{
  const DWARF_SYMBOLLIST *sym, *select = NULL;

  assert(symboltable!=NULL);
  for (sym=symboltable->next; sym!=NULL; sym=sym->next) {
    if (sym->address==address)
      return sym;   /* always return the function on an exact address match */
    if (!exact && sym->address<address)
      select = sym; /* optionally return the closest function at a lower address */
  }
  return select;
}

const DWARF_SYMBOLLIST *dwarf_sym_from_index(const DWARF_SYMBOLLIST *symboltable,unsigned index)
{
  const DWARF_SYMBOLLIST *sym;

  assert(symboltable!=NULL);
  for (sym=symboltable->next; sym!=NULL; sym=sym->next) {
    if (index--==0)
      return sym;
  }
  return NULL;
}

const char *dwarf_path_from_index(const DWARF_PATHLIST *filetable,int fileindex)
{
  const DWARF_PATHLIST *file;

  assert(filetable!=NULL);
  assert(fileindex>=0);
  for (file=filetable->next; file!=NULL && fileindex>0; file=file->next)
    fileindex--;
  return (file!=NULL) ? file->name : NULL;
}

const DWARF_LINELOOKUP *dwarf_line_from_address(const DWARF_LINELOOKUP *linetable,unsigned address)
{
  const DWARF_LINELOOKUP *line;

  assert(linetable!=NULL);
  for (line=linetable->next; line!=NULL; line=line->next)
    if (line->address<=address && (line->next==NULL || line->next->address>address))
      return line;
  return NULL;
}
