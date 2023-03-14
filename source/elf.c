/*
 * Routines for querying information on ELF files and post-processing them
 * for requirements of specific micro-controllers. At this moment, the utility
 * supports various ranges of the LPC family by NXP.
 *
 * Copyright 2015,2019-2023 CompuPhase
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
#define __POCC__OLDNAMES
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "elf.h"
#if defined WIN32 || defined _WIN32
# if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
#   include "strlcpy.h"
# endif
#elif defined __linux__
# include <bsd/string.h>
#endif

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined __GNUC__
# define PACKED        __attribute__((packed))
#else
# define PACKED
#endif

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
# pragma pack(1)        /* structures must be packed (byte-aligned) */
#elif defined MACOS && defined __MWERKS__
# pragma options align=mac68k
#else
# pragma pack(push)
# pragma pack(1)        /* structures must be packed (byte-aligned) */
# if defined __TURBOC__
#   pragma option -a-   /* "pack" pragma for older Borland compilers */
# endif
#endif

#if defined _MSC_VER
# define stricmp(s1,s2)    _stricmp((s1),(s2))
# define strdup(s)         _strdup(s)
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
  uint16_t shtrndx;     /* the index of the entry in the section header table that holds the section names (shstrtab) */
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
  uint16_t type;        /* 1 = relocatable, 2 = executable, 3 = shared, 4 = core */
  uint16_t machine;     /* 0x03 = x86, 0x28 = ARM, 0x32 = IA-64, 0x3e = x86-64 */
  uint32_t version;     /* 1 for the original ELF format */
  uint64_t entry;       /* memory address of the entry point */
  uint64_t phoff;       /* file offset to the start of the program header table */
  uint64_t shoff;       /* file offset to the start of the section header table */
  uint32_t flags;
  uint16_t ehsize;      /* the size of this header (52 bytes) */
  uint16_t phentsize;   /* the size of an entry in the program header table */
  uint16_t phnum;       /* the number of entries in the program header table */
  uint16_t shentsize;   /* the size of an entry in the section header table */
  uint16_t shnum;       /* the number of entries in the section header table */
  uint16_t shtrndx;     /* the index of the entry in the section header table that holds the section names (shstrtab) */
} PACKED ELF64HDR;

typedef struct tagELF32PROGRAM {
  uint32_t type;        /* 0 = unused, 1 = loadable, 2 = dynamic link info. */
  uint32_t offset;      /* file offset to the start of the segment  */
  uint32_t vaddr;       /* run-time memory address (virtual address) */
  uint32_t paddr;       /* data load memory address (physical address) */
  uint32_t filesz;      /* segment size in the file */
  uint32_t memsz;       /* segment size in memory */
  uint32_t flags;       /* read/write/execute flags */
  uint32_t align;       /* address alignment */
} PACKED ELF32PROGRAM;

typedef struct tagELF64PROGRAM {
  uint32_t type;        /* 0 = unused, 1 = loadable, 2 = dynamic link info. */
  uint32_t flags;       /* read/write/execute flags */
  uint64_t offset;      /* file offset to the start of the segment  */
  uint64_t vaddr;       /* run-time memory address (virtual address) */
  uint64_t paddr;       /* data load memory address (physical address) */
  uint64_t filesz;      /* segment size in the file */
  uint64_t memsz;       /* segment size in memory */
  uint64_t align;       /* address alignment */
} PACKED ELF64PROGRAM;

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

typedef struct tagELF64SECTION {
  uint32_t name;        /* index in the string table */
  uint32_t type;
  uint64_t flags;
  uint64_t addr;        /* memory address */
  uint64_t offset;      /* file offset to the start of the section */
  uint64_t size;        /* size of the section */
  uint32_t link;
  uint32_t info;
  uint64_t addralign;
  uint64_t entsize;     /* entry size, for sections that have fixed-length entries */
} PACKED ELF64SECTION;

typedef struct tagELF32SYMBOL {
  uint32_t name;        /* index in the string table (or 0 for anonymous) */
  uint32_t addr;        /* memory address */
  uint32_t size;        /* size of the symbol (or 0 for unknown) */
  uint8_t  info;        /* symbol type and binding */
  uint8_t  other;       /* visibility */
  uint16_t shndx;       /* index of the section the symbol is defined in*/
} PACKED ELF32SYMBOL;

