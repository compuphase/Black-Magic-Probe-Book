/*
 * General purpose "script" support for the Black Magic Probe, so that it can
 * automatically handle device-specific settings. It can use the GDB-RSP serial
 * interface, or the GDB-MI console interface.
 *
 * Copyright 2019-2020 CompuPhase
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
#if defined _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>
  #if defined _MSC_VER
    #define strdup(s)         _strdup(s)
    #define stricmp(s1,s2)    _stricmp((s1),(s2))
    #define strnicmp(s1,s2,n) _strnicmp((s1),(s2),(n))
  #endif
#else
  #include <unistd.h>
  #include <bsd/string.h>
  #include <sys/stat.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "bmp-script.h"
#include "specialfolder.h"

#if defined __linux__ || defined __FreeBSD__ || defined __APPLE__
#  define stricmp(s1,s2)    strcasecmp((s1),(s2))
#  define strnicmp(s1,s2,n) strncasecmp((s1),(s2),(n))
#endif
#if !defined sizearray
#  define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif
#if !defined _MAX_PATH
#  define _MAX_PATH 260
#endif

typedef struct tagREG_DEF {     /* register definition */
  const char *name;
  uint32_t address;
  uint8_t size;
  const char *mcu_list;
} REG_DEF;

typedef struct tagSCRIPT_DEF {  /* hard-coded script (in this source file) */
  const char *name;
  const char *mcu_list;
  const char *script;
} SCRIPT_DEF;

typedef struct tagSCRIPTLINE {  /* interpreted script instruction */
  uint32_t address; /* register address (destination) */
  uint32_t value;   /* value to store */
  uint8_t size;     /* size of the value / register */
  char oper;        /* '=', '|', '&' or '~' */
} SCRIPTLINE;

typedef struct tagSCRIPT {
  struct tagSCRIPT *next;
  const char *name;
  const SCRIPTLINE *lines;
  size_t count;     /* number of lines in the lines array */
} SCRIPT;

typedef struct tagREG_CACHE {
  const char *name;
  const SCRIPTLINE *lines;
  size_t count;     /* number of lines in the lines array */
  size_t index;     /* index of the current line */
} REG_CACHE;


