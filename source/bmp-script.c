/*
 * General purpose "script" support for the Black Magic Probe, so that it can
 * automatically handle device-specific settings. It can use the GDB-RSP serial
 * interface, or the GDB-MI console interface.
 *
 * Copyright 2019-2023 CompuPhase
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
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
# include <direct.h>
# if defined __MINGW32__ || defined __MINGW64__ || defined _MSC_VER
#   include "strlcpy.h"
# endif
# if defined _MSC_VER
#   define strdup(s)         _strdup(s)
#   define stricmp(s1,s2)    _stricmp((s1),(s2))
#   define strnicmp(s1,s2,n) _strnicmp((s1),(s2),(n))
# endif
#else
# include <unistd.h>
# include <bsd/string.h>
# include <sys/stat.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "bmp-script.h"
#include "specialfolder.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif


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

typedef struct tagREG_DEF {     /* register definition (hardcoded or from file) */
  const char *name;
  uint32_t address;
  uint8_t size;
  uint8_t matchlevel;
} REG_DEF;

typedef struct tagSCRIPT_DEF {  /* hard-coded script (in this source file) */
  const char *name;
  const char *mcu_list;
  const char *script;
} SCRIPT_DEF;

typedef struct tagSCRIPTLINE {  /* interpreted script instruction */
  OPERAND lvalue;   /* register address (destination) */
  OPERAND rvalue;   /* value to store */
  uint16_t oper;    /* one of the OP_xxx values */
} SCRIPTLINE;

typedef struct tagSCRIPT {
  struct tagSCRIPT *next;
  const char *name;
  int matchlevel;
  const SCRIPTLINE *lines;
  size_t count;     /* number of lines in the lines array */
} SCRIPT;

typedef struct tagREG_CACHE {
  const char *name;
  const SCRIPTLINE *lines;
  size_t count;     /* number of lines in the lines array */
  size_t index;     /* index of the current line */
} REG_CACHE;


