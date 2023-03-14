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

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elf.h"
#include "fileloader.h"

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
# define stricmp(s1,s2)  strcasecmp((s1),(s2))
#endif

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if !defined sizearray
# define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

typedef struct tagFILESECTION {
  struct tagFILESECTION *next;
  unsigned long address;  /* address for this section on the target */
  unsigned long size;     /* size of the section (in memory) */
  unsigned char *buffer;  /* the section in memory */
  unsigned long filepos;  /* file position of the section in the file */
  char *section_name;     /* ELF files only */
  short section_type;
  short file_type;
} FILESECTION;

static FILESECTION filesection_root = { NULL };

void filesection_clearall(void)
{
  while (filesection_root.next != NULL) {
    FILESECTION *sect = filesection_root.next;
    filesection_root.next = sect->next;
    if (sect->buffer != NULL)
      free(sect->buffer);
    if (sect->section_name != NULL)
      free(sect->section_name);
    free(sect);
  }
}

static FILESECTION *filesection_append(unsigned long address, unsigned char *data, unsigned long size)
{
  FILESECTION *sect = malloc(sizeof(FILESECTION));
  if (sect == NULL)
    return NULL;

  memset(sect, 0, sizeof(FILESECTION));
  sect->address = address;
  sect->size = size;
  sect->buffer = malloc(size * sizeof(unsigned char));
  if (sect->buffer == NULL) {
    free(sect);
    return NULL;
  }
  memcpy(sect->buffer, data, size);

  FILESECTION *tail = &filesection_root;
  while (tail->next != NULL)
    tail = tail->next;
  tail->next = sect;

  return sect;
}

static bool elf_load(FILE *fp)
{
  int type;
  unsigned long fileoffs, filesize, vaddr, paddr;
  for (int segment = 0; elf_segment_by_index(fp, segment, &type, NULL, &fileoffs, &filesize, &vaddr, &paddr, NULL) == ELFERR_NONE; segment++) {
    if (type == ELF_PT_LOAD && filesize != 0) {
      /* read the data from the ELF file */
      unsigned char *data = malloc(filesize);
      if (data == NULL) {
        filesection_clearall();
        return false;
      }
      fseek(fp, fileoffs, SEEK_SET);
      fread(data, 1, filesize, fp);
      /* append to the section list */
      FILESECTION *sect = filesection_append(paddr, data, filesize);
      if (sect == NULL) {
        filesection_clearall();
        return false;
      }
      sect->filepos = fileoffs; /* fill in extra data */
      sect->file_type = FILETYPE_ELF;
      sect->section_type = (vaddr == paddr) ? SECTIONTYPE_CODE : SECTIONTYPE_DATA;
      free(data);
    }
  }

  return (filesection_root.next != NULL);
}

static bool hex_getbyte(FILE *fp, unsigned char *byte)
{
  assert(fp != NULL);
  assert(byte != NULL);

  int c1 = fgetc(fp);
  if ('0' <= c1 && c1 <= '9')
    c1 = c1 - '0';
  else if ('a' <= c1 && c1 <= 'f')
    c1 = c1 - 'a' + 10;
  else if ('A' <= c1 && c1 <= 'F')
    c1 = c1 - 'A' + 10;
  else
    return false;

  int c2 = fgetc(fp);
  if ('0' <= c2 && c2 <= '9')
    c2 = c2 - '0';
  else if ('a' <= c2 && c2 <= 'f')
    c2 = c2 - 'a' + 10;
  else if ('A' <= c2 && c2 <= 'F')
    c2 = c2 - 'A' + 10;
  else
    return false;

  *byte = (unsigned char)((c1 << 4) | c2);
  return true;
}

