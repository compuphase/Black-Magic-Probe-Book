/*
 * Functions for reading and parsing CMSIS SVD files with MCU-specific register
 * definitions.
 *
 * Copyright 2020-2023 CompuPhase
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
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "svd-support.h"
#include "xmltractor.h"

#if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
# include "strlcpy.h"
#elif defined __linux__
# include <bsd/string.h>
#endif
#if defined _MSC_VER
# define strdup(s)   _strdup(s)
# if _MSC_VER < 1900
#   include "c99_snprintf.h"
# endif
#endif

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

struct tagPERIPHERAL;

typedef struct tagBITFIELD {
  const char *name;
  const char *description;
  short low_bit;                /* bit range */
  short high_bit;
} BITFIELD;

typedef struct tagREGISTER {
  const char *name;
  const char *description;
  unsigned long offset;         /* offset from base address */
  struct tagPERIPHERAL *peripheral;
  unsigned short count;         /* element count (for arrays), 1 for single fields */
  unsigned short size;          /* register size, for arrays: increment to next item */
  unsigned short indexbase;     /* for arrays: first element index (typically 0) */
  BITFIELD *field;
  unsigned field_count;         /* number of valid entries in the bitfield array */
  unsigned field_size;          /* allocation size for the bitfield array */
} REGISTER;

typedef struct tagPERIPHERAL {
  const char *name;
  const char *description;
  unsigned long address;        /* base address */
  unsigned range;               /* size of the address block, for finding a peripheral on address */
  REGISTER *reg;
  unsigned reg_count;
  unsigned reg_size;            /* number of valid entries in the register array */
} PERIPHERAL;               /* allocation size for the register array */

#define INVALID_ADDRESS (unsigned long)(~0)

#if !defined sizearray
# define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

static char svd_prefix[50]= ""; /* "header prefix" as defined in SVD files */
static int svd_regsize = 0;     /* width in bits of a register */
static PERIPHERAL *peripheral = NULL;
static unsigned peripheral_count = 0;
static unsigned peripheral_size = 0;


static PERIPHERAL *peripheral_find(const char *name)
{
  assert(name != NULL);

  for (unsigned idx = 0; idx < peripheral_count; idx++) {
    assert(peripheral != NULL && peripheral[idx].name != NULL);
    if (strcmp(peripheral[idx].name, name) == 0)
      return &peripheral[idx];
  }

  return NULL;
}

static PERIPHERAL *peripheral_add(const char *name, const char *description, unsigned long address)
{
  assert(name != NULL);
  assert(peripheral_find(name) == NULL);  /* should not already exist */

  PERIPHERAL entry;
  entry.name = strdup(name);
  entry.description = (description != NULL && strlen(description) > 0) ? strdup(description): NULL;
  entry.address = address;
  entry.range = 0;  /* filled in later */
  entry.reg = NULL;
  entry.reg_count = 0;
  entry.reg_size = 0;
  if (entry.name == NULL)
    return NULL;        /* adding new peripheral name failed */

  /* grow array, if necessary */
  assert(peripheral_count <= peripheral_size);
  if (peripheral_count >= peripheral_size) {
    int newsize = (peripheral_size == 0) ? 8 : 2 * peripheral_size;
    PERIPHERAL *newlist = malloc(newsize * sizeof(PERIPHERAL));
    if (newlist == NULL) {
      free((void*)entry.name);
      if (entry.description != NULL)
        free((void*)entry.description);
      return NULL;        /* growing the array failed */
    }

    /* copy and free old array  */
    if (peripheral_size > 0) {
      assert(peripheral != NULL);
      memcpy(newlist, peripheral, peripheral_size * sizeof(PERIPHERAL));
      free(peripheral);
    }
    peripheral = newlist;
    peripheral_size = newsize;
  }

  /* shift entries in the list up to keep the peripheral list sorted on name;
     this is needed for auto-completion */
  int top;
  for (top = peripheral_count; top > 0 && strcmp(peripheral[top - 1].name, entry.name) > 0; top--)
    memcpy(&peripheral[top], &peripheral[top - 1], sizeof(PERIPHERAL));
  peripheral[top] = entry;
  peripheral_count += 1;

  return &peripheral[top];
}