/* a subset of "machine" types */
#define EM_386      3   /* Intel 80386 */
#define EM_PPC      20  /* PowerPC */
#define EM_PPC64    21  /* 64-bit PowerPC */
#define EM_ARM      40  /* ARM 32-bit architecture (AARCH32) */
#define EM_IA_64    50  /* Intel IA-64 processor architecture */
#define EM_ST100    60  /* STMicroelectronics ST100 processor */
#define EM_X86_64   62  /* AMD x86-64 architecture */
#define EM_AVR      83  /* Atmel AVR 8-bit micro-controller */
#define EM_C166     116 /* Infineon C16x/XC16x processor */
#define EM_8051     165 /* Intel 8051 and variants */
#define EM_STXP7X   166 /* STMicroelectronics STxP7x family of configurable and extensible RISC processors */
#define EM_AARCH64  183 /* ARM 64-bit architecture (AARCH64) */
#define EM_AVR32    185 /* Atmel Corporation 32-bit microprocessor family */
#define EM_MCHP_PIC 204 /* Microchip 8-bit PIC(r) family */

#define SHT_NULL          0x0   /* Section header table entry unused */
#define SHT_PROGBITS      0x1   /* Program data */
#define SHT_SYMTAB        0x2   /* Symbol table */
#define SHT_STRTAB        0x3   /* String table */
#define SHT_RELA          0x4   /* Relocation entries with addends */
#define SHT_HASH          0x5   /* Symbol hash table */
#define SHT_DYNAMIC       0x6   /* Dynamic linking information */
#define SHT_NOTE          0x7   /* Notes */
#define SHT_NOBITS        0x8   /* Program space with no data (bss) */
#define SHT_REL           0x9   /* Relocation entries, no addends */
#define SHT_SHLIB         0x0A  /* Reserved */
#define SHT_DYNSYM        0x0B  /* Dynamic linker symbol table */
#define SHT_INIT_ARRAY    0x0E  /* Array of constructors */
#define SHT_FINI_ARRAY    0x0F  /* Array of destructors */
#define SHT_PREINIT_ARRAY 0x10  /* Array of pre-constructors */
#define SHT_GROUP         0x11  /* Section group */
#define SHT_SYMTAB_SHNDX  0x12  /* Extended section indices */
#define SHT_NUM           0x13  /* Number of defined types */

#define STT_NOTYPE   0  /* unspecified */
#define STT_OBJECT   1  /* data object */
#define STT_FUNC     2  /* code object */
#define STT_SECTION  3  /* section name */
#define STT_FILE     4  /* source filename */
#define STT_COMMON   5  /* common (uninitialized) data object */
#define STT_TLS      6  /* thread-local data object*/
#define STT_IFUNC   10  /* indirect code object (GNU, OS-specific) */


#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
#  pragma pack()        /* reset default packing */
#elif defined MACOS && defined __MWERKS__
#  pragma options align=reset
#else
#  pragma pack(pop)     /* reset previous packing */
#endif


#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
#  define stricmp(s1,s2)  strcasecmp((s1),(s2))
#endif
#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

#define SWAP16(v)     ((((v) >> 8) & 0xff) | (((v) & 0xff) << 8))
#define SWAP32(v)     ((((v) >> 24) & 0xff) | (((v) & 0xff0000) >> 8) | (((v) & 0xff00) << 8)  | (((v) & 0xff) << 24))


/** elf_info() verifies that the file is an ELF executable and returns important
 *  fields from the header.
 *
 *  \param fp           [in] File handle to the ELF file.
 *  \param wordsize     [out] Set to the size of a "word" in bits, either 32 or
 *                      64. This parameter may be NULL.
 *  \param bigendian    [out] Set to 1 if the ELF file uses Big Endian byte
 *                      order. This parameter may be NULL.
 *  \param machine      [out] Set to an identifier for the processor
 *                      architecture. This parameter may be NULL.
 *  \param entry_addr   [out] Set to the address of the entry point. This
 *                      parameter may be set to NULL.
 *
 *  \return An error code.
 */
