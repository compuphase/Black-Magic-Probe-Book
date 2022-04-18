/* Routines to get the line number and symbol tables from the DWARF debug
 * information (in an ELF file). For the symbol table, only the function and
 * variable symbols are stored.
 * For the moment, only 32-bit Little-Endian executables are supported.
 *
 * Copyright (c) 2015,2019-2022 CompuPhase
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
#include "demangle.h"
#include "elf.h"
#include "dwarf.h"

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
  TABLE_LINE_STR,
  /* ----- */
  TABLE_COUNT
};

typedef struct tagUNIT_HDR32 {
  uint32_t unit_length;     /* total length of this block, excluding this field */
  uint16_t version;         /* table format version */
  uint8_t  unit_type;       /* DWARF 5+, type of the compilation unit */
  uint8_t  address_size;    /* size in bytes of an address */
  uint32_t abbrev_offs;     /* offset into the .debug_abbrev table */
  /* more fields may follow, depending on unit_type */
} PACKED UNIT_HDR32;

typedef struct tagUNIT_HDR64 {
  uint32_t reserved;        /* must be 0xffffffff */
  uint64_t unit_length;     /* total length of this block, excluding this field */
  uint16_t version;         /* table format version */
  uint8_t  unit_type;       /* DWARF 5+, type of the compilation unit */
  uint8_t  address_size;    /* size in bytes of an address */
  uint64_t abbrev_offs;     /* offset into the .debug_abbrev table */
  /* more fields may follow, depending on unit_type */
} PACKED UNIT_HDR64;

typedef struct tagDWARF_PROLOGUE32 {  /* fixed part of the header */
  uint32_t total_length;    /* length of the line number table, minus the 4 bytes for this total_length field */
  uint16_t version;         /* prologue format version */
  uint8_t  address_size;    /* DWARF 5+, size in bytes of an address */
  uint8_t  segment_sel_size;/* DWARF 5+, size in bytes of a segment selector on the target system */
  uint32_t prologue_length; /* offset to the first opcode of the line number program (relative to this prologue_length field) */
  uint8_t  min_instruction_size;
  uint8_t  max_oper_per_instruction;  /* DWARF 4+, for VLIW architectures */
  uint8_t  default_is_stmt; /* default value to initialize the state machine with */
  int8_t   line_base;
  uint8_t  line_range;
  uint8_t  opcode_base;     /* number of the first special opcode */
  /* standard opcode lengths (array of length "opcode_base" - 1) */

  /* DWARF 2..4, include directories, a sequence of zero-terminated strings (and
     the sequence itself ends with a zero-byte) */
  /* DWARF 2..4, file names: base name, location, modification date, size */

  /* DWARF 5+, directory entry formats (length-prefixed array of ULEB128 pairs) */
  /* DWARF 5+, include directories, a length-prefixed sequence of entries in a format described in the earier table */
  /* DWARF 5+, filename entry formats (length-prefixed array of ULEB128 pairs) */
  /* DWARF 5+, filenames, a length-prefixed sequence of entries in a format described in the earier table */
} PACKED DWARF_PROLOGUE32;

typedef struct tagSTATE {
  uint32_t address;
  int file;
  int line;
  int column;
  int is_stmt;          /* whether the address is the start of a statement */
  int basic_block;      /* whether the address is the start of a basic block */
  int end_seq;
  int prologue_end;     /* DWARF 3+, function prologue end */
  int epiloge_begin;    /* DWARF 3+, function epilogue start */
  int isa;              /* DWARF 3+, instruction set architecture */
  unsigned op_index;    /* DWARF 4+, index within a "Very-Long-Instruction-Word" (VLIW) */
  int discriminator;    /* DWARF 4+, compiler-assigned id for a "block" to which the instruction belongs */
} STATE;

typedef struct tagPUBNAME_HDR32 {
  uint32_t totallength; /* total length of this block, excluding this field */
  uint16_t version;
  uint32_t info_offs;   /* offset into the comprehensive debug table */
  uint32_t info_size;   /* size of this symbol in the comprehensive debug table */
} PACKED PUBNAME_HDR32;

/* unit headers (DWARF 5+) */
#define DW_UT_compile                 0x01
#define DW_UT_type                    0x02
#define DW_UT_partial                 0x03
#define DW_UT_skeleton                0x04
#define DW_UT_split_compile           0x05
#define DW_UT_split_type              0x06
#define DW_UT_lo_user                 0x80
#define DW_UT_hi_user                 0xff

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
#define DW_TAG_dwarf_procedure        0x36
#define DW_TAG_restrict_type          0x37
#define DW_TAG_interface_type         0x38
#define DW_TAG_namespace              0x39
#define DW_TAG_imported_module        0x3a
#define DW_TAG_unspecified_type       0x3b
#define DW_TAG_partial_unit           0x3c
#define DW_TAG_imported_unit          0x3d
#define DW_TAG_condition              0x3f
#define DW_TAG_shared_type            0x40
#define DW_TAG_type_unit              0x41
#define DW_TAG_rvalue_reference_type  0x42
#define DW_TAG_template_alias         0x43
    /* DWARF 5+ */