static REGISTER *register_find(const PERIPHERAL *per, const char *name)
{
  assert(per != NULL);
  assert(name != NULL);

  for (unsigned idx = 0; idx < per->reg_count; idx++) {
    assert(per->reg != NULL && per->reg[idx].name != NULL);
    if (strcmp(per->reg[idx].name, name) == 0)
      return &per->reg[idx];
  }

  return NULL;
}

static REGISTER *register_add(PERIPHERAL *per, const char *name, const char *description, unsigned long offset,
                                  unsigned short size, unsigned short count, unsigned short arraybase)
{
  assert(per != NULL);
  assert(name != NULL);

  /* no duplicate register should exist, but some SVD files split up a register
     array into two definitions (for example, because there is a reserved range
     in the middle) */
  assert(register_find(per, name) == NULL || strchr(name, '%') != NULL);
  REGISTER *reg = register_find(per, name);
  if (reg != NULL)
    return reg;

  REGISTER entry;
  entry.name = strdup(name);
  entry.description = (description != NULL && strlen(description) > 0) ? strdup(description) : NULL;
  entry.offset = offset;
  entry.count = count;      /* element count (1 for single fields) */
  entry.size = size;        /* element size in bytes */
  entry.indexbase = arraybase;
  entry.peripheral = NULL;  /* filled in later */
  entry.field = NULL;
  entry.field_count = 0;
  entry.field_size = 0;
  if (entry.name == NULL)
    return NULL;        /* adding new register name failed */

  /* grow array */
  assert(per->reg_count <= per->reg_size);
  if (per->reg_count >= per->reg_size) {
    unsigned newsize = (per->reg_size == 0) ? 8 : 2 * per->reg_size;
    REGISTER *newlist = malloc(newsize * sizeof(REGISTER));
    if (newlist == NULL) {
      free((void*)entry.name);
      if (entry.description != NULL)
        free((void*)entry.description);
      return NULL;        /* growing the array failed */
    }

    /* copy and free old array  */
    if (per->reg_size > 0) {
      assert(per->reg != NULL);
      memcpy(newlist, per->reg, per->reg_size * sizeof(REGISTER));
      free(per->reg);
    }
    per->reg = newlist;
    per->reg_size = newsize;
  }

  /* shift entries in the list up to keep the register list sorted on name
     (for auto-completion) */
  int top;
  for (top = per->reg_count; top > 0 && strcmp(per->reg[top - 1].name, entry.name) > 0; top--)
    memcpy(&per->reg[top], &per->reg[top - 1], sizeof(REGISTER));
  per->reg[top] = entry;
  per->reg_count += 1;

  return &per->reg[top];
}

static BITFIELD *bitfield_add(REGISTER *reg, const char *name, const char *description,
                              const char *bitrange)
{
  assert(reg != NULL);
  assert(name != NULL);

  /* the documented pattern style (CMSIS-SVD specification 1.3.1) is [4:6] (where
     4 & 6 are arbitrary numbers), but I have seen [4-6], [4~6] and [4..6] in
     practice */
  short low = -1, high = -1;
  if (bitrange != NULL && strlen(bitrange) > 0) {
    if (*bitrange == '[')
      bitrange += 1;
    char *ptr;
    low = (short)strtol(bitrange, &ptr, 10);
    if (*ptr == ':' || *ptr == '-' || *ptr == '~' || *ptr == '.') {
      ptr += 1;
      while (*ptr == '.')
        ptr += 1;
      high = (short)strtol(ptr, NULL, 10);
      if (low > high) {
        short tmp = low;
        low = high;
        high = tmp;
      }
    } else {
      high = low;
    }
  }

  BITFIELD entry;
  entry.name = strdup(name);
  entry.description = (description != NULL && strlen(description) > 0) ? strdup(description) : NULL;
  entry.low_bit = low;
  entry.high_bit = high;
  if (entry.name == NULL)
    return NULL;        /* adding new bitfield name failed */

  /* grow array */
  assert(reg->field_count <= reg->field_size);
  if (reg->field_count >= reg->field_size) {
    int newsize = (reg->field_size == 0) ? 8 : 2 * reg->field_size;
    BITFIELD *newlist = malloc(newsize * sizeof(BITFIELD));
    if (newlist == NULL) {
      free((void*)entry.name);
      if (entry.description != NULL)
        free((void*)entry.description);
      return NULL;        /* growing the array failed */
    }
    /* copy and free old array  */

    if (reg->field_size > 0) {
      assert(reg->field != NULL);
      memcpy(newlist, reg->field, reg->field_size * sizeof(BITFIELD));
      free(reg->field);
    }
    reg->field = newlist;
    reg->field_size = newsize;
  }

  /* shift entries in the list up to keep the register list sorted on bit range */
  int top;
  for (top = reg->field_count; top > 0 && reg->field[top - 1].low_bit > entry.low_bit; top--)
    memcpy(&reg->field[top], &reg->field[top - 1], sizeof(BITFIELD));
  reg->field[top] = entry;
  reg->field_count += 1;

  return &reg->field[top];
}