int elf_info(FILE *fp,int *wordsize,int *bigendian,int *machine,unsigned long *entry_addr)
{
  ELF32HDR hdr;

  if (wordsize!=NULL)
    *wordsize=0;
  if (bigendian!=NULL)
    *bigendian=0;
  if (machine!=NULL)
    *machine=0;

  memset(&hdr,0,sizeof(hdr));
  fseek(fp,0,SEEK_SET);
  fread(&hdr,sizeof(hdr),1,fp);
  if (memcmp(hdr.magic,"\177ELF",4)!=0)
    return ELFERR_FILEFORMAT; /* magic not found, not a valid ELF file */
  if (hdr.shoff==0)
    return ELFERR_FILEFORMAT; /* we consider an ELF file without section header table as invalid */

  if (wordsize!=NULL)
    *wordsize=(hdr.wordsize==1) ? 32 : 64;

  if (bigendian!=NULL)
    *bigendian=(hdr.endian==2);

  if (machine!=NULL)
    *machine=(hdr.endian==2) ? SWAP16(hdr.machine) : hdr.machine;

  if (entry_addr!=NULL) {
    if (hdr.wordsize==1) {
      *entry_addr=hdr.entry;
    } else {
      ELF64HDR hdr64;
      fseek(fp,0,SEEK_SET);
      fread(&hdr64,sizeof(hdr64),1,fp);
      *entry_addr=(unsigned long)hdr64.entry;
    }
  }

  return ELFERR_NONE;
}

static int seek_shstrtab(FILE *fp,uint32_t *offset,uint32_t *size)
{
  ELF32HDR hdr;
  ELF32SECTION section;
  uint32_t offs;
  uint16_t idx,entrysize;

  assert(fp!=NULL);
  assert(offset!=NULL);
  assert(size!=NULL);

  memset(&hdr,0,sizeof(hdr));
  fseek(fp,0,SEEK_SET);
  fread(&hdr,sizeof(hdr),1,fp);
  if (memcmp(hdr.magic,"\177ELF",4)!=0)
    return ELFERR_FILEFORMAT; /* magic not found, not a valid ELF file */
  assert(hdr.wordsize==1);    /* not yet implemented for 64-bit */
  assert(hdr.shoff!=0);

  offs=hdr.shoff;
  idx=hdr.shtrndx;
  entrysize=hdr.shentsize;
  if (hdr.endian==2) {
    offs=SWAP32(offs);
    idx=SWAP16(idx);
    entrysize=SWAP16(entrysize);
  }
  assert(entrysize==sizeof(section));
  fseek(fp,offs+idx*entrysize,SEEK_SET);
  fread(&section,sizeof(section),1,fp);
  *offset=(hdr.endian==2) ? SWAP32(section.offset) : section.offset;
  *size=(hdr.endian==2) ? SWAP32(section.size) : section.size;
  fseek(fp,*offset,SEEK_SET);
  return ELFERR_NONE;
}

static const char *read_sectionnames(FILE *fp,uint32_t *size)
{
  char *stringtable;
  uint32_t offset;
  int err=seek_shstrtab(fp,&offset,size);
  if (err!=ELFERR_NONE)
    return NULL;

  stringtable=malloc(*size);
  if (stringtable==NULL)
    return NULL;
  fread(stringtable,1,*size,fp);
  return stringtable;
}

/** elf_segment_by_index() returns information on a segment ("program" in ELF
 *  jargon).
 *
 *  \param fp           File handle to the ELF file.
 *  \param index        The index of the segment to return, starting at 0.
 *  \param type         The type of the segment. This parameter may be NULL.
 *  \param flags        The read/write/execute flags of the segment. This
 *                      parameter may be NULL.
 *  \param offset       The file offset to the segment data. This parameter may
 *                      be NULL.
 *  \param filesize     The size of the segment data in the file. This parameter
 *                      may be NULL.
 *  \param vaddr        The (virtual) address at which the segment will be at
 *                      run-time. This parameter may be NULL.
 *  \param paddr        When initialized data must be copied from (Flash) ROM to
 *                      RAM, this address is where the initialization data will
 *                      be stored. This parameter may be NULL.
 *  \param memsize      The size that the segment has in memory. Note that this
 *                      relates to the "vaddr" address; the size of the data
 *                      stored at the "paddr" address is "filesize". This
 *                      parameter may be NULL.
 *
 *  \return An error code.
 */