static const REG_DEF register_defaults[] = {
  { "SYSCON_SYSMEMREMAP",   0x40048000, 4, "lpc8xx,lpc11xx,lpc12xx,lpc13xx" },  /**< LPC Cortex M0 series */
  { "SYSCON_SYSMEMREMAP",   0x40074000, 4, "lpc15xx" },                         /**< LPC15xx series */
  { "SCB_MEMMAP",           0x400FC040, 4, "lpc17xx" },                         /**< LPC175x/176x series */
  { "SCB_MEMMAP",           0xE01FC040, 4, "lpc21xx,lpc22xx,lpc23xx,lpc24xx" }, /**< LPC ARM7TDMI series */
  { "M4MEMMAP",             0x40043100, 4, "LPC43xx Cortex-M4" },               /**< LPC43xx series */

  { "RCC_APB2ENR",          0x40021018, 4, "STM32F1*" },                        /**< STM32F1 APB2 Peripheral Clock Enable Register */
  { "AFIO_MAPR",            0x40010004, 4, "STM32F1*" },                        /**< STM32F1 AF remap and debug I/O configuration */
  { "RCC_AHB1ENR",          0x40023830, 4, "STM32F4*,STM32F7*" },               /**< STM32F4 AHB1 Peripheral Clock Enable Register */
  { "GPIOB_MODER",          0x40020400, 4, "STM32F4*,STM32F7*" },               /**< STM32F4 GPIO Port B Mode Register */
  { "GPIOB_AFRL",           0x40020420, 4, "STM32F4*,STM32F7*" },               /**< STM32F4 GPIO Port B Alternate Function Low Register */
  { "GPIOB_OSPEEDR",        0x40020408, 4, "STM32F4*,STM32F7*" },               /**< STM32F4 GPIO Port B Output Speed Register */
  { "GPIOB_PUPDR",          0x4002040C, 4, "STM32F4*,STM32F7*" },               /**< STM32F4 GPIO Port B Pull-Up/Pull-Down Register */
  { "DBGMCU_CR",            0xE0042004, 4, "STM32F03,STM32F05,STM32F07,STM32F09,STM32F1*,STM32F2*,STM32F3*,STM32F4*,STM32F7*" },  /**< STM32 Debug MCU Configuration Register */

  { "TRACECLKDIV",          0x400480AC, 4, "LPC13xx" },
  { "TRACECLKDIV",          0x400740D8, 4, "LPC15xx" },
  { "IOCON_PIO0_9",         0x40044024, 4, "LPC13xx" },

  { "SCB_DHCSR",            0xE000EDF0, 4, "*" },   /**< Debug Halting Control and Status Register */
  { "SCB_DCRSR",            0xE000EDF4, 4, "*" },   /**< Debug Core Register Selector Register */
  { "SCB_DCRDR",            0xE000EDF8, 4, "*" },   /**< Debug Core Register Data Register */
  { "SCB_DEMCR",            0xE000EDFC, 4, "*" },   /**< Debug Exception and Monitor Control Register */

  { "TPIU_SSPSR",           0xE0040000, 4, "*" },   /**< Supported Parallel Port Sizes Register */
  { "TPIU_CSPSR",           0xE0040004, 4, "*" },   /**< Current Parallel Port Size Register */
  { "TPIU_ACPR",            0xE0040010, 4, "*" },   /**< Asynchronous Clock Prescaler Register */
  { "TPIU_SPPR",            0xE00400F0, 4, "*" },   /**< Selected Pin Protocol Register */
  { "TPIU_FFCR",            0xE0040304, 4, "*" },   /**< Formatter and Flush Control Register */
  { "TPIU_DEVID",           0xE0040FC8, 4, "*" },   /**< TPIU Type Register */

  { "DWT_CTRL",             0xE0001000, 4, "*" },   /**< Control Register */
  { "DWT_CYCCNT",           0xE0001004, 4, "*" },   /**< Cycle Count Register */

  { "ITM_TER",              0xE0000E00, 4, "*" },   /**< Trace Enable Register */
  { "ITM_TPR",              0xE0000E40, 4, "*" },   /**< Trace Privilege Register */
  { "ITM_TCR",              0xE0000E80, 4, "*" },   /**< Trace Control Register */
  { "ITM_LAR",              0xE0000FB0, 4, "*" },   /**< Lock Access Register */
  { "ITM_IWR",              0xE0000EF8, 4, "*" },   /**< Integration Write Register */
  { "ITM_IRR",              0xE0000EFC, 4, "*" },   /**< Integration Read Register */
  { "ITM_IMCR",             0xE0000F00, 4, "*" },   /**< Integration Mode Control Register */
  { "ITM_LSR",              0xE0000FB4, 4, "*" },   /**< Lock Status Register */
};