#define DW_TAG_coarray_type           0x44
#define DW_TAG_generic_subrange       0x45
#define DW_TAG_dynamic_type           0x46
#define DW_TAG_atomic_type            0x47
#define DW_TAG_call_site              0x48
#define DW_TAG_call_site_parameter    0x49
#define DW_TAG_skeleton_unit          0x4a
#define DW_TAG_immutable_type         0x4b
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
#define DW_AT_vtable_elem_location    0x4d  /* exprloc, loclist */
#define DW_AT_allocated               0x4e  /* constant, exprloc, reference */
#define DW_AT_associated              0x4f  /* constant, exprloc, reference */
#define DW_AT_data_location           0x50  /* exprloc */
#define DW_AT_byte_stride             0x51  /* constant, exprloc, reference */
#define DW_AT_entry_pc                0x52  /* address, constant */
#define DW_AT_use_UTF8                0x53  /* flag */
#define DW_AT_extension               0x54  /* reference */
#define DW_AT_ranges                  0x55  /* rnglist */
#define DW_AT_trampoline              0x56  /* address, flag, reference, string */
#define DW_AT_call_column             0x57  /* constant */
#define DW_AT_call_file               0x58  /* constant */
#define DW_AT_call_line               0x59  /* constant */
#define DW_AT_description             0x5a  /* string */
#define DW_AT_binary_scale            0x5b  /* constant */
#define DW_AT_decimal_scale           0x5c  /* constant */
#define DW_AT_small                   0x5d  /* reference */
#define DW_AT_decimal_sign            0x5e  /* constant */
#define DW_AT_digit_count             0x5f  /* constant */
#define DW_AT_picture_string          0x60  /* string */
#define DW_AT_mutable                 0x61  /* flag */
#define DW_AT_threads_scaled          0x62  /* flag */
#define DW_AT_explicit                0x63  /* flag */
#define DW_AT_object_pointer          0x64  /* reference */
#define DW_AT_endianity               0x65  /* constant */
#define DW_AT_elemental               0x66  /* flag */
#define DW_AT_pure                    0x67  /* flag */
#define DW_AT_recursive               0x68  /* flag */
#define DW_AT_signature               0x69  /* reference */
#define DW_AT_main_subprogram         0x6a  /* flag */
#define DW_AT_data_bit_offset         0x6b  /* constant */
#define DW_AT_const_expr              0x6c  /* flag */
#define DW_AT_enum_class              0x6d  /* flag */
#define DW_AT_linkage_name            0x6e  /* string */
    /* DWARF 5+ */
#define DW_AT_string_length_bit_size  0x6f  /* constant */
#define DW_AT_string_length_byte_size 0x70  /* constant */
#define DW_AT_rank                    0x71  /* constant, exprloc */
#define DW_AT_str_offsets_base        0x72  /* stroffsetsptr */
#define DW_AT_addr_base               0x73  /* addrptr */
#define DW_AT_rnglists_base           0x74  /* rnglistsptr */
#define DW_AT_dwo_name                0x76  /* string */
#define DW_AT_reference               0x77  /* flag */
#define DW_AT_rvalue_reference        0x78  /* flag */
#define DW_AT_macros                  0x79  /* macptr */
#define DW_AT_call_all_calls          0x7a  /* flag */
#define DW_AT_call_all_source_calls   0x7b  /* flag */
#define DW_AT_call_all_tail_calls     0x7c  /* flag */
#define DW_AT_call_return_pc          0x7d  /* address */
#define DW_AT_call_value              0x7e  /* exprloc */
#define DW_AT_call_origin             0x7f  /* exprloc */
#define DW_AT_call_parameter          0x80  /* reference */
#define DW_AT_call_pc                 0x81  /* address */
#define DW_AT_call_tail_call          0x82  /* flag */
#define DW_AT_call_target             0x83  /* exprloc */
#define DW_AT_call_target_clobbered   0x84  /* exprloc */
#define DW_AT_call_data_location      0x85  /* exprloc */
#define DW_AT_call_data_value         0x86  /* exprloc */
#define DW_AT_noreturn                0x87  /* flag */
#define DW_AT_alignment               0x88  /* constant */
#define DW_AT_export_symbols          0x89  /* flag */
#define DW_AT_deleted                 0x8a  /* flag */
#define DW_AT_defaulted               0x8b  /* constant */
#define DW_AT_loclists_base           0x8c  /* loclistsptr */
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
    /* DWARF 5+ */
#define DW_FORM_strx                  0x1a  /* string */
#define DW_FORM_addrx                 0x1b  /* address */
#define DW_FORM_ref_sup4              0x1c  /* reference relative to .debug_info of a supplementaty object file, 4 bytes */
#define DW_FORM_strp_sup              0x1d  /* string, 4-byte offset into the .debug_str section of a supplementary object file */
#define DW_FORM_data16                0x1e  /* constant, 16 bytes (MD5) */
#define DW_FORM_line_strp             0x1f  /* string, 4-byte offset into the .debug_line_str section */
#define DW_FORM_ref_sig8              0x20  /* 64-bit signature for a type defined in a different unit (already present in DWARF 4) */
#define DW_FORM_implicit_const        0x21  /* constant whose value is in the abbreviation's attribute, not in the .debug_info data */
#define DW_FORM_loclistx              0x22  /* loclist */
#define DW_FORM_rnglistx              0x23  /* rnglist */
#define DW_FORM_ref_sup8              0x24  /* reference relative to .debug_info of a supplementaty object file, 8 bytes */
#define DW_FORM_strx1                 0x25  /* string */
#define DW_FORM_strx2                 0x26  /* string */
#define DW_FORM_strx3                 0x27  /* string */
#define DW_FORM_strx4                 0x28  /* string */
#define DW_FORM_addrx1                0x29  /* address */
#define DW_FORM_addrx2                0x2a  /* address */
#define DW_FORM_addrx3                0x2b  /* address */
#define DW_FORM_addrx4                0x2c  /* address */