int elf_segment_by_index(FILE *fp,int index,
                         int *type, int *flags,
                         unsigned long *offset,unsigned long *filesize,
                         unsigned long *vaddr,unsigned long *paddr,
                         unsigned long *memsize)
{
  ELF32HDR hdr;

  assert(index>=0);
  if (type!=NULL)
    *type=-1;
  if (offset!=NULL)
    *offset=0;
  if (filesize!=NULL)
    *filesize=0;
  if (vaddr!=NULL)
    *vaddr=0;
  if (paddr!=NULL)
    *paddr=0;
  if (memsize!=NULL)
    *memsize=0;

  memset(&hdr,0,sizeof(hdr));
  fseek(fp,0,SEEK_SET);
  fread(&hdr,sizeof(hdr),1,fp);
  if (memcmp(hdr.magic,"\177ELF",4)!=0)
    return ELFERR_FILEFORMAT;  /* magic not found, not a valid ELF file */

  if (hdr.wordsize==1) {
    ELF32PROGRAM segment;
    uint32_t offs=hdr.phoff;
    int num=hdr.phnum;
    int size=hdr.phentsize;

    if (hdr.endian==2) {
      offs=SWAP32(offs);
      num=SWAP16(num);
      size=SWAP16(size);
    }
    if (offs==0)
      return ELFERR_FILEFORMAT;/* consider an ELF file without program header table as invalid */
    if (index>=num)
      return ELFERR_NOMATCH;   /* requested segment not present */
    assert(size==sizeof(segment));
    fseek(fp,offs+index*size,SEEK_SET);
    fread(&segment,sizeof(segment),1,fp);
    if (type!=NULL)
      *type=(hdr.endian==2) ? SWAP32(segment.type) : segment.type;
    if (flags!=NULL)
      *flags=(hdr.endian==2) ? SWAP32(segment.flags) : segment.flags;
    if (offset!=NULL)
      *offset=(hdr.endian==2) ? SWAP32(segment.offset) : segment.offset;
    if (filesize!=NULL)
      *filesize=(hdr.endian==2) ? SWAP32(segment.filesz) : segment.filesz;
    if (vaddr!=NULL)
      *vaddr=(hdr.endian==2) ? SWAP32(segment.vaddr) : segment.vaddr;
    if (paddr!=NULL)
      *paddr=(hdr.endian==2) ? SWAP32(segment.paddr) : segment.paddr;
    if (memsize!=NULL)
      *memsize=(hdr.endian==2) ? SWAP32(segment.memsz) : segment.memsz;
  } else {
    //??? re-read the header, but now using the 64-bit structure
    return ELFERR_FILEFORMAT;
  }

  return ELFERR_NONE;
}

/** elf_section_by_name() verifies that the file is an ELF executable and
 *  retrieves the file offset to requested section plus its length.
 *
 *  \param fp           [in] File handle to the ELF file.
 *  \param sectionname  [in] The name of the section to locate.
 *  \param offset       [out] Set to the file offset to the section in the ELF
 *                      file. This parameter may be NULL.
 *  \param address      [out] The memory address for the section. This parameter
 *                      may be NULL.
 *  \param length       [out] Set to the length of the section in the ELF file.
 *                      This parameter may be NULL.
 *
 *  \return An error code.
 */