static const SCRIPT_DEF script_defaults[] = {
  /* memory mapping (for Flash programming) */
  { "memremap", "lpc8xx,lpc11xx,lpc12xx,lpc13xx",
    "SYSCON_SYSMEMREMAP = 2"
  },
  { "memremap", "lpc15xx",
    "SYSCON_SYSMEMREMAP = 2"
  },
  { "memremap", "lpc17xx",
    "SCB_MEMMAP = 1"
  },
  { "memremap", "lpc21xx,lpc22xx,lpc23xx,lpc24xx",
    "SCB_MEMMAP = 1"
  },
  { "memremap", "LPC43xx Cortex-M4",
    "M4MEMMAP = 0"
  },

  /* MCU-specific & generic configuration for SWO tracing */
  { "swo_device", "STM32F1*",
    "RCC_APB2ENR |= 1 \n"
    "AFIO_MAPR |= 0x2000000 \n" /* 2 << 24 */
    "DBGMCU_CR |= 0x20 \n"      /* 1 << 5 */
  },
  { "swo_device", "STM32F03,STM32F05,STM32F07,STM32F09,STM32F2*,STM32F3*",
    "DBGMCU_CR |= 0x20 \n"      /* 1 << 5 */
  },
  { "swo_device", "STM32F4*,STM32F7*",
    "RCC_AHB1ENR |= 0x02 \n"    /* enable GPIOB clock */
    "GPIOB_MODER ~= 0x00c0 \n"  /* PB3: use alternate function */
    "GPIOB_MODER |= 0x0080 \n"
    "GPIOB_AFRL ~= 0xf000 \n"   /* set AF0 (==TRACESWO) on PB3 */
    "GPIOB_OSPEEDR |= 0x00c0 \n"/* set max speed on PB3 */
    "GPIOB_PUPDR ~= 0x00c0 \n"  /* no pull-up or pull-down on PB3 */
    "DBGMCU_CR |= 0x20 \n"      /* 1 << 5 */
  },
  { "swo_device", "LPC13xx",
     "TRACECLKDIV = 1 \n"
     "IOCON_PIO0_9 = 0x93 \n"
  },
  { "swo_device", "LPC15xx",
    "TRACECLKDIV = 1\n"
    /* LPC_SWM->PINASSIGN15 = (LPC_SWM->PINASSIGN15 & ~(0xff << 8)) | (pin << 8); */
  },

  { "swo_generic", "*",
    "SCB_DEMCR = 0x1000000 \n"   /* 1 << 24 */
    "TPIU_CSPSR = 1 \n"          /* protocol width = 1 bit */
    "TPIU_SPPR = $0 \n"          /* 1 = Manchester, 2 = Asynchronous */
    "TPIU_ACPR = $1 \n"          /* CPU clock divider */
    "TPIU_FFCR = 0 \n"           /* turn off formatter, discard ETM output */
    "ITM_LAR = 0xC5ACCE55 \n"    /* unlock access to ITM registers */
    "ITM_TCR = 0x11 \n"          /* (1 << 4) | 1 */
    "ITM_TPR = 0 \n"             /* privileged access is off */
  },

  { "swo_channels", "*",
    "ITM_TER = $0"              /* enable stimulus channel(s) */
  },
};


static SCRIPT script_root = { NULL, NULL, NULL, 0 };
static REG_CACHE cache = { NULL, NULL, 0, 0 };


static const char *skipleading(const char *str)
{
  assert(str != NULL);
  while (*str != '\0' && *str <= ' ')
    str++;
  return str;
}

static const char *skiptrailing(const char *base, const char *end)
{
  assert(base != NULL && end != NULL);
  while (end > base && *(end - 1) <= ' ')
    end--;
  return end;
}

static int mcu_match(const char *name, const char *list)
{
  const char *head, *separator;
  size_t namelen;

  assert(name != NULL && list != NULL);

  namelen = strlen(name);
  /* name should never be empty and should not have leading or trailing
     whitespace */
  assert(namelen > 0 && name[0] > ' ' && name[namelen - 1] > ' ');
  /* however, the name may have a suffix for the architecture (M3, M4 or M3/M4),
     and this suffix must be stripped off */
  if ((separator = strrchr(name, ' ')) != NULL && separator[1] == 'M' && isdigit(separator[2])) {
    separator = skiptrailing(name, separator);
    namelen = separator - name;
    assert(namelen > 0 && name[namelen - 1] > ' ');
  }

  head = skipleading(list);
  while (*head != '\0') {
    const char *tail, *wildcard;
    if ((separator = strchr(head, ',')) == NULL)
      separator = strchr(head, '\0');
    tail = skiptrailing(head, separator);
    if ((tail - head) == namelen && strnicmp(name, head, namelen) == 0)
      return 1;   /* exact match */
    if ((wildcard = strchr(head, '*')) != NULL && wildcard < tail) {
      /* the entry in the MCU list has a wildcard, match up to this position */
      size_t matchlen = wildcard - head;
      /* wildcard must be at the end of the entry */
      assert(wildcard[1] == ',' || wildcard[1] == ' ' || wildcard[1] == '\0');
      if (namelen > matchlen && (matchlen == 0 || strnicmp(name, head, matchlen) == 0))
        return 1; /* match on prefix */
    }
    head = (*separator != '\0') ? skipleading(separator + 1) : separator;
  }
  return 0;
}