/* line number opcodes */
#define DW_LNS_extended_op            0     /* version 2+ */
#define DW_LNS_copy                   1
#define DW_LNS_advance_pc             2
#define DW_LNS_advance_line           3
#define DW_LNS_set_file               4
#define DW_LNS_set_column             5
#define DW_LNS_negate_stmt            6
#define DW_LNS_set_basic_block        7
#define DW_LNS_const_add_pc           8
#define DW_LNS_fixed_advance_pc       9
#define DW_LNS_set_prologue_end       10    /* version 3+ */
#define DW_LNS_set_epilogue_begin     11
#define DW_LNS_set_isa                12
/* line number extended opcodes */
#define DW_LNE_end_sequence           1     /* version 2+ */
#define DW_LNE_set_address            2
#define DW_LNE_define_file            3     /* deprecated in version 5+ */
#define DW_LNE_set_discriminator      4     /* version 4+ */
#define DW_LNE_lo_user                0x80
#define DW_LNE_hi_user                0xff

/* location expression opcodes */
#define DW_OP_addr                    0x03  /* constant address (32-bit or 64-bit) */
#define DW_OP_deref                   0x06
#define DW_OP_const1u                 0x08  /* 1-byte constant */
#define DW_OP_const1s                 0x09  /* 1-byte constant */
#define DW_OP_const2u                 0x0a  /* 2-byte constant */
#define DW_OP_const2s                 0x0b  /* 2-byte constant */
#define DW_OP_const4u                 0x0c  /* 4-byte constant */
#define DW_OP_const4s                 0x0d  /* 4-byte constant */
#define DW_OP_const8u                 0x0e  /* 8-byte constant */
#define DW_OP_const8s                 0x0f  /* 8-byte constant */
#define DW_OP_constu                  0x10  /* ULEB128 constant */
#define DW_OP_consts                  0x11  /* SLEB128 constant */
#define DW_OP_dup                     0x12
#define DW_OP_drop                    0x13
#define DW_OP_over                    0x14
#define DW_OP_pick                    0x15  /* 1-byte stack index */
#define DW_OP_swap                    0x16
#define DW_OP_rot                     0x17
#define DW_OP_xderef                  0x18
#define DW_OP_abs                     0x19
#define DW_OP_and                     0x1a
#define DW_OP_div                     0x1b
#define DW_OP_minus                   0x1c
#define DW_OP_mod                     0x1d
#define DW_OP_mul                     0x1e
#define DW_OP_neg                     0x1f
#define DW_OP_not                     0x20
#define DW_OP_or                      0x21
#define DW_OP_plus                    0x22
#define DW_OP_plus_uconst             0x23  /* ULEB128 addend */
#define DW_OP_shl                     0x24
#define DW_OP_shr                     0x25
#define DW_OP_shra                    0x26
#define DW_OP_xor                     0x27
#define DW_OP_bra                     0x28  /* signed 2-byte constant */
#define DW_OP_eq                      0x29
#define DW_OP_ge                      0x2a
#define DW_OP_gt                      0x2b
#define DW_OP_le                      0x2c
#define DW_OP_lt                      0x2d
#define DW_OP_ne                      0x2e
#define DW_OP_skip                    0x2f  /* signed 2-byte constant */
#define DW_OP_lit0                    0x30  /* literals 0..31 = DW_OP_lit0 + literal */
#define DW_OP_reg0                    0x50  /* registers 0..31 = DW_OP_reg0 + regnum */
#define DW_OP_breg0                   0x70  /* SLEB128 offset, base registers 0..31 = DW_OP_breg0 + regnum */
#define DW_OP_regx                    0x90  /* ULEB128 register */
#define DW_OP_fbreg                   0x91  /* SLEB128 offset */
#define DW_OP_bregx                   0x92  /* ULEB128 register, SLEB128 offset */
#define DW_OP_piece                   0x93  /* ULEB128 size of piece */
#define DW_OP_deref_size              0x94  /* 1-byte size of data retrieved */
#define DW_OP_xderef_size             0x95  /* 1-byte size of data retrieved */
#define DW_OP_nop                     0x96
#define DW_OP_push_object_address     0x97
#define DW_OP_call2                   0x98  /* 2-byte offset of DIE */
#define DW_OP_call4                   0x99  /* 4-byte offset of DIE */
#define DW_OP_call_ref                0x9a  /* 4- or 8-byte offset of DIE */
#define DW_OP_form_tls_address        0x9b
#define DW_OP_call_frame_cfa          0x9c
#define DW_OP_bit_piece               0x9d  /* ULEB128 size, ULEB128 offset */
#define DW_OP_implicit_value          0x9e  /* ULEB128 size, block of that size */
#define DW_OP_stack_value             0x9f
    /* DWARF 5+ */