static char *strdel(char *str, size_t count)
{
  size_t length;
  assert(str != NULL);
  length= strlen(str);
  if (count > length)
    count = length;
  memmove(str, str+count, length-count+1);  /* include EOS byte */
  return str;
}

static char *strins(char *dest, size_t destsize, const char *src)
{
  size_t destlen, srclen;
  assert(dest != NULL);
  destlen = strlen(dest);
  assert(src != NULL);
  srclen = strlen(src);
  if (destlen+srclen >= destsize) {
    assert(destsize >= destlen+1);
    srclen = destsize-destlen-1;
  }
  memmove(dest+srclen, dest, destlen+1); /* include EOS byte */
  memcpy(dest, src, srclen);
  return dest;
}

static void reformat_description(char *string)
{
  char *p;
  /* replace all whitespace by space characters */
  for (p = string; *p != '\0'; p++)
    if (*p < ' ')
      *p = ' ';
  /* replace multiple spaces by a single one */
  for (p = string; *p != '\0'; p++) {
    if (*p == ' ') {
      int count = 0;
      while (p[count] == ' ')
        count++;
      if (count > 1)
        strdel(p + 1, count - 1);
    }
  }
}

void svd_clear(void)
{
  for (unsigned p = 0; p < peripheral_count; p++) {
    assert(peripheral != NULL && peripheral[p].name != NULL);
    free((void*)peripheral[p].name);
    if (peripheral[p].description != NULL)
      free((void*)peripheral[p].description);
    for (unsigned r = 0; r < peripheral[p].reg_count; r++) {
      assert(peripheral[p].reg != NULL && peripheral[p].reg[r].name != NULL);
      free((void*)peripheral[p].reg[r].name);
      if (peripheral[p].reg[r].description)
        free((void*)peripheral[p].reg[r].description);
      for (unsigned b = 0; b < peripheral[p].reg[r].field_count; b++) {
        assert(peripheral[p].reg[r].field != NULL && peripheral[p].reg[r].field[b].name != NULL);
        free((void*)peripheral[p].reg[r].field[b].name);
        if (peripheral[p].reg[r].field[b].description)
          free((void*)peripheral[p].reg[r].field[b].description);
      }
      if (peripheral[p].reg[r].field != NULL)
        free((void*)peripheral[p].reg[r].field);
    }
    if (peripheral[p].reg != NULL)
      free((void*)peripheral[p].reg);
  }
  if (peripheral != NULL) {
    free((void*)peripheral);
    peripheral = NULL;
  }
  peripheral_count = 0;
  peripheral_size = 0;
  svd_prefix[0] = '\0';
  svd_regsize = 0;
}

