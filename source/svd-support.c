/*
 * Functions for reading and parsing CMSIS SVD files with MCU-specific register
 * definitions.
 *
 * Copyright 2020-2021 CompuPhase
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
#include <stdlib.h>
#include <string.h>
#include "xmltractor.h"

#if defined __MINGW32__ || defined __MINGW64__
  #include "strlcpy.h"
#elif defined __linux__
  #include <bsd/string.h>
#elif defined(_MSC_VER) && _MSC_VER < 1900
  #include "c99_snprintf.h"
#endif

struct tagPERIPHERAL;

typedef struct tagREGISTER {
  const char *name;
  unsigned long offset;         /* offset from base address */
  struct tagPERIPHERAL *peripheral;
  unsigned short range;         /* for arrays: top of the array range */
  unsigned short increment;     /* for arrays: increment in bytes */
} REGISTER;

typedef struct tagPERIPHERAL {
  const char *name;
  unsigned long address;        /* base address */
  unsigned range;               /* size of the address block, for finding a peripheral on address */
  REGISTER *reg;
  int reg_count;
} PERIPHERAL;

#define INVALID_ADDRESS (unsigned long)(~0)

#if !defined sizearray
  #define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif

static char svd_prefix[50]= ""; /* "header prefix" as defined in SVD files */
static int svd_regsize = 0;     /* width in bits of a register */
static PERIPHERAL *peripheral = NULL;
static int peripheral_count = 0;


static PERIPHERAL *peripheral_find(const char *name)
{
  int idx;

  assert(name != NULL);

  for (idx = 0; idx < peripheral_count; idx++) {
    assert(peripheral != NULL && peripheral[idx].name != NULL);
    if (strcmp(peripheral[idx].name, name) == 0)
      return &peripheral[idx];
  }

  return NULL;
}

static PERIPHERAL *peripheral_add(const char *name, unsigned long address)
{
  PERIPHERAL *newlist, entry;
  int top;

  assert(peripheral_find(name) == NULL);  /* should not already exist */

  entry.name = strdup(name);
  entry.address = address;
  entry.range = 0;      /* filled in later */
  entry.reg = NULL;
  entry.reg_count = 0;
  if (entry.name == NULL)
    return NULL;        /* adding new peripheral name failed */

  /* grow array */
  newlist = malloc((peripheral_count + 1) * sizeof(PERIPHERAL));
  if (newlist == NULL) {
    free((void*)entry.name);
    return NULL;        /* growing the array failed */
  }

  /* copy and free old array  */
  if (peripheral_count > 0) {
    assert(peripheral != NULL);
    memcpy(newlist, peripheral, peripheral_count * sizeof(PERIPHERAL));
    free(peripheral);
  }

  /* shift entries in the list up to keep the peripheral list sorted on name;
     this is needed for auto-completion */
  for (top = peripheral_count; top > 0 && strcmp(newlist[top - 1].name, entry.name) > 0; top--)
    memcpy(&newlist[top], &newlist[top - 1], sizeof(PERIPHERAL));
  newlist[top] = entry;

  /* set new list (old list was already freed) */
  peripheral = newlist;
  peripheral_count += 1;

  return &newlist[top];
}

static REGISTER *register_find(const PERIPHERAL *per, const char *name)
{
  int idx;

  assert(per != NULL);
  assert(name != NULL);

  for (idx = 0; idx < per->reg_count; idx++) {
    assert(per->reg != NULL && per->reg[idx].name != NULL);
    if (strcmp(per->reg[idx].name, name) == 0)
      return &per->reg[idx];
  }

  return NULL;
}