#define DW_OP_implicit_pointer        0xa0  /* 4- or 8-byte offset of DIE, SLEB128 constant offset */
#define DW_OP_addrx                   0xa1  /* ULEB128 indirect address */
#define DW_OP_constx                  0xa2  /* ULEB128 indirect constant */
#define DW_OP_entry_value             0xa3  /* ULEB128 size, block of that size */
#define DW_OP_const_type              0xa4  /* ULEB128 type entry offset, 1-byte size, constant value */
#define DW_OP_regval_type             0xa5  /* ULEB128 register number, ULEB128 constant offset */
#define DW_OP_deref_type              0xa6  /* 1-byte size, ULEB128 type entry offset */
#define DW_OP_xderef_type             0xa7  /* 1-byte size, ULEB128 type entry offset */
#define DW_OP_convert                 0xa8  /* ULEB128 type entry offset */
#define DW_OP_reinterpret             0xa9  /* ULEB128 type entry offset */
#define DW_OP_lo_user                 0xe0
#define DW_OP_hi_user                 0xff

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
  long value;   /* value for implicit constant */
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
      cur->attributes[idx].value=attributes[idx].value;
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
                                        unsigned code_addr,unsigned code_range,
                                        unsigned data_addr,int fileindex,int line,
                                        int external)
{
  DWARF_SYMBOLLIST *cur,*pred;
  char demangled[256];

  assert(root!=NULL);
  assert(name!=NULL);

  if ((cur=(DWARF_SYMBOLLIST*)malloc(sizeof(DWARF_SYMBOLLIST)))==NULL)
    return NULL;      /* insufficient memory */

  if (demangle(demangled, sizeof(demangled), name))
    cur->name=strdup(demangled);
  else
    cur->name=strdup(name);
  if (cur==NULL) {
    free(cur);
    return NULL;      /* insufficient memory */
  }

  cur->code_addr=code_addr;
  cur->code_range=code_range;
  cur->data_addr=data_addr;
  cur->fileindex=fileindex;
  cur->line=line;
  cur->line_limit=0;  /* updated later */
  if (external)
    cur->scope=SCOPE_EXTERNAL;
  else if (code_range>0)
    cur->scope=SCOPE_UNIT;
  else
    cur->scope=SCOPE_UNKNOWN;
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
    value |= ~0u << shift;

  return value;
}

/* read_value() reads numeric data in various formats. It does not read address
   data or other fields where the data size depends on the bit size of the ELF
   file rather than on the format of the field. */
static int64_t read_value(FILE *fp,int format,int *size)
{
  int64_t value=0;
  int sz=0;

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
  case DW_FORM_data16:            /* constant, 16 bytes */
    fread(&value,8,1,fp);
    fread(&value,8,1,fp);
    sz=16;
    break;
  case DW_FORM_ref_sup4:          /* reference relative to .debug_info of a supplementaty object file, 4 bytes */
    fread(&value,4,1,fp);
    sz=4;
    break;
  case DW_FORM_ref_sup8:          /* reference relative to .debug_info of a supplementaty object file, 8 bytes */
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
  case DW_FORM_exprloc: {         /* block, unsigned LEB128-encoded length + data bytes */
    int datasz=(int)read_leb128(fp,0,&sz);
    int opc=0;
    sz+=datasz;
    if (datasz>=1) {
      fread(&opc,1,1,fp);
      datasz-=1;
    }
    if (opc==DW_OP_addr && datasz>0 && datasz<=sizeof value) {
      fread(&value,datasz,1,fp);
    } else {
      /* register/stack-relative location expressions are currently not supported */
      while (datasz-->0)
        fgetc(fp);
    }
    break;
  } /* DW_FORM_exprloc */
  default:
    assert(0);
  }
  if (size!=NULL)
    *size=sz;
  return value;
}

static void read_string(FILE *fp,int format,int stringtable,char *string,int max,int *size)
{
  int sz=0;
  int idx,count,byte;
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
  case DW_FORM_strp_sup:          /* string, 4-byte offset into the .debug_str section of a supplementary object file */
  case DW_FORM_line_strp:         /* string, 4-byte offset into the .debug_line_str section */
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
  #define MAX_ATTRIBUTES  50  /* max. number of attributes for a single tag */
  int unit,tag,attrib,format;
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
    /* get and check the abbreviation id (a sequence number relative to its unit) */
    int idx=(int)read_leb128(fp,0,&size);
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
      long value=0;
      attrib=(int)read_leb128(fp,0,&size);
      tablesize-=size;
      format=(int)read_leb128(fp,0,&size);
      tablesize-=size;
      if (attrib==0 && format==0)
        break;
      if (format==DW_FORM_implicit_const) {
        value=read_leb128(fp,0,&size);
        tablesize-=size;
      }
      assert(count<MAX_ATTRIBUTES);
      attributes[count].tag=attrib;
      attributes[count].format=format;
      attributes[count].value=value;
      count++;
    }
    /* store the abbreviation */
    abbrev_insert(abbrevlist,unit,idx,tag,flag,count,attributes);
  }
}