static const struct {
  const char *name;
  uint32_t address;
  uint8_t size;
  const char *mcu_list;
} register_defaults[] = {
  { "SYSCON_SYSMEMREMAP",   0x40048000, 4, "LPC8xx,LPC8N04,LPC11xx*,LPC11Axx,"
                                           "LPC11Cxx,LPC11Exx,LPC11Uxx,LPC11U3x,"
                                           "LPC122x,LPC13xx" },                 /**< LPC Cortex M0 series */
  { "SYSCON_SYSMEMREMAP",   0x40074000, 4, "LPC15xx" },                         /**< LPC15xx series */
  { "SYSCON_SYSMEMREMAP",   0x40000000, 4, "LPC5410x" },                        /**< LPC5410x series */
  { "SCB_MEMMAP",           0x400FC040, 4, "LPC17xx" },                         /**< LPC175x/176x series */
  { "SCB_MEMMAP",           0xE01FC040, 4, "LPC21xx,LPC22xx,LPC23xx,LPC24xx" }, /**< LPC ARM7TDMI series */
  { "M4MEMMAP",             0x40043100, 4, "LPC43xx*" },                        /**< LPC43xx series */
  { "PART_ID",              0X400483F4, 4, "LPC8N04,LPC11xx,LPC11Cxx,LPC11Exx,"
                                           "LPC11Uxx,LPC122x,LPC13xx" },        /**< DEVICE_ID register, LPC Cortex M0 series */
  { "PART_ID",              0x400483F8, 4, "LPC8xx,LPC11xx-XL,LPC11E6x,LPC11U3x,"
                                           "LPC11U6x" },                        /**< DEVICE_ID register, LPC Cortex M0 series */
  { "PART_ID",              0x400743F8, 4, "LPC15xx" },                         /**< DEVICE_ID register, LPC15xx series */
  { "PART_ID",              0x40043200, 4, "LPC43xx" },                         /**< CHIP_ID register, LPC43xx series */
  { "PART_ID",              0x40000FF8, 4, "LPC51Uxx,LPC54S0xx,LPC546xx" },     /**< DEVICE_ID0 register, LPC51Uxx, LPC5411x & LPC546xx series */
  { "PART_ID",              0x400003F8, 4, "LPC5410x" },                        /**< DEVICE_ID0 register, LPC5410x series */

  { "RCC_APB2ENR",          0x40021018, 4, "STM32F1*" },                        /**< STM32F1 APB2 Peripheral Clock Enable Register */
  { "AFIO_MAPR",            0x40010004, 4, "STM32F1*" },                        /**< STM32F1 AF remap and debug I/O configuration */
  { "RCC_AHB1ENR",          0x40023830, 4, "STM32F4*,STM32F7*" },               /**< STM32F4 AHB1 Peripheral Clock Enable Register */
  { "GPIOB_MODER",          0x40020400, 4, "STM32F4*,STM32F7*" },               /**< STM32F4 GPIO Port B Mode Register */
  { "GPIOB_AFRL",           0x40020420, 4, "STM32F4*,STM32F7*" },               /**< STM32F4 GPIO Port B Alternate Function Low Register */
  { "GPIOB_OSPEEDR",        0x40020408, 4, "STM32F4*,STM32F7*" },               /**< STM32F4 GPIO Port B Output Speed Register */
  { "GPIOB_PUPDR",          0x4002040C, 4, "STM32F4*,STM32F7*" },               /**< STM32F4 GPIO Port B Pull-Up/Pull-Down Register */
  { "DBGMCU_IDCODE",        0x40015800, 4, "STM32F03,STM32F05,STM32F07,STM32F09" }, /**< DBGMCU_IDCODE register, IDCODE in low 12 bits */
  { "DBGMCU_IDCODE",        0xE0042000, 4, "STM32F1*,STM32F2*,STM32F3*,STM32F4*,"
                                           "STM32F7*,GD32F1*,GD32F3*,GD32E230" },   /**< DBGMCU_IDCODE register, IDCODE in low 12 bits */
  { "DBGMCU_CR",            0xE0042004, 4, "STM32F03,STM32F05,STM32F07,STM32F09,"
                                           "STM32F1*,STM32F2*,STM32F3*,STM32F4*,"
                                           "STM32F7*,GD32F1*,GD32F3*,GD32E230" },   /**< STM32 Debug MCU Configuration Register */
  { "FLASHSIZE",            0x1FFFF7E0, 4, "STM32F1*" },
  { "FLASHSIZE",            0x1FFFF7CC, 4, "STM32F3*" },
  { "FLASHSIZE",            0x1FFF7A22, 4, "STM32F4*" },
  { "FLASHSIZE",            0x1FF07A22, 4, "STM32F72*,STM32F73*"},
  { "FLASHSIZE",            0x1FF0F442, 4, "STM32F74*,STM32F76*"},
  { "FLASHSIZE",            0x1FF8007C, 4, "STM32L0*" },
  { "FLASHSIZE",            0x1FFFF7CC, 4, "GD32F0*" },
  { "FLASHSIZE",            0x1FFFF7E0, 4, "GD32F1*,GD32F3*,GD32E230" },

  { "TRACECLKDIV",          0x400480AC, 4, "LPC13xx" },
  { "TRACECLKDIV",          0x400740D8, 4, "LPC15xx" },
  { "IOCON_PIO0_9",         0x40044024, 4, "LPC1315,LPC1316,LPC1317,LPC1345,LPC1346,LPC1347" },
  { "IOCON_PIO0_9",         0x40044064, 4, "LPC1311,LPC1313,LPC1342,LPC1343" },
  { "PINASSIGN15",          0x4003803C, 4, "LPC15xx" },

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
  { "memremap", "LPC8xx,LPC11xx*,LPC11Axx,LPC11Cxx,LPC11Exx,LPC11Uxx,LPC12xx,LPC13xx",
    "SYSCON_SYSMEMREMAP = 2"
  },
  { "memremap", "LPC15xx",
    "SYSCON_SYSMEMREMAP = 2"
  },
  { "memremap", "LPC17xx",
    "SCB_MEMMAP = 1"
  },
  { "memremap", "LPC21xx,LPC22xx,LPC23xx,LPC24xx",
    "SCB_MEMMAP = 1"
  },
  { "memremap", "LPC43xx*",
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
    "GPIOB_MODER &= ~0x00c0 \n" /* PB3: use alternate function */
    "GPIOB_MODER |= 0x0080 \n"
    "GPIOB_AFRL &= ~0xf000 \n"  /* set AF0 (==TRACESWO) on PB3 */
    "GPIOB_OSPEEDR |= 0x00c0 \n"/* set max speed on PB3 */
    "GPIOB_PUPDR &= ~0x00c0 \n" /* no pull-up or pull-down on PB3 */
    "DBGMCU_CR |= 0x20 \n"      /* 1 << 5 */
  },
  { "swo_device", "LPC13xx",
     "TRACECLKDIV = 1 \n"
     "IOCON_PIO0_9 = 0x93 \n"
  },
  { "swo_device", "LPC15xx",
    "TRACECLKDIV = 1 \n"
    "PINASSIGN15 &= ~0x0000ff00 \n"
    "PINASSIGN15 |=  0x00000100 \n" /* (pin << 8) */
  },

  /* swo_trace
     $0 = mode: 1 = Manchester, 2 = Asynchronous
     $1 = CPU clock divider, MCU clock / bitrate
     $2 = baudrate (only used for Cortex M0/M0+)
     $3 = memory address for variable; Cortex M0/M0+ */
  { "swo_trace", "*",
    "SCB_DEMCR = 0x1000000 \n"  /* TRCENA (1 << 24) */
    "TPIU_CSPSR = 1 \n"         /* protocol width = 1 bit */
    "TPIU_SPPR = $0 \n"         /* 1 = Manchester, 2 = Asynchronous */
    "TPIU_ACPR = $1 \n"         /* CPU clock divider */
    "TPIU_FFCR = 0 \n"          /* turn off formatter, discard ETM output */
    "ITM_LAR = 0xC5ACCE55 \n"   /* unlock access to ITM registers */
    "ITM_TCR = 0x11 \n"         /* SWOENA (1 << 4) | ITMENA (1 << 0) */
    "ITM_TPR = 0 \n"            /* privileged access is off */
  },
  { "swo_trace", "[M0]",
    "$3 = $2 \n"                /* overrule generic script for M0/M0+, set baudrate */
  },

  /* swo_channels
     $0 = enabled channel bit-mask
     $1 = memory address for variable; Cortex M0/M0+ */
  { "swo_channels", "*",
    "ITM_TER = $0 \n"           /* enable stimulus channel(s) */
  },
  { "swo_channels", "[M0]",
    "$1 = $0 \n"                /* overrule generic script for M0/M0+, mark channel(s) as enabled */
  },

  /* swo_profile (generic)
     $0 = mode: 1 = Manchester, 2 = Asynchronous
     $1 = CPU clock divider for SWO output, MCU clock / bitrate
     $2 = sampling interval divider (0=1K, 15=16K) */
  { "swo_profile", "*",
    "SCB_DEMCR = 0x1000000 \n"  /* TRCENA (1 << 24) */
    "TPIU_CSPSR = 1 \n"         /* protocol width = 1 bit */
    "TPIU_SPPR = $0 \n"         /* 1 = Manchester, 2 = Asynchronous */
    "TPIU_ACPR = $1 \n"         /* CPU clock divider */
    "TPIU_FFCR = 0 \n"          /* turn off formatter, discard ETM output */
    "ITM_LAR = 0xC5ACCE55 \n"   /* unlock access to ITM registers */
    "ITM_TCR = 0x10009 \n"      /* TraceBusID=1 (n << 16) | DWTENA (1 << 3) | ITMENA (1 << 0) */
    "ITM_TPR = 0 \n"            /* privileged access is off */
    "DWT_CTRL = $2<<1 | 0x1201 \n"  /* PCSAMPLENA (1 << 12) | CYCTAP (1 << 9) | POSTPRESET=15 (n << 1) | CYCCNTENA (1 << 0) */
  },

  /* swo_close (generic) */
  { "swo_close", "*",
    "SCB_DEMCR = 0 \n"
    "ITM_LAR = 0xC5ACCE55 \n"   /* unlock access to ITM registers */
    "ITM_TCR = 0 \n"
    "ITM_TPR = 0 \n"            /* privileged access is off */
  },

  /* reading microcontroller's "part id" */
  { "partid", "STM32F*",
    "$ = DBGMCU_IDCODE \n"
  },
  { "partid", "LPC8*,LPC11*,LPC12*,LPC13*,LPC15*,LPC43*,LPC546*",
    "$ = PART_ID \n"
  },

  /* reading the amount of Flash memory, on microcontrollers that support this */
  { "flashsize", "STM32F1*,STM32F3*,STM32F4*,STM32F72*,STM32F73*,STM32F74*,"
                 "STM32F76*,STM32L0*,GD32F0*,GD32F1*,GD32F3*,GD32E230",
    "$ = FLASHSIZE \n"
  },

};