static bool hex_getrecord(FILE *fp, int *type, unsigned *address, unsigned char *data, size_t *datasize)
{
  assert(fp != NULL);

  /* check leader */
  int c = fgetc(fp);
  if (c != ':')
    return false;

  unsigned checksum = 0;

  /* get data length */
  unsigned char rlen;
  if (!hex_getbyte(fp, &rlen))
    return false;
  checksum += rlen;

  /* get address */
  unsigned char raddr[2] ;
  if (!hex_getbyte(fp, &raddr[0]) || !hex_getbyte(fp, &raddr[1]))
    return false;
  checksum += (unsigned)raddr[0] + raddr[1];
  if (address != NULL)
    *address = ((unsigned)raddr[0] << 8) + raddr[1];

  /* get record type */
  unsigned char rtype;
  if (!hex_getbyte(fp, &rtype))
    return false;
  checksum += rtype;
  if (type != NULL)
    *type = rtype;

  /* get data */
  size_t maxsize = (data != NULL) ? *datasize : 0;
  if (datasize != NULL)
    *datasize = rlen;
  for (unsigned idx = 0; idx < rlen; idx++) {
    unsigned char c;
    if (!hex_getbyte(fp, &c))
      return false;
    checksum += c;
    if (idx < maxsize)
      data[idx] = c;
  }

  /* get checksum */
  unsigned char rcsum;
  if (!hex_getbyte(fp, &rcsum))
    return false;
  checksum += rcsum;

  /* eat up whitespace at the end of the line */
  bool linefeed = false;
  while ((c = fgetc(fp)) >= 0) {
    if (c == '\r' || c == '\n')
      linefeed = true;
    else if (c > ' ')
      break;
  }
  if (!linefeed)
    return false; /* non-whitespace text follows the line */
  ungetc(c, fp);  /* undo last read */

  return ((checksum & 0xff) == 0);
}

static bool hex_load(FILE *fp)
{
  /* prepare data buffer (for collecting the payload of the data records) */
  size_t bufsize = 1024;
  size_t bufused = 0;
  unsigned char *buffer = malloc(bufsize * sizeof(unsigned char));
  if (buffer == NULL)
    return false;
  memset(buffer, 0, bufsize * sizeof(unsigned char));

  bool eof_found = false;
  unsigned long baseaddr = 0;
  unsigned long sectionbase = 0;

  int type;
  unsigned address;
  unsigned char data[1024];
  size_t datasize;
  while (datasize = sizearray(data), hex_getrecord(fp, &type, &address, data, &datasize)) {
    /* record types 3 and 5 for the initial start address (in segmented 16:16
       and linear 32-bit architectures) are irrelevant for firmware downloading */
    if (type == 3 || type == 5)
      continue;
    /* end of record terminates the parsing (even if data folows it) */
    if (type == 1) {
      eof_found = true;
      break;
    }
    /* record type 2 and 4 set a base address (in segmented 16:16 and linear
       32-bit architectures), but carry no data */
    if (type == 2 || type == 4) {
      assert(datasize == 2);
      unsigned long addr = ((unsigned long)data[0] << 8) | data[1];
      baseaddr = (type == 2) ? addr << 4 : addr << 16;
      continue;
    }
    /* ignore any unknown record (though this may indicated an unsupported
       format) */
    assert(type == 0);
    if (type != 0)
      continue;
    /* check whether to add the data collected so far in a new section */
    unsigned long fulladdr = baseaddr + address;
    if (fulladdr < sectionbase || fulladdr > sectionbase + bufused) {
      /* gap in the data, these are separate sections */
      if (bufused > 0) {
        FILESECTION *sect = filesection_append(sectionbase, buffer, bufused);
        if (sect == NULL)
          break;
        sect->file_type = FILETYPE_HEX;
      }
      sectionbase = baseaddr;
      bufused = 0;
      memset(buffer, 0, bufsize * sizeof(unsigned char));
    }

    /* check whether to grow the buffer */
    assert(address >= bufused); /* relative address may not jump back */
    if (address + datasize > bufsize) {
      size_t newsize = bufsize;
      while (address + datasize > newsize)
        newsize *= 2;
      unsigned char *newbuf = malloc(newsize * sizeof(unsigned char));
      if (newbuf == NULL)
        break;  /* exit loop, force "false" to be returned */
      memcpy(newbuf, buffer, bufused * sizeof(unsigned char));
      free(buffer);
      buffer = newbuf;
      bufsize = newsize;
    }
    /* append the data to the buffer */
    memcpy(buffer + address, data, datasize);
    bufused = address + datasize;
  }

  /* append last section (except on error) */
  if (bufused > 0 && eof_found) {
    FILESECTION *sect = filesection_append(sectionbase, buffer, bufused);
    if (sect == NULL)
      filesection_clearall();
    else
      sect->file_type = FILETYPE_HEX;
  } else {
    filesection_clearall();
  }

  free(buffer);
  return (eof_found && filesection_root.next != NULL);
}