static const char *parseline(const char *line, const REG_DEF *registers, size_t reg_count,
                             char *oper, uint32_t *address, uint32_t *value, uint8_t *size)
{
  int invert = 0;

  assert(line != NULL);

  /* ignore any "set" command */
  line = skipleading(line);
  if (strncmp(line, "set", 3) == 0 && line[3] <= ' ')
    line = skipleading(line + 3);

  /* memory address or register*/
  assert(address != NULL && size != NULL);
  if (isdigit(*line)) {
    *address = strtoul(line, (char**)&line, 0);
    *size = 4;
  } else {
    const char *tail;
    size_t r;
    for (tail = line; isalnum(*tail) || *tail == '_'; tail++)
      {}
    assert(registers != NULL);
    for (r = 0; r < reg_count && strncmp(line, registers[r].name, (tail - line))!= 0; r++)
      {}
    assert(r < reg_count);  /* for predefined script, register should always be found */
    *address = registers[r].address;
    *size = registers[r].size;
    line = tail;
  }

  /* operation */
  line = skipleading(line);
  assert(*line == '=' || *line == '|' || *line == '&');
  assert(oper != NULL);
  *oper = *line++;
  if (*line == '=')
    line++;     /* allow |= to mean | and &= to mean & */
  line = skipleading(line);
  if (*line == '~') {
    invert = 1;
    line = skipleading(line + 1);
  }

  /* parameter */
  assert(value != NULL);
  if (*line == '$') {
    *value = SCRIPT_MAGIC + (line[1] - '0');
    line += 2;
    assert(!invert || *oper == '&');
    if (*oper == '&' && invert)
      *oper = '~';
  } else {
    *value = strtoul(line, (char**)&line, 0);
    if (invert)
      *value = ~(*value);
  }
  #ifndef NDEBUG
    while (*line != '\0' && *line != '\n' && *line <= ' ')
      line++;
    assert(*line == '\n' || *line == '\0');
  #endif

  return skipleading(line); /* only needed for hard-coded scripts */
}

/** bmscript_load() interprets any hardcoded script that matches the given MCU
 *  and adds these to a list. Then it does the same for scripts loaded from a
 *  support file. This way, additional scripts can be created (for new
 *  micro-controllers) and existing scripts can be overruled.
 */