static int read_unitheader(FILE *fp,UNIT_HDR32 *header,int *size)
{
  long mark;

  assert(fp!=NULL);
  assert(header!=NULL);
  assert(size!=NULL);
  mark=ftell(fp); /* may need to "un-read" */
  if (fread(header,sizeof(UNIT_HDR32),1,fp)==0)
    return 0;     /* read failed */
  assert(header->unit_length!=0xffffffff);  /* otherwise, should read 64-bit header */
  //??? on big_endian, swap version field before testing it
  if (header->version>=5) {
    *size=sizeof(UNIT_HDR32);
  } else {
    /* un-read v5 structure, then read & copy the v2..v4 structure
        uint32_t unit_length;    total length of this block, excluding this field
        uint16_t version;        DWARF version, up to version 4
        uint32_t abbrev_offs;    offset into the .debug_abbrev table
        uint8_t  address_size;   size in bytes of an address
     */
    #define HDRSIZE 11
    unsigned char hdr[HDRSIZE];
    fseek(fp,mark,SEEK_SET);
    fread(&hdr,1,HDRSIZE,fp);
    memcpy(&header->unit_length,hdr+0,4); /* redundant, identical to v5 */
    memcpy(&header->version,hdr+4,2);     /* redundant, identical to v5 */
    memcpy(&header->abbrev_offs,hdr+6,4);
    memcpy(&header->address_size,hdr+10,1);
    header->unit_type=DW_UT_compile;
    *size=HDRSIZE;
    #undef HDRSIZE
  }
  //??? on big_endian, swap fields
  return 1;
}

static int read_prologue(FILE *fp,DWARF_PROLOGUE32 *prologue,int *size)
{
  long mark;

  assert(fp!=NULL);
  assert(prologue!=NULL);
  assert(size!=NULL);
  mark=ftell(fp); /* may need to "un-read" */
  if (fread(prologue,sizeof(DWARF_PROLOGUE32),1,fp)==0)
    return 0;     /* read failed */
  assert(prologue->total_length!=0xffffffff);  /* otherwise, should read 64-bit prologue */
  //??? on big_endian, swap version field before testing it
  if (prologue->version>=5) {
    *size=sizeof(DWARF_PROLOGUE32);
  } else if (prologue->version==2 || prologue->version==3) {
    /* un-read v5 structure, then read & copy the v2/3 structure
        uint32_t total_length;     length of the line number table, minus the 4 bytes for this total_length field
        uint16_t version;          this prologue is for versions 2 & 3
        uint32_t prologue_length;  offset to the first opcode of the line number program (relative to this prologue_length field)
        uint8_t  min_instruction_size;
        uint8_t  default_is_stmt;  default value to initialize the state machine with
        int8_t   line_base;
        uint8_t  line_range;
        uint8_t  opcode_base;      number of the first special opcode
        standard opcode lengths (array of length "opcode_base" - 1)
        include directories, a sequence of zero-terminated strings (and the
           sequence itself ends with a zero-byte)
        file names: base name, location, modification date, size
     */
    #define HDRSIZE 15
    unsigned char hdr[HDRSIZE];
    fseek(fp,mark,SEEK_SET);
    fread(&hdr,1,HDRSIZE,fp);
    memcpy(&prologue->total_length,hdr+0,4);  /* redundant, identical to v5 */
    memcpy(&prologue->version,hdr+4,2);       /* redundant, identical to v5 */
    memcpy(&prologue->prologue_length,hdr+6,4);
    memcpy(&prologue->min_instruction_size,hdr+10,1);
    memcpy(&prologue->default_is_stmt,hdr+11,1);
    memcpy(&prologue->line_base,hdr+12,1);
    memcpy(&prologue->line_range,hdr+13,1);
    memcpy(&prologue->opcode_base,hdr+14,1);
    prologue->address_size=4; /* assume 32-bit, 64-bit not yet supported */
    prologue->segment_sel_size=0;
    prologue->max_oper_per_instruction=1;
    *size=HDRSIZE;
    #undef HDRSIZE
  } else if (prologue->version==4) {
    /* un-read v5 structure, then read & copy the v4 structure
        uint32_t total_length;     length of the line number table, minus the 4 bytes for this total_length field
        uint16_t version;          this prologue is for versions 2 & 3
        uint32_t prologue_length;  offset to the first opcode of the line number program (relative to this prologue_length field)
        uint8_t  min_instruction_size;
        uint8_t  max_oper_per_instruction;
        uint8_t  default_is_stmt;  default value to initialize the state machine with
        int8_t   line_base;
        uint8_t  line_range;
        uint8_t  opcode_base;      number of the first special opcode
        standard opcode lengths (array of length "opcode_base" - 1)
        include directories, a sequence of zero-terminated strings (and the
           sequence itself ends with a zero-byte)
        file names: base name, location, modification date, size
     */
    #define HDRSIZE 16
    unsigned char hdr[HDRSIZE];
    fseek(fp,mark,SEEK_SET);
    fread(&hdr,sizeof(hdr),1,fp);
    memcpy(&prologue->total_length,hdr+0,4);  /* redundant, identical to v5 */
    memcpy(&prologue->version,hdr+4,2);       /* redundant, identical to v5 */
    memcpy(&prologue->prologue_length,hdr+6,4);
    memcpy(&prologue->min_instruction_size,hdr+10,1);
    memcpy(&prologue->max_oper_per_instruction,hdr+11,1);
    memcpy(&prologue->default_is_stmt,hdr+12,1);
    memcpy(&prologue->line_base,hdr+13,1);
    memcpy(&prologue->line_range,hdr+14,1);
    memcpy(&prologue->opcode_base,hdr+15,1);
    prologue->address_size=4; /* assume 32-bit, 64-bit not yet supported */
    prologue->segment_sel_size=0;
    *size=HDRSIZE;
    #undef HDRSIZE
  } else {
    assert(0);  /* DWARF 1 is not supported */
    return 0;
  }
  //??? on big_endian, swap fields
  return 1;
}