bool hex_isvalid(FILE *fp)
{
  /* only test the first record (and make sure to read from the start) */
  rewind(fp);
  bool result = hex_getrecord(fp, NULL, NULL, NULL, NULL);
  rewind(fp);
  return result;
}

/** filesection_loadall() loads all sections in a file. For a BIN file (an
 *  "unknown" file type), all data is loaded as a single section. For an ELF
 *  file, consecutive sections are still loaded into separate memory blocks.
 *  In a HEX file, separate sections are created when there is a gap between
 *  data records, or a "jump" in the base address.
 *
 *  \return true on success, false on failure (file not found, insufficient
 *          memory).
 */
bool filesection_loadall(const char *filename)
{
  filesection_clearall();

  FILE *fp = fopen(filename, "rb");
  if (fp == NULL)
    return false;

  bool result = false;
  int wordsize;
  int err = elf_info(fp, &wordsize, NULL, NULL, NULL);
  if (err == ELFERR_NONE) {
    result = (wordsize == 32); /* only 32-bit architectures at this time */
    if (result)
      result = elf_load(fp);
  } else if (hex_isvalid(fp)) {
    result = hex_load(fp);
  } else {
    /* assume it to be a BIN file, which is loaded as a single section */
    fseek(fp, 0, SEEK_END);
    unsigned long filesize = ftell(fp);
    unsigned char *data = malloc(filesize);
    if (data != NULL) {
      fseek(fp, 0, SEEK_SET);
      fread(data, 1, filesize, fp);
      FILESECTION *sect = filesection_append(0, data, filesize);
      if (sect != NULL) {
        sect->file_type = FILETYPE_UNKNOWN;
        result = true;
      } else {
        filesection_clearall();
      }
      free(data);
    }
  }

  fclose(fp);
  return result;
}

/** filesection_getdata() returns memory block information on the requested
 *  section.
 */
bool filesection_getdata(unsigned index, unsigned long *address, unsigned char **buffer, unsigned long *size, int *type)
{
  FILESECTION *sect = filesection_root.next;
  while (sect != NULL && index > 0) {
    sect = sect->next;
    index -= 1;
  }
  if (sect == NULL)
    return false;

  if (address != NULL)
    *address = sect->address;
  if (size != NULL)
    *size = sect->size;
  if (type != NULL)
    *type = sect->section_type;
  if (buffer != NULL)
    *buffer = sect->buffer;

  return true;
}

/** filesection_gettype() returns the file type. It returns FILETYPE_UNKNOWN
 *  for a BIN file.
 */
int filesection_filetype(void)
{
  if (filesection_root.next == NULL)
    return FILETYPE_NONE;
  return filesection_root.next->file_type;
}

/** filesection_relocate() adds an offset to every section in the list.
 *  This is needed to download a BIN file to the correct address (because BIN
 *  files do no specify the "load" address).
 */
void filesection_relocate(unsigned long offset)
{
  for (FILESECTION *sect = filesection_root.next; sect != NULL; sect = sect->next)
    sect->address += offset;
}