bool svd_load(const char *filename)
{
  assert(filename != NULL);
  svd_clear();

  FILE *fp = fopen(filename, "rt");
  if (fp == NULL)
    return false;
  /* allocate memory for the entire file, plus a zero-terminator */
  fseek(fp, 0, SEEK_END);
  size_t filesize = ftell(fp);
  char *buffer = malloc((filesize+1) * sizeof(char));
  if (buffer == NULL) {
    /* insufficient memory */
    fclose(fp);
    return false;
  }
  /* read the file as one long string (and terminate the string) */
  fseek(fp, 0, SEEK_SET);
  fread(buffer, 1, filesize, fp);
  buffer[filesize] = '\0';
  fclose(fp);

  /* parse the information */
  xt_Node *xmlroot = xt_parse(buffer);
  if (xmlroot == NULL || xmlroot->szname != 6 || strncmp(xmlroot->name, "device", 6) != 0) {
    /* not an XML file, or not in the correct format */
    free(buffer);
    return false;
  }

  svd_regsize = 32; /* default register width for (ARM Cortex) */
  xt_Node *xmlfield = xt_find_child(xmlroot, "size");
  if (xmlfield == NULL)
    xmlfield = xt_find_child(xmlroot, "width");
  if (xmlfield != NULL)
    svd_regsize = (int)strtol(xmlfield->content, NULL, 0);

  xmlfield = xt_find_child(xmlroot, "headerDefinitionsPrefix");
  if (xmlfield != NULL && xmlfield->szcontent < sizearray(svd_prefix)) {
    strncpy(svd_prefix, xmlfield->content, xmlfield->szcontent);
    svd_prefix[xmlfield->szcontent] = '\0';
  }

  xmlroot = xt_find_child(xmlroot, "peripherals");
  if (xmlroot != NULL) {
    xmlroot = xt_find_child(xmlroot, "peripheral");
    while (xmlroot != NULL) {
      char periph_name[100] = "";
      xmlfield = xt_find_child(xmlroot, "name");
      if (xmlfield != NULL && xmlfield->szcontent < sizearray(periph_name)) {
        strncpy(periph_name, xmlfield->content, xmlfield->szcontent);
        periph_name[xmlfield->szcontent] = '\0';
      }
      char periph_descr[256] = "";
      xmlfield = xt_find_child(xmlroot, "description");
      if (xmlfield != NULL && xmlfield->szcontent < sizearray(periph_descr)) {
        strncpy(periph_descr, xmlfield->content, xmlfield->szcontent);
        periph_descr[xmlfield->szcontent] = '\0';
        reformat_description(periph_descr);
      }
      xmlfield = xt_find_child(xmlroot, "baseAddress");
      unsigned long base_addr = (xmlfield != NULL) ? strtoul(xmlfield->content, NULL, 0) : 0;

      PERIPHERAL *per = peripheral_add(periph_name, periph_descr, base_addr);
      xt_Node *xmlreg = xt_find_child(xmlroot, "registers");
      if (xmlreg != NULL && per != NULL) {
        xmlreg = xt_find_child(xmlreg, "register");
        while (xmlreg != NULL) {
          char reg_name[100] = "";
          xmlfield = xt_find_child(xmlreg, "name");
          if (xmlfield != NULL && xmlfield->szcontent < sizearray(reg_name)) {
            strncpy(reg_name, xmlfield->content, xmlfield->szcontent);
            reg_name[xmlfield->szcontent] = '\0';
          }
          char reg_descr[256] = "";
          xmlfield = xt_find_child(xmlreg, "description");
          if (xmlfield != NULL && xmlfield->szcontent < sizearray(reg_descr)) {
            strncpy(reg_descr, xmlfield->content, xmlfield->szcontent);
            reg_descr[xmlfield->szcontent] = '\0';
            reformat_description(reg_descr);
          }
          xmlfield = xt_find_child(xmlreg, "addressOffset");
          unsigned long offset = (xmlfield != NULL) ? strtoul(xmlfield->content, NULL, 0) : 0;
          xmlfield = xt_find_child(xmlreg, "dim");
          unsigned short dim = (xmlfield != NULL) ? (unsigned)strtoul(xmlfield->content, NULL, 0) : 1;
          xmlfield = xt_find_child(xmlreg, "dimIndex");
          unsigned short indexbase = (xmlfield != NULL) ? (unsigned)strtoul(xmlfield->content, NULL, 0) : 0;
          xmlfield = xt_find_child(xmlreg, "dimIncrement");
          unsigned short regsize = (xmlfield != NULL) ? (unsigned)strtoul(xmlfield->content, NULL, 0) : (unsigned short)(svd_regsize / 8);
          /* add the register information to the list */
          REGISTER *reg = register_add(per, reg_name, reg_descr, offset, regsize, dim, indexbase);
          /* check for bit fields, add these too if present */
          xt_Node *xmlbitf = xt_find_child(xmlreg, "fields");
          if (xmlbitf != NULL && reg != NULL) {
            xmlbitf = xt_find_child(xmlbitf, "field");
            while (xmlbitf != NULL) {
              char bitf_name[100]= "";
              xmlfield = xt_find_child(xmlbitf, "name");
              if (xmlfield != NULL && xmlfield->szcontent < sizearray(bitf_name)) {
                strncpy(bitf_name, xmlfield->content, xmlfield->szcontent);
                bitf_name[xmlfield->szcontent] = '\0';
              }
              char bitf_descr[256] = "";
              xmlfield = xt_find_child(xmlbitf, "description");
              if (xmlfield != NULL && xmlfield->szcontent < sizearray(bitf_descr)) {
                strncpy(bitf_descr, xmlfield->content, xmlfield->szcontent);
                bitf_descr[xmlfield->szcontent] = '\0';
                reformat_description(bitf_descr);
              }
              /* three syntaxes for specifying a bit range exist (!) */
              char bitf_range[256] = "";
              xmlfield = xt_find_child(xmlbitf, "bitRange");    /* pattern style */
              if (xmlfield != NULL && xmlfield->szcontent < sizearray(bitf_range)) {
                strncpy(bitf_range, xmlfield->content, xmlfield->szcontent);
                bitf_range[xmlfield->szcontent] = '\0';
              }
              xmlfield = xt_find_child(xmlbitf, "bitOffset");   /* offset/width style */
              int bitoffs = (xmlfield != NULL) ? (int)strtol(xmlfield->content, NULL, 0) : -1;
              xmlfield = xt_find_child(xmlbitf, "bitWidth");
              int bitwidth = (xmlfield != NULL) ? (int)strtol(xmlfield->content, NULL, 0) : -1;
              if (bitoffs >= 0 && bitwidth >= 0)
                sprintf(bitf_range, "[%d:%d]", bitoffs, bitoffs + bitwidth - 1);
              xmlfield = xt_find_child(xmlbitf, "lsb");       /* LSB/MSB style */
              bitoffs = (xmlfield != NULL) ? (int)strtol(xmlfield->content, NULL, 0) : -1;
              xmlfield = xt_find_child(xmlbitf, "msb");
              bitwidth = (xmlfield != NULL) ? (int)strtol(xmlfield->content, NULL, 0) : -1;
              if (bitoffs >= 0 && bitwidth >= 0)
                sprintf(bitf_range, "[%d:%d]", bitoffs, bitwidth);
              //??? parse enumeratedValues attached to the bit field
              bitfield_add(reg, bitf_name, bitf_descr, bitf_range);
              xmlbitf = xt_find_sibling(xmlbitf, "field");
            }
          }
          xmlreg = xt_find_sibling(xmlreg, "register");
        }
      }

      xmlroot = xt_find_sibling(xmlroot, "peripheral");
    }
  }

  /* no longer need the allocated buffer */
  free(buffer);

  /* set the "back-links" of the register definitions to the peripheral
     definitions; set the address range of each peripheral */
  for (unsigned idx = 0; idx < peripheral_count; idx++) {
    if (peripheral[idx].reg_count == 0)
      continue;
    assert(peripheral[idx].reg != NULL);
    unsigned long top = 0;
    for (unsigned ridx = 0; ridx < peripheral[idx].reg_count; ridx++) {
      peripheral[idx].reg[ridx].peripheral = &peripheral[idx];
      assert(peripheral[idx].reg[ridx].count > 0);
      assert(peripheral[idx].reg[ridx].size > 0);
      if (peripheral[idx].reg[ridx].offset > top)
        top = peripheral[idx].reg[ridx].offset + peripheral[idx].reg[ridx].count * peripheral[idx].reg[ridx].size;
    }
    peripheral[idx].range = top;
  }

  return true;
}