static REGISTER *register_add(PERIPHERAL *per, const char *name, unsigned long offset,
                              unsigned short range, unsigned short increment)
{
  REGISTER *newlist, *reg, entry;
  int top;

  assert(per != NULL);
  assert(name != NULL);

  /* no duplicate register should exist, but some SVD files split up a register
     array into two definitions (for example, because there is a reserved range
     in the middle) */
  assert(register_find(per, name) == NULL || strchr(name, '%') != NULL);
  if ((reg = register_find(per, name)) != NULL)
    return reg;

  entry.name = strdup(name);
  entry.offset = offset;
  entry.range = range;
  entry.increment = increment;
  entry.peripheral = NULL;  /* filled in later */
  if (entry.name == NULL)
    return NULL;        /* adding new peripheral name failed */

  /* grow array */
  newlist = malloc((per->reg_count + 1) * sizeof(REGISTER));
  if (newlist == NULL) {
    free((void*)entry.name);
    return NULL;        /* growing the array failed */
  }

  /* copy and free old array  */
  if (per->reg_count > 0) {
    assert(per->reg != NULL);
    memcpy(newlist, per->reg, per->reg_count * sizeof(REGISTER));
    free(per->reg);
  }

  /* shift entries in the list up to keep the peripheral list sorted on name
     (for auto-completion) */
  for (top = per->reg_count; top > 0 && strcmp(newlist[top - 1].name, entry.name) > 0; top--)
    memcpy(&newlist[top], &newlist[top - 1], sizeof(REGISTER));
  newlist[top] = entry;

  /* set new list (old list was already freed) */
  per->reg = newlist;
  per->reg_count += 1;

  return &newlist[top];
}

void svd_clear(void)
{
  int p;
  for (p = 0; p < peripheral_count; p++) {
    int r;
    assert(peripheral != NULL && peripheral[p].name != NULL);
    free((void*)peripheral[p].name);
    for (r = 0; r < peripheral[p].reg_count; r++) {
      assert(peripheral[p].reg != NULL && peripheral[p].reg[r].name != NULL);
      free((void*)peripheral[p].reg[r].name);
    }
    if (peripheral[p].reg != NULL)
      free(peripheral[p].reg);
  }
  if (peripheral != NULL) {
    free(peripheral);
    peripheral = NULL;
  }
  peripheral_count = 0;
  svd_prefix[0] = '\0';
  svd_regsize = 0;
}