int filesection_patch_vecttable(const char *driver, unsigned int *checksum)
{
  assert(checksum!=NULL);
  *checksum=0;

  /* find the section at memory address 0 (the vector table) */
  FILESECTION *sect;
  for (sect = filesection_root.next; sect != NULL && sect->address != 0; sect = sect->next)
    {}
  if (sect == NULL || sect->size < 8*sizeof(uint32_t))
    return FSERR_NO_VECTTABLE;

  int chksum_idx = 0;
  assert(driver != NULL);
  if (stricmp(driver,"lpc8xx")==0 || stricmp(driver,"lpc11xx")==0 ||
      stricmp(driver,"lpc15xx")==0 || stricmp(driver,"lpc17xx")==0 ||
      stricmp(driver,"lpc43xx")==0 || stricmp(driver,"lpc546xx")==0)
  {
    chksum_idx = 7;
  } else if (stricmp(driver,"lpc21xx")==0 || stricmp(driver,"lpc22xx")==0
             || stricmp(driver,"lpc23xx")==0 || stricmp(driver,"lpc24xx")==0)
  {
    chksum_idx = 5;
  } else {
    return FSERR_NO_DRIVER;
  }

  uint32_t vect[8];
  memcpy(vect, sect->buffer, sizeof vect);

  uint32_t sum = 0;
  for (int idx = 0; idx < 8; idx++) {
    if (idx != chksum_idx)
      sum += vect[idx];
  }
  sum = ~sum + 1;   /* make two's complement */
  assert(checksum != NULL);
  *checksum = sum;

  if (sum == vect[chksum_idx])
    return FSERR_CHKSUMSET;
  vect[chksum_idx] = sum;
  return FSERR_NONE;
}


#define CRP_ADDRESS 0x000002fc  /* hard-coded address for the CRP magic value */
static uint32_t *get_crp_pos(void)
{
  FILESECTION *sect;
  for (sect = filesection_root.next; sect != NULL; sect = sect->next)
    if (sect->address <= CRP_ADDRESS && CRP_ADDRESS + 4 <= sect->address + sect->size)
      break;
  if (sect == NULL)
    return NULL;
  return (uint32_t*)(sect->buffer + (CRP_ADDRESS - sect->address));
}

int filesection_get_crp(void)
{
  uint32_t *magic_ptr = get_crp_pos();
  if (magic_ptr == NULL)
    return 0;
  switch (*magic_ptr) {
  case 0x12345678:
    return 1;       /* SWD disabled, read & compare Flash memory disabled, erase sector 0 disabled unless full Flash is erased (but other sectors can be individually erased/rewritten) */
  case 0x87654321:
    return 2;       /* all of CRP1, plus erase & write disabled, with the exception of full Flash erase */
  case 0x43218765:
    return 3;       /* SWD disabled & ISP disabled (no way to unlock the chip unless the firmware has a "self-erase" function in its code) */
  case 0x4E697370:
    return 4;       /* "no isp": boot pin on reset (for entering ISP mode) disabled (SWD still enabled, so not truly code protection) */
  case 0xbc00b657:
    return 9;       /* placeholder signature */
  }
  return 0;
}

/** filesection_set_crp() overrules the CRP setting in the target file. The
 *  target file must be prepared for CRP; typically for CRP level "9" (no
 *  CRP), but it may also be set to other levels, which are then overruled.
 */
bool filesection_set_crp(int crp)
{
  uint32_t *magic_ptr = get_crp_pos();
  if (magic_ptr == NULL)
    return false;
  uint32_t magic = 0;
  switch (crp) {
  case 1:
    magic = 0x12345678;
    break;
  case 2:
    magic = 0x87654321;
    break;
  case 3:
    magic = 0x43218765;
    break;
  /* case 4 is "no isp" mode, and is not truly CRP, so it is not supported here */
  case 9:
    magic = 0xBC00B657;
    break;
  default:
    return false;
  }

  if (*magic_ptr == 0x12345678 || *magic_ptr == 0x87654321 || *magic_ptr == 0x43218765 || *magic_ptr == 0xBC00B657) {
    *magic_ptr = magic;
    return true;
  }
  return false;
}