int elf_section_by_name(FILE *fp,const char *sectionname,unsigned long *offset,
                        unsigned long *address,unsigned long *length)
{
  ELF32HDR hdr;

  assert(sectionname!=NULL && strlen(sectionname)>0);
  if (offset!=NULL)
    *offset=0;
  if (address!=NULL)
    *address=0;
  if (length!=NULL)
    *length=0;

  memset(&hdr,0,sizeof(hdr));
  fseek(fp,0,SEEK_SET);
  fread(&hdr,sizeof(hdr),1,fp);
  if (memcmp(hdr.magic,"\177ELF",4)!=0)
    return ELFERR_FILEFORMAT; /* magic not found, not a valid ELF file */

  if (hdr.wordsize==1) {
    ELF32SECTION section;
    uint32_t offs=hdr.shoff;
    int num=hdr.shnum;
    int size=hdr.shentsize;
    int idx=hdr.shtrndx;
    const char *stringtable;
    int idx_section=-1;

    if (hdr.endian==2) {
      offs=SWAP32(offs);
      num=SWAP16(num);
      size=SWAP16(size);
      idx=SWAP16(idx);
    }
    if (offs==0)
      return ELFERR_FILEFORMAT; /* consider an ELF file without section header table as invalid */
    assert(size==sizeof(section));
    /* get the string table first, to find the index of the requested section */
    stringtable=read_sectionnames(fp,(uint32_t*)&size);
    if (stringtable!=NULL) {
      const char *ptr=stringtable;
      while (ptr-stringtable<size && idx_section==-1) {
        if (strcmp(ptr,sectionname)==0)
          idx_section=(int)(ptr-stringtable);  /* this is the index in the string table */
        ptr+=strlen(ptr)+1;
      }
      free((void*)stringtable);
    }
    /* now find the section that has the index that is found */
    offs=hdr.shoff;
    if (hdr.endian==2)
      offs=SWAP32(offs);
    fseek(fp,offs,SEEK_SET);
    for (idx=0; idx<num; idx++) {
      int nidx;
      fread(&section,sizeof(section),1,fp);
      nidx= (hdr.endian==2) ? SWAP32(section.name) : section.name;
      if (nidx==idx_section) {
        if (offset!=NULL)
          *offset=(hdr.endian==2) ? SWAP32(section.offset) : section.offset;
        if (address!=NULL)
          *address=(hdr.endian==2) ? SWAP32(section.addr) : section.addr;
        if (length!=NULL)
          *length=(hdr.endian==2) ? SWAP32(section.size) : section.size;
        break;  /* no need to search further */
      }
    }

  } else {
    //??? re-read the header, but now using the 64-bit structure
    return ELFERR_FILEFORMAT;
  }

  return ELFERR_NONE;
}

/** elf_section_by_address() finds the first section at or after a given
 *  address, and return its name, offset in the file, start address and length.
 *  Note that only sections with code or data are considered (sections with
 *  symbolic information or relocation tables are ignored).
 *
 *  \param fp           File handle to the ELF file.
 *  \param baseaddr     The memory address to start the search at.
 *  \param sectionname  Set to the name of the section. This parameter may be
 *                      NULL.
 *  \param namelength   The size of te buffer that parameter sectionname points
 *                      to, in characters.
 *  \param offset       Set to the file offset to the section in the ELF file.
 *                      This parameter may be NULL.
 *  \param address      The memory address for the section. This parameter may
 *                      be NULL.
 *  \param length       Set to the length of the section in the ELF file.
 *                      This parameter may be NULL.
 *
 *  \return An error code.
 */