const char *svd_mcu_prefix(void)
{
  return svd_prefix;
}

/** svd_peripheral() returns the peripheral at the given index.
 *  \param index        The index of the peripheral to return, starting from 0.
 *  \param address      The base address of the peripheral registers. This
 *                      parameter may be NULL.
 *  \param description  An optional description of the peripheral. This
 *                      parameter may be NULL.
 *
 *  \return A pointer to the name of the peripheral, or NULL if parameter
 *          "index" is out of range.
 */
const char *svd_peripheral(unsigned index, unsigned long *address, const char **description)
{
  if (index >= peripheral_count)
    return NULL;
  if (address != NULL)
    *address = peripheral[index].address;
  if (description != NULL)
    *description = peripheral[index].description;
  return peripheral[index].name;
}

/** svd_register() returns the register at the given index.
 *  \param peripheral   [in] The name of the peripheral.
 *  \param index        The index of the peripheral to return, starting from 0.
 *  \param address      [out] The base address of the register. This parameter
 *                      may be NULL.
 *  \param range        [out] The address range of the register (in bytes). This
 *                      parameter may be NULL.
 *  \param description  [out] An optional description of the register. This
 *                      parameter may be NULL.
 *
 *  \return A pointer to the name of the register on success, or NULL on
 *          failure. Reasons for failure are that the peripheral is not found,
 *          or that the index parameter is out of range.
 */