int bmscript_load(const char *mcu)
{
  REG_DEF *registers = NULL;
  size_t reg_size = 0, reg_count = 0;
  SCRIPTLINE *lines = NULL;
  size_t line_size = 0, line_count = 0;
  SCRIPT *script;
  char path[_MAX_PATH];
  FILE *fp;
  int idx;

  /* the name in the root is set the the MCU name, to detect double loading of
     the same script */
  if (script_root.name != NULL && strcmp(script_root.name, mcu) == 0) {
    idx = 0;
    for (script = script_root.next; script != NULL; script = script->next)
      idx++;
    return idx;
  }
  bmscript_clear();  /* unload any scripts loaded at this point */

  if (folder_AppData(path, sizearray(path))) {
    strlcat(path, DIR_SEPARATOR "BlackMagic", sizearray(path));
    #if defined _WIN32
      mkdir(path);
    #else
      mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    #endif
    strlcat(path, DIR_SEPARATOR "bmscript", sizearray(path));
  }

  /* create a list of registers, to use in script parsing
     first step: the hard-coded registers */
  for (idx = 0; idx < sizearray(register_defaults); idx++) {
    if (mcu_match(mcu, register_defaults[idx].mcu_list)) {
      assert(reg_count <= reg_size);
      if (reg_count == reg_size) {
        /* we need to grow the array for the registers */
        size_t newsize = (reg_size == 0) ? 8 : 2 * reg_size;
        REG_DEF *newbuf = (REG_DEF*)realloc(registers, newsize * sizeof(REG_DEF));
        if (newbuf != NULL) {
          reg_size = newsize;
          registers = newbuf;
        }
      }
      if (reg_count < reg_size) {
        registers[reg_count].name = strdup(register_defaults[idx].name);
        registers[reg_count].address = register_defaults[idx].address;
        registers[reg_count].size = register_defaults[idx].size;
        registers[reg_count].mcu_list = NULL;
        if (registers[reg_count].name != NULL)
          reg_count += 1;
      }
    }
  }

  /* second step: get the registers from the file */
  if (strlen(path) > 0 && (fp = fopen(path, "rt")) != NULL) {
    char line[512], regname[64], address[64], mcu_list[256], *ptr;
    while (fgets(line, sizearray(line), fp) != NULL) {
      if ((ptr = strchr(line, '#')) != NULL)
        *ptr = '\0';  /* strip comments */
      /* check whether this matches a register definition line */
      if (sscanf(line, "define %s [%[^]]] = %s", regname, mcu_list, address) == 3
          && mcu_match(mcu, mcu_list))
      {
        unsigned long addr;
        int size = 4;
        if (address[0] == '{' && (ptr = strchr(address, '}')) != NULL) {
          addr = strtoul(ptr + 1, NULL, 0);
          if (strncmp(address, "{short}", 7) == 0)
            size = 2;
          else if (strncmp(address, "{char}", 6) == 0 || strncmp(address, "{byte}", 6) == 0)
            size = 1;
        } else {
          addr = strtoul(address, NULL, 0);
        }
        /* check whether this definition overrules a default register definition */
        for (idx = 0; idx < reg_count && strcmp(registers[idx].name, regname) != 0; idx++)
          {}
        if (idx < reg_count) {
          /* change the existing entry */
          registers[idx].address = addr;
          registers[idx].size = size;
        } else {
          /* add a new entry */
          assert(reg_count <= reg_size);
          if (reg_count == reg_size) {
            /* we need to grow the array for the registers */
            size_t newsize = (reg_size == 0) ? 8 : 2 * reg_size;
            REG_DEF *newbuf = (REG_DEF*)realloc(registers, newsize * sizeof(REG_DEF));
            if (newbuf != NULL) {
              reg_size = newsize;
              registers = newbuf;
            }
          }
          if (reg_count < reg_size) {
            registers[reg_count].name = strdup(regname);
            registers[reg_count].address = addr;
            registers[reg_count].size = size;
            registers[reg_count].mcu_list = NULL;
            if (registers[reg_count].name != NULL)
              reg_count += 1;
          }
        }
      }
    }
    fclose(fp);
  }

  /* interpret the scripts, first step: the hard-coded scripts */
  for (idx = 0; idx < sizearray(script_defaults); idx++) {
    if (mcu_match(mcu, script_defaults[idx].mcu_list)) {
      const char *head;
      line_count = 0;
      head = skipleading(script_defaults[idx].script);
      while (*head != '\0') {
        /* make space for a new entry in the line list */
        assert(line_count <= line_size);
        if (line_count == line_size) {
          /* we need to grow the array for the registers */
          size_t newsize = (line_size == 0) ? 8 : 2 * line_size;
          SCRIPTLINE *newbuf = (SCRIPTLINE*)realloc(lines, newsize * sizeof(SCRIPTLINE));
          if (newbuf != NULL) {
            lines = newbuf;
            line_size = newsize;
          }
        }
        if (line_count < line_size) {
          head = parseline(head, registers, reg_count,
                           &lines[line_count].oper, &lines[line_count].address,
                           &lines[line_count].value, &lines[line_count].size);
          line_count += 1;
        }
      }
      /* add the script to the list */
      if ((script = (SCRIPT*)malloc(sizeof(SCRIPT))) != NULL) {
        script->name = strdup(script_defaults[idx].name);
        script->lines = (SCRIPTLINE*)malloc(line_count * sizeof(SCRIPTLINE));
        if (script->name != NULL && script->lines != NULL) {
          memcpy((void*)script->lines, lines, line_count * sizeof(SCRIPTLINE));
          script->count = line_count;
          script->next = script_root.next;
          script_root.next = script;
        } else {
          if (script->name != NULL)
            free((void*)script->name);
          if (script->lines != NULL)
            free((void*)script->lines);
        }
      }
    }
  }

  /* now read the scripts from the file */
  if (strlen(path) > 0 && (fp = fopen(path, "rt")) != NULL) {
    char line[512], scriptname[64], mcu_list[256];
    int inscript = 0;
    while (fgets(line, sizearray(line), fp) != NULL) {
      char *ptr;
      if ((ptr = strchr(line, '#')) != NULL)
        *ptr = '\0';  /* strip comments */
      if (*skipleading(line) == '\0')
        continue;     /* ignore empty lines (after stripping comments) */
      /* check whether this matches a register definition line */
      if (sscanf(line, "define %s [%[^]]]", scriptname, mcu_list) == 2
          && strchr(line, '=') == NULL && mcu_match(mcu, mcu_list))
      {
        assert(!inscript);  /* if inscript is set, the previous script had no 'end' */
        inscript = 1;
        line_count = 0;
      } else if (inscript && strncmp(line, "end", 3) == 0 && line[3] <= ' ') {
        /* end script (add it to the list) */
        if ((script = (SCRIPT*)malloc(sizeof(SCRIPT))) != NULL) {
          script->name = strdup(scriptname);
          script->lines = (SCRIPTLINE*)malloc(line_count * sizeof(SCRIPTLINE));
          if (script->name != NULL && script->lines != NULL) {
            memcpy((void*)script->lines, lines, line_count * sizeof(SCRIPTLINE));
            script->count = line_count;
            script->next = script_root.next;
            script_root.next = script;
          } else {
            if (script->name != NULL)
              free((void*)script->name);
            if (script->lines != NULL)
              free((void*)script->lines);
          }
        }
        inscript = 0;
      } else if (inscript) {
        /* add line to script, make space for a new entry in the line list */
        assert(line_count <= line_size);
        if (line_count == line_size) {
          /* we need to grow the array for the registers */
          size_t newsize = (line_size == 0) ? 8 : 2 * line_size;
          SCRIPTLINE *newbuf = (SCRIPTLINE*)realloc(lines, newsize * sizeof(SCRIPTLINE));
          if (newbuf != NULL) {
            lines = newbuf;
            line_size = newsize;
          }
        }
        if (line_count < line_size) {
          parseline(line, registers, reg_count,
                    &lines[line_count].oper, &lines[line_count].address,
                    &lines[line_count].value, &lines[line_count].size);
          line_count += 1;
        }
      }
    }
    assert(!inscript);  /* if inscript is set, the last script had no 'end' */
    fclose(fp);
  }

  /* free the register list */
  for (idx = 0; idx < reg_count; idx++) {
    assert(registers[idx].name != NULL);
    free((void*)registers[idx].name);
  }
  free((void*)registers);
  /* free the temporary lines list */
  free((void*)lines);

  /* count the scripts, for the return value */
  idx = 0;
  for (script = script_root.next; script != NULL; script = script->next)
    idx++;
  return idx;
}