int elf_section_by_address(FILE *fp,unsigned long baseaddr,
                           char *sectionname,size_t namelength,unsigned long *offset,
                           unsigned long *address,unsigned long *length)
{
  ELF32HDR hdr;

  if (sectionname!=NULL && namelength>0)
    *sectionname='\0';
  if (offset!=NULL)
    *offset=0;
  if (address!=NULL)
    *address=0;
  if (length!=NULL)
    *length=0;

  memset(&hdr,0,sizeof(hdr));
  fseek(fp,0,SEEK_SET);
  fread(&hdr,sizeof(hdr),1,fp);
  if (memcmp(hdr.magic,"\177ELF",4)!=0)
    return ELFERR_FILEFORMAT; /* magic not found, not a valid ELF file */
  if (hdr.shoff==0)
    return ELFERR_FILEFORMAT; /* we consider an ELF file without section header table as invalid */

  if (hdr.wordsize==1) {
    ELF32SECTION section;
    uint32_t offs=hdr.shoff;
    int num=hdr.shnum;
    int size=hdr.shentsize;
    int idx,nearest_idx;
    uint32_t nearest_addr;
    int idx_section;

    if (hdr.endian==2) {
      offs=SWAP32(offs);
      num=SWAP16(num);
      size=SWAP16(size);
    }
    assert(size==sizeof(section));

    /* find the section index nearest (but not below) the base address */
    idx_section=0;
    nearest_idx=-1;
    nearest_addr=UINT_MAX;
    fseek(fp,offs,SEEK_SET);
    for (idx=0; idx<num; idx++) {
      uint32_t addr;
      fread(&section,sizeof(section),1,fp);
      if (section.type!=SHT_PROGBITS || section.size==0)
        continue;
      addr=(hdr.endian==2) ? SWAP32(section.addr) : section.addr;
      if (addr>=baseaddr && addr<nearest_addr) {
        nearest_addr=addr;
        nearest_idx=idx;
        idx_section=(hdr.endian==2) ? SWAP32(section.name) : section.name;
        if (offset!=NULL)
          *offset=(hdr.endian==2) ? SWAP32(section.offset) : section.offset;
        if (address!=NULL)
          *address=(hdr.endian==2) ? SWAP32(section.addr) : section.addr;
        if (length!=NULL)
          *length=(hdr.endian==2) ? SWAP32(section.size) : section.size;
      }
    }
    if (nearest_idx<0)
      return ELFERR_NOMATCH;

    /* look up the section name in the string table */
    if (sectionname!=NULL && namelength>0) {
      uint32_t tblsize;
      const char *stringtable=read_sectionnames(fp,&tblsize);
      if (stringtable!=NULL) {
        const char *ptr=stringtable+idx_section;
        strlcpy(sectionname,ptr,namelength);
        free((void*)stringtable);
      }
    }

  } else {
    //??? re-read the header, but now using the 64-bit structure
    return ELFERR_FILEFORMAT;
  }

  return ELFERR_NONE;
}

/** elf_load_symbols() loads the symbol table from an ELF file (if one is
 *  present).
 *
 *  To query the required size for holding the symbol table, call this function
 *  with parameter "symbols" set to NULL. The number of entries will be returned
 *  in parameter "number". After allocating sufficient memory, call this
 *  function again, with "number" set to the number of allocated entries.
 *
 *  \param fp         [in] The file poiner to the ELF file.
 *  \param symbols    [out] A pointer to an array that will hold the symbol
 *                    table, or NULL to query the required size of the symbol
 *                    table.
 *  \param number     [in/out] If the "symbols" parameter is non-NULL, this
 *                    parameter holds the maximum number of entries that the
 *                    array can hold. If the "symbols" parameter is NULL, this
 *                    parameter will be set to the required number of entries.
 *
 *  \return An error value.
 */
int elf_load_symbols(FILE *fp,ELF_SYMBOL *symbols,unsigned *number)
{
  unsigned long offset,length;
  char *stringtable;
  ELF32SYMBOL sym;
  int i,total,err;
  unsigned size;

  assert(number!=NULL);
  size=(symbols!=NULL) ? *number : 0;
  if (symbols!=NULL && *number>0)
    memset(symbols,0,*number*sizeof(ELF_SYMBOL));
  *number=0;

  /* first read the symbol string table */
  assert(fp!=NULL);
  err=elf_section_by_name(fp,".strtab",&offset,NULL,&length);
  if (err!=ELFERR_NONE)
    return err;
  stringtable=malloc(length);
  if (stringtable==NULL)
    return ELFERR_MEMORY;
  fseek(fp,offset,SEEK_SET);
  fread(stringtable,1,length,fp);

  /* now get the symbol table */
  err=elf_section_by_name(fp,".symtab",&offset,NULL,&length);
  if (err!=ELFERR_NONE) {
    free((void*)stringtable);
    return err;
  }

  assert(length % sizeof(ELF32SYMBOL)==0);
  total=length/sizeof(ELF32SYMBOL);
  fseek(fp,offset,SEEK_SET);
  for (i=0; i<total; i++) {
    int type;
    fread(&sym,sizeof(ELF32SYMBOL),1,fp);
    if (sym.name==0)
      continue; /* ignore anonymous symbols */
    type=sym.info & 0x0f;
    if (type!=STT_OBJECT && type!=STT_FUNC && type!=STT_COMMON)
      continue; /* collect only functions & variables */
    if (*number<size) {
      const char *ptr=stringtable+sym.name;
      assert(symbols!=NULL);  /* otherwise "size" would be zero (and this "if" never entered) */
      symbols[*number].name=strdup(ptr);
      symbols[*number].address=sym.addr;
      symbols[*number].size=sym.size;
      symbols[*number].is_func=(type==STT_FUNC);
      symbols[*number].is_ext=(((sym.info >> 4) & 1)!=0);
      if (symbols[*number].name==NULL) {
        elf_clear_symbols(symbols,*number);
        return ELFERR_MEMORY;
      }
    }
    *number+=1;
  }

  free((void*)stringtable);
  return ELFERR_NONE;
}