const char *svd_register(const char *peripheral, unsigned index, unsigned long *offset,
                         int *range, const char **description)
{
  assert(peripheral != NULL);
  const PERIPHERAL *per = peripheral_find(peripheral);
  if (per == NULL)
    return NULL;

  if (index >= per->reg_count)
    return NULL;
  if (offset != NULL)
    *offset = per->reg[index].offset;
  if (range != NULL)
    *range = per->reg[index].count * per->reg[index].size;
  if (description != NULL)
    *description = per->reg[index].description;
  return per->reg[index].name;
}

/** svd_bitfield() returns information of a bitfield in a register.
 *  \param peripheral   [in] The name of the peripheral.
 *  \param regname      [in] The name of the register.
 *  \param index        The index of the peripheral to return, starting from 0.
 *  \param low_bit      [out] The first bit in the field range. This parameter
 *                      may be NULL.
 *  \param high_bit     [out] The last bit in the field range (the number of
 *                      bits is (high_bit - low_bit + 1). This parameter may be
 *                      NULL.
 *  \param description  [out] An optional description of the bit field. This
 *                      parameter may be NULL.
 *
 *  \return A pointer to the name of the name of the bitfield on success, or
 *          NULL on failure. Reasons for failure are that the peripheral or
 *          register are not found, or that the index parameter is out of range.
 *
 *  \note To get cleaned-up names for the peripheral and register from user
 *        input, use svd_lookup().
 */
const char *svd_bitfield(const char *peripheral, const char *regname, unsigned index,
                         short *low_bit, short *high_bit, const char **description)
{
  assert(peripheral != NULL);
  assert(regname != NULL);
  const PERIPHERAL *per = peripheral_find(peripheral);
  if (per == NULL)
    return NULL;
  const REGISTER *reg = register_find(per, regname);
  if (reg == NULL)
    return NULL;

  if (index >= reg->field_count)
    return NULL;
  if (low_bit != NULL)
    *low_bit = reg->field[index].low_bit;
  if (high_bit != NULL)
    *high_bit = reg->field[index].high_bit;
  if (description != NULL)
    *description = reg->field[index].description;
  return reg->field[index].name;
}

/** register_parse() looks up a peripheral/register combination and returns
 *  the structure with the information.
 *  \param symbol       [in] The register name (must include the peripheral).
 *  \param suffix       [out] Will point to the first character behind the
 *                      register name upon return. This parameter may be NULL.
 *
 *  \return The register structure on success, NULL on failure.
 */
static const REGISTER *register_parse(const char *symbol, const char **suffix)
{
  char reg_name[100] = "";
  const char *p;
  const PERIPHERAL *per;
  const REGISTER *reg;

  assert(symbol != NULL);

  /* check whether the symbol starts with the prefix, if so, skip the prefix */
  size_t len = strlen(svd_prefix);
  if (len > 0 && len < strlen(symbol) && strncmp(symbol, svd_prefix, len) == 0)
    symbol += len;

  /* assume the peripheral name is separated from the register name by either
     '.', '->' or '_' */
  per = NULL;
  if (((p = strchr(symbol, '-')) != NULL && *(p + 1) == '>')
      || (p = strchr(symbol, '.')) != NULL
      || (p = strchr(symbol, '_')) != NULL)
  {
    char periph_name[100] = "";
    if ((len = p - symbol) < sizearray(periph_name)) {
      strncpy(periph_name, symbol, len);
      periph_name[len] = '\0';
      symbol += len;
      if (*symbol == '-')
        symbol += 2;  /* skip '->' */
      else
        symbol += 1;  /* skip '.' or '_' */
      /* check whether there is a peripheral with that name */
      per = peripheral_find(periph_name);
    }
  }
  if (per == NULL)
    return NULL;

  /* find the register in the peripheral */
  p = strchr(symbol, '[');
  len = (p != NULL) ? p - symbol : strlen(symbol);
  while (len > 0 && symbol[len - 1] <= ' ')
    len -= 1; /* handle case for space characters between the register name and the '[' */
  strncpy(reg_name, symbol, len);
  reg_name[len] = '\0';
  if (p != NULL)
    strlcat(reg_name, "%s", sizearray(reg_name));
  reg = register_find(per, reg_name);

  if (suffix != NULL)
    *suffix = (p != NULL) ? p : symbol + len;

  return reg;
}