static void clear_state(STATE *state,int default_is_stmt)
{
  /* set default state (see DWARF documentation, DWARF 2.0 pp. 52 / DWARF 3.0 pp. 94) */
  state->address=0;
  state->op_index=0;
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
  DWARF_PROLOGUE32 prologue;
  STATE state;
  int dirpos,opcode,lebsize,prologue_size;
  int unit,idx;
  long value;
  unsigned tableoffset,tablesize;
  char path[_MAX_PATH];
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
  prologue_size=sizeof(prologue); /* initial assumption */
  while (tablesize>prologue_size) {
    uint8_t *std_argcnt;  /* array with argument counts for standard opcodes */
    long count;
    int byte;
    /* check the prologue */
    read_prologue(fp,&prologue,&prologue_size);
    /* read the argument counts for the standard opcodes */
    std_argcnt=(uint8_t*)malloc(prologue.opcode_base-1*sizeof(uint8_t));
    if (std_argcnt==NULL)
      return 0;
    fread(std_argcnt,1,prologue.opcode_base-1,fp);
    assert(prologue.version<5); //??? for DWARF 5+, the format for the include-paths and filenames tables is different
    /* read the include-paths table */
    while ((byte=fgetc(fp))!=EOF && byte!='\0') {
      for (idx=0; byte!=EOF && byte!='\0'; idx++) {
        path[idx]=(char)byte;
        byte=fgetc(fp);
      }
      path[idx]='\0';
      path_insert(&include_list,path);
    }
    /* read the filenames table */
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
        assert(prologue.max_oper_per_instruction==1); /* for VLIW architecture, the calculation below must be adjusted */
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
  UNIT_HDR32 header;
  ABBREVLIST abbrev_root = { NULL };
  const ABBREVLIST *abbrev;
  int unit,idx,size;
  unsigned long tablesize;
  char name[256],str[256];
  int64_t value;
  int file,line;

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
    unsigned long unitsize;
    uint32_t code_addr=0, code_addr_end=0;
    uint32_t data_addr=0;
    int external=0;
    int declaration=0;
    int level=0;
    int hdrsize;
    read_unitheader(fp,&header,&hdrsize);
    unitsize=header.unit_length-(hdrsize-4);
    assert(unitsize<0xfffffff0);  /* if larger, should read the 64-bit version of the structure */
    *address_size=header.address_size;
    tablesize-=unitsize+hdrsize;
    name[0]='\0';
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
        int format=abbrev->attributes[idx].format;
        if (format==DW_FORM_indirect) {
          /* format is specified in the .debug_info data (not in the abbreviation) */
          format=read_leb128(fp,1,&size);
          unitsize-=size;
        }
        switch (format) {
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
        case DW_FORM_ref_sup4:
        case DW_FORM_ref_sup8:
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
        case DW_FORM_strp_sup:
        case DW_FORM_block:             /* block, unsigned LEB128-encoded length + data bytes */
        case DW_FORM_block1:            /* block, 1-byte length + up to 255 data bytes */
        case DW_FORM_block2:            /* block, 2-byte length + up to 64K data bytes */
        case DW_FORM_block4:            /* block, 4-byte length + up to 4G data bytes */
          read_string(fp,abbrev->attributes[idx].format,tables[TABLE_STR].offset,str,sizeof(str),&size);
          break;
        case DW_FORM_line_strp:
          read_string(fp,abbrev->attributes[idx].format,tables[TABLE_LINE_STR].offset,str,sizeof(str),&size);
          break;
        case DW_FORM_implicit_const:
          value=abbrev->attributes[idx].value;
          size=0;
          break;
        default:
          assert(0);
        }
        unitsize-=size;
        if (abbrev->tag==DW_TAG_subprogram || abbrev->tag==DW_TAG_variable || abbrev->tag==DW_TAG_formal_parameter) {
          //??? also handle DW_TAG_lexical_block for the scope of local variables
          /* store selected fields */
          switch (abbrev->attributes[idx].tag) {
          case DW_AT_name:
            strcpy(name,str);
            break;
          case DW_AT_low_pc:
            if (abbrev->tag==DW_TAG_subprogram)
              code_addr=(uint32_t)value;
            break;
          case DW_AT_high_pc:
            if (abbrev->tag==DW_TAG_subprogram) {
              code_addr_end=(uint32_t)value;
              /* depending on the format, the "high pc" value is an offset
                 instead of an address */
              if (abbrev->attributes[idx].format!=DW_FORM_addr)
                code_addr_end+=code_addr;
            }
            break;
          case DW_AT_decl_file:
            file=pathxref_find(xreftable,unit,(int)value-1);
            break;
          case DW_AT_decl_line:
            line=(int)value;
            break;
          case DW_AT_location:
            if (abbrev->tag==DW_TAG_variable)
              data_addr=(uint32_t)value;  /* global / static variable */
            break;
          case DW_AT_external:
            if (abbrev->tag==DW_TAG_variable)
              external=(int)value;
            break;
          case DW_AT_declaration:
            declaration=(int)value;
            break;
          }
        }
      } /* for (idx<abbrev->count) */
      if ((abbrev->tag==DW_TAG_subprogram && code_addr_end>code_addr)
          || (abbrev->tag==DW_TAG_variable && data_addr!=0))
        declaration=0;
      if ((abbrev->tag==DW_TAG_subprogram || abbrev->tag==DW_TAG_variable || abbrev->tag==DW_TAG_formal_parameter)
          && !declaration) {
        /* inlined functions are added as if they have address 0; when inline
           functions get instantiated, these are added as "references" to
           functions; these are not handled */
        assert(code_addr_end>=code_addr);
        if (name[0]!='\0' && file>=0)
          symname_insert(symboltable,name,code_addr,code_addr_end-code_addr,
                         data_addr,file,line,external);
        name[0]='\0';
        code_addr=code_addr_end=0;
        data_addr=0;
        external=0;
        declaration=0;
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

static void dwarf_postprocess(DWARF_SYMBOLLIST *symboltable,const DWARF_LINELOOKUP *linetable)
{
  DWARF_SYMBOLLIST *sym;

  assert(symboltable!=NULL);
  for (sym=symboltable->next; sym!=NULL; sym=sym->next) {
    if (DWARF_IS_FUNCTION(sym)) {
      /* go through the line table to find the line range for the function */
      uint32_t addr=sym->code_addr+sym->code_range;
      const DWARF_LINELOOKUP *line;
      DWARF_SYMBOLLIST *lcl;
      assert(linetable!=NULL);
      for (line=linetable->next; line!=NULL && sym->line_limit==0; line=line->next) {
        if (line->address<addr && (line->next==NULL || line->next->address>=addr)
            && line->line>=sym->line_limit)
          sym->line_limit=line->line+1; /* +1 for consistency with DWARF address range */
      }
      /* collect all local variables that are declared within this line range */
      for (lcl=symboltable->next; lcl!=NULL; lcl=lcl->next) {
        if (lcl->fileindex==sym->fileindex
            && lcl->line>=sym->line && lcl->line<sym->line_limit
            && lcl->scope==SCOPE_UNKNOWN)
        {
          assert(lcl->code_addr==0);  /* nested functions don't occur */
          lcl->scope=SCOPE_FUNCTION;
          lcl->line_limit=sym->line_limit;
          assert(lcl->line_limit>lcl->line);
        }
      }
    }
  }
}

/* dwarf_read() returns three lists: a list with source code line numbers,
 * a list with functions and a list with the file paths (referred to by the
 * other two lists)
 */
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

  result=elf_info(fp,&wordsize,NULL,NULL,NULL);
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
  elf_section_by_name(fp,".debug_line_str",&tables[TABLE_LINE_STR].offset,NULL,&tables[TABLE_LINE_STR].size);

  result=1;
  /* the line table also holds information for the file path table and the path
     cross-reference; the table is therefore mandatory in the DWARF format and
     it is the first one to parse */
  if (tables[TABLE_LINE].offset!=0)
    result=dwarf_linetable(fp,tables,linetable,filetable,&xreftable);
  /* the information table implicitly parses the abbreviations table, but it
     discards that table before returning */
  if (result && tables[TABLE_INFO].offset!=0)
    result=dwarf_infotable(fp,tables,symboltable,address_size,&xreftable);

  pathxref_deletetable(&xreftable);

  /* now that we have seen all functions, we can update the scope of local
     variables */
  dwarf_postprocess(symboltable,linetable);

  return result;
}

void dwarf_cleanup(DWARF_LINELOOKUP *linetable,DWARF_SYMBOLLIST *symboltable,DWARF_PATHLIST *filetable)
{
  line_deletetable(linetable);
  symname_deletetable(symboltable);
  path_deletetable(filetable);
}

/** dwarf_sym_from_name() returns a function or variable that matches the name,
 *  and that is in scope.
 *  - Functions and variables with function scope (locals & arguments) are
 *    matched if the fileindex matches and the lineindex is in range. This test
 *    is skipped in fileindex or lineindex is -1.
 *  - Functions and variables with unit scope are found if the fileindex
 *    matches. This test is skipped if fileindex is -1.
 *  - External functions and variables are always matched, but are matched last.
 */
const DWARF_SYMBOLLIST *dwarf_sym_from_name(const DWARF_SYMBOLLIST *symboltable,
                                            const char *name,int fileindex,int lineindex)
{
  const DWARF_SYMBOLLIST *sym;

  assert(symboltable!=NULL);
  assert(name!=NULL);
  /* check local variables */
  if (fileindex>=0 && lineindex>=0) {
    for (sym=symboltable->next; sym!=NULL; sym=sym->next) {
      assert(sym->name!=NULL);
      if (sym->scope==SCOPE_FUNCTION
          && sym->fileindex==fileindex
          && sym->line<=lineindex && lineindex<sym->line_limit
          && strcmp(sym->name,name)==0)
        return sym;
    }
  }
  /* check static globals */
  if (fileindex>=0) {
    for (sym=symboltable->next; sym!=NULL; sym=sym->next) {
      assert(sym->name!=NULL);
      if (sym->scope==SCOPE_UNIT
          && sym->fileindex==fileindex
          && strcmp(sym->name,name)==0)
        return sym;
    }
  }
  /* check external symbols */
  for (sym=symboltable->next; sym!=NULL; sym=sym->next) {
    assert(sym->name!=NULL);
    if (sym->scope==SCOPE_EXTERNAL
        && strcmp(sym->name,name)==0)
      return sym;
  }
  return NULL;
}

const DWARF_SYMBOLLIST *dwarf_sym_from_address(const DWARF_SYMBOLLIST *symboltable,unsigned address,int exact)
{
  const DWARF_SYMBOLLIST *sym, *select = NULL;

  assert(symboltable!=NULL);
  for (sym=symboltable->next; sym!=NULL; sym=sym->next) {
    if (sym->code_range==0) {
      /* check variable */
      if (sym->data_addr==address)
        return sym;   /* always return the variable on an exact address match */
    } else {
      /* check function */
      if (sym->code_addr==address)
        return sym;   /* always return the function on an exact address match */
      if (!exact && sym->code_addr<address)
        select=sym;   /* optionally return the closest function at a lower address */
    }
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

/** dwarf_collect_functions_in_file() stores the pointers to all "code" symbols
 *  that appear in a file into a list.
 *
 *  To check the size of the list, call this function with parameter "list" set
 *  to 0. The number of entries to allocate is returned. Then allocate a buffer
 *  of sufficient size, and call this function again with that buffer.
 *
 *  Note that the returned list holds pointers to symbols, not the symbols
 *  themselves. The list can be sorted on function names or function addresses.
 */
unsigned dwarf_collect_functions_in_file(const DWARF_SYMBOLLIST *symboltable,int fileindex,
                                         int sort,const DWARF_SYMBOLLIST *list[],int numentries)
{
  const DWARF_SYMBOLLIST *sym;
  int count;

  assert(symboltable!=NULL);
  if (list==NULL)
    numentries=0;

  count=0;
  for (sym=symboltable->next; sym!=NULL; sym=sym->next) {
    if (sym->fileindex==fileindex && DWARF_IS_FUNCTION(sym)) {
      if (count<numentries) {
        int pos;
        if (sort==DWARF_SORT_ADDRESS) {
          for (pos=0; pos<count && list[pos]->code_addr<sym->code_addr; pos++)
            {}
        } else {
          for (pos=0; pos<count && strcmp(list[pos]->name,sym->name)<0; pos++)
            {}
        }
        assert(list!=NULL);
        if (pos<count)
          memmove(&list[pos+1],&list[pos],(count-pos)*sizeof(DWARF_SYMBOLLIST*));
        list[pos]=sym;
      }
      count+=1;
    }
  }
  return count;
}

/** dwarf_path_from_fileindex() returns the path of the source file (or NULL
 *  if parameter "fileindex" is invalid)
 */
const char *dwarf_path_from_fileindex(const DWARF_PATHLIST *filetable,int fileindex)
{
  const DWARF_PATHLIST *file;

  assert(filetable!=NULL);
  assert(fileindex>=0);
  for (file=filetable->next; file!=NULL && fileindex>0; file=file->next)
    fileindex--;
  return (file!=NULL) ? file->name : NULL;
}

/** dwarf_fileindex_from_path() looks up the path in the file table and
 *  returns the index in the table (if found). The function first tries a
 *  full path, but also compares the base names of the files (if a full path
 *  match fails).
 *
 *  \return The file index, or -1 on failure.
 */
int dwarf_fileindex_from_path(const DWARF_PATHLIST *filetable,const char *path)
{
  const DWARF_PATHLIST *file;
  int fileindex=0;

  assert(filetable!=NULL);
  assert(path!=NULL);
  /* try full path first */
  for (file=filetable->next; file!=NULL; file=file->next) {
    if (strcmp(path,file->name)==0)
      return fileindex;
    fileindex++;
  }
  /* re-try, comparing only base names */
  fileindex=0;
  for (file=filetable->next; file!=NULL; file=file->next) {
    const char *filename=file->name;
    const char *base;
    if ((base = strrchr(filename, '/')) != NULL)
      filename = base + 1;
    #if defined _WIN32
      if ((base = strrchr(filename, '\\')) != NULL)
        filename = base + 1;
    #endif
    if (strcmp(path,filename)==0)
      return fileindex;
    fileindex++;
  }
  return -1;
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