void elf_clear_symbols(ELF_SYMBOL *symbols,unsigned number)
{
  assert(symbols!=NULL);
  for (unsigned i=0; i<number; i++)
    if (symbols[i].name!=NULL)
      free((void*)symbols[i].name);
}

/** elf_check_vecttable() returns whether the vector table of the NXP LPC
 *  microcontroller has the correct checksum.
 */
int elf_check_vecttable(FILE *fp)
{
  assert(fp!=NULL);
  int wordsize,bigendian,machine;
  int result=elf_info(fp,&wordsize,&bigendian,&machine,NULL);
  if (result!=ELFERR_NONE || wordsize!=32 || machine!=EM_ARM)
    return ELFERR_FILEFORMAT; /* only 32-bit ARM architecture */

  /* find the section at memory address 0 (the vector table) */
  unsigned long offset,address,length;
  result=elf_section_by_address(fp,0,NULL,0,&offset,&address,&length);
  if (result!=ELFERR_NONE || address!=0 || length<8*sizeof(uint32_t))
    return ELFERR_FILEFORMAT;

  uint32_t vect[8];
  fseek(fp,offset,SEEK_SET);
  fread(vect,sizeof(uint32_t),sizearray(vect),fp);
  uint32_t sum=0;
  for (int idx=0; idx<8; idx++) {
    if (bigendian)
      vect[idx]=SWAP32(vect[idx]);
    sum+=vect[idx];
  }

  return (sum == 0) ? ELFERR_NONE : ELFERR_CHKSUMERR;
}

/** elf_patch_vecttable() updates the checksum in the vector table in the ELF
 *  file, for LPC micro-controllers.
 *
 *  \param fp         The file poiner to the ELF file.
 *  \param driver     The name of the micro-controller class, which is the name
 *                    of the target driver of the Black Magic Probe.
 *  \param checksum   Will be set to the calculated checksum on return.
 *
 *  \return An error value.
 */