/** svd_xlate_name() translates a peripheral/register name to an address
 *  specification in a format used by GDB.
 *  \param symbol       [in] The register name (must include the peripheral; the
 *                      prefix is optional).
 *  \param alias        [out] The output buffer. In-place translation is
 *                      allowed: parameter "alias" may be the same buffer as
 *                      parameter "symbol".
 *  \param alias_size   The size of the output buffer.
 *
 *  \return true on success, false on failure.
 */
bool svd_xlate_name(const char *symbol, char *alias, size_t alias_size)
{
  const REGISTER *reg;
  const char *suffix;
  unsigned long address;

  assert(symbol != NULL);
  reg = register_parse(symbol, &suffix);
  if (reg == NULL)
    return false;

  assert(reg->peripheral != NULL);
  address = reg->peripheral->address + reg->offset;

  assert(alias != NULL);
  assert(alias_size > 0);

  /* if the register ends with '%s' and the symbol has a '[' at that location,
     that means an array syntax */
  assert(suffix != NULL);
  if (strchr(reg->name, '%') != NULL && *suffix == '[') {
    char regindex[50] = "", *p;
    int len;
    suffix += 1;
    while (*suffix != '\0' && *suffix <= ' ')
      suffix += 1;
    p = strchr(suffix, ']');
    if (p != NULL && (len = p - suffix) < sizearray(regindex)) {
      while (len > 0 && suffix[len - 1] <= ' ')
        len -= 1;
      strncpy(regindex, suffix, len);
      regindex[len] = '\0';
    }
    if (strlen(regindex) == 0)
      return false;
    snprintf(alias, alias_size, "{unsigned}(0x%lx+%d*(%s))", address, reg->size, regindex);
  } else {
    snprintf(alias, alias_size, "{unsigned}0x%lx", address);
  }

  return true;
}

/** svd_xlate_all_names() translate all register names in the "text" parameter
 *  to their respective addresses.
 *  \param text     [in/out] The text with register names.
 *  \param maxsize  The size of the buffer that "text" points to.
 *
 *  \return The number of translated registers.
 */
int svd_xlate_all_names(char *text, size_t maxsize)
{
  assert(text != NULL);
  int count = 0;
  char *head = text;
  while (*head != '\0') {
    unsigned len;
    /* extract next word */
    while (*head != '\0' && *head <= ' ')
      head += 1;
    char *tail;
    for (tail = head; *tail > ' '; tail++)
      {}
    len = tail - head;
    char word[50];
    if (len == 0 || len >= sizearray(word)) {
      /* no more words (but trailing white-space), or word too long -> skip */
      head = tail;
      continue;
    }
    strncpy(word, head, len);
    word[len] = '\0';
    /* check to replace the register by a memory address */
    char alias[50];
    if (svd_xlate_name(word, alias, sizearray(alias))) {
      /* delete word, then insert the alias */
      strdel(head, len);
      strins(head, maxsize - (head - text), alias);
      head += strlen(alias);
      count += 1;
    } else {
      head = tail;
    }
  }
  return count;
}

/** svd_lookup() looks up a peripheral or register.
 *  \param symbol       [in] Input name, may include a prefix, and it may be the
 *                      name of a peripheral or a register.
 *  \param index        The number of matches to skip. If there are multiple
 *                      registers that match the description, incrementing the
 *                      index on each call lets you look up each match.
 *  \param periph_name  [out] Set to point to the name of the peripheral (or to
 *                      point to NULL on failure). This name excludes any MCU
 *                      prefix. This parameter may be set to NULL.
 *  \param reg_name     [out] Set to point to the name of the register (or to
 *                      point to NULL if register name is absent). This
 *                      parameter may be set to NULL.
 *  \param address      [out] Set to the base address of the peripheral or to
 *                      the address of the register. This parameter may be set
 *                      to NULL.
 *  \param description  [out] Set to point to the description string of the
 *                      peripheral or register. This parameter may be set to
 *                      NULL.
 *
 *  \return number of matches, or 0 on failure.
 */