int svd_load(const char *filename)
{
  FILE *fp;
  char *buffer;
  size_t filesize;
  xt_Node *xmlroot, *xmlfield;
  int idx;

  assert(filename != NULL);
  svd_clear();

  fp = fopen(filename, "rt");
  if (fp == NULL)
    return 0;
  /* allocate memory for the entire file, plus a zero-terminator */
  fseek(fp, 0, SEEK_END);
  filesize = ftell(fp);
  buffer = malloc((filesize+1) * sizeof(char));
  if (buffer == NULL) {
    /* insufficient memory */
    fclose(fp);
    return 0;
  }
  /* read the file as one long string (and terminate the string) */
  fseek(fp, 0, SEEK_SET);
  fread(buffer, 1, filesize, fp);
  buffer[filesize] = '\0';
  fclose(fp);

  /* parse the information */
  xmlroot = xt_parse(buffer);
  if (xmlroot == NULL || xmlroot->szname != 6 || strncmp(xmlroot->name, "device", 6) != 0) {
    /* not an XML file, or not in the correct format */
    free(buffer);
    return 0;
  }

  svd_regsize = 32; /* default register with for (ARM Cortex) */
  xmlfield = xt_find_child(xmlroot, "size");
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
    xt_Node *xmlreg;
    PERIPHERAL *per;
    char periph_name[50] = "";
    unsigned long base_addr;
    xmlroot = xt_find_child(xmlroot, "peripheral");
    while (xmlroot != NULL) {
      xmlfield = xt_find_child(xmlroot, "name");
      if (xmlfield != NULL && xmlfield->szcontent < sizearray(periph_name)) {
        strncpy(periph_name, xmlfield->content, xmlfield->szcontent);
        periph_name[xmlfield->szcontent] = '\0';
      }
      xmlfield = xt_find_child(xmlroot, "baseAddress");
      base_addr = (xmlfield != NULL) ? strtoul(xmlfield->content, NULL, 0) : 0;

      per = peripheral_add(periph_name, base_addr);
      xmlreg = xt_find_child(xmlroot, "registers");
      if (xmlreg != NULL && per != NULL) {
        char reg_name[50] = "";
        unsigned long offset;
        unsigned short dim;
        unsigned short increment;
        xmlreg = xt_find_child(xmlreg, "register");
        while (xmlreg != NULL) {
          xmlfield = xt_find_child(xmlreg, "name");
          if (xmlfield != NULL && xmlfield->szcontent < sizearray(reg_name)) {
            strncpy(reg_name, xmlfield->content, xmlfield->szcontent);
            reg_name[xmlfield->szcontent] = '\0';
          }
          xmlfield = xt_find_child(xmlreg, "addressOffset");
          offset = (xmlfield != NULL) ? strtoul(xmlfield->content, NULL, 0) : 0;
          xmlfield = xt_find_child(xmlreg, "dim");
          dim = (xmlfield != NULL) ? (unsigned)strtoul(xmlfield->content, NULL, 0) : 1;
          xmlfield = xt_find_child(xmlreg, "dimIncrement");
          increment = (xmlfield != NULL) ? (unsigned)strtoul(xmlfield->content, NULL, 0) : (unsigned short)(svd_regsize / 8);
          /* add the register information to the list */
          register_add(per, reg_name, offset, dim, increment);
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
  for (idx = 0; idx < peripheral_count; idx++) {
    int ridx;
    unsigned long top = 0;
    if (peripheral[idx].reg_count == 0)
      continue;
    assert(peripheral[idx].reg != NULL);
    for (ridx = 0; ridx < peripheral[idx].reg_count; ridx++) {
      peripheral[idx].reg[ridx].peripheral = &peripheral[idx];
      assert(peripheral[idx].reg[ridx].range > 0);
      assert(peripheral[idx].reg[ridx].increment > 0);
      if (peripheral[idx].reg[ridx].offset > top)
        top = peripheral[idx].reg[ridx].offset
              + (peripheral[idx].reg[ridx].range - 1) * peripheral[idx].reg[ridx].increment;
    }
    peripheral[idx].range = top;
  }

  return 1;
}

const char *svd_mcu_prefix(void)
{
  return svd_prefix;
}

const char *svd_peripheral(unsigned index, unsigned long *address)
{
  if (index >= peripheral_count)
    return NULL;
  if (address != NULL)
    *address = peripheral[index].address;
  return peripheral[index].name;
}

const char *svd_register(const char *peripheral, unsigned index, unsigned long *offset)
{
  static const char *cache_name;
  static const PERIPHERAL *cache_periph;
  const PERIPHERAL *per;

  assert(peripheral != NULL);
  if (peripheral == cache_name)
    per = cache_periph;
  else
    cache_periph = per = peripheral_find(peripheral);
  if (per == NULL)
    return NULL;

  if (index >= per->reg_count)
    return NULL;
  if (offset != NULL)
    *offset = per->reg[index].offset;
  return per->reg[index].name;
}

static const REGISTER *register_parse(const char *symbol, const char **suffix)
{
  int len;
  char periph_name[50] = "";
  char reg_name[50] = "";
  const char *p;
  const PERIPHERAL *per;
  const REGISTER *reg;

  assert(symbol != NULL);

  /* check whether the symbol starts with the prefix, if so, skip the prefix */
  len = strlen(svd_prefix);
  if (len > 0 && len < strlen(symbol) && strncmp(symbol, svd_prefix, len) == 0)
    symbol += len;

  /* assume the peripheral name is separated from the register name by either
     '.', '->' or '_' */
  per = NULL;
  if (((p = strchr(symbol, '-')) != NULL && *(p + 1) == '>')
      || (p = strchr(symbol, '.')) != NULL
      || (p = strchr(symbol, '_')) != NULL)
  {
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

int svd_xlate_name(const char *symbol, char *alias, size_t alias_size)
{
  const REGISTER *reg;
  const char *suffix;
  unsigned long address;

  assert(symbol != NULL);
  reg = register_parse(symbol, &suffix);
  if (reg == NULL)
    return 0;

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
      return 0;
    snprintf(alias, alias_size, "{unsigned}(0x%lx+%d*(%s))", address, reg->increment, regindex);
  } else {
    snprintf(alias, alias_size, "{unsigned}0x%lx", address);
  }

  return 1;
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

int svd_xlate_all_names(char *text, size_t maxsize)
{
  char *head, *tail;
  int count = 0;
  char word[50], alias[50];

  assert(text != NULL);
  head = text;
  while (*head != '\0') {
    unsigned len;
    /* extract next word */
    while (*head != '\0' && *head <= ' ')
      head += 1;
    for (tail = head; *tail > ' '; tail++)
      {}
    len = tail - head;
    if (len == 0 || len >= sizearray(word)) {
      /* no more words (but trailing white-space), or word too long -> skip */
      head = tail;
      continue;
    }
    strncpy(word, head, len);
    word[len] = '\0';
    /* check to replace the register by a memory address */
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