static SCRIPT script_root = { NULL, NULL, 0, NULL, 0 };
static REG_CACHE cache = { NULL, NULL, 0, 0 };


static const char *skipleading(const char *str, bool skip_nl)
{
  assert(str != NULL);
  while (*str != '\0' && *str <= ' ' && (skip_nl || *str != '\n'))
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

/** architecture_match() compares two MCU "family" strings, where an "x" in the
 *  "architecture" string is a wildcard for a digit or a letter. The comparison
 *  is case-insensitive (but the "x" must be lower case).
 *
 *  \param architecture   [IN] The family or series name, with optional
 *                        wildcards.
 *  \param mcufamily      [IN] The name of the microcontroller, as returned by
 *                        the Black Magic Probe.
 *
 *  \return 0 for a mismatch; 1 for a perfect match (no wildcards); 2+ for a
 *          match with one or more wildcards.
 *
 *  \note Not handled here, but a '*' at the end of the "architecture" string is
 *        a "match all" wildcard (for matching on the prefix).
 */
int architecture_match(const char *architecture, const char *mcufamily)
{
  int wildcards = 0;
  int i;
  for (i=0; architecture[i] != '\0' && mcufamily[i] != '\0'; i++) {
    /* if the character in the architecture is a lower case "x", it is a
       wild-card; otherwise the comparison is case-insensitive */
    if (architecture[i] == 'x') {
      if (!isalnum(mcufamily[i]))
        return 0;
      wildcards++;
    } else if (toupper(architecture[i]) != toupper(mcufamily[i])) {
      return 0;
    }
  }
  if (architecture[i] == '\0' && mcufamily[i] == '\0')
    return 1 + wildcards; /* match successful, but quality depends on wildcard count */
  return 0;
}

/** mcu_match() returns whether the MCU family name matches any of the names in
 *  the list. If there is a match, it returns the lowest match level (see
 *  architecture_match() function).
 */
static int mcu_match(const char *mcufamily, const char *list)
{
  const char *head, *separator;
  char matchname[50];
  int matchlevel = 0;

  assert(mcufamily != NULL && list != NULL);
  size_t namelen = strlen(mcufamily);

  /* name should never be empty and should not have leading or trailing
     whitespace */
  assert(namelen > 0 && mcufamily[0] > ' ' && mcufamily[namelen - 1] > ' ');
  /* however, the name may have a suffix for the architecture (M3, M4 or M3/M4),
     and this suffix must be stripped off */
  if ((separator = strrchr(mcufamily, ' ')) != NULL && separator[1] == 'M' && isdigit(separator[2])) {
    separator = skiptrailing(mcufamily, separator);
    namelen = separator - mcufamily;
    assert(namelen > 0 && mcufamily[namelen - 1] > ' ');
  }

  head = skipleading(list, true);
  while (*head != '\0') {
    const char *tail;
    if ((separator = strchr(head, ',')) == NULL)
      separator = strchr(head, '\0');
    tail = skiptrailing(head, separator);
    size_t matchlen = tail - head;
    if (matchlen == namelen && matchlen < sizearray(matchname)) {
      if (head[matchlen - 1] == '*')
        matchlen -= 1; /* strip off any trailing '*' */
      strncpy(matchname, head, matchlen);
      matchname[matchlen] = '\0';
      int level = architecture_match(matchname, mcufamily);
      if (level > 0 && (matchlevel == 0 || level < matchlevel))
        matchlevel = level;
    }
    head = (*separator != '\0') ? skipleading(separator + 1, true) : separator;
  }
  if (matchlevel > 0)
    return matchlevel;  /* exact match, skip match on prefix */

  /* no exact match found, try matching items on prefix */
  head = skipleading(list, true);
  while (*head != '\0') {
    const char *tail, *wildcard;
    if ((separator = strchr(head, ',')) == NULL)
      separator = strchr(head, '\0');
    tail = skiptrailing(head, separator);
    if ((wildcard = strchr(head, '*')) != NULL && wildcard < tail) {
      /* the entry in the MCU list has a wildcard, match up to this position */
      size_t matchlen = wildcard - head;
      /* wildcard must be at the end of the entry */
      assert(wildcard[1] == ',' || wildcard[1] == ' ' || wildcard[1] == '\0');
      if (matchlen == 0)
        return true;   /* match-all wildcard */
      if (namelen > matchlen && matchlen < sizearray(matchname)) {
        char mcuname[50];
        strncpy(mcuname, mcufamily, matchlen);
        mcuname[matchlen] = '\0';
        strncpy(matchname, head, matchlen);
        matchname[matchlen] = '\0';
        int level = architecture_match(matchname, mcuname);
        if (level > 0 && (matchlevel == 0 || level < matchlevel))
          matchlevel = level;
      }
    }
    head = (*separator != '\0') ? skipleading(separator + 1, true) : separator;
  }

  return matchlevel;
}

static bool growbuffer(void **buffer, size_t itemsize, size_t *current, size_t required)
{
  assert(buffer != NULL);
  assert(current != NULL);
  assert(required != 0);
  if (required <= *current)
    return true;  /* buffer large enough, nothing to do */

  size_t newsize = (*current == 0) ? 8 : 2 * *current;
  void *newbuf = malloc(newsize * itemsize);
  if (newbuf == NULL)
    return false; /* allocation fails, leave original buffer untouched (still too small) */

  if (*current > 0) {
    assert(*buffer != NULL);
    memcpy(newbuf, *buffer, *current * itemsize);
    free(*buffer);
  }
  *current = newsize;
  *buffer = newbuf;
  return true;
}

static int find_register(const char *name, const REG_DEF *registers, size_t reg_count)
{
  assert(name != NULL);
  assert(reg_count == 0 || registers != NULL);
  size_t r;
  for (r = 0; r < reg_count && strcmp(name, registers[r].name) != 0; r++)
    {}
  if (r >= reg_count)
    return -1;
  return (int)r;
}

static const SCRIPT *find_script(const char *name)
{
  assert(name != NULL);
  assert(*name != '\0');
  const SCRIPT *script;
  for (script = script_root.next; script != NULL && stricmp(name, script->name) != 0; script = script->next)
    {}
  return script;
}

/** parseline() parses a script line, substituting registers and variable
 *  definitions.
 *
 *  \param line       [in] Text line.
 *  \param registers  [in] Array with registers that match the MCU.
 *  \param reg_count  The number of entries in the register array.
 *  \param oper       [out] The operation code, should be one of the OP_xxx
 *                    enumeration values.
 *  \param lvalue     [out] The address of the register or memory location to
 *                    set.
 *  \param rvalue     [out] The value to set the register or memory location to,
 *                    or an address for a dereferenced assignment.
 *
 *  \return Pointer to the start of the next line in the script, or NULL for a
 *          syntax error.
 */
static const char *parseline(const char *line, const REG_DEF *registers, size_t reg_count,
                             uint16_t *oper, OPERAND *lvalue, OPERAND *rvalue)
{
  assert(line != NULL);

  /* ignore any "set" command */
  line = skipleading(line, false);
  if (strncmp(line, "set", 3) == 0 && line[3] <= ' ')
    line = skipleading(line + 3, false);

  /* lvalue (memory address or register) */
  assert(lvalue != NULL);
  memset(lvalue, 0, sizeof(OPERAND));
  if (isdigit(*line)) {
    lvalue->data = strtoul(line, (char**)&line, 0);
    lvalue->size = 4;
    lvalue->type = OT_ADDRESS;
  } else if (*line == '$') {
    lvalue->data = isdigit(line[1]) ? line[1] - '0' : ~0;
    lvalue->size = 4;
    lvalue->type = OT_PARAM;
    line += 1 + (isdigit(line[1]) != 0);
  } else {
    const char *tail;
    for (tail = line; isalnum(*tail) || *tail == '_'; tail++)
      {}
    assert(tail != line);   /* should be an alphanumeric symbol */
    size_t len = tail - line;
    char regname[64];
    assert(len < sizearray(regname));  /* register names should not be this long */
    if (len >= sizearray(regname))
      return NULL;
    strncpy(regname, line, len);
    regname[len] = '\0';
    assert(registers != NULL);
    int r = find_register(regname, registers, reg_count);
    assert(r >= 0);  /* for predefined script, register should always be found */
    if (r < 0)
      return NULL;
    lvalue->data = registers[r].address;
    lvalue->size = registers[r].size;
    lvalue->type = OT_ADDRESS;
    line = tail;
  }

  /* operation */
  line = skipleading(line, false);
  assert(oper != NULL);
  switch (*line) {
  case '=':
    *oper = OP_MOV;
    line++;
    break;
  case '|':
    *oper = OP_ORR;
    line++;
    if (*line == '=')
      line++;       /* '=' should follow '|', but this isn't enforced yet */
    break;
  case '&':
    *oper = OP_AND;
    line++;
    if (*line == '=')
      line++;       /* '=' should follow '&', but this isn't enforced yet */
    line = skipleading(line, false);
    if (*line == '~')
      *oper = OP_AND_INV;
    break;
  default:
    return NULL;
  }

  /* rvalue (literal, register or parameter) */
  assert(rvalue != NULL);
  memset(rvalue, 0, sizeof(OPERAND));
  bool dereferenced = false;
  line = skipleading(line, false);
  if (*line == '*') {
    dereferenced = true;
    line = skipleading(line, false);
  }
  if (isdigit(*line)) {
    rvalue->data = strtoul(line, (char**)&line, 0);
    rvalue->size = 4;
    rvalue->type = dereferenced ? OT_ADDRESS : OT_LITERAL;
    if (*oper==OP_AND_INV) {
      rvalue->data = ~(rvalue->data); /* literal can be inverted right-away */
      *oper=OP_AND;
    }
  } else if (*line == '$') {
    if (!isdigit(line[1]))
      return NULL;
    rvalue->data = line[1] - '0';
    rvalue->size = 4;
    rvalue->type = OT_PARAM;  //??? limitation: cannot dereference a parameter
    line = skipleading(line + 2, false);
    if (*line == '<' && *(line + 1) == '<') {
      line = skipleading(line + 2, false);
      rvalue->pshift = (uint8_t)strtoul(line, (char**)&line, 0);
      line = skipleading(line, false);
    }
    if (*line == '|') {
      line = skipleading(line + 1, false);
      rvalue->plit = strtoul(line, (char**)&line, 0);
    }
  } else {
    const char *tail;
    size_t r;
    for (tail = line; isalnum(*tail) || *tail == '_'; tail++)
      {}
    assert(registers != NULL);
    for (r = 0; r < reg_count && strncmp(line, registers[r].name, (tail - line))!= 0; r++)
      {}
    assert(r < reg_count);  /* for predefined script, register should always be found */
    if (r >= reg_count)
      return NULL;
    rvalue->data = registers[r].address;
    rvalue->size = registers[r].size;
    rvalue->type = OT_ADDRESS;
    line = tail;
  }
# ifndef NDEBUG
    line = skipleading(line, false);
    assert(*line == '\n' || *line == '\0');
# endif
  if (*line > ' ')
    return NULL;  /* after parsing the line, should land on whitespace or \0 */

  return skipleading(line, true); /* only needed for hard-coded scripts */
}

/** bmscript_load() interprets any hardcoded script that matches the given MCU
 *  and adds these to a list. Then it does the same for scripts loaded from a
 *  support file. This way, additional scripts can be created (for new
 *  micro-controllers) and existing scripts can be overruled.
 *
 *  Scripts can be matched on MCU family name, or on architecture name.
 *
 *  \param mcu    [in] The MCU family name. This parameter must be valid.
 *  \param arch   [in] The Cortex architecture name (M0, M3, etc.). This
 *                parameter may be NULL.
 *
 *  \return The number of scripts that are loaded.
 */
int bmscript_load(const char *mcu, const char *arch)
{
  assert(mcu != NULL);

  /* the name in the root is set to the MCU name, to detect double loading of
     the same script */
  if (script_root.name != NULL && strcmp(script_root.name, mcu) == 0) {
    size_t idx = 0;
    for (const SCRIPT *script = script_root.next; script != NULL; script = script->next)
      idx++;
    return idx;
  }
  bmscript_clear();  /* unload any scripts loaded at this point */

  char path[_MAX_PATH];
  if (folder_AppData(path, sizearray(path))) {
    strlcat(path, DIR_SEPARATOR "BlackMagic", sizearray(path));
#   if defined _MSC_VER
      _mkdir(path);
#   elif defined _WIN32
      mkdir(path);
#   else
      mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
#   endif
    strlcat(path, DIR_SEPARATOR "bmscript", sizearray(path));
  }

  /* create a list of registers, to use in script parsing
     first step: the hard-coded registers */
  REG_DEF *registers = NULL;
  size_t reg_size = 0, reg_count = 0;
  for (size_t idx = 0; idx < sizearray(register_defaults); idx++) {
    int level = mcu_match(mcu, register_defaults[idx].mcu_list);
    if (level > 0) {
      int reg = find_register(register_defaults[idx].name, registers, reg_count);
      if (reg < 0) {
        /* add new definition (register does not yet exist) */
        if (growbuffer((void**)&registers, sizeof(REG_DEF), &reg_size, reg_count + 1)) {
          registers[reg_count].name = strdup(register_defaults[idx].name);
          registers[reg_count].address = register_defaults[idx].address;
          registers[reg_count].size = register_defaults[idx].size;
          registers[reg_count].matchlevel = level;
          if (registers[reg_count].name != NULL)
            reg_count += 1;
        }
      } else if (level < registers[reg].matchlevel) {
        /* overrule existing definition (level lower than the existing definition);
           note that we do not need to copy the name, because it is already set */
        registers[reg].address = register_defaults[idx].address;
        registers[reg].size = register_defaults[idx].size;
        registers[reg].matchlevel = (uint8_t)level;
      }
    }
  }

  /* second step: get the registers from the file */
  FILE *fp;
  if (strlen(path) > 0 && (fp = fopen(path, "rt")) != NULL) {
    char line[512], regname[64], address[64], mcu_list[256];
    while (fgets(line, sizearray(line), fp) != NULL) {
      char *ptr;
      if ((ptr = strchr(line, '#')) != NULL)
        *ptr = '\0';  /* strip comments */
      /* check whether this matches a register definition line */
      if (sscanf(line, "define %s [%[^]]] = %s", regname, mcu_list, address) == 3) {
        int level = mcu_match(mcu, mcu_list);
        if (level > 0) {
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
          int reg = find_register(regname, registers, reg_count);
          if (reg < 0) {
            /* register does not yet exist, add a new entry */
            if (growbuffer((void**)&registers, sizeof(REG_DEF), &reg_size, reg_count + 1)) {
              registers[reg_count].name = strdup(regname);
              registers[reg_count].address = addr;
              registers[reg_count].size = (uint8_t)size;
              registers[reg_count].matchlevel = level;
              if (registers[reg_count].name != NULL)
                reg_count += 1;
            }
          } else if (level <= registers[reg].matchlevel) {
            /* change the existing entry; in this case, we overrule the default
               when the level is lower *or equal* because register definitions
               on the configuration file may overrule the hardcoded ones */
            registers[reg].address = addr;
            registers[reg].size = (uint8_t)size;
            registers[reg].matchlevel = (uint8_t)level;
          }
        }
      }
    }
    fclose(fp);
  }

  char arch_name[50] = "";
  if (arch != NULL && strlen(arch) > 0) {
    assert(strlen(arch) < sizearray(arch_name) - 2);
    sprintf(arch_name, "[%s]", arch);
  }

  /* interpret the scripts, first step: the hard-coded scripts */
  SCRIPTLINE *lines = NULL;
  size_t line_size = 0, line_count = 0;
  for (size_t idx = 0; idx < sizearray(script_defaults); idx++) {
    int mcu_level = mcu_match(mcu, script_defaults[idx].mcu_list);
    int arch_level = (arch_name[0] != '\0') ? mcu_match(arch_name, script_defaults[idx].mcu_list) : 0;
    if (mcu_level > 0 || arch_level == 1) {
      SCRIPT *script = (SCRIPT*)find_script(script_defaults[idx].name);
      if (script != NULL && ((mcu_level > 0 && mcu_level < script->matchlevel) || (arch_level > 0 && arch_level < script->matchlevel))) {
        /* delete the script lines, the script content is overruled by the new
           one (better match level) */
        if (script->count >0)
          free((void*)script->lines);
        script->lines = NULL;
        script->count= 0;
      } else if (script != NULL) {
        /* script exists and it has an equal or better match than the new entry;
           skip this script */
        continue;
      }
      line_count = 0;
      const char *head = skipleading(script_defaults[idx].script, true);
      while (head != NULL && *head != '\0') {
        /* add a new entry in the line list */
        if (growbuffer((void**)&lines, sizeof(SCRIPTLINE), &line_size, line_count + 1)) {
          head = parseline(head, registers, reg_count,
                           &lines[line_count].oper, &lines[line_count].lvalue,
                           &lines[line_count].rvalue);
          line_count += 1;
        }
      }
      /* add the script to the list, either create a new script entry or re-use
         the existing entry */
      if (script == NULL) {
        script = (SCRIPT*)malloc(sizeof(SCRIPT));
        if (script != NULL) {
          script->name = strdup(script_defaults[idx].name);
          if (script->name == NULL) {
            free(script);
            script = NULL;
          }
        }
      }
      if (script != NULL) {
        assert(script->name != NULL);
        script->lines = NULL;
        if (line_count > 0) {
          script->lines = (SCRIPTLINE*)malloc(line_count * sizeof(SCRIPTLINE));
          if (script->lines != NULL)
            memcpy((void*)script->lines, lines, line_count * sizeof(SCRIPTLINE));
          else
            line_count = 0;
        }
        script->count = line_count;
        assert(mcu_level > 0 || arch_level > 0);  /* only one of these should be set */
        script->matchlevel = (mcu_level > 0) ? mcu_level : arch_level;
        script->next = script_root.next;
        script_root.next = script;
      }
    }
  }

  /* now read the scripts from the file */
  if (strlen(path) > 0 && (fp = fopen(path, "rt")) != NULL) {
    char line[512], scriptname[64], mcu_list[256];
    bool inscript = false;
    SCRIPT *script = NULL;
    while (fgets(line, sizearray(line), fp) != NULL) {
      char *ptr;
      if ((ptr = strchr(line, '#')) != NULL)
        *ptr = '\0';  /* strip comments */
      if (*skipleading(line, true) == '\0')
        continue;     /* ignore empty lines (after stripping comments) */
      /* check whether this matches a register definition line */
      if (sscanf(line, "define %s [%[^]]]", scriptname, mcu_list) == 2 && strchr(line, '=') == NULL) {
        assert(!inscript);  /* if inscript is set, the previous script had no 'end' */
        int mcu_level = mcu_match(mcu, mcu_list);
        int arch_level = (arch_name[0] != '\0') ? mcu_match(arch_name, mcu_list) : 0;
        if (mcu_level > 0 || arch_level == 1) {
          script = (SCRIPT*)find_script(scriptname);
          if (script != NULL && ((mcu_level > 0 && mcu_level <= script->matchlevel) || (arch_level > 0 && arch_level <= script->matchlevel))) {
            /* delete the script lines, the script content is overruled by the new
               one (better match level) */
            if (script->count >0)
              free((void*)script->lines);
            script->lines = NULL;
            script->count= 0;
            inscript = true;
          } else if (script == NULL) {
            /* script does not exist, must be created */
            inscript = true;
          } else {
            inscript = false; /* should already be false */
          }
        }
        line_count = 0;
      } else if (inscript && strncmp(line, "end", 3) == 0 && line[3] <= ' ') {
        /* end script, add it to the list, either create a new script entry or
           re-use the existing entry */
        if (script == NULL) {
          script = (SCRIPT*)malloc(sizeof(SCRIPT));
          if (script != NULL) {
            script->name = strdup(scriptname);
            if (script->name == NULL) {
              free(script);
              script = NULL;
            }
          }
        }
        if (script != NULL) {
          assert(script->name != NULL);
          script->lines = NULL;
          if (line_count > 0) {
            script->lines = (SCRIPTLINE*)malloc(line_count * sizeof(SCRIPTLINE));
            if (script->lines != NULL)
              memcpy((void*)script->lines, lines, line_count * sizeof(SCRIPTLINE));
            else
              line_count = 0;
          }
          script->count = line_count;
          script->next = script_root.next;
          script_root.next = script;
        }
        inscript = false;
      } else if (inscript) {
        /* add line to script */
        if (growbuffer((void**)&lines, sizeof(SCRIPTLINE), &line_size, line_count + 1)) {
          parseline(line, registers, reg_count,
                    &lines[line_count].oper, &lines[line_count].lvalue,
                    &lines[line_count].rvalue);
          //??? error message on parse error
          line_count += 1;
        }
      }
    }
    assert(!inscript);  /* if inscript is set, the last script had no 'end' */
    fclose(fp);
  }

  /* free the register list */
  for (size_t idx = 0; idx < reg_count; idx++) {
    assert(registers[idx].name != NULL);
    free((void*)registers[idx].name);
  }
  free((void*)registers);
  /* free the temporary lines list */
  free((void*)lines);

  /* count the scripts, for the return value */
  size_t count = 0;
  for (const SCRIPT *script = script_root.next; script != NULL; script = script->next)
    count++;
  return count;
}

void bmscript_clear(void)
{
  bmscript_clearcache();
  while (script_root.next != NULL) {
    SCRIPT *script = script_root.next;
    script_root.next = script->next;
    assert(script->name != NULL); /* the script is not added to the list if any pointers are invalid */
    free((void*)script->name);
    assert((script->count == 0 && script->lines == NULL) || (script->count > 0 && script->lines != NULL));
    if (script->count >0)
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
 *  \param name     [in] The name of te script; may be set to NULL to continue
 *                  on the last active script.
 *  \param oper     [out] The operation code, should be one of the OP_xxx
 *                  enumeration values.
 *  \param lvalue   [out] The address of the register or memory location to set.
 *  \param rvalue   [out] The value to set the register or memory location to,
 *                  or an address for a dereferenced assigmnent.
 *  \param size     [out] The size of the register/variable in bytes.
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
bool bmscript_line(const char *name, uint16_t *oper, OPERAND *lvalue, OPERAND *rvalue)
{
  if (name == NULL)
    name = cache.name;
  assert(name != NULL);
  assert(oper != NULL && lvalue != NULL && rvalue != NULL);

  if (cache.name == NULL || strcmp(name, cache.name) != 0) {
    const SCRIPT *script;
    /* find a script with the given name */
    for (script = script_root.next; script != NULL && stricmp(name, script->name) != 0; script = script->next)
      {}
    if (script == NULL)
      return false;     /* no script with matching name is found */

    cache.name = script->name;
    cache.lines = script->lines;
    cache.count = script->count;
    cache.index = 0;
  }

  assert(cache.index <= cache.count);
  if (cache.index == cache.count)
    return false; /* end of script reached */
  assert(cache.lines != NULL);
  *oper = cache.lines[cache.index].oper;
  *lvalue = cache.lines[cache.index].lvalue;
  *rvalue = cache.lines[cache.index].rvalue;
  cache.index += 1;

  return true;
}

bool bmscript_line_fmt(const char *name, char *line, const unsigned long *params, size_t paramcount)
{
  uint16_t oper;
  OPERAND lvalue, rvalue;
  if (bmscript_line(name, &oper, &lvalue, &rvalue)) {
    char operstr[10];
    switch (oper) {
    case OP_MOV:
      strcpy(operstr, "=");
      break;
    case OP_ORR:
      strcpy(operstr, "|=");
      break;
    case OP_AND:
      strcpy(operstr, "&=");
      break;
    case OP_AND_INV:
      strcpy(operstr, "&= ~");
      break;
    default:
      assert(0);
    }
    if (rvalue.type == OT_ADDRESS)
      strcat(operstr, " *");
    bool print_cmd = false;
    if (lvalue.type == OT_PARAM) {
      if (lvalue.data == ~0)
        print_cmd = true;
      else if (params != NULL && lvalue.data < paramcount)
        lvalue.data = (uint32_t)params[lvalue.data];  /* replace parameters */
      else
        return false; /* invalid parameter */
    }
    if (rvalue.type == OT_PARAM) {
      if (params != NULL && rvalue.data < paramcount)
        rvalue.data = (uint32_t)params[rvalue.data];  /* replace parameters */
      else
        return false; /* invalid parameter */
      if (rvalue.pshift > 0)
        rvalue.data <<= rvalue.pshift;
      rvalue.data |= rvalue.plit;
    }
    if (print_cmd) {
      switch (rvalue.size) {
      case 1:
        sprintf(line, "print /x {char}0x%x\n", rvalue.data);
        break;
      case 2:
        sprintf(line, "print /x {short}0x%x\n", rvalue.data);
        break;
      default:
        sprintf(line, "print /x {int}0x%x\n", rvalue.data);
      }
    } else {
      uint16_t size =(lvalue.size > 0)? lvalue.size : rvalue.size;
      switch (size) {
      case 1:
        sprintf(line, "set {char}0x%x %s 0x%x\n", lvalue.data, operstr, rvalue.data & 0xff);
        break;
      case 2:
        sprintf(line, "set {short}0x%x %s 0x%x\n", lvalue.data, operstr, rvalue.data & 0xffff);
        break;
      case 4:
        sprintf(line, "set {int}0x%x %s 0x%x\n", lvalue.data, operstr, rvalue.data);
        break;
      default:
        assert(0);
      }
    }
    return true;
  }
  return false;
}