void bmscript_clear(void)
{
  while (script_root.next != NULL) {
    SCRIPT *script = script_root.next;
    script_root.next = script->next;
    assert(script->name != NULL); /* the script is not added to the list if any pointers are invalid */
    free((void*)script->name);
    assert(script->lines != NULL);
    free((void*)script->lines);
    free(script);
  }
  if (script_root.name != NULL) {
    free((void*)script_root.name);
    script_root.name = NULL;
  }
}

/** bmscript_clearcache() clears the cache for the script most recently found.
 *  It is needed if you want to run the same script on the same MCU a second
 *  time. If the cache is not cleared in between, scriptline() would return
 *  false (for end of script reached) immediately.
 */
void bmscript_clearcache(void)
{
  if (cache.name != NULL)
    free((void*)cache.name);
  if (cache.lines != NULL)
    free((void*)cache.lines);
  cache.name = NULL;
  cache.lines = NULL;
  cache.count = 0;
  cache.index = 0;
}

/** bmp_scriptline() returns the next instruction from a script for a specific
 *  micro-controller. When this function is called with a new script name or a
 *  new mcu name, the first instruction for the requested script that matches
 *  the given mcu is returned. For every next call with the same parameters, the
 *  next instruction is returned, until the script completes.
 *
 *  \param name     The name of te script; may be set to NULL to continue on the
 *                  last active script.
 *  \param oper     The operation code, should be '=', '|' or '&'.
 *  \param address  The address of the register or memory location to set.
 *  \param value    The value to set the register or memory location to.
 *  \param size     The size of the register in bytes.
 *
 *  \return 1 of success, 0 on failure. Failure can mean that no script matches,
 *          or that the script contains no more instructions.
 *
 *  \note The script can be for a specific device or it can be a generic script.
 *        In this last case, the script has a "*" in its device list.
 *
 *        Each line in the script has a register/memory setting (it is assumed
 *        that registers are memory-mapped). The setting consists of an address,
 *        a value, a size, and an operator. The size is typically 4 (32-bit
 *        registers), but may be 1 or 2 as well. The operator is '=' for a
 *        simple assignment ("value" is stored at "address"), '|' to set bits in
 *        the current register value, and '&' to clear bits. For the last
 *        option: a 1 bit in value, clears that bit im the register (so it is an
 *        AND with the inverse of "value").
 */