int svd_lookup(const char *symbol, int index, const char **periph_name, const char **reg_name,
               unsigned long *address, const char **description)
{
  assert(symbol != NULL);

  /* preset outputs */
  if (periph_name != NULL)
    *periph_name = NULL;
  if (reg_name != NULL)
    *reg_name = NULL;
  if (address != NULL)
    *address = 0;
  if (description != NULL)
    *description = NULL;

  if (peripheral_count == 0)
    return 0; /* quick exit */

  /* check whether the symbol starts with the prefix, if so, skip the prefix */
  size_t len = strlen(svd_prefix);
  if (len > 0 && len < strlen(symbol) && strncmp(symbol, svd_prefix, len) == 0)
    symbol += len;

  /* split the symbol into peripheral and register; assume the peripheral
     name is separated from the register name by either '.', '->' or '_' */
  char p_name[100] = "";
  char r_name[100] = "";
  const char *p;
  if (((p = strchr(symbol, '-')) != NULL && *(p + 1) == '>')
      || (p = strchr(symbol, '.')) != NULL
      || (p = strchr(symbol, '_')) != NULL
      || (p = strchr(symbol, ' ')) != NULL)
  {
    if ((len = p - symbol) < sizearray(p_name)) {
      strncpy(p_name, symbol, len);
      p_name[len] = '\0';
      symbol += len;
      if (*symbol == '-')
        symbol += 2;  /* skip '->' */
      else
        symbol += 1;  /* skip '.' or '_' */
      strlcpy(r_name, symbol, sizearray(r_name));
    }
  } else {
    /* no separator, check whether it is a peripheral name; otherwise assume
       it is a register */
    if (peripheral_find(symbol))
      strlcpy(p_name, symbol, sizearray(p_name));
    else
      strlcpy(r_name, symbol, sizearray(r_name));
  }

  /* remove array suffix from the register name */
  int arrayindex = -1;
  if (r_name[0] != '\0') {
    char *p2 = strchr(r_name, '[');
    if (p2 != NULL) {
      *p2++ = '\0'; /* strip off suffix */
      while (*p2 != ' ')
        p2++;
      if (isdigit(*p2))
        arrayindex = (int)strtol(p2, NULL, 0);
    }
  }

  /* look up peripheral */
  int count = 1;  /* preset for most cases */
  const PERIPHERAL *per = NULL;
  const REGISTER *reg = NULL;
  if (p_name[0] != '\0') {
    /* find the given peripheral */
    per = peripheral_find(p_name);
  } else if (r_name[0]!= '\0') {
    /* a register name is set, but the peripheral name is not, look up the
       register in any peripheral */
    assert(peripheral != NULL);
    count = 0;
    for (unsigned idx = 0; idx < peripheral_count; idx++)  {
      const REGISTER *r = register_find(&peripheral[idx], r_name);
      if (r != NULL) {
        count += 1;
        if (index-- == 0)
          reg = r;
      }
    }
    /* on failure, also check whether the register is in fact an array */
    strlcat(r_name, "%s", sizearray(r_name));
    for (unsigned idx = 0; idx < peripheral_count; idx++) {
      const REGISTER *r = register_find(&peripheral[idx], r_name);
      if (r != NULL) {
        count += 1;
        if (index-- == 0)
          reg = r;
      }
    }
    if (reg != NULL)
      per = reg->peripheral;
  }
  if (per == NULL)
    return 0;

  /* look up register, unless that was already implicitly done */
  if (r_name[0]!= '\0' && reg == NULL) {
    assert(per != NULL);
    reg = register_find(per, r_name);
    if (reg == NULL) {
      /* on failure, also check whether the register is in fact an array */
      strlcat(r_name, "%s", sizearray(r_name));
      reg = register_find(per, r_name);
    }
    if (reg == NULL)
      return 0; /* register name was set, but not found -> failure */
  }

  /* store pointers */
  if (periph_name != NULL)
    *periph_name = per->name;
  /* if only a peripheral is specified, return information on the peripheral */
  if (reg != NULL) {
    if (reg_name != NULL)
      *reg_name = reg->name;
    if (description != NULL)
      *description = reg->description;
    if (address != NULL) {
      *address = per->address + reg->offset;
      if (arrayindex >= 0 && arrayindex >= reg->indexbase && (arrayindex - reg->indexbase) <= reg->count)
        *address += (arrayindex - reg->indexbase);
    }
  } else {
    if (address != NULL)
      *address = per->address;
    if (description != NULL)
      *description = per->description;
  }

  return count;
}