int elf_patch_vecttable(FILE *fp,const char *driver,unsigned int *checksum)
{
  assert(checksum!=NULL);
  *checksum=0;

  assert(fp!=NULL);
  int wordsize,bigendian,machine;
  int result=elf_info(fp,&wordsize,&bigendian,&machine,NULL);
  if (result!=ELFERR_NONE || wordsize!=32 || machine!=EM_ARM)
    return ELFERR_FILEFORMAT; /* only 32-bit ARM architecture */

  /* find the section at memory address 0 (the vector table) */
  unsigned long offset,address,length;
  result=elf_section_by_address(fp,0,NULL,0,&offset,&address,&length);
  if (result!=ELFERR_NONE || address!=0 || length<8*sizeof(uint32_t))
    return ELFERR_FILEFORMAT;

  result=ELFERR_NONE;

  assert(driver!=NULL);
  if (stricmp(driver,"lpc8xx")==0 || stricmp(driver,"lpc11xx")==0 ||
      stricmp(driver,"lpc15xx")==0 || stricmp(driver,"lpc17xx")==0 ||
      stricmp(driver,"lpc43xx")==0 || stricmp(driver,"lpc546xx")==0)
  {
    uint32_t vect[8];
    memset(vect,0,sizeof vect);
    fseek(fp,offset,SEEK_SET);
    fread(vect,sizeof(uint32_t),sizearray(vect),fp);

    uint32_t sum = 0;
    for (int idx=0; idx<7; idx++) {
      if (bigendian)
        vect[idx]=SWAP32(vect[idx]);
      sum+=vect[idx];
    }
    sum=~sum+1; /* make two's complement */
    assert(checksum!=NULL);
    *checksum=sum;

    if (sum==vect[7]) {
      result=ELFERR_CHKSUMSET;
    } else {
      if (bigendian)
        sum=SWAP32(sum);
      vect[7]=sum;
      fseek(fp,offset,SEEK_SET);
      fwrite(vect,sizeof(uint32_t),sizearray(vect),fp);
    }
  } else if (stricmp(driver,"lpc21xx")==0 || stricmp(driver,"lpc22xx")==0
             || stricmp(driver,"lpc23xx")==0 || stricmp(driver,"lpc24xx")==0)
  {
    uint32_t vect[8];
    memset(vect,0,sizeof vect);
    fseek(fp,offset,SEEK_SET);
    fread(vect,sizeof(uint32_t),sizearray(vect),fp);

    uint32_t sum = 0;
    for (int idx = 0; idx<8; idx++) {
      if (bigendian)
        vect[idx]=SWAP32(vect[idx]);
      if (idx!=5)
        sum+=vect[idx];
    }
    sum=~sum+1; /* make two's complement */
    assert(checksum!=NULL);
    *checksum=sum;

    if (sum==vect[5]) {
      result=ELFERR_CHKSUMSET;
    } else {
      if (bigendian)
        sum=SWAP32(sum);
      vect[5]=sum;
      fseek(fp,offset,SEEK_SET);
      fwrite(vect,sizeof(uint32_t),sizearray(vect),fp);
    }
  } else {
    return ELFERR_UNKNOWNDRIVER;
  }

  return result;
}

/** elf_check_crp() checks the code-read-protection level for LPC
 *  micro-controllers in the ELF file.
 *  file.
 *  \param fp         The file poiner to the ELF file.
 *  \param crp        Will be set to the crp level on return (0 for none, 4 for
 *                    "no isp" mode).
 *
 *  \return An error value.
 */
int elf_check_crp(FILE *fp,int *crp)
{
# define CRP_ADDRESS 0x000002fc  /* hard-coded address for the CRP magic value */
  unsigned long offset,base,address,length;
  uint32_t magic;
  int wordsize,bigendian,machine,result;

  assert(crp!=NULL);
  *crp=0;

  assert(fp!=NULL);
  result=elf_info(fp,&wordsize,&bigendian,&machine,NULL);
  if (result!=ELFERR_NONE || wordsize!=32 || machine!=EM_ARM)
    return ELFERR_FILEFORMAT;   /* only 32-bit ARM architecture */

  /* find the section where the CRP "magic" may be stored */
  base=0;
  for ( ;; ) {
    result=elf_section_by_address(fp,base,NULL,0,&offset,&address,&length);
    if (result!=ELFERR_NONE || address>CRP_ADDRESS)
      return ELFERR_FILEFORMAT;
    if (address<=CRP_ADDRESS && address+length>CRP_ADDRESS+4)
      break;
    base+=length;
  }

  result=ELFERR_NONE;

  offset+=(CRP_ADDRESS-address);
  fseek(fp,offset,SEEK_SET);
  fread(&magic,sizeof(uint32_t),1,fp);
  switch (magic) {
  case 0x12345678:
    *crp=1;         /* SWD disabled, read & compare Flash memory disabled, erase sector 0 disabled unless full Flash is erased (but other sectors can be individually erased/rewritten) */
    break;
  case 0x87654321:
    *crp=2;         /* all of CRP1, plus erase & write disabled, with the exception of full Flash erase */
    break;
  case 0x43218765:
    *crp=3;         /* SWD disabled & ISP disabled (no way to unlock the chip unless the firmware has a "self-erase" function in its code) */
    break;
  case 0x4E697370:
    *crp=4;         /* "no isp": boot pin on reset (for entering ISP mode) disabled (SWD still enabled, so not truly code protection) */
    break;
  case 0xbc00b657:
    *crp=9;         /* placeholder signature */
    break;
  }

  return result;
}