int bmscript_line(const char *name, char *oper, uint32_t *address, uint32_t *value, uint8_t *size)
{
  if (name == NULL)
    name = cache.name;
  assert(name != NULL);
  assert(oper != NULL && address != NULL && value != NULL && size != NULL);

  if (cache.name == NULL || strcmp(name, cache.name) != 0) {
    const SCRIPT *script;
    /* find a script with the given name */
    for (script = script_root.next; script != NULL && stricmp(name, script->name) != 0; script = script->next)
      {}
    if (script == NULL)
      return 0;     /* no script with matching name is found */

    if (cache.name != NULL)
      free((void*)cache.name);
    cache.name = strdup(name);
    cache.lines = script->lines;
    cache.count = script->count;
    cache.index = 0;
  }

  assert(cache.index <= cache.count);
  if (cache.index == cache.count)
    return 0; /* end of script reached */
  assert(cache.lines != NULL);
  *oper = cache.lines[cache.index].oper;
  *address = cache.lines[cache.index].address;
  *value = cache.lines[cache.index].value;
  *size = cache.lines[cache.index].size;
  cache.index += 1;

  return 1;
}

int bmscript_line_fmt(const char *name, char *line, const unsigned long *params)
{
  char oper;
  uint32_t address, value;
  uint8_t size;
  if (bmscript_line(name, &oper, &address, &value, &size)) {
    char operstr[10];
    switch (oper) {
    case '=':
      strcpy(operstr, "=");
      break;
    case '|':
      strcpy(operstr, "|=");
      break;
    case '&':
      strcpy(operstr, "&=");
      break;
    case '~':
      strcpy(operstr, "&= ~");
      break;
    default:
      assert(0);
    }
    if ((value & ~0xf) == SCRIPT_MAGIC) {
      assert(params != NULL);
      value = (uint32_t)params[value & 0xf];  /* replace parameters */
    }
    switch (size) {
    case 1:
      sprintf(line, "set {char}0x%x %s 0x%x\n", address, operstr, value & 0xff);
      break;
    case 2:
      sprintf(line, "set {short}0x%x %s 0x%x\n", address, operstr, value & 0xffff);
      break;
    case 4:
      sprintf(line, "set {int}0x%x %s 0x%x\n", address, operstr, value);
      break;
    default:
      assert(0);
    }
    return 1;
  }
  return 0;
}

